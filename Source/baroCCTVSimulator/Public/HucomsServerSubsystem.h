// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "IHttpRouter.h"          // IHttpRouter, FHttpRequestHandler, FHttpRouteHandle
#include "HttpResultCallback.h"   // FHttpResultCallback
#include "Containers/Queue.h"     // TQueue (완료 프레임 MPSC)
#include "HucomsServerSubsystem.generated.h"

class APTZCamera;
class UPTZCaptureComponent;
class FMjpegStreamServer;
class FRHIGPUTextureReadback;
struct FHttpServerRequest;

/** 비동기 스트림 캡처의 채널 상태 머신(게임 스레드 소유). */
enum class EStreamCapState : uint8
{
	Idle,      // 제출 가능
	InFlight,  // EnqueueCopy 됨 — IsReady 대기 중
};

/** 워커 스레드에서 인코딩을 마친 스트림 프레임 — 게임 스레드 Tick 이 drain 해 MJPEG 로 송신. */
struct FCompletedStreamFrame
{
	int32 HttpPort = 0;      // 어느 채널로 보낼지(포트로 조인 — UObject/포인터 참조 회피)
	uint64 Seq = 0;          // 제출 시퀀스(역전 프레임 드랍 가드)
	TArray<uint8> Jpeg;      // 완성된 JPEG 바이트
};

/**
 * FHucomsChannel — 한 APTZCamera = 한 Hucoms 서버 채널.
 *
 * 각 카메라를 자기 고유 포트에 독립 Hucoms CGI 서버(HTTP) + 연속 MJPEG 스트림(TCP)으로 노출한다.
 * 채널은 자기만의 "정준 PTZ 상태(raw Hucoms 단위)"를 소유하고 그 카메라에만 미러링한다.
 * baro_calory 의 devices.list[].{host,port} 와 채널이 1:1 이라, 클라이언트가 카메라별로 개별 접속한다.
 */
struct FHucomsChannel
{
	TWeakObjectPtr<APTZCamera> Camera;
	int32 HttpPort = 0;
	int32 MjpegPort = 0;

	// --- HTTP CGI 라우터 ---
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;

	// --- 정준 PTZ 상태 (raw Hucoms 단위) = 이 카메라의 와이어 진실 소스 ---
	int32 CurPan = 0, CurTilt = 0, CurZoom = 0, CurFocus = 0;
	int32 TgtPan = 0, TgtTilt = 0, TgtZoom = 0, TgtFocus = 0;
	bool bPtEnable = true, bZfEnable = true;

	// --- 연속(velocity) 이동 상태 (pt_control/zf_control setptmove) — 0 = 정지 ---
	// 단위: pan/tilt = centi-deg/sec, zoom = tick/sec. 부호는 raw Hucoms 증감 방향
	// (pan+ = 우측, tilt+ = 아래, zoom+ = 망원). goptzfpos/setcenter(절대 이동) 수신 시 0으로 리셋된다.
	float PanVel = 0.f, TiltVel = 0.f, ZoomVel = 0.f;

	// 고정형 카메라(APTZCamera::bFixedMode 복사). true 면 이 채널은 goptzfpos/setcenter 명령과
	// 모터 슬루를 무시하고 설치 자세로 고정한다(스트림/스냅샷은 정상). capabilityptz 는 PTZ 미지원 광고.
	bool bFixed = false;

	// --- 연속 MJPEG 스트림 서버(RTSP 브리지 입력) ---
	FMjpegStreamServer* Stream = nullptr;
	float StreamAccum = 0.f;

	// --- 스트림 송신 fps 실측 (1초 창) — HUD 표시용 ---
	float FpsWindowAccum = 0.f;
	int32 FpsWindowFrames = 0;
	float MeasuredStreamFps = 0.f;

	// --- 렌더 자원 생명주기("이 카메라를 켜 둘 것인가") ---
	// 이 카메라를 마지막으로 "쓴" 월드 시각(초). 수요 = 스냅샷/스트림 캡처/PTZ 이동 명령.
	// 상태 폴링(getptzfpos·capabilityptz)은 수요가 아니다 — 헬스체크가 전 채널을 켜 두면
	// 재설계가 무효화된다. 센티널 음수라 월드 t≈0 에서 "방금 썼다"로 오탐하지 않는다.
	double LastDemandTime = -1.0e9;

	// --- 비동기 스트림 캡처(bAsyncStreamCapture=true) ---
	// GPU→CPU 리드백을 게임 스레드 무정지로 수행(FRHIGPUTextureReadback). 채널당 in-flight 1개:
	// 상태 머신이 "소비 후 재사용"을 보장하므로 링버퍼 불필요(프레임 유실 없음).
	TUniquePtr<FRHIGPUTextureReadback> StreamReadback;
	int32 ReadbackW = 0, ReadbackH = 0;                  // 현재 스테이징 크기
	EStreamCapState StreamCapState = EStreamCapState::Idle;
	uint64 StreamCapSeq = 0;                             // 제출 시퀀스(단조 증가)
	uint64 LastDeliveredSeq = 0;                         // 마지막으로 UpdateFrame 한 시퀀스
};

/**
 * FPTZCameraSpawnSpec — DefaultGame.ini 로 런타임에 추가 스폰할 CCTV 카메라 1대의 명세.
 *
 * BuildChannels 최상단에서 GetAllActorsOfClass 전에 스폰되므로, 같은 열거 패스에서 채널·포트를
 * 받아 /scene/cameras·Hucoms CGI·MJPEG 에 자동 노출된다(레벨 무수정으로 높이별 카메라 배치용).
 * 포트는 반드시 명시한다 — 자동 포트(BaseHttpPort+index)는 액터 열거 순서에 좌우돼 비결정적이다.
 */
USTRUCT()
struct FPTZCameraSpawnSpec
{
	GENERATED_BODY()

	/** 광학중심 월드 위치(cm). 높이는 Z(예: 16 m = 1600). 피벗 레버암 0 이라 액터 위치 = 광학중심. */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	FVector Location = FVector::ZeroVector;

	/** 설치 방위(월드 yaw, deg). 주차 행 중심을 조준한다. */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	float YawDeg = 0.f;

	/** 설치 하향각(deg, 음수=아래). BuildChannels 가 tilt 로 이관한다(기본 -20 = 20도 하향). */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	float PitchDeg = -20.f;

	/** Hucoms HTTP CGI 포트(명시 필수). */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	int32 HttpPort = 0;

	/** 연속 MJPEG 스트림 포트(명시 필수, 8095 씬 제어 포트와 겹치지 말 것). */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	int32 MjpegPort = 0;

	/** 고정형 카메라 여부(true 면 PTZ 명령 무시). */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	bool bFixedMode = false;

	/** 식별 메모(에디터 라벨·로그용, 예: "16m"). */
	UPROPERTY(EditAnywhere, Category = "Spawn")
	FString Note;
};

/**
 * UHucomsServerSubsystem — 인엔진 "Hucoms PTZ CCTV 행세" 서버 (멀티 카메라)
 *
 * 목표: baro_calory(Node) 가 sim 의 IP 로 접속해도 실기와 동일하게 동작하도록, Hucoms HTTP CGI 표면을
 * UE 안에서 그대로 응답한다. **레벨의 각 APTZCamera 마다 자기 포트에 독립 서버(채널)를 띄운다.**
 *   - GET /cgi-bin/control/ptzf_status.cgi   (getptzfpos / goptzfpos / getptzstatus / setptzstatus / lensreset)
 *   - GET /cgi-bin/control/ptz_centering.cgi (action=setcenter, type=point|box) - 조준의 핵심
 *   - GET /cgi-bin/control/capabilityptz.cgi (action=getPTZ)
 *   - GET /cgi-bin/image/jpeg.cgi            (활성 카메라 실렌더 JPEG, 실패 시 4바이트 스텁)
 *   - GET /cgi-bin/image/mjpeg.cgi           (단일 프레임 멀티파트 / 연속 스트림은 별도 TCP 포트)
 *
 * 포트 부여: 카메라의 HucomsHttpPort / HucomsMjpegPort 가 >0 이면 그 값을, 0 이면
 *   BaseHttpPort/BaseMjpegPort + (카메라 인덱스) 로 자동 부여한다. baro_calory devices[].port 와 맞출 것.
 *
 * 설계 핵심 (fidelity): 채널이 정준 PTZ 상태를 소유(Hucoms 정수 단위) → getptzfpos 라운드트립 정확.
 *   모터 슬루를 Tick 에서 시뮬, APTZCamera 는 그 current 를 미러링(SnapToTarget).
 *   setcenter 는 TAN 핀홀 역투영 + 현재 틸트를 포함한 구면 짐벌 기하를 사용.
 *
 * 수명: 게임/PIE 월드의 BeginPlay 에 채널 서버들 시작, Deinitialize 에 정지.
 */
UCLASS(config = Game)
class BAROCCTVSIMULATOR_API UHucomsServerSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//==================================================================================
	// Config (DefaultGame.ini 의 [/Script/baroCCTVSimulator.HucomsServerSubsystem] 로 오버라이드 가능)
	//==================================================================================

	/** 자동 포트 부여 시작값(HTTP CGI). 카메라 HucomsHttpPort=0 이면 BaseHttpPort + 카메라 인덱스. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Server")
	int32 BaseHttpPort = 8081;

	/** 자동 포트 부여 시작값(연속 MJPEG). 카메라 HucomsMjpegPort=0 이면 BaseMjpegPort + 카메라 인덱스. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Server")
	int32 BaseMjpegPort = 8091;

	/**
	 * wide 프리셋(zoompos 0)에서의 수평 FOV(deg). 실측값 57.14 (cam-001, 2026-07-14).
	 * 옛 값 69.88 은 "픽셀↔각도는 선형"이라는 틀린 가정으로 역산된 유물이라 22% 넓었다.
	 * 세로 FOV 는 별도 상수를 두지 않는다 — rectilinear 광학에서 종횡비로 유도되는 값이고,
	 * 옛 WideVFovDeg(30.48) 역시 같은 선형 모델의 유물이었다(그래서 세로가 30%나 빗나갔다).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Optics")
	float WideHFovDeg = 57.14f;

	/**
	 * setcenter 가 쓰는 초점거리 배율. 1.0 = 기하학적으로 정확한 카메라(기본).
	 *
	 * 실기 펌웨어는 기하는 맞게 풀지만 자기가 믿는 초점거리가 실제 렌즈와 어긋나서 조준이
	 * 빗나간다(줌에 따라 0.99~1.11, 망원 포화 구간에선 0.75 — 즉 과회전). 그 결함을 시뮬에서
	 * 일부러 재현하고 싶을 때(예: 클릭 보정 파이프라인을 실카메라 없이 검증) 이 값을 바꾼다.
	 * 시뮬을 '정확한 카메라'로 쓰는 평상시에는 1.0 으로 둔다.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Optics")
	float SetCenterFocalGain = 1.f;

	/** Pan 모터 슬루 속도 (centi-degree/sec). 9000 = 90deg/s. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Motor")
	float PanSlewCdPerSec = 9000.f;

	/** Tilt 모터 슬루 속도 (centi-degree/sec). 6000 = 60deg/s. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Motor")
	float TiltSlewCdPerSec = 6000.f;

	/** Zoom 슬루 속도 (raw tick/sec). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Motor")
	float ZoomSlewPerSec = 30000.f;

	/** Hucoms pan(centi-deg) -> UE Yaw 부호 (화면 방향 보정용, 라운드트립과 무관). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Mapping")
	float PanToYawSign = 1.f;

	/**
	 * Hucoms tilt(centi-deg) -> UE Pitch 부호.
	 * 실기 규약(fov-convert.mjs, cam-001 필드검증): higher tiltpos = 카메라가 '아래'를 봄.
	 * UE 는 +Pitch = '위'. 따라서 tiltpos↑ 를 UE 에서 아래로 만들려면 -1 로 부호를 뒤집는다.
	 * 이 값은 '실기와 같은 절대 방향으로 렌더'하기 위한 것 — 함부로 +1 로 바꾸면 sim 이 실기와
	 * 상하 반대로 렌더되어 절대 tiltpos 명령/프리셋/호밍이 전부 뒤집힌다. 조작 방향은 setcenter/UI 에서 처리.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Mapping")
	float TiltToPitchSign = -1.f;

	/** jpeg.cgi 스냅샷 가로 해상도. QHD(2560x1440) — 굶은 4K 보다 TSR 로 수렴된 QHD 가 더 선명하고
	 *  VRAM 여유가 있다(현재 RT 지오메트리 예산 초과 경고 있음). 센터링 논리프레임(1920x1080)과 무관 —
	 *  클릭 좌표는 클라이언트가 naturalWidth 기준으로 보내고 서버에서 1920 로 스케일된다.
	 *  (DefaultGame.ini [/Script/baroCCTVSimulator.HucomsServerSubsystem] 로 오버라이드 가능. VRAM 해결 후 4K 상향 가능.) */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "64"))
	int32 SnapshotWidth = 2560;

	/** jpeg.cgi 스냅샷 세로 해상도 (QHD = 1440). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "64"))
	int32 SnapshotHeight = 1440;

	/** JPEG 품질 (1~100). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "1", ClampMax = "100"))
	int32 JpegQuality = 92;

	/** 스냅샷 캡처 시 TSR 히스토리 워밍업 프레임 수. **실측 결과 0(단발)이 가장 선명**하고 워밍업은
	 *  오히려 소프트닝(연속 CaptureScene 이 정지프레임을 재블렌딩) + GPU 낭비라 0 권장. (라플라시안 분산
	 *  N0=1358 > N8=1174.) 선명도 핵심은 워밍업이 아니라 TAA-on + Lumen(PTZCaptureComponent). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "0", ClampMax = "32"))
	int32 SnapshotWarmupFrames = 0;

	/** 캡처 노출 보정(EV). SceneCapture 자동노출이 뷰포트보다 밝게 잡혀 "희게 뜨는" 것을 음수로 낮춘다.
	 *  뷰포트 톤 실측 정합 결과 -0.7 이 최적(캡처 mean 159→126, 목표 122). DefaultGame.ini 로 무리빌드 튜닝 가능. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture")
	float CaptureExposureBias = -0.7f;

	/** 캡처 컬러 대비(1=기본). 캡처 대비(std)가 뷰포트보다 낮아 밋밋 → 상향. 실측 정합 결과 1.6 (std 47→61, 목표 69). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Capture", meta = (ClampMin = "0.5", ClampMax = "3.0"))
	float CaptureContrast = 1.6f;

	/** 레벨에 APTZCamera 가 하나도 없을 때 기본 카메라를 자동 생성할지(자율 검증용). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Camera")
	bool bAutoSpawnCameraIfNone = true;

	/**
	 * 런타임에 추가 스폰할 카메라 목록(레벨 무수정 배치). DefaultGame.ini 의
	 * [/Script/baroCCTVSimulator.HucomsServerSubsystem] 에서 +SpawnCameras=(...) 로 지정.
	 * BEVHeight 파인튜닝용 높이별 카메라(8/12/16/20 m) 배치에 사용한다.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Camera")
	TArray<FPTZCameraSpawnSpec> SpawnCameras;

	/** 연속 MJPEG 스트림 서버(RTSP 브리지 입력) 활성화. 채널마다 자기 MJPEG 포트에 하나씩. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream")
	bool bEnableMjpegStream = true;

	/** 스트림 프레임레이트(캡처/송신 상한). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "1", ClampMax = "60"))
	int32 StreamFps = 15;

	/** 스트림 가로 해상도(스냅샷과 별개, FOV 동일 유지하며 경량화). */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "64"))
	int32 StreamWidth = 1280;

	/** 스트림 세로 해상도. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "64"))
	int32 StreamHeight = 720;

	/** 스트림 JPEG 품질. */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream", meta = (ClampMin = "1", ClampMax = "100"))
	int32 StreamJpegQuality = 80;

	//==================================================================================
	// 렌더 자원 생명주기 — "쓰는 카메라만 켠다"
	//
	// 0.1.8 까지는 한 번이라도 캡처된 카메라가 EndPlay 까지 SceneCapture + persistent
	// ViewState + 텍스처 시점 등록을 영구히 물고 있었다. 그 결과 클라이언트 0·캡처 0 인
	// 유휴 상태에서도 6대분 렌더 자원이 GPU 에 남아 1150ms/frame(2.5 fps)이 나왔다.
	// 실사용은 "한 번에 한 대씩 본다"이므로, 쓰는 카메라만 켜고 나머지는 즉시 끈다.
	//==================================================================================

	/**
	 * 동시에 렌더 자원을 들고 있을 수 있는 카메라 수. 기본 1 = 한 번에 한 대(실사용 패턴) —
	 * 새 카메라를 쓰면 직전 카메라는 그 자리에서 꺼진다.
	 * MJPEG 클라이언트가 붙어 있는 채널은 스트림이 끊기므로 축출 대상에서 제외된다.
	 * 0 = 상한 없음(레거시 0.1.8 동작).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Lifecycle", meta = (ClampMin = "0"))
	int32 MaxActiveCameras = 1;

	/**
	 * 축출 유예(초). 이 시간 안에 쓴 카메라는 MaxActiveCameras 규칙으로 끄지 않는다.
	 *
	 * 없으면 여러 카메라를 번갈아 쓰는 소비자(예: 검출기가 6대를 2~3초 주기로 순회 폴링)에서
	 * 매 요청마다 껐다 켜는 churn 이 나 오히려 더 무겁다(실측: 1분에 173회 재생성).
	 * 사람이 카메라를 바꿔 보는 속도(수 초~수 분)보다는 짧게 두어 전환 시엔 즉시 꺼지게 한다.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Lifecycle", meta = (ClampMin = "0"))
	float MinWarmSeconds = 5.f;

	/**
	 * 마지막 수요로부터 이 시간(초)이 지나고 MJPEG 클라이언트도 없으면 켜져 있던 카메라도 끈다.
	 * 아무도 안 볼 때 자원을 0 으로 되돌리는 장치다.
	 * 주의: 폴 간격이 이 값보다 긴 클라이언트는 매 요청마다 콜드 재생성된다 — warm 유지가
	 * 필요하면 폴 간격보다 크게 올릴 것. 0 = 시간 기반 해제 안 함(레거시).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Lifecycle", meta = (ClampMin = "0"))
	float IdleReleaseSeconds = 30.f;

	/**
	 * 꺼져 있던 카메라가 다시 켜진 "첫 캡처"에만 추가로 줄 워밍업 프레임 수.
	 * 정상 상태의 SnapshotWarmupFrames=0(실측 최적)은 그대로 두고, 콜드 시작에서만
	 * Lumen/노출 히스토리가 프레임 0 이라 뜨는 것을 보정한다.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Lifecycle", meta = (ClampMin = "0", ClampMax = "32"))
	int32 RecreateWarmupFrames = 4;

	/**
	 * 연속 MJPEG 스트림 캡처를 비동기 GPU 리드백으로 수행할지(v0.1.10~).
	 *
	 * true(기본): 스트림 프레임을 FRHIGPUTextureReadback 으로 회수 + JPEG 인코딩을 워커 스레드로
	 *   → 게임 스레드가 캡처마다 GPU 완료를 동기 대기(실측 6대 26.8~432ms 블로킹)하지 않는다.
	 *   품질은 바이트 단위로 동일(같은 렌더). 스트림에 1~2프레임 레이턴시(CCTV 무해).
	 * false: 0.1.9 동기 경로(ReadPixels + 게임스레드 인코딩) — 롤백/비교용.
	 * 스냅샷(jpeg.cgi)은 이 값과 무관하게 항상 동기(저빈도·즉시 응답).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Hucoms|Stream")
	bool bAsyncStreamCapture = true;

	//==================================================================================
	// USubsystem / UWorldSubsystem / FTickableGameObject
	//==================================================================================
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	//==================================================================================
	// 진단/테스트용 (BlueprintCallable)
	//==================================================================================

	/** 현재 채널(카메라/포트/PTZ) 요약 문자열. */
	UFUNCTION(BlueprintCallable, Category = "Hucoms")
	FString DebugStateString() const;

	/** 활성 채널 수(=서빙 중인 카메라 수). */
	UFUNCTION(BlueprintPure, Category = "Hucoms")
	int32 GetChannelCount() const { return Channels.Num(); }

	/** 채널별 상태 한 줄씩(카메라·포트·실측 스트림 fps·클라이언트 수) — sim HUD 표시용. */
	TArray<FString> GetChannelStatusLines() const;

	/**
	 * 이 카메라에 실제로 바인딩된 Hucoms 포트(HTTP/MJPEG)를 반환. 채널이 바인딩한 실효 포트가
	 * 진실의 출처라, SceneControl 의 /scene/cameras 가 이 값으로 baro_calory device.port 와 조인한다
	 * (자동 포트 부여 규칙을 다른 서브시스템이 중복 계산하지 않도록). 채널 없으면 false.
	 */
	bool GetCameraPorts(const APTZCamera* Cam, int32& OutHttpPort, int32& OutMjpegPort) const;

	//==================================================================================
	// 런타임 카메라 생명주기 (씬 제어 API /scene/cameras 가 호출) — v0.1.13
	//
	// BEV 파인튜닝의 병목이 "카메라 포즈를 바꾸려면 ini 수정 + 재시작"이었다. 스폰/이동/삭제를
	// 실행 중에 열되, 레벨 저작 카메라(폴 위 PTZCamera_N)는 보호한다 — 저작은 이교수님 소관.
	//==================================================================================

	/** 이 카메라가 스폰 경로(자동/config/API)로 만들어졌는가 — 이동/삭제 허용 판정. */
	bool IsSpawnedCamera(const APTZCamera* Cam) const;

	/** 스펙대로 카메라를 지금 스폰하고 채널(HTTP CGI + MJPEG)까지 기동한다. 실패 시 nullptr + OutError. */
	APTZCamera* SpawnCameraRuntime(const FPTZCameraSpawnSpec& Spec, FString& OutError);

	/**
	 * 스폰 카메라의 설치 자세 갱신. 널이 아닌 인자만 반영한다.
	 * PitchDeg 는 액터 회전이 아니라 채널 tilt 로 산다(BuildChannels 의 이관 규약과 동일) —
	 * 액터에도 기록해 스냅샷 왕복이 성립한다. 레벨 저작 카메라면 false + OutError.
	 */
	bool UpdateCameraPose(APTZCamera* Cam, const FVector* Location, const float* YawDeg, const float* PitchDeg, FString& OutError);

	/** 스폰 카메라를 채널 정리(in-flight flush·라우트 unbind·MJPEG 정지·캡처 반납) 후 파괴. 레벨 저작이면 false. */
	bool RemoveCameraRuntime(APTZCamera* Cam, FString& OutError);

	/** 현재 스폰 카메라들을 재스폰 가능한 스펙으로 직렬화(/scene/snapshot 용). OutCams 는 같은 순서. */
	void GetSpawnedCameraSpecs(TArray<FPTZCameraSpawnSpec>& OutSpecs, TArray<APTZCamera*>* OutCams = nullptr) const;

private:
	bool bServersStarted = false;
	bool bAutoSpawnAttempted = false;
	bool bConfigCamerasSpawned = false;

	// 카메라별 채널 (TSharedPtr 로 안정된 포인터 → 라우트 핸들러 람다가 캡처).
	TArray<TSharedPtr<FHucomsChannel>> Channels;

	/** 스폰 경로(자동/config/API)로 만들어진 카메라 — 레벨 저작과 구분(이동/삭제 게이트). */
	TSet<TWeakObjectPtr<const APTZCamera>> SpawnedCameras;

	void StartServers();
	void StopServers();

	/** 카메라 1대의 채널 상태를 만들어 Channels 에 추가(포트·tilt 이관·bFixed·sim 설정). */
	TSharedPtr<FHucomsChannel> CreateChannelForCamera(APTZCamera* Cam, int32 InHttpPort, int32 InMjpegPort);

	/** 채널 1개의 라우터 획득·라우트 바인딩·MJPEG 기동(StartServers 의 채널부와 동일 경로). 실패 시 false. */
	bool StartChannelServers(TSharedPtr<FHucomsChannel> ChPtr);

	/** 채널 1개 정지 — 라우트 unbind·라우터 해제·MJPEG 정지(캡처 반납은 호출자 책임). */
	void StopChannel(FHucomsChannel& Ch);

	/** 이 카메라의 채널을 찾는다. 없으면 nullptr. */
	TSharedPtr<FHucomsChannel> FindChannelFor(const APTZCamera* Cam) const;

	/** 레벨의 APTZCamera(bServeHucoms) 들을 열거해 채널을 만든다(포트 부여 + 카메라 sim 설정). */
	void BuildChannels();

	/** SpawnCameras(config) 명세대로 카메라를 런타임 스폰한다. BuildChannels 최상단에서 1회 호출. */
	void SpawnConfiguredCameras(UWorld* World);

	void ConfigureCameraForSim(APTZCamera* Cam);
	void MirrorChannel(FHucomsChannel& Ch);

	/** 카메라의 캡처 컴포넌트를 찾거나 생성. 없으면 nullptr. */
	UPTZCaptureComponent* ResolveCapture(APTZCamera* Cam);

	/** 채널의 카메라를 렌더해 JPEG 바이트를 채운다. 실패 시 false(호출부가 스텁 폴백). */
	bool RenderSnapshotJpeg(FHucomsChannel& Ch, TArray<uint8>& OutBytes);

	// --- 렌더 자원 생명주기("쓰는 카메라만 켠다") ---

	/** 캡처 컴포넌트를 찾기만 한다(ResolveCapture 와 달리 생성하지 않음). 없으면 nullptr. */
	UPTZCaptureComponent* FindCapture(const FHucomsChannel& Ch) const;

	/** 이 채널이 지금 렌더 자원을 들고 있는가(= 켜져 있는가). */
	bool IsChannelWarm(const FHucomsChannel& Ch) const;

	/**
	 * 이 카메라를 "썼다"고 기록하고(수요 스탬프), MaxActiveCameras 를 넘겨 켜져 있는
	 * 다른 카메라를 끈다. 모든 캡처 경로와 PTZ 이동 명령에서 호출한다.
	 */
	void StampDemand(FHucomsChannel& Ch);

	/** 채널의 렌더 자원을 반납한다(이미 꺼져 있으면 no-op). Reason 은 로그용. */
	void ReleaseChannelCapture(FHucomsChannel& Ch, const TCHAR* Reason);

	// --- 비동기 스트림 캡처 파이프라인(bAsyncStreamCapture=true) ---

	/** 완료(인코딩 끝난) 프레임 큐 — 워커가 Enqueue, 게임 스레드 Tick 이 Dequeue. SharedPtr 라
	 *  서브시스템 사후에도 워커가 안전하게 Enqueue(버려짐). */
	TSharedPtr<TQueue<FCompletedStreamFrame, EQueueMode::Mpsc>, ESPMode::ThreadSafe> CompletedFrames;

	/** 완료 큐를 비우며 각 프레임을 해당 채널 MJPEG 로 송신(게임 스레드). */
	void DrainCompletedFrames();

	/** InFlight 리드백이 준비됐으면 픽셀을 회수(렌더 스레드 Lock/복사) + 워커 인코딩 킥. 상태 Idle 복귀. */
	void CollectStreamReadback(FHucomsChannel& Ch);

	/** 스트림 프레임 1장 비동기 제출(PrepareCapture→RenderOnce→EnqueueCopy). 상태 InFlight. */
	void SubmitStreamCapture(FHucomsChannel& Ch, bool bCold);

	// --- CGI 핸들러 (채널별) ---
	bool HandlePtzfStatus(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandlePtzCentering(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCapabilityPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	/** pt_control.cgi (action=setptmove) — 연속 Pan/Tilt velocity 제어(방향+속도, stop 으로 정지). */
	bool HandlePtControl(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	/** zf_control.cgi (action=setzfmove/onepush) — 연속 Zoom/Focus velocity 제어. */
	bool HandleZfControl(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleJpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMjpeg(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	/**
	 * 캡처 튜닝(노출/대비/JPEG품질/워밍업/해상도) 조회·설정 — Hucoms 실기 프로토콜에는 없는
	 * 시뮬레이터 전용 디버그 API. 쿼리 파라미터로 준 항목만 갱신하고, 항상 현재 전체 상태를
	 * JSON으로 반환한다. 값은 전 채널(카메라) 공유 — 어느 포트로 호출해도 동일하게 적용된다
	 * (RenderSnapshotJpeg 가 이 subsystem 멤버를 매 캡처마다 읽으므로 재빌드 불필요, 즉시 반영).
	 * UI는 이 서브시스템에 두지 않는다 — webUI(baro_calory) 쪽 책임, 여긴 이미지 생성만.
	 */
	bool HandleTuning(FHucomsChannel& Ch, const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// --- 명령 적용 (채널별) ---
	void ApplyGoPtz(FHucomsChannel& Ch, const FHttpServerRequest& Req);
	void ApplySetCenter(FHucomsChannel& Ch, const FHttpServerRequest& Req);
	FString BuildPtzPosBody(const FHucomsChannel& Ch) const;
};
