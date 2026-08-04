// Fill out your copyright notice in the Description page of Project Settings.

#include "HucomsServerSubsystem.h"

#include "HucomsProtocol.h"
#include "PTZCamera.h"
#include "PTZCaptureComponent.h"
#include "MjpegStreamServer.h"

#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"      // FHttpServerRequest, EHttpServerRequestVerbs
#include "HttpPath.h"
#include "IHttpRouter.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/CameraComponent.h"
#include "ContentStreaming.h"   // IStreamingManager — CCTV 시점을 텍스처 스트리머에 등록

// --- 비동기 스트림 캡처 파이프라인 ---
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"           // FTextureRenderTargetResource
#include "RenderingThread.h"           // ENQUEUE_RENDER_COMMAND, FlushRenderingCommands
#include "RHIGPUReadback.h"            // FRHIGPUTextureReadback
#include "RHICommandList.h"            // FRHICommandListImmediate, Transition
#include "ImageUtils.h"                // FImageUtils::CompressImage
#include "ImageCore.h"                 // FImage
#include "Async/Async.h"               // AsyncTask
#include "Modules/ModuleManager.h"     // ImageWrapper 선로딩
#include "IImageWrapperModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogHucomsSim, Log, All);

//======================================================================================
// 로컬 헬퍼 (쿼리 파싱 / 응답 생성)
//======================================================================================
namespace
{
	FString GetQ(const FHttpServerRequest& Req, const TCHAR* Key, const FString& Def = FString())
	{
		if (const FString* V = Req.QueryParams.Find(Key))
		{
			return *V;
		}
		return Def;
	}

	bool HasQ(const FHttpServerRequest& Req, const TCHAR* Key)
	{
		return Req.QueryParams.Contains(Key);
	}

	int32 GetQInt(const FHttpServerRequest& Req, const TCHAR* Key, int32 Def)
	{
		if (const FString* V = Req.QueryParams.Find(Key))
		{
			return FCString::Atoi(**V);
		}
		return Def;
	}

	float GetQFloat(const FHttpServerRequest& Req, const TCHAR* Key, float Def)
	{
		if (const FString* V = Req.QueryParams.Find(Key))
		{
			return FCString::Atof(**V);
		}
		return Def;
	}

	void AppendUtf8(TArray<uint8>& Out, const FString& Str)
	{
		FTCHARToUTF8 Utf8(*Str);
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	/** text/plain 응답 ('key = value' 본문). 실기와 동일하게 항상 200. */
	TUniquePtr<FHttpServerResponse> MakeText(const FString& Body, const FString& ContentType = TEXT("text/plain"))
	{
		return FHttpServerResponse::Create(Body, ContentType);
	}

	/** 바이너리 응답 (JPEG/멀티파트). content-type 그대로 보존. */
	TUniquePtr<FHttpServerResponse> MakeBytes(TArray<uint8>&& Bytes, const FString& ContentType)
	{
		return FHttpServerResponse::Create(MoveTemp(Bytes), ContentType);
	}

	/** 4바이트 빈 JPEG (SOI+EOI). 렌더 실패 시 스텁 - fake-camera-client 와 동일. */
	TArray<uint8> StubJpeg()
	{
		return TArray<uint8>({ 0xFF, 0xD8, 0xFF, 0xD9 });
	}

	/** Cur 를 Tgt 방향으로 MaxStep 만큼 한 발 이동(선형 슬루). 일반 축용. */
	int32 StepLinear(int32 Cur, int32 Tgt, int32 MaxStep)
	{
		const int32 D = Tgt - Cur;
		if (FMath::Abs(D) <= MaxStep) { return Tgt; }
		return Cur + ((D > 0) ? MaxStep : -MaxStep);
	}
}

//======================================================================================
// Subsystem lifecycle
//======================================================================================
bool UHucomsServerSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// 실제 플레이(게임/PIE)에서만 서버를 띄운다. 에디터 프리뷰/인스펙터 월드는 제외.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UHucomsServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// 서버 시작은 월드 BeginPlay 이후(액터 스폰 완료)에 한다.
}

void UHucomsServerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	StartServers();
}

void UHucomsServerSubsystem::Deinitialize()
{
	StopServers();
	Super::Deinitialize();
}

//======================================================================================
// 채널 구성 (레벨의 카메라 열거 -> 포트 부여)
//======================================================================================
void UHucomsServerSubsystem::BuildChannels()
{
	Channels.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// config 스포너: DefaultGame.ini 의 +SpawnCameras=(...) 로 지정한 높이별 카메라를 GetAllActorsOfClass
	// 전에 스폰해, 같은 열거 패스에서 채널·포트를 받게 한다(자동스폰 선례와 동일 경로).
	SpawnConfiguredCameras(World);

	TArray<AActor*> Actors;
	UGameplayStatics::GetAllActorsOfClass(World, APTZCamera::StaticClass(), Actors);

	// 카메라가 하나도 없으면 기본 카메라 자동 생성(1회) — 박스아웃 동작 + 자율 검증용.
	if (Actors.Num() == 0 && bAutoSpawnCameraIfNone && !bAutoSpawnAttempted)
	{
		bAutoSpawnAttempted = true;

		FVector Loc(0.f, 0.f, 300.f);
		float Yaw = 0.f;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (AActor* Pawn = PC->GetPawn())
			{
				Loc = Pawn->GetActorLocation() + FVector(0.f, 0.f, 250.f);
				Yaw = Pawn->GetActorRotation().Yaw;
			}
		}
		const FRotator Rot(-15.f, Yaw, 0.f); // 살짝 아래를 보는 CCTV 자세

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (APTZCamera* Spawned = World->SpawnActor<APTZCamera>(APTZCamera::StaticClass(), Loc, Rot, Params))
		{
			Actors.Add(Spawned);
			SpawnedCameras.Add(Spawned);
			UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 레벨에 APTZCamera 없음 -> 자동 생성 @ %s (yaw=%.1f)"), *Loc.ToString(), Yaw);
		}
	}

	// AutoIndex 는 '자동 포트' 카메라에서만 증가한다. 명시 포트(config 스포너 카메라)는 인덱스를
	// 소비하지 않으므로, 명시 카메라가 몇 대 섞여도 레벨의 자동 포트 카메라는 8081/8082… 순서를 유지한다.
	int32 AutoIndex = 0;
	for (AActor* A : Actors)
	{
		APTZCamera* Cam = Cast<APTZCamera>(A);
		if (!Cam || !Cam->bServeHucoms)
		{
			continue;
		}

		const bool bAutoPort = (Cam->HucomsHttpPort <= 0) || (Cam->HucomsMjpegPort <= 0);
		CreateChannelForCamera(Cam,
			(Cam->HucomsHttpPort  > 0) ? Cam->HucomsHttpPort  : (BaseHttpPort  + AutoIndex),
			(Cam->HucomsMjpegPort > 0) ? Cam->HucomsMjpegPort : (BaseMjpegPort + AutoIndex));
		if (bAutoPort) { ++AutoIndex; }
	}
}

void UHucomsServerSubsystem::SpawnConfiguredCameras(UWorld* World)
{
	if (bConfigCamerasSpawned || !World)
	{
		return;
	}
	bConfigCamerasSpawned = true;

	for (const FPTZCameraSpawnSpec& Spec : SpawnCameras)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const FRotator Rot(Spec.PitchDeg, Spec.YawDeg, 0.f); // Pitch 는 BuildChannels 가 tilt 로 이관

		APTZCamera* Cam = World->SpawnActor<APTZCamera>(APTZCamera::StaticClass(), Spec.Location, Rot, Params);
		if (!Cam)
		{
			UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] config 카메라 스폰 실패 (%s @ %s)"), *Spec.Note, *Spec.Location.ToString());
			continue;
		}
		Cam->bServeHucoms = true;
		Cam->bFixedMode   = Spec.bFixedMode;
		SpawnedCameras.Add(Cam);
		if (Spec.HttpPort  > 0) { Cam->HucomsHttpPort  = Spec.HttpPort; }
		if (Spec.MjpegPort > 0) { Cam->HucomsMjpegPort = Spec.MjpegPort; }
#if WITH_EDITOR
		if (!Spec.Note.IsEmpty()) { Cam->SetActorLabel(FString::Printf(TEXT("PTZ_Spawn_%s"), *Spec.Note)); }
#endif
		UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] config 카메라 스폰: '%s' @ %s  http=%d mjpeg=%d"),
			*Spec.Note, *Spec.Location.ToString(), Spec.HttpPort, Spec.MjpegPort);
	}
}

//======================================================================================
// HTTP server start/stop (채널별)
//======================================================================================
void UHucomsServerSubsystem::StartServers()
{
	if (bServersStarted)
	{
		return;
	}

	BuildChannels();
	if (Channels.Num() == 0)
	{
		UE_LOG(LogHucomsSim, Warning, TEXT("[Hucoms] 서빙할 APTZCamera(bServeHucoms) 가 없음 -> 서버 미기동."));
		return;
	}

	// 비동기 스트림 인코딩을 워커 스레드에서 하려면 ImageWrapper 모듈이 게임 스레드에서 미리
	// 로드돼 있어야 한다(FImageUtils::CompressImage 는 비게임스레드면 GetModulePtr 만 씀 → null 이면 실패).
	if (bAsyncStreamCapture)
	{
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		CompletedFrames = MakeShared<TQueue<FCompletedStreamFrame, EQueueMode::Mpsc>, ESPMode::ThreadSafe>();
	}

	FHttpServerModule& Http = FHttpServerModule::Get();
	int32 StartedCount = 0;

	for (TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		if (StartChannelServers(ChPtr)) { ++StartedCount; }
	}

	Http.StartAllListeners();
	bServersStarted = true;

	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 시뮬레이터 서버 시작 — 채널 %d/%d 개."), StartedCount, Channels.Num());
}

void UHucomsServerSubsystem::StopServers()
{
	// 비동기 리드백 종료 규율: 채널을 파괴하기 전에 in-flight 렌더커맨드(EnqueueCopy/Lock)를
	// 전부 완료시킨다. 이 커맨드들이 채널 소유 FRHIGPUTextureReadback 을 raw 로 물고 있어,
	// flush 없이 파괴하면 use-after-free 다. flush 후에는 pending 커맨드가 없어 게임스레드
	// 파괴(TUniquePtr)가 안전하다. (워커 인코딩 태스크는 값복사만 참조 — 채널과 무관.)
	if (bAsyncStreamCapture)
	{
		bool bAnyInFlight = false;
		for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
		{
			if (ChPtr.IsValid() && ChPtr->StreamCapState != EStreamCapState::Idle)
			{
				bAnyInFlight = true;
				break;
			}
		}
		if (bAnyInFlight)
		{
			FlushRenderingCommands();
		}
	}

	for (TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		StopChannel(*ChPtr);
	}
	Channels.Reset();

	if (bServersStarted)
	{
		// 이 프로젝트만 HTTP 서버를 쓰므로 전역 정지로 충분(모든 채널 리스너 정지).
		FHttpServerModule::Get().StopAllListeners();
		bServersStarted = false;
		UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 시뮬레이터 서버 정지(모든 채널)."));
	}
}

TSharedPtr<FHucomsChannel> UHucomsServerSubsystem::CreateChannelForCamera(APTZCamera* Cam, int32 InHttpPort, int32 InMjpegPort)
{
	TSharedPtr<FHucomsChannel> Ch = MakeShared<FHucomsChannel>();
	Ch->Camera    = Cam;
	Ch->HttpPort  = InHttpPort;
	Ch->MjpegPort = InMjpegPort;

	// 홈 포즈 정렬: pan 은 설치 heading(+X)을 그대로 보게 0.
	Ch->CurPan = Ch->TgtPan = 0;
	// 상하 조준 이관: 광학축은 액터 Pitch 를 상속하지 않으므로(롤 방지), 설치 시 액터에
	// 넣어둔 하방 조준(Pitch)을 '틸트'로 옮겨 같은 화각을 롤 없이 재현한다.
	//   MirrorChannel: UE pitch = TiltToPitchSign * (tilt/100)  =>  tilt = pitch / TiltToPitchSign * 100.
	//   (TiltToPitchSign=±1 이므로 pitch*Sign*100 과 동일.)
	const float InstallPitchDeg = Cam->GetActorRotation().Pitch;
	Ch->CurTilt = Ch->TgtTilt = HucomsProtocol::ClampTilt(
		FMath::RoundToInt(InstallPitchDeg * TiltToPitchSign * 100.f));
	Ch->CurZoom = Ch->TgtZoom = 0;
	Ch->CurFocus = Ch->TgtFocus = 0;

	// 고정형: 이 채널은 PTZ 명령/모터 슬루를 무시하고 설치 자세(CurTilt=InstallPitch 등)로 고정.
	Ch->bFixed = Cam->bFixedMode;

	ConfigureCameraForSim(Cam);
	Channels.Add(Ch);
	return Ch;
}

bool UHucomsServerSubsystem::StartChannelServers(TSharedPtr<FHucomsChannel> ChPtr)
{
	FHucomsChannel& Ch = *ChPtr;
	FHttpServerModule& Http = FHttpServerModule::Get();

	// bFailOnBindFailure=true: 포트를 즉시 바인드 시도, 실패(중복/점유) 시 nullptr -> 명확한 진단.
	Ch.Router = Http.GetHttpRouter(Ch.HttpPort, /*bFailOnBindFailure=*/true);
	if (!Ch.Router.IsValid())
	{
		UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] 라우터 획득/바인드 실패 (port %d). 포트 중복/점유 확인 (카메라 %s)."),
			Ch.HttpPort, *GetNameSafe(Ch.Camera.Get()));
		return false;
	}

	// 라우트 바인딩: 핸들러 람다가 채널(TSharedPtr)을 캡처해 그 채널의 상태에 작용.
	auto BindGet = [this, ChPtr](const TCHAR* Path,
		bool (UHucomsServerSubsystem::*Fn)(FHucomsChannel&, const FHttpServerRequest&, const FHttpResultCallback&))
	{
		FHttpRouteHandle H = ChPtr->Router->BindRoute(
			FHttpPath(FString(Path)),
			EHttpServerRequestVerbs::VERB_GET,
			FHttpRequestHandler::CreateLambda(
				[this, ChPtr, Fn](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
				{
					return (this->*Fn)(*ChPtr, Req, OnComplete);
				}));
		if (H.IsValid())
		{
			ChPtr->RouteHandles.Add(H);
		}
		else
		{
			UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] 라우트 바인딩 실패: %s (port %d)"), Path, ChPtr->HttpPort);
		}
	};

	BindGet(TEXT("/cgi-bin/control/ptzf_status.cgi"),   &UHucomsServerSubsystem::HandlePtzfStatus);
	BindGet(TEXT("/cgi-bin/control/ptz_centering.cgi"), &UHucomsServerSubsystem::HandlePtzCentering);
	BindGet(TEXT("/cgi-bin/control/capabilityptz.cgi"), &UHucomsServerSubsystem::HandleCapabilityPtz);
	BindGet(TEXT("/cgi-bin/control/pt_control.cgi"),    &UHucomsServerSubsystem::HandlePtControl);
	BindGet(TEXT("/cgi-bin/control/zf_control.cgi"),    &UHucomsServerSubsystem::HandleZfControl);
	BindGet(TEXT("/cgi-bin/image/jpeg.cgi"),            &UHucomsServerSubsystem::HandleJpeg);
	BindGet(TEXT("/cgi-bin/image/mjpeg.cgi"),           &UHucomsServerSubsystem::HandleMjpeg);
	BindGet(TEXT("/api/tuning"),                        &UHucomsServerSubsystem::HandleTuning);

	// 연속 MJPEG 스트림 서버(채널별 포트).
	if (bEnableMjpegStream)
	{
		Ch.Stream = new FMjpegStreamServer();
		if (!Ch.Stream->StartServer(Ch.MjpegPort, StreamFps))
		{
			delete Ch.Stream;
			Ch.Stream = nullptr;
			UE_LOG(LogHucomsSim, Error, TEXT("[Hucoms] MJPEG 스트림 서버 시작 실패 (port %d)"), Ch.MjpegPort);
		}
	}

	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 채널 기동: 카메라 '%s'  HTTP :%d  MJPEG :%d"),
		*GetNameSafe(Ch.Camera.Get()), Ch.HttpPort, (Ch.Stream ? Ch.MjpegPort : -1));
	return true;
}

void UHucomsServerSubsystem::StopChannel(FHucomsChannel& Ch)
{
	if (Ch.Router.IsValid())
	{
		for (const FHttpRouteHandle& H : Ch.RouteHandles)
		{
			if (H.IsValid())
			{
				Ch.Router->UnbindRoute(H);
			}
		}
	}
	Ch.RouteHandles.Reset();
	Ch.Router.Reset();

	if (Ch.Stream)
	{
		Ch.Stream->StopServer();
		delete Ch.Stream;
		Ch.Stream = nullptr;
	}
}

TSharedPtr<FHucomsChannel> UHucomsServerSubsystem::FindChannelFor(const APTZCamera* Cam) const
{
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		if (ChPtr.IsValid() && ChPtr->Camera.Get() == Cam)
		{
			return ChPtr;
		}
	}
	return nullptr;
}

bool UHucomsServerSubsystem::IsSpawnedCamera(const APTZCamera* Cam) const
{
	return Cam && SpawnedCameras.Contains(Cam);
}

APTZCamera* UHucomsServerSubsystem::SpawnCameraRuntime(const FPTZCameraSpawnSpec& Spec, FString& OutError)
{
	UWorld* World = GetWorld();
	if (!World) { OutError = TEXT("월드 없음"); return nullptr; }
	if (Spec.HttpPort <= 0 || Spec.MjpegPort <= 0)
	{
		OutError = TEXT("httpPort/mjpegPort 명시 필수 (자동 부여는 액터 열거순이라 비결정적 — 허용하지 않음)");
		return nullptr;
	}
	if (Spec.HttpPort == Spec.MjpegPort)
	{
		OutError = TEXT("httpPort 와 mjpegPort 가 같음");
		return nullptr;
	}
	// 기존 채널과의 포트 충돌은 bind 실패보다 앞서 명확히 거른다(어느 카메라와 겹치는지 말해 준다).
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		if (!ChPtr.IsValid()) { continue; }
		const int32 Ports[2] = { Spec.HttpPort, Spec.MjpegPort };
		for (int32 P : Ports)
		{
			if (ChPtr->HttpPort == P || ChPtr->MjpegPort == P)
			{
				OutError = FString::Printf(TEXT("포트 %d 는 카메라 '%s' 가 사용 중"), P, *GetNameSafe(ChPtr->Camera.Get()));
				return nullptr;
			}
		}
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FRotator Rot(Spec.PitchDeg, Spec.YawDeg, 0.f); // Pitch 는 CreateChannelForCamera 가 tilt 로 이관
	APTZCamera* Cam = World->SpawnActor<APTZCamera>(APTZCamera::StaticClass(), Spec.Location, Rot, Params);
	if (!Cam) { OutError = TEXT("카메라 액터 스폰 실패"); return nullptr; }

	Cam->bServeHucoms   = true;
	Cam->bFixedMode     = Spec.bFixedMode;
	Cam->HucomsHttpPort  = Spec.HttpPort;
	Cam->HucomsMjpegPort = Spec.MjpegPort;
	SpawnedCameras.Add(Cam);
#if WITH_EDITOR
	if (!Spec.Note.IsEmpty()) { Cam->SetActorLabel(FString::Printf(TEXT("PTZ_Api_%s"), *Spec.Note)); }
#endif

	// 부팅 시 카메라 0대여서 서버 미기동이었던 경우의 지연 초기화(비동기 인코딩 전제 조건 포함).
	if (bAsyncStreamCapture && !CompletedFrames)
	{
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		CompletedFrames = MakeShared<TQueue<FCompletedStreamFrame, EQueueMode::Mpsc>, ESPMode::ThreadSafe>();
	}

	TSharedPtr<FHucomsChannel> Ch = CreateChannelForCamera(Cam, Spec.HttpPort, Spec.MjpegPort);
	if (!StartChannelServers(Ch))
	{
		Channels.Remove(Ch);
		SpawnedCameras.Remove(Cam);
		Cam->Destroy();
		OutError = FString::Printf(TEXT("포트 바인드 실패 (http=%d — OS 레벨 점유/중복 확인)"), Spec.HttpPort);
		return nullptr;
	}
	FHttpServerModule::Get().StartAllListeners();   // 이미 시작된 리스너는 no-op, 새 포트만 개시
	bServersStarted = true;

	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 런타임 카메라 스폰(API): '%s' @ %s yaw=%.1f pitch=%.1f http=%d mjpeg=%d"),
		*Cam->GetName(), *Spec.Location.ToString(), Spec.YawDeg, Spec.PitchDeg, Spec.HttpPort, Spec.MjpegPort);
	return Cam;
}

bool UHucomsServerSubsystem::UpdateCameraPose(APTZCamera* Cam, const FVector* Location, const float* YawDeg, const float* PitchDeg, FString& OutError)
{
	if (!IsSpawnedCamera(Cam))
	{
		OutError = TEXT("레벨 저작 카메라는 API 로 이동할 수 없음 (스폰 카메라만 — 레벨 저작은 에디터에서)");
		return false;
	}
	TSharedPtr<FHucomsChannel> Ch = FindChannelFor(Cam);

	if (Location) { Cam->SetActorLocation(*Location); }
	if (YawDeg)
	{
		FRotator R = Cam->GetActorRotation();
		R.Yaw = *YawDeg;
		R.Roll = 0.f;
		Cam->SetActorRotation(R);
	}
	if (PitchDeg)
	{
		// 설치 피치는 채널 tilt 로 산다(BuildChannels 이관 규약). 액터 회전에도 기록해
		// GetSpawnedCameraSpecs(스냅샷)가 액터만 봐도 왕복이 성립하게 한다.
		FRotator R = Cam->GetActorRotation();
		R.Pitch = *PitchDeg;
		Cam->SetActorRotation(R);
		if (Ch.IsValid())
		{
			Ch->CurTilt = Ch->TgtTilt = HucomsProtocol::ClampTilt(
				FMath::RoundToInt(*PitchDeg * TiltToPitchSign * 100.f));
		}
	}
	if (Ch.IsValid()) { MirrorChannel(*Ch); }   // 다음 캡처가 즉시 새 자세로 렌더되게
	return true;
}

bool UHucomsServerSubsystem::RemoveCameraRuntime(APTZCamera* Cam, FString& OutError)
{
	if (!IsSpawnedCamera(Cam))
	{
		OutError = TEXT("레벨 저작 카메라는 API 로 삭제할 수 없음 (스폰 카메라만)");
		return false;
	}

	TSharedPtr<FHucomsChannel> Ch = FindChannelFor(Cam);
	if (Ch.IsValid())
	{
		// 비동기 리드백 종료 규율(StopServers 와 동일): in-flight 렌더커맨드가 채널 소유
		// readback 을 물고 있으면 flush 후에만 파괴가 안전하다.
		if (bAsyncStreamCapture && Ch->StreamCapState != EStreamCapState::Idle)
		{
			FlushRenderingCommands();
		}
		ReleaseChannelCapture(*Ch, TEXT("카메라 삭제(API)"));
		StopChannel(*Ch);
		Channels.Remove(Ch);
	}
	SpawnedCameras.Remove(Cam);
	const FString Name = Cam->GetName();
	Cam->Destroy();
	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 런타임 카메라 삭제(API): '%s'"), *Name);
	return true;
}

void UHucomsServerSubsystem::GetSpawnedCameraSpecs(TArray<FPTZCameraSpawnSpec>& OutSpecs, TArray<APTZCamera*>* OutCams) const
{
	OutSpecs.Reset();
	if (OutCams) { OutCams->Reset(); }
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		if (!ChPtr.IsValid()) { continue; }
		APTZCamera* Cam = ChPtr->Camera.Get();
		if (!Cam || !SpawnedCameras.Contains(Cam)) { continue; }

		FPTZCameraSpawnSpec Spec;
		Spec.Location  = Cam->GetActorLocation();
		Spec.YawDeg    = Cam->GetActorRotation().Yaw;
		// 설치 피치는 채널 tilt 가 진실(이관 규약의 역변환, Sign=±1 이라 tilt*Sign/100).
		Spec.PitchDeg  = ChPtr->CurTilt * TiltToPitchSign / 100.f;
		Spec.HttpPort  = ChPtr->HttpPort;
		Spec.MjpegPort = ChPtr->MjpegPort;
		Spec.bFixedMode = ChPtr->bFixed;
		OutSpecs.Add(Spec);
		if (OutCams) { OutCams->Add(Cam); }
	}
}

//======================================================================================
// Tick - 채널별 모터 슬루 + 카메라 미러 + 스트림
//======================================================================================
void UHucomsServerSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bServersStarted)
	{
		return;
	}

	const int32 PanStep  = FMath::Max(1, FMath::RoundToInt(PanSlewCdPerSec  * DeltaTime));
	const int32 TiltStep = FMath::Max(1, FMath::RoundToInt(TiltSlewCdPerSec * DeltaTime));
	const int32 ZoomStep = FMath::Max(1, FMath::RoundToInt(ZoomSlewPerSec   * DeltaTime));
	const float Interval = 1.f / FMath::Max(1, StreamFps);

	// 렌더 자원 생명주기 판정용 현재 시각(월드 시간, 초).
	const UWorld* TickWorld = GetWorld();
	const double Now = TickWorld ? TickWorld->GetTimeSeconds() : 0.0;

	// 워커가 인코딩을 끝낸 스트림 프레임을 MJPEG 로 송신(게임 스레드).
	if (bAsyncStreamCapture)
	{
		DrainCompletedFrames();
	}

	for (TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		FHucomsChannel& Ch = *ChPtr;

		if (Ch.bFixed)
		{
			// 고정형: 모터 슬루 없이 설치 자세로 고정(Cur=Tgt). 명령이 무시되므로 Tgt 는 초기 설치값 그대로.
			Ch.CurPan   = Ch.TgtPan;
			Ch.CurTilt  = Ch.TgtTilt;
			Ch.CurZoom  = Ch.TgtZoom;
			Ch.CurFocus = Ch.TgtFocus;
		}
		else
		{
			// 연속(velocity) 이동: setptmove/setzfmove 로 설정된 속도가 있으면 Cur 를 직접 적분하고
			// Tgt=Cur 로 동기해 아래 goto 슬루와 충돌하지 않게 한다. 한계 클램프 시 해당 축 자동 정지.
			if (Ch.PanVel != 0.f)
			{
				Ch.CurPan = HucomsProtocol::WrapPan(Ch.CurPan + FMath::RoundToInt(Ch.PanVel * DeltaTime));
				Ch.TgtPan = Ch.CurPan;
			}
			if (Ch.TiltVel != 0.f)
			{
				const int32 Raw = Ch.CurTilt + FMath::RoundToInt(Ch.TiltVel * DeltaTime);
				const int32 New = HucomsProtocol::ClampTilt(Raw);
				if (New != Raw) { Ch.TiltVel = 0.f; }   // 한계에서 클램프됨 -> 자동 정지
				Ch.CurTilt = Ch.TgtTilt = New;
			}
			if (Ch.ZoomVel != 0.f)
			{
				const int32 Raw = Ch.CurZoom + FMath::RoundToInt(Ch.ZoomVel * DeltaTime);
				const int32 New = HucomsProtocol::ClampZoom(Raw);
				if (New != Raw) { Ch.ZoomVel = 0.f; }
				Ch.CurZoom = Ch.TgtZoom = New;
			}

			// Pan: 0/35999 이음매를 넘어 최단 호로 이동.
			{
				const int32 D = HucomsProtocol::ShortestPanDiff(Ch.CurPan, Ch.TgtPan);
				if (FMath::Abs(D) <= PanStep)
				{
					Ch.CurPan = HucomsProtocol::WrapPan(Ch.TgtPan);
				}
				else
				{
					Ch.CurPan = HucomsProtocol::WrapPan(Ch.CurPan + ((D > 0) ? PanStep : -PanStep));
				}
			}

			Ch.CurTilt  = StepLinear(Ch.CurTilt,  Ch.TgtTilt,  TiltStep);
			Ch.CurZoom  = StepLinear(Ch.CurZoom,  Ch.TgtZoom,  ZoomStep);
			Ch.CurFocus = Ch.TgtFocus; // 포커스는 즉시
		}

		MirrorChannel(Ch);

		// --- 이 카메라를 켜 둘 것인가 ---
		// 슬루/미러(위)는 무조건 돌린다 — 정준 PTZ 상태는 항상 정확해야 getptzfpos 가 맞다.
		// 무거운 것(렌더 자원·텍스처 시점 등록)만 실제 수요를 따른다.
		const bool bHasClients = Ch.Stream && Ch.Stream->HasClients();
		bool bWarm = IsChannelWarm(Ch);

		// 비동기: 완료된 in-flight 리드백은 클라이언트 유무와 무관하게 회수한다 — 클라이언트가
		// 방금 끊긴 채널도 마지막 프레임을 끝내고 Idle 로 돌아와야 유휴 해제가 안전하다.
		if (bAsyncStreamCapture && Ch.StreamCapState == EStreamCapState::InFlight
			&& Ch.StreamReadback.IsValid() && Ch.StreamReadback->IsReady())
		{
			CollectStreamReadback(Ch);
		}

		// 유휴 해제: 아무도 안 보고, 마지막 사용에서 IdleReleaseSeconds 가 지났으면 끈다.
		// 단 in-flight 리드백이 있으면(비동기) 완료까지 미룬다 — RT 파괴는 렌더 FIFO 로 안전하나
		// 상태를 깔끔히 두기 위해.
		if (bWarm && !bHasClients && IdleReleaseSeconds > 0.f
			&& Ch.StreamCapState == EStreamCapState::Idle
			&& (Now - Ch.LastDemandTime) > static_cast<double>(IdleReleaseSeconds))
		{
			ReleaseChannelCapture(Ch, *FString::Printf(TEXT("유휴 %.0f초"), Now - Ch.LastDemandTime));
			bWarm = false;
		}

		// CCTV 시점을 텍스처 스트리머에 등록 — 스트리머는 플레이어 뷰만 시점으로 쓰고
		// SceneCapture 뷰는 등록하지 않으므로(UE5.8 GameViewportClient.cpp:1913 확인),
		// 줌으로 당긴 원거리 텍스처가 저해상도 mip 으로 뭉개진다. 카메라 위치 + 현재 줌
		// FOV(FOVScreenSize = 폭/tan(HFOV/2))를 등록해 mip 이 CCTV 기준으로 올라온다.
		// 단 '켜져 있는' 카메라만 — 0.1.8 은 6대를 무조건 매 틱 등록해서, 아무도 안 보는
		// 카메라의 원거리 텍스처까지 고해상도 mip 으로 상주시켜 스트리밍 풀을 넘겼다.
		if ((bWarm || bHasClients) && Ch.Camera.IsValid())
		{
			const APTZCamera* Cam = Ch.Camera.Get();
			if (Cam->CameraComp)
			{
				const float HalfHFovRad = FMath::DegreesToRadians(FMath::Max(1.f, Cam->CameraComp->FieldOfView)) * 0.5f;
				const float ScreenSize = static_cast<float>(FMath::Max(StreamWidth, SnapshotWidth));
				IStreamingManager::Get().AddViewInformation(
					Cam->CameraComp->GetComponentLocation(), ScreenSize,
					ScreenSize / FMath::Max(FMath::Tan(HalfHFovRad), 0.01f));
			}
		}

		// 연속 MJPEG: 클라이언트가 있을 때만 StreamFps 로 캡처(없으면 렌더 비용 0).
		if (bHasClients)
		{
			Ch.StreamAccum += DeltaTime;
			Ch.FpsWindowAccum += DeltaTime;
			if (Ch.StreamAccum >= Interval)
			{
				// 잔여 시간을 보존해야 실효 fps 가 StreamFps 에 수렴한다(0 리셋은 게임 틱 경계로
				// 양자화되어 항상 목표 미달). 게임 fps < StreamFps 인 구간에서 부채가 무한 누적되어
				// 히치 후 따라잡기 폭주하지 않도록 한 프레임치로 클램프.
				Ch.StreamAccum = FMath::Min(Ch.StreamAccum - Interval, Interval);
				if (UPTZCaptureComponent* Cap = ResolveCapture(Ch.Camera.Get()))
				{
					// 꺼져 있다 다시 켜지는 첫 프레임만 워밍업(자원 생성 전에 판정).
					const bool bCold = !Cap->HasCaptureResources();
					// 이 카메라를 쓴다 → 수요 기록 + 다른 카메라 끄기(VRAM 을 먼저 비우고 할당).
					StampDemand(Ch);

					if (bAsyncStreamCapture)
					{
						// 비동기: Idle 일 때만 제출(이전 프레임이 in-flight 면 이 슬롯은 건너뜀).
						// 완성 프레임은 DrainCompletedFrames 가 UpdateFrame + FpsWindowFrames 처리.
						if (Ch.StreamCapState == EStreamCapState::Idle)
						{
							SubmitStreamCapture(Ch, bCold);
						}
					}
					else
					{
						// 동기(레거시): 게임 스레드가 GPU 완료까지 블로킹. 롤백/비교용.
						const int32 Warmup = bCold ? RecreateWarmupFrames : 0;
						TArray64<uint8> Jpeg;
						if (Cap->CaptureJpeg(StreamWidth, StreamHeight, StreamJpegQuality, Jpeg, Warmup, CaptureExposureBias, CaptureContrast) && Jpeg.Num() > 0)
						{
							TArray<uint8> Frame;
							Frame.Append(Jpeg.GetData(), IntCastChecked<int32>(Jpeg.Num()));
							Ch.Stream->UpdateFrame(Frame);
							++Ch.FpsWindowFrames;
						}
					}
				}
			}
			// 1초 창으로 실측 송신 fps 갱신 (HUD 표시용)
			if (Ch.FpsWindowAccum >= 1.f)
			{
				Ch.MeasuredStreamFps = Ch.FpsWindowFrames / Ch.FpsWindowAccum;
				Ch.FpsWindowAccum = 0.f;
				Ch.FpsWindowFrames = 0;
			}
		}
		else
		{
			Ch.MeasuredStreamFps = 0.f;
			Ch.FpsWindowAccum = 0.f;
			Ch.FpsWindowFrames = 0;
		}
	}
}

TStatId UHucomsServerSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UHucomsServerSubsystem, STATGROUP_Tickables);
}

TArray<FString> UHucomsServerSubsystem::GetChannelStatusLines() const
{
	TArray<FString> Lines;
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		const FHucomsChannel& Ch = *ChPtr;
		const FString Name = Ch.Camera.IsValid() ? Ch.Camera->GetName() : TEXT("(카메라 없음)");
		const int32 Clients = Ch.Stream ? Ch.Stream->GetClientCount() : 0;
		if (Clients > 0)
		{
			Lines.Add(FString::Printf(TEXT("%s  http:%d  mjpeg:%d  ▶ %.1f fps  (클라이언트 %d)"),
				*Name, Ch.HttpPort, Ch.MjpegPort, Ch.MeasuredStreamFps, Clients));
		}
		else if (IsChannelWarm(Ch))
		{
			Lines.Add(FString::Printf(TEXT("%s  http:%d  mjpeg:%d  — 켜짐 (클라이언트 없음, 곧 해제)"),
				*Name, Ch.HttpPort, Ch.MjpegPort));
		}
		else
		{
			Lines.Add(FString::Printf(TEXT("%s  http:%d  mjpeg:%d  — 꺼짐 (렌더 자원 0)"),
				*Name, Ch.HttpPort, Ch.MjpegPort));
		}
	}
	return Lines;
}

bool UHucomsServerSubsystem::GetCameraPorts(const APTZCamera* Cam, int32& OutHttpPort, int32& OutMjpegPort) const
{
	if (!Cam) { return false; }
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		if (ChPtr.IsValid() && ChPtr->Camera.Get() == Cam)
		{
			OutHttpPort = ChPtr->HttpPort;
			OutMjpegPort = ChPtr->MjpegPort;
			return true;
		}
	}
	return false;
}

//======================================================================================
// Camera configure / mirror / capture
//======================================================================================
void UHucomsServerSubsystem::ConfigureCameraForSim(APTZCamera* Cam)
{
	if (!Cam)
	{
		return;
	}
	// sim 의 설정된 wide HFOV(현재 기본 57.14)에 1x 를 맞추고, Hucoms 범위를 시각화할 수 있게 한계를 넓힌다.
	Cam->BaseFOV       = WideHFovDeg;
	Cam->PanMin        = FMath::Min(Cam->PanMin, -180.f);
	Cam->PanMax        = FMath::Max(Cam->PanMax,  180.f);
	Cam->TiltMin       = FMath::Min(Cam->TiltMin, HucomsProtocol::TiltPosMin / 100.f); // -20deg
	Cam->TiltMax       = FMath::Max(Cam->TiltMax, HucomsProtocol::TiltPosMax / 100.f); // +90deg
	Cam->MaxZoomFactor = FMath::Max(Cam->MaxZoomFactor, 60.f);

	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 카메라 sim 설정: %s (BaseFOV=%.2f)"), *Cam->GetName(), WideHFovDeg);
}

void UHucomsServerSubsystem::MirrorChannel(FHucomsChannel& Ch)
{
	APTZCamera* Cam = Ch.Camera.Get();
	if (!Cam)
	{
		return;
	}

	const float Yaw   = FMath::UnwindDegrees(PanToYawSign * (Ch.CurPan / 100.f));
	const float Pitch = TiltToPitchSign * (Ch.CurTilt / 100.f);
	const float HFov  = HucomsProtocol::ZoomPosToHFov(Ch.CurZoom, WideHFovDeg);
	const float Zf    = HucomsProtocol::HFovToZoomFactor(HFov, WideHFovDeg);

	// 정준 current 를 카메라에 즉시 반영(채널이 모터, 카메라는 미러).
	Cam->SetPanTilt(Yaw, Pitch);
	Cam->SetZoomFactor(Zf);
	Cam->SnapToTarget();
}

UPTZCaptureComponent* UHucomsServerSubsystem::ResolveCapture(APTZCamera* Cam)
{
	if (!Cam)
	{
		return nullptr;
	}
	UPTZCaptureComponent* Cap = Cam->FindComponentByClass<UPTZCaptureComponent>();
	if (!Cap)
	{
		Cap = NewObject<UPTZCaptureComponent>(Cam);
		Cap->RegisterComponent();
		UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 캡처 컴포넌트 생성: %s 에 부착"), *Cam->GetName());
	}
	return Cap;
}

//======================================================================================
// 렌더 자원 생명주기 — "쓰는 카메라만 켠다"
//======================================================================================
UPTZCaptureComponent* UHucomsServerSubsystem::FindCapture(const FHucomsChannel& Ch) const
{
	APTZCamera* Cam = Ch.Camera.Get();
	return Cam ? Cam->FindComponentByClass<UPTZCaptureComponent>() : nullptr;
}

bool UHucomsServerSubsystem::IsChannelWarm(const FHucomsChannel& Ch) const
{
	const UPTZCaptureComponent* Cap = FindCapture(Ch);
	return Cap && Cap->HasCaptureResources();
}

void UHucomsServerSubsystem::ReleaseChannelCapture(FHucomsChannel& Ch, const TCHAR* Reason)
{
	UPTZCaptureComponent* Cap = FindCapture(Ch);
	if (!Cap || !Cap->HasCaptureResources())
	{
		return; // 이미 꺼져 있음.
	}
	Cap->ReleaseCaptureResources();
	UE_LOG(LogHucomsSim, Log, TEXT("[Hucoms] 카메라 끔: %s (http:%d) — %s"),
		*GetNameSafe(Ch.Camera.Get()), Ch.HttpPort, Reason);
}

void UHucomsServerSubsystem::StampDemand(FHucomsChannel& Ch)
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	Ch.LastDemandTime = Now;

	if (MaxActiveCameras <= 0)
	{
		return; // 상한 없음(레거시 0.1.8 동작).
	}

	// 켜져 있는 '다른' 카메라를 모은다. 두 종류는 축출하지 않는다:
	//  - MJPEG 클라이언트가 붙은 채널: 끄면 스트림이 끊긴다.
	//  - 방금(MinWarmSeconds 이내) 쓴 채널: 여러 카메라를 번갈아 쓰는 소비자에서 껐다 켜는
	//    churn 이 나 오히려 더 무겁다(실측: 검출기가 6대 순회 폴링 시 1분에 173회 재생성).
	TArray<FHucomsChannel*> Warm;
	for (const TSharedPtr<FHucomsChannel>& OtherPtr : Channels)
	{
		FHucomsChannel* Other = OtherPtr.Get();
		if (!Other || Other == &Ch)
		{
			continue;
		}
		if (Other->Stream && Other->Stream->HasClients())
		{
			continue; // 시청 중 — 축출 금지.
		}
		if ((Now - Other->LastDemandTime) <= static_cast<double>(MinWarmSeconds))
		{
			continue; // 방금 썼다 — 유예.
		}
		if (Other->StreamCapState != EStreamCapState::Idle)
		{
			continue; // 비동기 리드백 in-flight — 완료(Idle) 후 유휴 해제가 처리.
		}
		if (IsChannelWarm(*Other))
		{
			Warm.Add(Other);
		}
	}

	// 지금 쓰는 채널이 한 자리를 차지하므로 다른 채널에 남길 수 있는 자리는 상한-1.
	const int32 KeepOthers = FMath::Max(0, MaxActiveCameras - 1);
	if (Warm.Num() <= KeepOthers)
	{
		return;
	}
	// 최근에 쓴 것부터 보존하고 나머지를 끈다(기본 상한 1 이면 전부 꺼진다).
	Warm.Sort([](const FHucomsChannel& A, const FHucomsChannel& B)
	{
		return A.LastDemandTime > B.LastDemandTime;
	});
	for (int32 i = KeepOthers; i < Warm.Num(); ++i)
	{
		ReleaseChannelCapture(*Warm[i], TEXT("다른 카메라 사용"));
	}
}

//======================================================================================
// 비동기 스트림 캡처 파이프라인 (게임 스레드 무정지 GPU 리드백 + 워커 인코딩)
//======================================================================================
void UHucomsServerSubsystem::DrainCompletedFrames()
{
	if (!CompletedFrames.IsValid())
	{
		return;
	}
	FCompletedStreamFrame Frame;
	while (CompletedFrames->Dequeue(Frame))
	{
		// 포트로 채널 조인(UObject/포인터 참조 회피 — 워커는 포트·시퀀스만 안다).
		for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
		{
			FHucomsChannel* Ch = ChPtr.Get();
			if (!Ch || Ch->HttpPort != Frame.HttpPort)
			{
				continue;
			}
			// 역전 프레임 드랍(단일 in-flight 라 사실상 항상 증가 — belt-and-suspenders).
			if (Frame.Seq > Ch->LastDeliveredSeq && Ch->Stream)
			{
				Ch->Stream->UpdateFrame(Frame.Jpeg);
				Ch->LastDeliveredSeq = Frame.Seq;
				++Ch->FpsWindowFrames;
			}
			break;
		}
	}
}

void UHucomsServerSubsystem::SubmitStreamCapture(FHucomsChannel& Ch, bool bCold)
{
	UPTZCaptureComponent* Cap = ResolveCapture(Ch.Camera.Get());
	if (!Cap)
	{
		return;
	}
	// 스냅샷과 동일한 노출/대비 보정을 스트림에도 적용(튜닝 정합) — 같은 렌더 = 같은 화질.
	UTextureRenderTarget2D* RT = Cap->PrepareCapture(StreamWidth, StreamHeight, CaptureExposureBias, CaptureContrast);
	if (!RT)
	{
		return;
	}
	// 꺼져 있다 켜지는 첫 프레임만 워밍업(Lumen/노출 히스토리가 프레임 0 이라 뜨는 것 보정).
	if (bCold)
	{
		for (int32 i = 0; i < RecreateWarmupFrames; ++i)
		{
			Cap->RenderOnce();
		}
	}
	Cap->RenderOnce(); // 이 프레임 렌더 큐잉

	// 리드백 스테이징 확보 — 스트림은 크기 고정이라 최초 1회만 생성.
	if (!Ch.StreamReadback.IsValid() || Ch.ReadbackW != StreamWidth || Ch.ReadbackH != StreamHeight)
	{
		if (Ch.StreamReadback.IsValid())
		{
			FlushRenderingCommands(); // 크기 변경(희귀) — 옛 readback 참조 커맨드 소진 후 교체.
		}
		Ch.StreamReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("HucomsStreamReadback"));
		Ch.ReadbackW = StreamWidth;
		Ch.ReadbackH = StreamHeight;
	}

	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return;
	}
	FRHIGPUTextureReadback* Readback = Ch.StreamReadback.Get();
	const int32 W = StreamWidth, H = StreamHeight;

	ENQUEUE_RENDER_COMMAND(HucomsEnqueueStreamCopy)(
		[Readback, RTResource, W, H](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* Tex = RTResource->TextureRHI;
			if (!Tex)
			{
				return;
			}
			// SceneCapture 후 RT 는 SRVMask — 복사원 상태로 전이 후 복사, 다시 SRV 로 복귀.
			RHICmdList.Transition(FRHITransitionInfo(Tex, ERHIAccess::SRVMask, ERHIAccess::CopySrc));
			Readback->EnqueueCopy(RHICmdList, Tex, FIntVector(0, 0, 0), 0, FIntVector(W, H, 1));
			RHICmdList.Transition(FRHITransitionInfo(Tex, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
		});

	Ch.StreamCapState = EStreamCapState::InFlight;
	++Ch.StreamCapSeq;
}

void UHucomsServerSubsystem::CollectStreamReadback(FHucomsChannel& Ch)
{
	FRHIGPUTextureReadback* Readback = Ch.StreamReadback.Get();
	if (!Readback)
	{
		Ch.StreamCapState = EStreamCapState::Idle;
		return;
	}
	const int32 W = Ch.ReadbackW, H = Ch.ReadbackH;
	const int32 Quality = StreamJpegQuality;   // 제출 시점 공유 멤버 스냅샷.
	const int32 Port = Ch.HttpPort;
	const uint64 Seq = Ch.StreamCapSeq;
	TSharedPtr<TQueue<FCompletedStreamFrame, EQueueMode::Mpsc>, ESPMode::ThreadSafe> Queue = CompletedFrames;

	ENQUEUE_RENDER_COMMAND(HucomsCollectStreamReadback)(
		[Readback, W, H, Quality, Port, Seq, Queue](FRHICommandListImmediate& RHICmdList)
		{
			int32 RowPitchPixels = 0;
			void* Mapped = Readback->Lock(RowPitchPixels, nullptr);
			if (!Mapped)
			{
				return;
			}
			// de-pitch: 스테이징 row stride(픽셀) 가 W 보다 클 수 있어 행 단위 복사(소유 버퍼로).
			TArray64<uint8> Pixels;
			Pixels.SetNumUninitialized(static_cast<int64>(W) * H * 4);
			const uint8* Src = static_cast<const uint8*>(Mapped);
			uint8* Dst = Pixels.GetData();
			const int32 RowBytes = W * 4;
			const int64 SrcPitchBytes = static_cast<int64>(RowPitchPixels) * 4;
			for (int32 y = 0; y < H; ++y)
			{
				FMemory::Memcpy(Dst + static_cast<int64>(y) * RowBytes, Src + static_cast<int64>(y) * SrcPitchBytes, RowBytes);
			}
			Readback->Unlock();

			// JPEG 인코딩은 워커 스레드로(게임 스레드 무부하). 값복사만 참조 — UObject/채널 무관.
			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
				[Pixels = MoveTemp(Pixels), W, H, Quality, Port, Seq, Queue]() mutable
				{
					FImage Image;
					// BGRA8 + sRGB: 기존 GetRenderTargetImage 경로와 바이트 동일(감마는 태깅뿐).
					Image.Init(W, H, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
					FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), static_cast<int64>(W) * H * 4);
					TArray64<uint8> Jpeg;
					if (FImageUtils::CompressImage(Jpeg, TEXT("jpg"), Image, Quality) && Jpeg.Num() > 0 && Queue.IsValid())
					{
						FCompletedStreamFrame Done;
						Done.HttpPort = Port;
						Done.Seq = Seq;
						Done.Jpeg.Append(Jpeg.GetData(), IntCastChecked<int32>(Jpeg.Num()));
						Queue->Enqueue(MoveTemp(Done));
					}
				});
		});

	// 다음 제출 허용. 렌더 FIFO 상 이 collect(Lock/Unlock) 가 다음 EnqueueCopy 보다 먼저 실행되므로
	// readback 재사용에 프레임 유실이 없다.
	Ch.StreamCapState = EStreamCapState::Idle;
}

bool UHucomsServerSubsystem::RenderSnapshotJpeg(FHucomsChannel& Ch, TArray<uint8>& OutBytes)
{
	APTZCamera* Cam = Ch.Camera.Get();
	UPTZCaptureComponent* Cap = ResolveCapture(Cam);
	if (!Cap)
	{
		return false;
	}

	// 꺼져 있던 카메라를 다시 켜는 첫 캡처면 워밍업을 더 준다(Lumen/노출 히스토리가 프레임 0 이라
	// 뜨는 것을 보정). 정상 상태의 SnapshotWarmupFrames=0(실측 최적)은 그대로 둔다.
	// CaptureJpeg 가 자원을 만들어 버리므로 반드시 호출 '전'에 판정한다.
	const int32 Warmup = Cap->HasCaptureResources()
		? SnapshotWarmupFrames
		: FMath::Max(SnapshotWarmupFrames, RecreateWarmupFrames);

	// 이 카메라를 쓴다 → 수요 기록 + 다른 카메라 끄기(VRAM 을 먼저 비우고 할당).
	StampDemand(Ch);

	// 캡처 직전, 정준 상태를 카메라 FOV 에 한 번 더 반영(요청-틱 사이 정합 보장).
	MirrorChannel(Ch);

	TArray64<uint8> Jpeg;
	// 선명도=TAA-on+Lumen(단발), 톤=노출/대비 보정으로 뷰포트에 정합.
	if (!Cap->CaptureJpeg(SnapshotWidth, SnapshotHeight, JpegQuality, Jpeg, Warmup, CaptureExposureBias, CaptureContrast) || Jpeg.Num() == 0)
	{
		return false;
	}

	OutBytes.Reset();
	OutBytes.Append(Jpeg.GetData(), IntCastChecked<int32>(Jpeg.Num()));
	return true;
}

//======================================================================================
// CGI handlers (채널별)
//======================================================================================
bool UHucomsServerSubsystem::HandlePtzfStatus(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Action = GetQ(Req, TEXT("action"));
	FString Body;

	if (Action == TEXT("getptzfpos"))
	{
		Body = BuildPtzPosBody(Ch);
	}
	else if (Action == TEXT("goptzfpos"))
	{
		ApplyGoPtz(Ch, Req);
		Body.Empty(); // 실기: 성공 시 본문 없음. 클라이언트는 'Error:' 접두만 검사.
	}
	else if (Action == TEXT("getptzstatus"))
	{
		Body = FString::Printf(TEXT("ptstatus = %s\nzfstatus = %s\n"),
			Ch.bPtEnable ? TEXT("enable") : TEXT("disable"),
			Ch.bZfEnable ? TEXT("enable") : TEXT("disable"));
	}
	else if (Action == TEXT("setptzstatus"))
	{
		if (HasQ(Req, TEXT("ptstatus"))) { Ch.bPtEnable = (GetQ(Req, TEXT("ptstatus")) == TEXT("enable")); }
		if (HasQ(Req, TEXT("zfstatus"))) { Ch.bZfEnable = (GetQ(Req, TEXT("zfstatus")) == TEXT("enable")); }
		Body = FString::Printf(TEXT("ptstatus = %s\nzfstatus = %s\n"),
			Ch.bPtEnable ? TEXT("enable") : TEXT("disable"),
			Ch.bZfEnable ? TEXT("enable") : TEXT("disable"));
	}
	else if (Action == TEXT("lensreset"))
	{
		Body.Empty();
	}
	else
	{
		Body = TEXT("Error: invalid parameter\n");
	}

	OnComplete(MakeText(Body));
	return true;
}

bool UHucomsServerSubsystem::HandlePtzCentering(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Action = GetQ(Req, TEXT("action"));
	if (Action == TEXT("setcenter"))
	{
		ApplySetCenter(Ch, Req);
		OnComplete(MakeText(FString())); // 성공: 빈 본문.
	}
	else
	{
		OnComplete(MakeText(TEXT("Error: invalid parameter\n")));
	}
	return true;
}

bool UHucomsServerSubsystem::HandleCapabilityPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// getPTZ / getCapabilitiesPTZAll 공통 - 클라이언트 파서는 '[' 헤더 줄을 건너뛴다.
	static const TCHAR* CapsPtz =
		TEXT("[Capabilities PTZ]\n")
		TEXT("PanSupported = Yes\n")
		TEXT("TiltSupported = Yes\n")
		TEXT("ZoomSupported = Yes\n")
		TEXT("FocusSupported = Yes\n")
		TEXT("EndlessPanSupported = No\n")
		TEXT("AutoFocusSupported = Yes\n")
		TEXT("PresetSupported = 128\n")
		TEXT("AutopanSupported = No\n")
		TEXT("AutopancwSupported = No\n")
		TEXT("TourSupported = No\n");
	// 고정형: 실기 고정형 CCTV 와 동일하게 PTZ 미지원으로 광고 — 클라이언트가 PTZ 조작 UI 를 숨길 수 있다.
	static const TCHAR* CapsFixed =
		TEXT("[Capabilities PTZ]\n")
		TEXT("PanSupported = No\n")
		TEXT("TiltSupported = No\n")
		TEXT("ZoomSupported = No\n")
		TEXT("FocusSupported = No\n")
		TEXT("EndlessPanSupported = No\n")
		TEXT("AutoFocusSupported = No\n")
		TEXT("PresetSupported = 0\n")
		TEXT("AutopanSupported = No\n")
		TEXT("AutopancwSupported = No\n")
		TEXT("TourSupported = No\n");
	OnComplete(MakeText(FString(Ch.bFixed ? CapsFixed : CapsPtz)));
	return true;
}

bool UHucomsServerSubsystem::HandleJpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 채널 카메라를 실제 렌더해 JPEG 반환. 실패 시 4바이트 스텁 폴백
	// (클라이언트는 content-type 이 image/jpeg 인지를 엄격히 검사하므로 폴백도 image/jpeg).
	TArray<uint8> Jpeg;
	if (RenderSnapshotJpeg(Ch, Jpeg))
	{
		OnComplete(MakeBytes(MoveTemp(Jpeg), TEXT("image/jpeg")));
	}
	else
	{
		UE_LOG(LogHucomsSim, Warning, TEXT("[Hucoms] jpeg.cgi 렌더 실패 -> 스텁 폴백 (port %d)."), Ch.HttpPort);
		OnComplete(MakeBytes(StubJpeg(), TEXT("image/jpeg")));
	}
	return true;
}

bool UHucomsServerSubsystem::HandleMjpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 단일 프레임(실제 렌더) 멀티파트. 연속 스트림은 채널의 MJPEG TCP 포트.
	TArray<uint8> Jpeg;
	if (!RenderSnapshotJpeg(Ch, Jpeg))
	{
		Jpeg = StubJpeg();
	}

	const FString Boundary = TEXT("baroworldboundary");
	TArray<uint8> Body;
	AppendUtf8(Body, FString::Printf(
		TEXT("--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"), *Boundary, Jpeg.Num()));
	Body.Append(Jpeg);
	AppendUtf8(Body, FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));

	OnComplete(MakeBytes(MoveTemp(Body),
		FString::Printf(TEXT("multipart/x-mixed-replace;boundary=%s"), *Boundary)));
	return true;
}

bool UHucomsServerSubsystem::HandleTuning(FHucomsChannel& /*Ch*/, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 전 채널 공유 값(RenderSnapshotJpeg 가 이 subsystem 멤버를 매 캡처마다 읽음) — 준 항목만 갱신,
	// 나머지는 유지. 클램프는 헤더의 UPROPERTY meta 범위와 동일(에디터 Details 클램프는 런타임엔
	// 미적용이라 여기서 직접 강제 — 외부 HTTP 입력 경계이므로 검증 필요).
	if (HasQ(Req, TEXT("exposureBias")))  { CaptureExposureBias  = FMath::Clamp(GetQFloat(Req, TEXT("exposureBias"), CaptureExposureBias), -4.f, 4.f); }
	if (HasQ(Req, TEXT("contrast")))      { CaptureContrast      = FMath::Clamp(GetQFloat(Req, TEXT("contrast"), CaptureContrast), 0.5f, 3.0f); }
	if (HasQ(Req, TEXT("jpegQuality")))   { JpegQuality           = FMath::Clamp(GetQInt(Req, TEXT("jpegQuality"), JpegQuality), 1, 100); }
	if (HasQ(Req, TEXT("warmupFrames")))  { SnapshotWarmupFrames  = FMath::Clamp(GetQInt(Req, TEXT("warmupFrames"), SnapshotWarmupFrames), 0, 32); }
	if (HasQ(Req, TEXT("width")))         { SnapshotWidth         = FMath::Clamp(GetQInt(Req, TEXT("width"), SnapshotWidth), 64, 7680); }
	if (HasQ(Req, TEXT("height")))        { SnapshotHeight        = FMath::Clamp(GetQInt(Req, TEXT("height"), SnapshotHeight), 64, 4320); }

	const FString Body = FString::Printf(
		TEXT("{\"exposureBias\":%.3f,\"contrast\":%.3f,\"jpegQuality\":%d,\"warmupFrames\":%d,\"width\":%d,\"height\":%d}"),
		CaptureExposureBias, CaptureContrast, JpegQuality, SnapshotWarmupFrames, SnapshotWidth, SnapshotHeight);
	OnComplete(MakeText(Body, TEXT("application/json")));
	return true;
}

//======================================================================================
// Command application (채널별)
//======================================================================================
void UHucomsServerSubsystem::ApplyGoPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req)
{
	// 고정형 카메라는 이동 명령(goptzfpos)을 무시한다 — 설치 자세로 고정.
	// (getptzfpos 는 고정된 Cur 를 그대로 반환하므로 baro_calory 라운드트립은 유지된다.)
	if (Ch.bFixed)
	{
		return;
	}

	// 절대 이동은 진행 중인 연속(velocity) 이동을 취소한다(실기와 동일 — goto 가 jog 를 멈춘다).
	Ch.PanVel = Ch.TiltVel = Ch.ZoomVel = 0.f;

	// 카메라를 움직인다 = 곧 이 카메라를 본다 → 미리 켜 둔다(센터링 플로우 첫 장 품질).
	StampDemand(Ch);

	// 절대 이동(go-to). 클라이언트는 panpos/tiltpos 항상, zoompos/focuspos 는 선택 전송.
	if (HasQ(Req, TEXT("panpos")))   { Ch.TgtPan   = HucomsProtocol::WrapPan(GetQInt(Req, TEXT("panpos"), Ch.CurPan)); }
	if (HasQ(Req, TEXT("tiltpos")))  { Ch.TgtTilt  = HucomsProtocol::ClampTilt(GetQInt(Req, TEXT("tiltpos"), Ch.CurTilt)); }
	if (HasQ(Req, TEXT("zoompos")))  { Ch.TgtZoom  = HucomsProtocol::ClampZoom(GetQInt(Req, TEXT("zoompos"), Ch.CurZoom)); }
	if (HasQ(Req, TEXT("focuspos"))) { Ch.TgtFocus = HucomsProtocol::ClampFocus(GetQInt(Req, TEXT("focuspos"), Ch.CurFocus)); }

	UE_LOG(LogHucomsSim, Verbose, TEXT("[Hucoms] :%d goptzfpos -> pan=%d tilt=%d zoom=%d"), Ch.HttpPort, Ch.TgtPan, Ch.TgtTilt, Ch.TgtZoom);
}

//======================================================================================
// 연속(velocity) PTZ — pt_control.cgi / zf_control.cgi (Hucoms 스펙 §8.2 / §8.3)
//   실기: 방향(right/left/up/down/in/out) + 속도(1~100)로 모터를 계속 돌리다가 방향=stop 으로 정지.
//   성공 = 빈 본문 200, 실패 = "Error:<설명>". goptzfpos(절대 이동)와 상호배타적(둘 중 하나가 다른 하나를 취소).
//======================================================================================
namespace
{
	// 방향 문자열 + 속도(1~100) -> 부호 있는 velocity(native units/sec). 정지/미지정은 nullptr 반환(무변경).
	// PosDir/NegDir 는 raw Hucoms '증가/감소' 방향의 문자열.
	bool ParseVelocity(const FString& Dir, int32 Speed, float MaxRate, const TCHAR* PosDir, const TCHAR* NegDir, float& OutVel)
	{
		if (Dir == TEXT("stop")) { OutVel = 0.f; return true; }
		const float Frac = FMath::Clamp(Speed, 1, 100) / 100.f;
		if (Dir == PosDir) { OutVel = +Frac * MaxRate; return true; }
		if (Dir == NegDir) { OutVel = -Frac * MaxRate; return true; }
		return false; // 미지정/알 수 없는 값 -> 이 축은 건드리지 않음
	}
}

bool UHucomsServerSubsystem::HandlePtControl(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Action = GetQ(Req, TEXT("action"));
	if (Action != TEXT("setptmove"))
	{
		OnComplete(MakeText(TEXT("Error: invalid parameter\n")));
		return true;
	}
	// 고정형: 명령 무시(성공 응답). 실기 고정형 CCTV 와 동일.
	if (!Ch.bFixed)
	{
		// 조작 중 = 곧 이 카메라를 본다 → 미리 켜 둔다.
		StampDemand(Ch);

		// pan: right=+(우측, panpos↑) / left=-. tilt: down=+(아래, tiltpos↑) / up=-(위).
		if (HasQ(Req, TEXT("pan")))
		{
			ParseVelocity(GetQ(Req, TEXT("pan")), GetQInt(Req, TEXT("panspeed"), 50),
				PanSlewCdPerSec, TEXT("right"), TEXT("left"), Ch.PanVel);
		}
		if (HasQ(Req, TEXT("tilt")))
		{
			ParseVelocity(GetQ(Req, TEXT("tilt")), GetQInt(Req, TEXT("tiltspeed"), 50),
				TiltSlewCdPerSec, TEXT("down"), TEXT("up"), Ch.TiltVel);
		}
		UE_LOG(LogHucomsSim, Verbose, TEXT("[Hucoms] :%d setptmove -> panVel=%.0f tiltVel=%.0f"), Ch.HttpPort, Ch.PanVel, Ch.TiltVel);
	}
	OnComplete(MakeText(FString())); // 성공: 빈 본문
	return true;
}

bool UHucomsServerSubsystem::HandleZfControl(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Action = GetQ(Req, TEXT("action"));
	if (Action == TEXT("onepush"))
	{
		// 원-푸시 AF: sim 은 포커스가 항상 즉시 수렴이라 no-op 성공.
		OnComplete(MakeText(FString()));
		return true;
	}
	if (Action != TEXT("setzfmove"))
	{
		OnComplete(MakeText(TEXT("Error: invalid parameter\n")));
		return true;
	}
	if (!Ch.bFixed)
	{
		// 조작 중 = 곧 이 카메라를 본다 → 미리 켜 둔다.
		StampDemand(Ch);

		// zoom: in=+(망원, zoompos↑) / out=-. focus 연속은 sim 에서 즉시 수렴이라 미지원(파라미터는 수용).
		if (HasQ(Req, TEXT("zoom")))
		{
			ParseVelocity(GetQ(Req, TEXT("zoom")), GetQInt(Req, TEXT("zoomspeed"), 50),
				ZoomSlewPerSec, TEXT("in"), TEXT("out"), Ch.ZoomVel);
		}
		UE_LOG(LogHucomsSim, Verbose, TEXT("[Hucoms] :%d setzfmove -> zoomVel=%.0f"), Ch.HttpPort, Ch.ZoomVel);
	}
	OnComplete(MakeText(FString()));
	return true;
}

void UHucomsServerSubsystem::ApplySetCenter(FHucomsChannel& Ch, const FHttpServerRequest& Req)
{
	// 고정형 카메라는 센터링(조준)도 무시한다 — 설치 자세로 고정.
	if (Ch.bFixed)
	{
		return;
	}

	// 센터링(절대 조준)도 진행 중인 연속 이동을 취소한다.
	Ch.PanVel = Ch.TiltVel = Ch.ZoomVel = 0.f;

	// 센터링 후 곧 스냅샷을 찍는 플로우라 미리 켜 둔다(첫 장이 콜드로 뜨는 것 방지).
	StampDemand(Ch);

	// 픽셀(1920x1080 논리 프레임) -> pan/tilt 델타. TAN 핀홀 + 구면 짐벌 모델로 실기 펌웨어 재현.
	// 기준은 '현재 위치(Cur)' - 실기는 지금 보고 있는 자세에서 센터링한다.
	const FString Type = GetQ(Req, TEXT("type"), TEXT("point"));

	float PixelX, PixelY;
	if (Type == TEXT("box"))
	{
		const float StartX = GetQFloat(Req, TEXT("center.startx"), 0.f);
		const float StartY = GetQFloat(Req, TEXT("center.starty"), 0.f);
		const float EndX   = GetQFloat(Req, TEXT("center.endx"),   HucomsProtocol::FrameW);
		const float EndY   = GetQFloat(Req, TEXT("center.endy"),   HucomsProtocol::FrameH);

		PixelX = (StartX + EndX) * 0.5f;
		PixelY = (StartY + EndY) * 0.5f;

		// 박스 크기 -> 줌인: 작은 박스일수록 더 줌인 (fake-camera 모델과 동일).
		const float BoxArea   = FMath::Abs((EndX - StartX) * (EndY - StartY));
		const float FrameArea = (float)HucomsProtocol::FrameW * (float)HucomsProtocol::FrameH;
		const float Coverage  = (FrameArea > 0.f) ? FMath::Clamp(BoxArea / FrameArea, 0.f, 1.f) : 1.f;
		Ch.TgtZoom = HucomsProtocol::ClampZoom(Ch.CurZoom + FMath::RoundToInt((1.f - Coverage) * 10000.f));
	}
	else // point
	{
		PixelX = GetQFloat(Req, TEXT("center.pointx"), HucomsProtocol::FrameW * 0.5f);
		PixelY = GetQFloat(Req, TEXT("center.pointy"), HucomsProtocol::FrameH * 0.5f);
	}

	// 델타 환산은 '현재 줌의 실효 FOV' 기준 — 광각 상수를 그대로 쓰면 화면상 같은 클릭
	// 오프셋이 줌 배율만큼 과이동한다(예: 10x 줌에서 10배 오버슈트로 엉뚱한 곳을 조준).
	// 세로 FOV 는 인자로 넘기지 않는다: rectilinear 광학은 가로·세로가 같은 초점거리 하나를
	// 공유하고(실기 실측에서 팬/틸트로 역산한 초점거리가 0.1% 이내 일치), 옛 WideVFovDeg 는
	// 선형 모델의 유물이라 그걸 쓰는 순간 세로가 30% 빗나갔다.
	const float CurHFov = HucomsProtocol::ZoomPosToHFov(Ch.CurZoom, WideHFovDeg);

	// 현재 틸트를 함께 넘긴다 — 팬 축이 월드 수직축이라 광축이 기울면 조준 기하가 달라진다
	// (가로 클릭에도 틸트가 조금 딸려 움직인다). 실기가 정확히 그렇게 동작한다.
	int32 PanDeltaCd, TiltDeltaCd;
	HucomsProtocol::PixelToDeltaCentideg(PixelX, PixelY, CurHFov, Ch.CurTilt / 100.f,
		PanDeltaCd, TiltDeltaCd, SetCenterFocalGain);

	Ch.TgtPan  = HucomsProtocol::WrapPan(Ch.CurPan + PanDeltaCd);
	// 실기 setcenter 규약(fov-convert.mjs ptzToWidePixel, cam-001 필드검증): 프레임에서 아래(y+)에
	// 있는 대상을 중앙으로 가져오려면 tiltpos 를 '올린다'(higher tiltpos = 카메라가 아래를 봄).
	// => 델타를 '더한다'. (기존 '-' 는 렌더 없는 fake-camera mock 의 미검증 부호를 답습한 상하반전 버그.)
	Ch.TgtTilt = HucomsProtocol::ClampTilt(Ch.CurTilt + TiltDeltaCd);

	UE_LOG(LogHucomsSim, Verbose, TEXT("[Hucoms] :%d setcenter(%s) px=(%.0f,%.0f) -> dPan=%d dTilt=%d => tgt pan=%d tilt=%d zoom=%d"),
		Ch.HttpPort, *Type, PixelX, PixelY, PanDeltaCd, TiltDeltaCd, Ch.TgtPan, Ch.TgtTilt, Ch.TgtZoom);
}

FString UHucomsServerSubsystem::BuildPtzPosBody(const FHucomsChannel& Ch) const
{
	// 'key = value' 텍스트. settle 판정에 panpos/tiltpos/zoompos 숫자 필수.
	return FString::Printf(TEXT("panpos = %d\ntiltpos = %d\nzoompos = %d\nfocuspos = %d\n"),
		Ch.CurPan, Ch.CurTilt, Ch.CurZoom, Ch.CurFocus);
}

FString UHucomsServerSubsystem::DebugStateString() const
{
	FString Out = FString::Printf(TEXT("Hucoms 채널 %d개:"), Channels.Num());
	for (const TSharedPtr<FHucomsChannel>& ChPtr : Channels)
	{
		const FHucomsChannel& Ch = *ChPtr;
		Out += FString::Printf(TEXT("\n  [:%d] cam=%s cur(pan=%d tilt=%d zoom=%d)"),
			Ch.HttpPort, *GetNameSafe(Ch.Camera.Get()), Ch.CurPan, Ch.CurTilt, Ch.CurZoom);
	}
	return Out;
}
