// Copyright gbox3d. All Rights Reserved.

#include "SceneControlSubsystem.h"

#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"    // FHttpServerRequest, EHttpServerRequestVerbs, EHttpServerResponseCodes
#include "HttpPath.h"
#include "IHttpRouter.h"

#include "Engine/World.h"
#include "EngineUtils.h"                     // TActorIterator
#include "Engine/HitResult.h"                // FHitResult (가시성 라인트레이스)
#include "CollisionQueryParams.h"            // FCollisionQueryParams, ECC_Visibility
#include "Components/ChildActorComponent.h"
#include "Components/MeshComponent.h"         // UMeshComponent (차량 렌더 메시 aggregate bounds)
#include "UObject/UnrealType.h"              // FIntProperty, FArrayProperty, FindFProperty, FScriptArrayHelper
#include "UObject/SoftObjectPath.h"          // FSoftObjectPath::GetAssetName (Mesh_List 가 soft 참조일 때)
#include "Misc/FileHelper.h"                 // scene-help.md 로드 (/scene/help — 산문은 파일, 코드는 서빙만)

#include "PTZCamera.h"                        // APTZCamera (오버레이 투영 대상 카메라)
#include "HucomsProtocol.h"                   // zoompos->HFOV 공용 실측표
#include "HucomsServerSubsystem.h"           // 카메라 실효 포트 조회(GetCameraPorts)
#include "Camera/CameraComponent.h"          // UCameraComponent (광학 포즈)
#include "Camera/CameraTypes.h"              // FMinimalViewInfo, ECameraProjectionMode
#include "Kismet/GameplayStatics.h"          // GetViewProjectionMatrix
#include "Interfaces/IPluginManager.h"       // .uplugin VersionName 런타임 조회(플러그인 버전 노출)

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

DEFINE_LOG_CATEGORY_STATIC(LogSceneCtrl, Log, All);

//======================================================================================
// 로컬 헬퍼 (카탈로그 상수 / JSON 조립·파싱 / BP 리플렉션 호출)
//======================================================================================
namespace
{
	// BP_Car / BP_Plate 실측 카탈로그(2026-07-07). scene-control-client.mjs SIM_CATALOG 와 동일 캐논.
	const TCHAR* KorList[] = { TEXT("가"), TEXT("나"), TEXT("다"), TEXT("라"), TEXT("마") };
	const TCHAR* PlateTypeNames[] = { TEXT("일반"), TEXT("영업용"), TEXT("전기차") };
	struct FColorDef { const TCHAR* Name; float R, G, B; };
	const FColorDef ColorDefs[] = {
		{ TEXT("화이트"), 0.94f, 0.94f, 0.90f },
		{ TEXT("블랙"),   0.04f, 0.04f, 0.05f },
		{ TEXT("실버"),   0.65f, 0.67f, 0.70f },
		{ TEXT("그레이"), 0.40f, 0.42f, 0.45f },
		{ TEXT("레드"),   0.45f, 0.10f, 0.12f },
		{ TEXT("옐로"),   0.90f, 0.78f, 0.30f },
		{ TEXT("그린"),   0.12f, 0.30f, 0.20f },
		{ TEXT("블루"),   0.15f, 0.22f, 0.60f },
	};

	struct FSlotInfo { FString Id; FString Label; FString Type; FTransform Xform; };
	struct FGroundReference
	{
		bool bValid = false;
		double ZCm = 0.0;
		int32 SampleCount = 0;
		double MaxDeviationCm = 0.0;
	};

	FString SerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	// application/json 응답 + 상태코드. baro_calory 는 sim 의 HTTP status 를 코드로 되돌린다
	// (scene-control-client.mjs: 404->NOT_FOUND, 409->OCCUPIED, 400->BAD_INPUT).
	TUniquePtr<FHttpServerResponse> JsonResp(const TSharedRef<FJsonObject>& Obj, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(SerializeJson(Obj), TEXT("application/json"));
		R->Code = Code;
		return R;
	}

	TUniquePtr<FHttpServerResponse> JsonError(EHttpServerResponseCodes Code, const FString& Msg)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("error"), Msg);
		return JsonResp(O, Code);
	}

	FString BodyToString(const FHttpServerRequest& Req)
	{
		if (Req.Body.Num() == 0) { return FString(); }
		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Req.Body.GetData()), Req.Body.Num());
		return FString(Conv.Length(), Conv.Get());
	}

	// 빈 바디 => 빈 오브젝트({}) (reset 등). 파싱 실패 => nullptr (호출부가 400).
	TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Req)
	{
		const FString Str = BodyToString(Req);
		if (Str.IsEmpty()) { return MakeShared<FJsonObject>(); }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return nullptr; }
		return Root;
	}

	TSharedRef<FJsonObject> PlacementToJson(const FVector& Loc, const FRotator& Rot)
	{
		TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetNumberField(TEXT("x"), Loc.X); L->SetNumberField(TEXT("y"), Loc.Y); L->SetNumberField(TEXT("z"), Loc.Z);
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("pitch"), Rot.Pitch); R->SetNumberField(TEXT("yaw"), Rot.Yaw); R->SetNumberField(TEXT("roll"), Rot.Roll);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("location"), L);
		O->SetObjectField(TEXT("rotation"), R);
		return O;
	}

	TSharedRef<FJsonObject> TransformToJson(const FTransform& X)
	{
		return PlacementToJson(X.GetLocation(), X.Rotator());
	}

	// 빠진 성분은 0 — 항등이 곧 "변형 없음"이다.
	void ParsePlacement(const TSharedPtr<FJsonObject>& O, FVector& OutLoc, FRotator& OutRot)
	{
		OutLoc = FVector::ZeroVector;
		OutRot = FRotator::ZeroRotator;
		const TSharedPtr<FJsonObject>* L;
		if (O->TryGetObjectField(TEXT("location"), L))
		{
			double x = 0, y = 0, z = 0;
			(*L)->TryGetNumberField(TEXT("x"), x); (*L)->TryGetNumberField(TEXT("y"), y); (*L)->TryGetNumberField(TEXT("z"), z);
			OutLoc = FVector(x, y, z);
		}
		const TSharedPtr<FJsonObject>* R;
		if (O->TryGetObjectField(TEXT("rotation"), R))
		{
			double p = 0, yw = 0, rl = 0;
			(*R)->TryGetNumberField(TEXT("pitch"), p); (*R)->TryGetNumberField(TEXT("yaw"), yw); (*R)->TryGetNumberField(TEXT("roll"), rl);
			OutRot = FRotator(p, yw, rl);
		}
	}

	FTransform ParseTransform(const TSharedPtr<FJsonObject>& O)
	{
		FVector Loc; FRotator Rot;
		ParsePlacement(O, Loc, Rot);
		return FTransform(Rot, Loc);
	}

	TSharedRef<FJsonObject> CarToJson(const FSimCarState& S)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), S.Id);
		if (S.SlotId.IsEmpty()) { O->SetField(TEXT("slotId"), MakeShared<FJsonValueNull>()); }
		else { O->SetStringField(TEXT("slotId"), S.SlotId); }
		O->SetObjectField(TEXT("transform"), TransformToJson(S.Transform));
		O->SetObjectField(TEXT("offset"), PlacementToJson(S.OffsetLocation, S.OffsetRotation));
		O->SetNumberField(TEXT("carType"), S.CarType);
		O->SetNumberField(TEXT("color"), S.Color);
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetNumberField(TEXT("type"), S.PlateType);
		P->SetStringField(TEXT("city"), S.City);
		P->SetStringField(TEXT("prefix"), S.Prefix);
		P->SetStringField(TEXT("kor"), S.Kor);
		P->SetStringField(TEXT("number"), S.Number);
		O->SetObjectField(TEXT("plate"), P);
		return O;
	}

	// "BP_ParkingSlot_5m_C" -> "5m"
	FString SlotTypeFromClass(const FString& ClassName, const FString& Prefix)
	{
		FString S = ClassName;
		S.RemoveFromStart(Prefix);
		S.RemoveFromStart(TEXT("_"));
		S.RemoveFromEnd(TEXT("_C"));
		return S.IsEmpty() ? TEXT("slot") : S;
	}

	bool NaturalLess(const FString& A, const FString& B)
	{
		int32 IA = 0;
		int32 IB = 0;
		while (IA < A.Len() && IB < B.Len())
		{
			const bool bDigitA = FChar::IsDigit(A[IA]);
			const bool bDigitB = FChar::IsDigit(B[IB]);
			if (bDigitA && bDigitB)
			{
				uint64 NA = 0;
				uint64 NB = 0;
				int32 DigitsA = 0;
				int32 DigitsB = 0;
				while (IA < A.Len() && FChar::IsDigit(A[IA]))
				{
					NA = NA * 10 + uint64(A[IA] - TCHAR('0'));
					++IA;
					++DigitsA;
				}
				while (IB < B.Len() && FChar::IsDigit(B[IB]))
				{
					NB = NB * 10 + uint64(B[IB] - TCHAR('0'));
					++IB;
					++DigitsB;
				}
				if (NA != NB) { return NA < NB; }
				if (DigitsA != DigitsB) { return DigitsA < DigitsB; }
				continue;
			}
			if (bDigitA != bDigitB) { return bDigitA; }

			const TCHAR CA = FChar::ToLower(A[IA]);
			const TCHAR CB = FChar::ToLower(B[IB]);
			if (CA != CB) { return CA < CB; }
			++IA;
			++IB;
		}
		return A.Len() < B.Len();
	}

	void CollectSlots(UWorld* World, const FString& Prefix, TArray<FSlotInfo>& Out)
	{
		if (!World) { return; }
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) { continue; }
			const FString CN = A->GetClass()->GetName();
			if (!CN.StartsWith(Prefix)) { continue; }
			FSlotInfo S;
			S.Id = A->GetName();                       // 배치 액터의 오브젝트명 = 안정 id(쿠킹 후 유지).
#if WITH_EDITOR
			S.Label = A->GetActorLabel();              // 에디터 Outliner 표시명. RPC 키로 쓰지 않고 UI 표시용으로만 노출.
#else
			S.Label = S.Id;                            // 패키지/런타임 빌드는 ActorLabel API가 없으므로 안정 id로 폴백.
#endif
			if (S.Label.IsEmpty()) { S.Label = S.Id; }
			S.Type = SlotTypeFromClass(CN, Prefix);
			S.Xform = A->GetActorTransform();
			Out.Add(S);
		}
		Out.Sort([](const FSlotInfo& A, const FSlotInfo& B) { return NaturalLess(A.Label, B.Label); });
	}

	// 슬롯 액터 배치 원점 Z의 중앙값. 짝수 개면 가운데 두 값의 평균을 쓴다.
	FGroundReference CalculateGroundReference(const TArray<FSlotInfo>& Slots)
	{
		FGroundReference Out;
		Out.SampleCount = Slots.Num();
		if (Slots.IsEmpty()) { return Out; }

		TArray<double> ZValues;
		ZValues.Reserve(Slots.Num());
		for (const FSlotInfo& Slot : Slots) { ZValues.Add(Slot.Xform.GetLocation().Z); }
		ZValues.Sort();

		const int32 Mid = ZValues.Num() / 2;
		Out.ZCm = (ZValues.Num() % 2 == 0)
			? (ZValues[Mid - 1] + ZValues[Mid]) * 0.5
			: ZValues[Mid];
		for (const double Z : ZValues)
		{
			Out.MaxDeviationCm = FMath::Max(Out.MaxDeviationCm, FMath::Abs(Z - Out.ZCm));
		}
		Out.bValid = true;
		return Out;
	}

	// --- 차종 목록 ---
	// BP_Car 의 Mesh_List(StaticMesh 배열) 원소 순서가 selected_Car 인덱스의 유일한 진실이다
	// (Change_Car = Array_Get(Mesh_List, selected_Car) -> SetStaticMesh). 이름을 C++ 에 베끼면
	// BP 에서 차를 추가·재정렬하는 순간 조용히 어긋나므로, CDO 를 리플렉션으로 읽어 그대로 노출한다.
	const TCHAR* CarMeshListProp = TEXT("Mesh_List");

	// Mesh_List 를 못 읽었을 때만 쓰는 과거 값(2026-07-07 실측). 이름 없이 인덱스만 노출된다.
	constexpr int32 LegacyCarCount = 23;

	// 에셋명 -> 표시명. "현대_쏘나타" -> "현대 쏘나타", "기아_봉고_탑차" -> "기아 봉고 탑차".
	FString CarAssetToDisplayName(const FString& AssetName)
	{
		return AssetName.Replace(TEXT("_"), TEXT(" "));
	}

	// 에셋명 -> 검출 클래스 라벨(car/truck/van). BP_Car.Mesh_List 에 클래스 메타가 없어 이름 기반 휴리스틱.
	// 현재 23종에 버스는 없다(최대 승합=스타렉스=van). JS scene-control-client.mjs 와 동일하게 유지할 것.
	FString CarAssetToClass(const FString& AssetName)
	{
		static const TCHAR* TruckKeys[] = { TEXT("봉고"), TEXT("탑차"), TEXT("포터") };
		static const TCHAR* VanKeys[]   = { TEXT("스타렉스"), TEXT("카니발") };
		for (const TCHAR* K : TruckKeys) { if (AssetName.Contains(K)) { return TEXT("truck"); } }
		for (const TCHAR* K : VanKeys)   { if (AssetName.Contains(K)) { return TEXT("van"); } }
		return TEXT("car");
	}

	// 실패 시 false + Out 비움. 성공 시 Out[i] = 인덱스 i 의 에셋명.
	bool CollectCarAssetNames(UClass* CarCls, TArray<FString>& Out)
	{
		Out.Reset();
		if (!CarCls) { return false; }
		UObject* CDO = CarCls->GetDefaultObject();
		FArrayProperty* Arr = FindFProperty<FArrayProperty>(CarCls, CarMeshListProp);
		if (!CDO || !Arr) { return false; }

		// FSoftObjectProperty 는 FObjectPropertyBase 를 상속하므로 soft 를 먼저 본다.
		FSoftObjectProperty* SoftInner = CastField<FSoftObjectProperty>(Arr->Inner);
		FObjectPropertyBase* HardInner = CastField<FObjectPropertyBase>(Arr->Inner);
		if (!SoftInner && !HardInner) { return false; }

		FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(CDO));
		const int32 N = Helper.Num();
		Out.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			const uint8* Elem = Helper.GetRawPtr(i);
			if (SoftInner)
			{
				// soft 참조면 메시를 로드하지 않고 경로에서 이름만 취한다.
				Out.Add(SoftInner->GetPropertyValue(Elem).ToSoftObjectPath().GetAssetName());
			}
			else
			{
				const UObject* Mesh = HardInner->GetObjectPropertyValue(Elem);
				Out.Add(Mesh ? Mesh->GetName() : FString());
			}
		}
		return N > 0;
	}

	// 표시 중인 메시 컴포넌트만 actor-local 좌표로 합친다. UMeshComponent 로 한정해
	// 번호판의 가변 TextRender bounds가 차량 치수에 섞이지 않게 하고, child actor는 재귀 포함한다.
	FBox CalculateRenderedMeshBounds(const AActor* Car)
	{
		FBox Box(ForceInit);
		if (!Car) { return Box; }

		Car->ForEachComponent<UMeshComponent>(/*bIncludeFromChildActors=*/true,
			[&](UMeshComponent* Mesh)
			{
				if (!Mesh || !Mesh->IsRegistered() || !Mesh->IsVisible()) { return; }
				const FTransform ComponentToActor =
					Mesh->GetComponentTransform().GetRelativeTransform(Car->GetActorTransform());
				const FBox MeshBox = Mesh->CalcBounds(ComponentToActor).GetBox();
				if (MeshBox.IsValid && !MeshBox.GetSize().IsNearlyZero()) { Box += MeshBox; }
			});
		return Box;
	}

	bool SetIntProp(UObject* Obj, const TCHAR* Name, int32 Val)
	{
		if (!Obj) { return false; }
		FIntProperty* P = FindFProperty<FIntProperty>(Obj->GetClass(), Name);
		if (!P) { return false; }
		P->SetPropertyValue_InContainer(Obj, Val);
		return true;
	}

	// 단일 int32 파라미터 BP 함수 호출(Change_Car/Change_Color/Change_Plate). 파라미터 struct 레이아웃이
	// UFunction 시그니처(int32 1개)와 일치해야 한다.
	void CallIntFn(AActor* A, const TCHAR* FnName, int32 Val)
	{
		if (!A) { return; }
		if (UFunction* Fn = A->FindFunction(FName(FnName)))
		{
			struct { int32 V; } Params{ Val };
			A->ProcessEvent(Fn, &Params);
		}
	}

	// 앞/뒤 자식 BP_Plate 의 Change_Text(FString) 호출로 번호판 글자 설정.
	void SetPlateTextOnChildren(AActor* Car, const FString& PlateStr)
	{
		if (!Car) { return; }
		TArray<UChildActorComponent*> Kids;
		Car->GetComponents<UChildActorComponent>(Kids);
		for (UChildActorComponent* K : Kids)
		{
			AActor* Plate = K ? K->GetChildActor() : nullptr;
			if (!Plate) { continue; }
			if (UFunction* Fn = Plate->FindFunction(FName(TEXT("Change_Text"))))
			{
				struct { FString S; } Params{ PlateStr };
				Plate->ProcessEvent(Fn, &Params);
			}
		}
	}

	// 한국 신형 번호판(앞 3자리 + 한글 + 뒤 4자리)으로 자릿수를 정규화한다.
	// BP_Plate::Change_Text 파싱(Num_3=sub(0,3), Kor=sub(3), Num_4=sub(4,4))이 이 자릿수를 전제하므로
	// 앞자리가 2자리면 Num_3 칸에 한글이 섞여 "###" 대신 "##한글"이 되어 유럽식처럼 쪼개진다.
	// 저장값=렌더값 일치를 위해 spawn/patch 시 S 자체를 정규화한다(응답·웹UI 표시도 정규화값).
	void NormalizeKoreanPlate(FSimCarState& S)
	{
		auto Digits = [](const FString& In, int32 Width)
		{
			FString D;
			for (int32 i = 0; i < In.Len(); ++i) { if (FChar::IsDigit(In[i])) { D.AppendChar(In[i]); } }
			if (D.Len() > Width) { D = D.Right(Width); }
			while (D.Len() < Width) { D = FString(TEXT("0")) + D; }  // 부족분 좌측 0패딩(정상 입력이면 미발생)
			return D;
		};
		S.Prefix = Digits(S.Prefix, 3);
		S.Number = Digits(S.Number, 4);
		const FString K = S.Kor.Left(1);
		S.Kor = K.IsEmpty() ? FString(TEXT("가")) : K;
	}

	//----------------------------------------------------------------------------------
	// 오버레이 투영: 카메라 열거 / 카메라→JSON / 월드점→픽셀(그라운드-트루스)
	//----------------------------------------------------------------------------------
	struct FCamEntry { APTZCamera* Cam = nullptr; int32 Http = 0; int32 Mjpeg = 0; };

	// 레벨의 APTZCamera 를 열거하고 각 카메라의 실효 Hucoms 포트를 채워 이름순 정렬.
	void CollectCameras(UWorld* World, TArray<FCamEntry>& Out)
	{
		if (!World) { return; }
		UHucomsServerSubsystem* Hu = World->GetSubsystem<UHucomsServerSubsystem>();
		for (TActorIterator<APTZCamera> It(World); It; ++It)
		{
			APTZCamera* C = *It;
			if (!C) { continue; }
			FCamEntry E; E.Cam = C; E.Http = C->HucomsHttpPort; E.Mjpeg = C->HucomsMjpegPort;
			int32 H = 0, M = 0;
			if (Hu && Hu->GetCameraPorts(C, H, M)) { E.Http = H; E.Mjpeg = M; }  // 채널이 실제 바인딩한 포트 우선
			Out.Add(E);
		}
		Out.Sort([](const FCamEntry& A, const FCamEntry& B) { return A.Cam->GetName() < B.Cam->GetName(); });
	}

	// 광학 포즈 = CameraComp 월드 트랜스폼(pan/tilt 가 이미 구워진, 씬캡처가 실제 렌더하는 시점).
	FTransform CameraOpticalTransform(const APTZCamera* C)
	{
		if (C && C->CameraComp) { return C->CameraComp->GetComponentTransform(); }
		return C ? C->GetActorTransform() : FTransform::Identity;
	}

	// 대상 카메라 광학중심에서 차량 AABB 표본점 15개(중심+8코너+6면중심)로 라인트레이스해 가시 비율(0..1).
	// 차량 자신은 무시 → '다른 물체(차/배경)에 의한 가림'만 측정한다(자기 실루엣 자가림은 제외).
	// 주의: 씬 배경/차량에 Visibility 채널 콜리전이 없으면 트레이스가 막히지 않아 항상 1.0 이 나온다(라이브 검증 대상).
	float ComputeVisibleRatio(UWorld* World, const APTZCamera* Cam, const AActor* CarActor)
	{
		if (!World || !Cam || !CarActor) { return 0.f; }
		const FVector Eye = CameraOpticalTransform(Cam).GetLocation();
		const FBox Box = CarActor->GetComponentsBoundingBox(/*bNonColliding=*/true);
		if (!Box.IsValid) { return 0.f; }

		const FVector Cn = Box.GetCenter();
		const FVector Ex = Box.GetExtent();
		TArray<FVector, TInlineAllocator<15>> Samples;
		Samples.Add(Cn);
		for (int32 sx = -1; sx <= 1; sx += 2)
			for (int32 sy = -1; sy <= 1; sy += 2)
				for (int32 sz = -1; sz <= 1; sz += 2)
					Samples.Add(Cn + FVector(sx * Ex.X, sy * Ex.Y, sz * Ex.Z)); // 8 코너
		Samples.Add(Cn + FVector(+Ex.X, 0, 0)); Samples.Add(Cn + FVector(-Ex.X, 0, 0)); // 6 면중심
		Samples.Add(Cn + FVector(0, +Ex.Y, 0)); Samples.Add(Cn + FVector(0, -Ex.Y, 0));
		Samples.Add(Cn + FVector(0, 0, +Ex.Z)); Samples.Add(Cn + FVector(0, 0, -Ex.Z));

		FCollisionQueryParams Params(SCENE_QUERY_STAT(BaroVisibility), /*bTraceComplex=*/false);
		Params.AddIgnoredActor(CarActor);

		int32 Visible = 0;
		for (const FVector& P : Samples)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Eye, P, ECC_Visibility, Params)) { ++Visible; }
		}
		return Samples.Num() > 0 ? static_cast<float>(Visible) / static_cast<float>(Samples.Num()) : 0.f;
	}

	// 오버레이 투영에 필요한 것 중 "Hucoms 로 재현 불가능한" 부분만 노출한다 = 외부 파라미터(extrinsic).
	//   mount.location : 광학중심 월드 위치. 피벗이 Root 와 동일위치(레버암 0)라 pan/tilt 에 불변 = 고정.
	//   mount.baseYaw  : 설치 방위(pan=0 기준 yaw). 광학 yaw = baseYaw + CurrentPan 이므로 역산.
	//   wideHFovDeg    : 1x 수평 FOV(hfovFromZoomPos 스케일 기준).
	//   intrinsics     : 이 카메라의 BaseFOV로 스케일된 zoompos→HFOV 표와 해석 규칙.
	//   groundReference/heightAboveReferenceGroundCm : 슬롯 배치 원점 기반 기준면과 광학중심 높이.
	// FOV·PTZ 는 실카메라와 동일하게 Hucoms(getptzfpos + zoompos)에서 가져온다 → 여기서 중복 노출하지 않음.
	TSharedRef<FJsonObject> CameraToJson(const FCamEntry& E, const FGroundReference& Ground)
	{
		APTZCamera* C = E.Cam;
		const FTransform X = CameraOpticalTransform(C);
		const FVector Loc = X.GetLocation();
		const double BaseYaw = X.Rotator().Yaw - C->GetCurrentPan();

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), C->GetName());
		O->SetNumberField(TEXT("hucomsPort"), E.Http);
		O->SetNumberField(TEXT("mjpegPort"), E.Mjpeg);
		O->SetBoolField(TEXT("fixed"), C->bFixedMode);

		TSharedRef<FJsonObject> Mount = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetNumberField(TEXT("x"), Loc.X); L->SetNumberField(TEXT("y"), Loc.Y); L->SetNumberField(TEXT("z"), Loc.Z);
		Mount->SetObjectField(TEXT("location"), L);
		Mount->SetNumberField(TEXT("baseYaw"), BaseYaw);
		O->SetObjectField(TEXT("mount"), Mount);

		O->SetNumberField(TEXT("wideHFovDeg"), C->BaseFOV);

		// 시뮬 광학은 이상적 핀홀(rectilinear) — 왜곡 0, principal point = 프레임 중앙,
		// roll 0(설계상 팬/틸트 피벗이 롤을 만들지 않음). 소비자는 focal_px = 0.5*W / tan(hfov/2) 로 계산.
		O->SetStringField(TEXT("projection"), TEXT("pinhole"));
		O->SetField(TEXT("distortion"), MakeShared<FJsonValueNull>());
		O->SetNumberField(TEXT("rollDeg"), X.Rotator().Roll);

		TSharedRef<FJsonObject> Intrinsics = MakeShared<FJsonObject>();
		Intrinsics->SetStringField(TEXT("interpolation"), TEXT("linear"));
		Intrinsics->SetBoolField(TEXT("clamp"), true);
		TArray<TSharedPtr<FJsonValue>> ZoomHfov;
		ZoomHfov.Reserve(HucomsProtocol::ZoomHfovTableCount);
		for (const HucomsProtocol::FZoomHfovPoint& Point : HucomsProtocol::ZoomHfovTable)
		{
			TSharedRef<FJsonObject> Anchor = MakeShared<FJsonObject>();
			Anchor->SetNumberField(TEXT("zoomPos"), Point.ZoomPos);
			Anchor->SetNumberField(
				TEXT("hfovDeg"),
				HucomsProtocol::ZoomPosToHFov(Point.ZoomPos, C->BaseFOV));
			ZoomHfov.Add(MakeShared<FJsonValueObject>(Anchor));
		}
		Intrinsics->SetArrayField(TEXT("zoomHfov"), ZoomHfov);
		O->SetObjectField(TEXT("intrinsics"), Intrinsics);

		if (Ground.bValid)
		{
			TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
			Ref->SetNumberField(TEXT("zCm"), Ground.ZCm);
			Ref->SetStringField(TEXT("method"), TEXT("parkingSlotPlacementOriginMedian"));
			Ref->SetNumberField(TEXT("sampleCount"), Ground.SampleCount);
			Ref->SetNumberField(TEXT("maxDeviationCm"), Ground.MaxDeviationCm);
			O->SetObjectField(TEXT("groundReference"), Ref);
			O->SetNumberField(TEXT("heightAboveReferenceGroundCm"), Loc.Z - Ground.ZCm);
		}
		else
		{
			O->SetField(TEXT("groundReference"), MakeShared<FJsonValueNull>());
			O->SetField(TEXT("heightAboveReferenceGroundCm"), MakeShared<FJsonValueNull>());
		}
		return O;
	}

	// UE 실제 뷰·투영행렬로 월드점→픽셀(FSceneView::ProjectWorldToScreen 과 동일 매핑).
	TSharedRef<FJsonValue> ProjectPointJson(const FMatrix& VP, const FVector& P, int32 W, int32 H)
	{
		const FVector4 R = VP.TransformFVector4(FVector4(P, 1.f));
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		const bool bBehind = (R.W <= KINDA_SMALL_NUMBER);
		bool bVisible = false;
		double X = 0.0, Y = 0.0;
		if (!bBehind)
		{
			const double Nx = R.X / R.W, Ny = R.Y / R.W;
			X = (Nx * 0.5 + 0.5) * W;
			Y = (0.5 - Ny * 0.5) * H;                       // NDC Y up -> 픽셀 Y down
			bVisible = (X >= 0 && X <= W && Y >= 0 && Y <= H);
		}
		O->SetNumberField(TEXT("x"), X);
		O->SetNumberField(TEXT("y"), Y);
		O->SetBoolField(TEXT("visible"), bVisible);
		O->SetBoolField(TEXT("behind"), bBehind);
		return MakeShared<FJsonValueObject>(O);
	}
}

//======================================================================================
// Subsystem lifecycle
//======================================================================================
bool USceneControlSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USceneControlSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	StartServer();
}

void USceneControlSubsystem::Deinitialize()
{
	StopServer();
	Super::Deinitialize();
}

//======================================================================================
// HTTP server start/stop (월드 전역 1포트)
//======================================================================================
void USceneControlSubsystem::StartServer()
{
	if (bStarted) { return; }

	ResolveCarClass();   // 실패해도 서버는 뜬다(스폰 시 500 반환).

	FHttpServerModule& Http = FHttpServerModule::Get();
	Router = Http.GetHttpRouter(ScenePort, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		UE_LOG(LogSceneCtrl, Error, TEXT("[Scene] 라우터 바인드 실패 (port %d) — 포트 중복/점유 확인."), ScenePort);
		return;
	}

	auto Bind = [this](const TCHAR* Path, EHttpServerRequestVerbs Verbs,
		bool (USceneControlSubsystem::*Fn)(const FHttpServerRequest&, const FHttpResultCallback&))
	{
		FHttpRouteHandle H = Router->BindRoute(FHttpPath(FString(Path)), Verbs,
			FHttpRequestHandler::CreateLambda(
				[this, Fn](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
				{
					return (this->*Fn)(Req, OnComplete);
				}));
		if (H.IsValid()) { Routes.Add(H); }
		else { UE_LOG(LogSceneCtrl, Error, TEXT("[Scene] 라우트 바인딩 실패: %s"), Path); }
	};

	using EV = EHttpServerRequestVerbs;
	Bind(TEXT("/scene/catalog"),   EV::VERB_GET, &USceneControlSubsystem::HandleCatalog);
	Bind(TEXT("/scene/slots"),     EV::VERB_GET, &USceneControlSubsystem::HandleSlots);
	Bind(TEXT("/scene/cameras"),   EV::VERB_GET | EV::VERB_POST, &USceneControlSubsystem::HandleCameras);
	Bind(TEXT("/scene/cameras/:id"), EV::VERB_PATCH | EV::VERB_DELETE, &USceneControlSubsystem::HandleCameraById);
	Bind(TEXT("/scene/snapshot"),  EV::VERB_GET | EV::VERB_POST, &USceneControlSubsystem::HandleSnapshot);
	Bind(TEXT("/scene/project"),   EV::VERB_POST, &USceneControlSubsystem::HandleProject);
	Bind(TEXT("/scene/cars"),      EV::VERB_GET | EV::VERB_POST, &USceneControlSubsystem::HandleCars);
	Bind(TEXT("/scene/cars/:id"),  EV::VERB_GET | EV::VERB_PATCH | EV::VERB_DELETE, &USceneControlSubsystem::HandleCarById);
	Bind(TEXT("/scene/reset"),     EV::VERB_POST, &USceneControlSubsystem::HandleReset);
	// 자기서술 — 소스·문서 없이 접속한 에이전트가 포트 하나로 전체 계약을 발견한다.
	Bind(TEXT("/scene/help"),      EV::VERB_GET, &USceneControlSubsystem::HandleHelp);
	Bind(TEXT("/scene"),           EV::VERB_GET, &USceneControlSubsystem::HandleHelp);

	Http.StartAllListeners();
	bStarted = true;
	UE_LOG(LogSceneCtrl, Log, TEXT("[Scene] 씬 제어 서버 시작 — http://127.0.0.1:%d/scene/*"), ScenePort);
}

void USceneControlSubsystem::StopServer()
{
	// 스폰한 차량 정리(월드 종료 시).
	for (TPair<FString, FSimCarState>& Pair : Cars)
	{
		if (AActor* A = Pair.Value.Actor.Get()) { A->Destroy(); }
	}
	Cars.Reset();
	SlotOccupancy.Reset();

	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& H : Routes)
		{
			if (H.IsValid()) { Router->UnbindRoute(H); }
		}
	}
	Routes.Reset();
	Router.Reset();

	if (bStarted)
	{
		// StopAllListeners 는 전역(HucomsServerSubsystem 과 공유)이나 idempotent — 월드 종료 시 둘 다 정리된다.
		FHttpServerModule::Get().StopAllListeners();
		bStarted = false;
		UE_LOG(LogSceneCtrl, Log, TEXT("[Scene] 씬 제어 서버 정지."));
	}
}

//======================================================================================
// 스폰 / BP 반영
//======================================================================================
UClass* USceneControlSubsystem::ResolveCarClass()
{
	if (CarClass) { return CarClass; }
	UClass* Loaded = StaticLoadClass(AActor::StaticClass(), nullptr, *CarBlueprintPath);
	if (!Loaded)
	{
		UE_LOG(LogSceneCtrl, Error, TEXT("[Scene] 차량 BP 클래스 로드 실패: %s"), *CarBlueprintPath);
		return nullptr;
	}
	CarClass = Loaded;
	return CarClass;
}

const TArray<FString>& USceneControlSubsystem::GetCarAssetNames()
{
	if (!bCarAssetNamesResolved)
	{
		bCarAssetNamesResolved = true;
		// ResolveCarClass() 가 BP_Car 를 로드한다 — 첫 스폰 히치가 이 시점으로 앞당겨질 뿐이다.
		if (!CollectCarAssetNames(ResolveCarClass(), CarAssetNames))
		{
			UE_LOG(LogSceneCtrl, Warning, TEXT("[Scene] BP_Car.%s 읽기 실패 — 차종명 없이 인덱스만 노출한다."), CarMeshListProp);
		}
	}
	return CarAssetNames;
}

const TArray<FBox>& USceneControlSubsystem::GetCarBoundsCm()
{
	if (bCarBoundsResolved) { return CarBoundsCm; }
	bCarBoundsResolved = true;

	const TArray<FString>& Assets = GetCarAssetNames();
	CarBoundsCm.Init(FBox(ForceInit), Assets.Num());
	UWorld* World = GetWorld();
	UClass* Cls = ResolveCarClass();
	if (!World || !Cls || Assets.IsEmpty()) { return CarBoundsCm; }

	// 한 대만 transient로 만들고 차종을 순차 적용한다. 카탈로그 첫 요청에서만 발생하며,
	// 같은 요청 중 Destroy하므로 게임 상태·Cars/SlotOccupancy에는 들어가지 않는다.
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Probe = World->SpawnActor<AActor>(
		Cls, FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!Probe)
	{
		UE_LOG(LogSceneCtrl, Warning, TEXT("[Scene] 차량 bounds probe 스폰 실패."));
		return CarBoundsCm;
	}
	// 렌더 스레드가 다음 프레임을 준비해도 probe가 노출되지 않게 즉시 숨긴다.
	// bounds 필터는 owner hidden과 무관한 component IsVisible()을 사용해 원래 표시 상태를 보존한다.
	Probe->SetActorHiddenInGame(true);
	Probe->SetActorEnableCollision(false);
	Probe->SetActorTickEnabled(false);

	// 표시 메시 상태를 결정하는 모든 BP setter를 같은 값으로 통과시키는 canonical 상태.
	FSimCarState Canonical;
	Canonical.Color = 0;
	Canonical.PlateType = 0;
	Canonical.Prefix = TEXT("000");
	Canonical.Kor = TEXT("가");
	Canonical.Number = TEXT("0000");

	for (int32 i = 0; i < Assets.Num(); ++i)
	{
		Canonical.CarType = i;
		ApplyToActor(Probe, Canonical);
		CarBoundsCm[i] = CalculateRenderedMeshBounds(Probe);

		const FVector Size = CarBoundsCm[i].GetSize();
		if (!CarBoundsCm[i].IsValid || Size.ContainsNaN()
			|| Size.X <= KINDA_SMALL_NUMBER || Size.Y <= KINDA_SMALL_NUMBER || Size.Z <= KINDA_SMALL_NUMBER)
		{
			CarBoundsCm[i] = FBox(ForceInit);
			UE_LOG(
				LogSceneCtrl,
				Warning,
				TEXT("[Scene] 차량 bounds 계산 실패: carType=%d asset=%s"),
				i,
				*Assets[i]);
		}
	}

	Probe->Destroy();
	return CarBoundsCm;
}

int32 USceneControlSubsystem::ClampCarType(int32 InCarType)
{
	const int32 N = GetCarAssetNames().Num();
	return FMath::Clamp(InCarType, 0, (N > 0 ? N : LegacyCarCount) - 1);
}

AActor* USceneControlSubsystem::SpawnCarActor(const FTransform& Xform)
{
	UWorld* World = GetWorld();
	UClass* Cls = ResolveCarClass();
	if (!World || !Cls) { return nullptr; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AActor>(Cls, Xform.GetLocation(), Xform.Rotator(), Params);
}

void USceneControlSubsystem::ApplyToActor(AActor* Car, const FSimCarState& S)
{
	if (!Car) { return; }
	// selected_Color/selected_Plate 를 먼저 세팅하면 Change_Car(내부에서 Change_Color(selected_Color)+
	// Change_Plate(selected_Plate)+Update_Plate 호출)가 이 값을 반영한다. 이어 명시 호출로 확실히 재적용.
	SetIntProp(Car, TEXT("selected_Color"), S.Color);
	SetIntProp(Car, TEXT("selected_Plate"), S.PlateType);
	CallIntFn(Car, TEXT("Change_Car"), S.CarType);
	CallIntFn(Car, TEXT("Change_Color"), S.Color);
	CallIntFn(Car, TEXT("Change_Plate"), S.PlateType);
	// 번호판 글자. 한국 신형(앞3자리+한글+뒤4자리)으로 정규화된 S 를 "###가####" 로 연결해 넘긴다.
	// (BP_Plate::Change_Text 파싱: Num_3=sub(0,3), Kor=sub(3), Num_4=sub(4,4) — NormalizeKoreanPlate 참조.)
	const FString PlateStr = S.Prefix + S.Kor + S.Number;
	SetPlateTextOnChildren(Car, PlateStr);
}

//======================================================================================
// 라우트 핸들러
//======================================================================================
bool USceneControlSubsystem::HandleCatalog(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	FString Level;
	if (UWorld* W = GetWorld()) { Level = W->GetMapName(); Level.RemoveFromStart(W->StreamingLevelsPrefix); }
	O->SetStringField(TEXT("level"), Level);

	// 플러그인 버전 — .uplugin VersionName 을 단일 소스로 읽어 노출(웹 /simulator 에서 확인용).
	FString PluginVer = TEXT("?");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("baroCCTVSimulator"))) { PluginVer = Plugin->GetDescriptor().VersionName; }
	O->SetStringField(TEXT("pluginVersion"), PluginVer);

	// 차종: BP_Car.Mesh_List 를 읽어 index/name/asset/boundsCm 을 싣는다. carCount 는 그 길이(구 계약 유지).
	const TArray<FString>& CarAssets = GetCarAssetNames();
	const TArray<FBox>& CarBounds = GetCarBoundsCm();
	O->SetNumberField(TEXT("carCount"), CarAssets.Num() > 0 ? CarAssets.Num() : LegacyCarCount);

	TArray<TSharedPtr<FJsonValue>> CarsJson;
	for (int32 i = 0; i < CarAssets.Num(); ++i)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetNumberField(TEXT("index"), i);
		C->SetStringField(TEXT("name"), CarAssetToDisplayName(CarAssets[i]));
		C->SetStringField(TEXT("asset"), CarAssets[i]);
		C->SetStringField(TEXT("class"), CarAssetToClass(CarAssets[i]));
		if (CarBounds.IsValidIndex(i) && CarBounds[i].IsValid)
		{
			const FVector Center = CarBounds[i].GetCenter();
			const FVector Size = CarBounds[i].GetSize();
			TSharedRef<FJsonObject> CenterJson = MakeShared<FJsonObject>();
			CenterJson->SetNumberField(TEXT("x"), Center.X);
			CenterJson->SetNumberField(TEXT("y"), Center.Y);
			CenterJson->SetNumberField(TEXT("z"), Center.Z);
			TSharedRef<FJsonObject> SizeJson = MakeShared<FJsonObject>();
			SizeJson->SetNumberField(TEXT("x"), Size.X);
			SizeJson->SetNumberField(TEXT("y"), Size.Y);
			SizeJson->SetNumberField(TEXT("z"), Size.Z);
			TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
			BoundsJson->SetStringField(TEXT("coordinateSpace"), TEXT("actorLocal"));
			BoundsJson->SetObjectField(TEXT("center"), CenterJson);
			BoundsJson->SetObjectField(TEXT("size"), SizeJson);
			BoundsJson->SetStringField(TEXT("source"), TEXT("renderedMeshAggregate"));
			C->SetObjectField(TEXT("boundsCm"), BoundsJson);
		}
		else
		{
			C->SetField(TEXT("boundsCm"), MakeShared<FJsonValueNull>());
		}
		CarsJson.Add(MakeShared<FJsonValueObject>(C));
	}
	O->SetArrayField(TEXT("cars"), CarsJson);

	TArray<TSharedPtr<FJsonValue>> Colors;
	for (int32 i = 0; i < UE_ARRAY_COUNT(ColorDefs); ++i)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetNumberField(TEXT("index"), i);
		C->SetStringField(TEXT("name"), ColorDefs[i].Name);
		TArray<TSharedPtr<FJsonValue>> RGB = {
			MakeShared<FJsonValueNumber>(ColorDefs[i].R),
			MakeShared<FJsonValueNumber>(ColorDefs[i].G),
			MakeShared<FJsonValueNumber>(ColorDefs[i].B) };
		C->SetArrayField(TEXT("rgb"), RGB);
		Colors.Add(MakeShared<FJsonValueObject>(C));
	}
	O->SetArrayField(TEXT("colors"), Colors);

	TArray<TSharedPtr<FJsonValue>> Plates;
	for (int32 i = 0; i < UE_ARRAY_COUNT(PlateTypeNames); ++i)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetNumberField(TEXT("index"), i);
		P->SetStringField(TEXT("name"), PlateTypeNames[i]);
		Plates.Add(MakeShared<FJsonValueObject>(P));
	}
	O->SetArrayField(TEXT("plateTypes"), Plates);

	TArray<TSharedPtr<FJsonValue>> Kor;
	for (int32 i = 0; i < UE_ARRAY_COUNT(KorList); ++i) { Kor.Add(MakeShared<FJsonValueString>(KorList[i])); }
	O->SetArrayField(TEXT("korList"), Kor);

	OnComplete(JsonResp(O));
	return true;
}

bool USceneControlSubsystem::HandleSlots(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	TArray<FSlotInfo> Slots;
	CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FSlotInfo& S : Slots)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), S.Id);
		O->SetStringField(TEXT("label"), S.Label);
		O->SetStringField(TEXT("type"), S.Type);
		O->SetObjectField(TEXT("transform"), TransformToJson(S.Xform));
		const FString* CarId = SlotOccupancy.Find(S.Id);
		O->SetBoolField(TEXT("occupied"), CarId != nullptr);
		if (CarId) { O->SetStringField(TEXT("carId"), *CarId); }
		else { O->SetField(TEXT("carId"), MakeShared<FJsonValueNull>()); }
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("slots"), Arr);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleCameras(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// POST = 런타임 스폰(v0.1.13) — BEV 데이터셋용 카메라 포즈 다양화가 목적. 레벨 무수정.
	if (Req.Verb == EHttpServerRequestVerbs::VERB_POST)
	{
		TSharedPtr<FJsonObject> Body = ParseBody(Req);
		if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

		const TSharedPtr<FJsonObject>* LocObj;
		if (!Body->TryGetObjectField(TEXT("location"), LocObj))
		{
			OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("location {x,y,z} 필요 (광학중심 월드 cm)")));
			return true;
		}
		FPTZCameraSpawnSpec Spec;
		{
			double x = 0, y = 0, z = 0;
			(*LocObj)->TryGetNumberField(TEXT("x"), x); (*LocObj)->TryGetNumberField(TEXT("y"), y); (*LocObj)->TryGetNumberField(TEXT("z"), z);
			Spec.Location = FVector(x, y, z);
		}
		double Yaw = 0.0, Pitch = -20.0;   // pitch 기본 -20 = 하향 20도(config 스포너와 동일 기본)
		Body->TryGetNumberField(TEXT("yawDeg"), Yaw);
		Body->TryGetNumberField(TEXT("pitchDeg"), Pitch);
		Spec.YawDeg = static_cast<float>(Yaw);
		Spec.PitchDeg = static_cast<float>(Pitch);
		int32 HttpPort = 0, MjpegPort = 0;
		Body->TryGetNumberField(TEXT("httpPort"), HttpPort);
		Body->TryGetNumberField(TEXT("mjpegPort"), MjpegPort);
		Spec.HttpPort = HttpPort;
		Spec.MjpegPort = MjpegPort;
		if (HttpPort == ScenePort || MjpegPort == ScenePort)
		{
			OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, FString::Printf(TEXT("씬 제어 포트(%d)와 충돌"), ScenePort)));
			return true;
		}
		bool bFixed = false; Body->TryGetBoolField(TEXT("fixed"), bFixed);
		Spec.bFixedMode = bFixed;
		Body->TryGetStringField(TEXT("note"), Spec.Note);

		UHucomsServerSubsystem* Hu = GetWorld() ? GetWorld()->GetSubsystem<UHucomsServerSubsystem>() : nullptr;
		if (!Hu) { OnComplete(JsonError(EHttpServerResponseCodes::ServerError, TEXT("Hucoms 서브시스템 없음"))); return true; }
		FString Err;
		APTZCamera* Cam = Hu->SpawnCameraRuntime(Spec, Err);
		if (!Cam) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, Err)); return true; }

		TArray<FSlotInfo> Slots;
		CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
		FCamEntry E; E.Cam = Cam; E.Http = Spec.HttpPort; E.Mjpeg = Spec.MjpegPort;
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("camera"), CameraToJson(E, CalculateGroundReference(Slots)));
		OnComplete(JsonResp(Root));
		return true;
	}

	TArray<FCamEntry> Cams;
	CollectCameras(GetWorld(), Cams);
	TArray<FSlotInfo> Slots;
	CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
	const FGroundReference Ground = CalculateGroundReference(Slots);
	UHucomsServerSubsystem* Hu = GetWorld() ? GetWorld()->GetSubsystem<UHucomsServerSubsystem>() : nullptr;

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FCamEntry& E : Cams)
	{
		TSharedRef<FJsonObject> O = CameraToJson(E, Ground);
		// 이동/삭제 가능 여부(스폰 카메라 vs 레벨 저작)를 목록에서 바로 알게 한다.
		O->SetBoolField(TEXT("spawned"), Hu && Hu->IsSpawnedCamera(E.Cam));
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("cameras"), Arr);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleCameraById(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Id = Req.PathParams.FindRef(TEXT("id"));
	TArray<FCamEntry> Cams;
	CollectCameras(GetWorld(), Cams);
	const int32 IdAsPort = FCString::Atoi(*Id);   // 숫자면 hucomsPort 로도 매칭(devices 조인 키와 동일)
	const FCamEntry* Found = Cams.FindByPredicate([&](const FCamEntry& E)
	{
		return E.Cam->GetName() == Id || (IdAsPort > 0 && E.Http == IdAsPort);
	});
	if (!Found)
	{
		OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("카메라 없음: %s"), *Id)));
		return true;
	}
	UHucomsServerSubsystem* Hu = GetWorld() ? GetWorld()->GetSubsystem<UHucomsServerSubsystem>() : nullptr;
	if (!Hu) { OnComplete(JsonError(EHttpServerResponseCodes::ServerError, TEXT("Hucoms 서브시스템 없음"))); return true; }

	if (Req.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		const FString Name = Found->Cam->GetName();
		FString Err;
		if (!Hu->RemoveCameraRuntime(Found->Cam, Err))
		{
			OnComplete(JsonError(EHttpServerResponseCodes::Forbidden, Err));
			return true;
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("removed"), Name);
		OnComplete(JsonResp(Root));
		return true;
	}

	// PATCH = 설치 자세 갱신(넘긴 필드만): location(cm) / yawDeg(설치 방위) / pitchDeg(설치 하향각→tilt).
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	FVector Loc; bool bHasLoc = false;
	const TSharedPtr<FJsonObject>* LocObj;
	if (Body->TryGetObjectField(TEXT("location"), LocObj))
	{
		double x = 0, y = 0, z = 0;
		(*LocObj)->TryGetNumberField(TEXT("x"), x); (*LocObj)->TryGetNumberField(TEXT("y"), y); (*LocObj)->TryGetNumberField(TEXT("z"), z);
		Loc = FVector(x, y, z); bHasLoc = true;
	}
	double YawD = 0.0, PitchD = 0.0;
	const bool bHasYaw = Body->TryGetNumberField(TEXT("yawDeg"), YawD);
	const bool bHasPitch = Body->TryGetNumberField(TEXT("pitchDeg"), PitchD);
	if (!bHasLoc && !bHasYaw && !bHasPitch)
	{
		OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("갱신할 필드 없음 (location/yawDeg/pitchDeg)")));
		return true;
	}
	const float Yaw = static_cast<float>(YawD), Pitch = static_cast<float>(PitchD);
	FString Err;
	if (!Hu->UpdateCameraPose(Found->Cam, bHasLoc ? &Loc : nullptr, bHasYaw ? &Yaw : nullptr, bHasPitch ? &Pitch : nullptr, Err))
	{
		OnComplete(JsonError(EHttpServerResponseCodes::Forbidden, Err));
		return true;
	}

	TArray<FSlotInfo> Slots;
	CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> CamJson = CameraToJson(*Found, CalculateGroundReference(Slots));
	CamJson->SetBoolField(TEXT("spawned"), true);
	Root->SetObjectField(TEXT("camera"), CamJson);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleProject(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	// 카메라 선택: cameraId(액터명) 우선, 없으면 hucomsPort. 둘 다 없으면 첫 카메라.
	FString CamId; Body->TryGetStringField(TEXT("cameraId"), CamId);
	int32 WantPort = 0; const bool bHasPort = Body->TryGetNumberField(TEXT("hucomsPort"), WantPort);

	TArray<FCamEntry> Cams;
	CollectCameras(GetWorld(), Cams);
	const FCamEntry* Found = Cams.FindByPredicate([&](const FCamEntry& E)
	{
		if (!CamId.IsEmpty()) { return E.Cam->GetName() == CamId; }
		if (bHasPort) { return E.Http == WantPort; }
		return false;
	});
	if (!Found && Cams.Num() > 0 && CamId.IsEmpty() && !bHasPort) { Found = &Cams[0]; }
	if (!Found) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, TEXT("카메라를 찾을 수 없습니다(cameraId/hucomsPort)"))); return true; }

	// 해상도(선택) — 없으면 논리 1920x1080. 수평 FOV + 종횡비로 뷰·투영행렬을 구성한다.
	int32 W = 1920, H = 1080;
	const TSharedPtr<FJsonObject>* ResObj;
	if (Body->TryGetObjectField(TEXT("resolution"), ResObj))
	{
		int32 rw = 0, rh = 0;
		if ((*ResObj)->TryGetNumberField(TEXT("width"), rw) && rw > 0) { W = rw; }
		if ((*ResObj)->TryGetNumberField(TEXT("height"), rh) && rh > 0) { H = rh; }
	}

	APTZCamera* Cam = Found->Cam;
	FMinimalViewInfo View;
	const FTransform X = CameraOpticalTransform(Cam);
	View.Location = X.GetLocation();
	View.Rotation = X.Rotator();
	View.FOV = Cam->GetCurrentFOV();                     // 수평 FOV
	View.AspectRatio = (float)W / (float)H;
	View.bConstrainAspectRatio = true;
	View.ProjectionMode = ECameraProjectionMode::Perspective;

	FMatrix ViewM, ProjM, VP;
	UGameplayStatics::GetViewProjectionMatrix(View, ViewM, ProjM, VP);

	TArray<TSharedPtr<FJsonValue>> OutPts;
	const TArray<TSharedPtr<FJsonValue>>* Pts;
	if (Body->TryGetArrayField(TEXT("points"), Pts))
	{
		for (const TSharedPtr<FJsonValue>& V : *Pts)
		{
			const TSharedPtr<FJsonObject> PO = V->AsObject();
			if (!PO.IsValid()) { continue; }
			double x = 0, y = 0, z = 0;
			PO->TryGetNumberField(TEXT("x"), x); PO->TryGetNumberField(TEXT("y"), y); PO->TryGetNumberField(TEXT("z"), z);
			OutPts.Add(ProjectPointJson(VP, FVector(x, y, z), W, H));
		}
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("cameraId"), Cam->GetName());
	Root->SetNumberField(TEXT("fovDeg"), Cam->GetCurrentFOV());
	TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
	Res->SetNumberField(TEXT("width"), W); Res->SetNumberField(TEXT("height"), H);
	Root->SetObjectField(TEXT("resolution"), Res);
	Root->SetArrayField(TEXT("points"), OutPts);
	OnComplete(JsonResp(Root));
	return true;
}

void USceneControlSubsystem::EvictSlotOccupant(const FString& SlotId)
{
	// force 덮어쓰기 전에 대상 슬롯의 기존 점유 차량을 완전히 제거한다.
	// 이걸 생략하면 SpawnCarActor(AlwaysSpawn)가 옛 차 위에 새 차를 겹쳐 스폰하고,
	// SlotOccupancy 값만 새 carId 로 바뀌어 옛 차가 유령처럼 남는다(옛 차 DELETE 시 새 차 점유가 지워지는 교차 오염도 유발).
	const FString* OccIdPtr = SlotOccupancy.Find(SlotId);
	if (!OccIdPtr) { return; }
	const FString OccId = *OccIdPtr;              // 복사 — 아래 Remove 가 이 포인터를 무효화한다.
	if (FSimCarState* Occ = Cars.Find(OccId))
	{
		if (AActor* A = Occ->Actor.Get()) { A->Destroy(); }
		Cars.Remove(OccId);                       // 다른 키 제거는 TMap 의 타 원소 포인터를 무효화하지 않는다(PATCH 의 S 안전).
	}
	SlotOccupancy.Remove(SlotId);                 // 매핑도 정리(점유 차량이 외부에서 이미 사라진 경우까지 self-heal).
}

bool USceneControlSubsystem::HandleCars(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// GET = 목록. 선택: ?visibility=<cameraId|hucomsPort> 이면 각 차량에 그 카메라 기준 visibleRatio(0..1) 를 실는다.
	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		const FString* VisParam = Req.QueryParams.Find(TEXT("visibility"));
		const bool bWantVis = VisParam && !VisParam->IsEmpty();
		APTZCamera* VisCam = nullptr;
		if (bWantVis)
		{
			TArray<FCamEntry> Cams; CollectCameras(GetWorld(), Cams);
			const bool bNumeric = VisParam->IsNumeric();
			const int32 WantPort = bNumeric ? FCString::Atoi(**VisParam) : 0;
			for (const FCamEntry& E : Cams)
			{
				if ((bNumeric && E.Http == WantPort) || (!bNumeric && E.Cam && E.Cam->GetName() == *VisParam))
				{ VisCam = E.Cam; break; }
			}
			if (!VisCam) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("카메라 없음: %s"), **VisParam))); return true; }
		}

		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TPair<FString, FSimCarState>& Pair : Cars)
		{
			TSharedRef<FJsonObject> CarObj = CarToJson(Pair.Value);
			if (VisCam)
			{
				CarObj->SetNumberField(TEXT("visibleRatio"),
					ComputeVisibleRatio(GetWorld(), VisCam, Pair.Value.Actor.Get()));
			}
			Arr.Add(MakeShared<FJsonValueObject>(CarObj));
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("cars"), Arr);
		if (VisCam) { Root->SetStringField(TEXT("visibilityCamera"), VisCam->GetName()); }
		OnComplete(JsonResp(Root));
		return true;
	}

	// POST = 스폰
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	FSimCarState S;
	int32 CarType = 0, Color = 0;
	Body->TryGetNumberField(TEXT("carType"), CarType);
	Body->TryGetNumberField(TEXT("color"), Color);
	S.CarType = ClampCarType(CarType);
	S.Color = FMath::Clamp(Color, 0, 7);

	const TSharedPtr<FJsonObject>* PlateObj;
	if (Body->TryGetObjectField(TEXT("plate"), PlateObj))
	{
		int32 PT = 0; (*PlateObj)->TryGetNumberField(TEXT("type"), PT); S.PlateType = FMath::Clamp(PT, 0, 2);
		(*PlateObj)->TryGetStringField(TEXT("city"), S.City);
		(*PlateObj)->TryGetStringField(TEXT("prefix"), S.Prefix);
		(*PlateObj)->TryGetStringField(TEXT("kor"), S.Kor);
		(*PlateObj)->TryGetStringField(TEXT("number"), S.Number);
	}

	bool bForce = false; Body->TryGetBoolField(TEXT("force"), bForce);

	// 배치 기준: slotId(주차면 트랜스폼) 또는 transform(자유 좌표) 중 하나.
	// 둘 다 오면 어느 쪽을 버릴지 서버가 정할 근거가 없으므로 거절한다 — 주차면 기준 변형은 offset 이다.
	FString SlotId;
	Body->TryGetStringField(TEXT("slotId"), SlotId);
	const TSharedPtr<FJsonObject>* TObj = nullptr;
	const bool bHasTransform = Body->TryGetObjectField(TEXT("transform"), TObj);
	if (!SlotId.IsEmpty() && bHasTransform)
	{
		OnComplete(JsonError(EHttpServerResponseCodes::BadRequest,
			TEXT("slotId 와 transform 은 동시에 지정할 수 없음 (주차면 기준 변형은 offset)")));
		return true;
	}

	if (!SlotId.IsEmpty())
	{
		TArray<FSlotInfo> Slots; CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
		const FSlotInfo* Found = Slots.FindByPredicate([&](const FSlotInfo& X) { return X.Id == SlotId; });
		if (!Found) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("주차면 없음: %s"), *SlotId))); return true; }
		if (SlotOccupancy.Contains(SlotId))
		{
			if (!bForce) { OnComplete(JsonError(EHttpServerResponseCodes::Conflict, FString::Printf(TEXT("주차면 점유됨: %s"), *SlotId))); return true; }
			EvictSlotOccupant(SlotId);   // force = 기존 점유 차량 파괴 후 덮어쓰기(겹쳐 스폰 방지).
		}
		S.BaseTransform = Found->Xform;
		S.SlotId = SlotId;
	}
	else if (bHasTransform)
	{
		S.BaseTransform = ParseTransform(*TObj);
	}
	else
	{
		OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("slotId 또는 transform 필요"))); return true;
	}

	// 기준 로컬 변형(선택). 없으면 항등 = 예전과 동일한 배치.
	const TSharedPtr<FJsonObject>* OffObj;
	if (Body->TryGetObjectField(TEXT("offset"), OffObj)) { ParsePlacement(*OffObj, S.OffsetLocation, S.OffsetRotation); }
	RecomposePlacement(S);

	// 스폰은 AlwaysSpawn 이라 변형으로 옆 차와 겹쳐도 밀려나지 않는다 — 준 좌표에 그대로 선다.
	AActor* Car = SpawnCarActor(S.Transform);
	if (!Car) { OnComplete(JsonError(EHttpServerResponseCodes::ServerError, TEXT("차량 스폰 실패 (BP 로드 확인)"))); return true; }

	S.Id = FString::Printf(TEXT("car-%02d"), ++CarSeq);
	S.Actor = Car;
	NormalizeKoreanPlate(S);
	ApplyToActor(Car, S);

	Cars.Add(S.Id, S);
	if (!S.SlotId.IsEmpty()) { SlotOccupancy.Add(S.SlotId, S.Id); }

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("car"), CarToJson(S));
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleCarById(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	const FString Id = Req.PathParams.FindRef(TEXT("id"));
	FSimCarState* S = Cars.Find(Id);
	if (!S) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("차량 없음: %s"), *Id))); return true; }

	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("car"), CarToJson(*S));
		OnComplete(JsonResp(Root));
		return true;
	}

	if (Req.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		if (AActor* A = S->Actor.Get()) { A->Destroy(); }
		if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); }
		Cars.Remove(Id);
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("removed"), Id);
		OnComplete(JsonResp(Root));
		return true;
	}

	// PATCH = 부분 갱신(넘긴 필드만). S 는 TMap 슬롯 포인터 — Remove 를 안 하므로 유효.
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }

	int32 Tmp;
	if (Body->TryGetNumberField(TEXT("carType"), Tmp)) { S->CarType = ClampCarType(Tmp); }
	if (Body->TryGetNumberField(TEXT("color"), Tmp)) { S->Color = FMath::Clamp(Tmp, 0, 7); }
	const TSharedPtr<FJsonObject>* PlateObj;
	if (Body->TryGetObjectField(TEXT("plate"), PlateObj))
	{
		int32 PT; if ((*PlateObj)->TryGetNumberField(TEXT("type"), PT)) { S->PlateType = FMath::Clamp(PT, 0, 2); }
		FString Str;
		if ((*PlateObj)->TryGetStringField(TEXT("city"), Str)) { S->City = Str; }
		if ((*PlateObj)->TryGetStringField(TEXT("prefix"), Str)) { S->Prefix = Str; }
		if ((*PlateObj)->TryGetStringField(TEXT("kor"), Str)) { S->Kor = Str; }
		if ((*PlateObj)->TryGetStringField(TEXT("number"), Str)) { S->Number = Str; }
	}

	// 배치 갱신 — 셋 다 "값"이지 누적 델타가 아니다(같은 offset 을 두 번 보내도 결과가 같다).
	//   slotId    : 주차면 이동(기준 교체, offset 은 따라간다) / "" = 주차면에서 분리(현 위치 유지)
	//   transform : 자유 좌표로 기준 교체(붙어 있던 주차면은 점유 해제)
	//   offset    : 기준 로컬 변형 교체
	FString NewSlot;
	const bool bHasSlot = Body->TryGetStringField(TEXT("slotId"), NewSlot);
	const TSharedPtr<FJsonObject>* TObj = nullptr;
	const bool bHasTransform = Body->TryGetObjectField(TEXT("transform"), TObj);
	if (bHasSlot && !NewSlot.IsEmpty() && bHasTransform)
	{
		OnComplete(JsonError(EHttpServerResponseCodes::BadRequest,
			TEXT("slotId 와 transform 은 동시에 지정할 수 없음 (주차면 기준 변형은 offset)")));
		return true;
	}

	bool bPlacementDirty = false;
	if (bHasSlot && NewSlot != S->SlotId)
	{
		bool bForce = false; Body->TryGetBoolField(TEXT("force"), bForce);
		if (!NewSlot.IsEmpty())
		{
			TArray<FSlotInfo> Slots; CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
			const FSlotInfo* Found = Slots.FindByPredicate([&](const FSlotInfo& X) { return X.Id == NewSlot; });
			if (!Found) { OnComplete(JsonError(EHttpServerResponseCodes::NotFound, FString::Printf(TEXT("주차면 없음: %s"), *NewSlot))); return true; }
			if (SlotOccupancy.Contains(NewSlot))
			{
				if (!bForce) { OnComplete(JsonError(EHttpServerResponseCodes::Conflict, FString::Printf(TEXT("주차면 점유됨: %s"), *NewSlot))); return true; }
				EvictSlotOccupant(NewSlot);   // force = 대상 슬롯의 기존 차량 파괴(옮겨가는 차와 항상 다른 차 → S 안전).
			}
			if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); }
			S->SlotId = NewSlot;
			S->BaseTransform = Found->Xform;
			SlotOccupancy.Add(NewSlot, Id);
			bPlacementDirty = true;
		}
		else
		{
			// 분리는 점유만 푼다 — 기준도 배치도 그대로라 차는 있던 자리에 서 있는다.
			if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); }
			S->SlotId = TEXT("");
		}
	}

	if (bHasTransform)
	{
		if (!S->SlotId.IsEmpty()) { SlotOccupancy.Remove(S->SlotId); S->SlotId = TEXT(""); }
		S->BaseTransform = ParseTransform(*TObj);
		bPlacementDirty = true;
	}

	const TSharedPtr<FJsonObject>* OffObj;
	if (Body->TryGetObjectField(TEXT("offset"), OffObj))
	{
		ParsePlacement(*OffObj, S->OffsetLocation, S->OffsetRotation);
		bPlacementDirty = true;
	}

	if (bPlacementDirty)
	{
		RecomposePlacement(*S);
		if (AActor* A = S->Actor.Get()) { A->SetActorTransform(S->Transform); }
	}

	NormalizeKoreanPlate(*S);
	if (AActor* A = S->Actor.Get()) { ApplyToActor(A, *S); }

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("car"), CarToJson(*S));
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleReset(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	const int32 N = Cars.Num();
	for (TPair<FString, FSimCarState>& Pair : Cars)
	{
		if (AActor* A = Pair.Value.Actor.Get()) { A->Destroy(); }
	}
	Cars.Reset();
	SlotOccupancy.Reset();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("cleared"), N);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::RestoreCarFromJson(const TSharedPtr<FJsonObject>& CarObj, FString& OutError)
{
	FSimCarState S;
	int32 CarType = 0, Color = 0;
	CarObj->TryGetNumberField(TEXT("carType"), CarType);
	CarObj->TryGetNumberField(TEXT("color"), Color);
	S.CarType = ClampCarType(CarType);
	S.Color = FMath::Clamp(Color, 0, 7);

	const TSharedPtr<FJsonObject>* PlateObj;
	if (CarObj->TryGetObjectField(TEXT("plate"), PlateObj))
	{
		int32 PT = 0; (*PlateObj)->TryGetNumberField(TEXT("type"), PT); S.PlateType = FMath::Clamp(PT, 0, 2);
		(*PlateObj)->TryGetStringField(TEXT("city"), S.City);
		(*PlateObj)->TryGetStringField(TEXT("prefix"), S.Prefix);
		(*PlateObj)->TryGetStringField(TEXT("kor"), S.Kor);
		(*PlateObj)->TryGetStringField(TEXT("number"), S.Number);
	}

	const TSharedPtr<FJsonObject>* OffObj;
	if (CarObj->TryGetObjectField(TEXT("offset"), OffObj)) { ParsePlacement(*OffObj, S.OffsetLocation, S.OffsetRotation); }

	// 기준: slotId(주차면) 우선, 자유 배치는 baseTransform(스냅샷 GET 이 슬롯 미배치 차량에만 실어 준다).
	// 응답의 transform(합성 결과)을 기준으로 쓰면 offset 이 두 번 적용되므로 읽지 않는다.
	FString SlotId;
	CarObj->TryGetStringField(TEXT("slotId"), SlotId);   // JSON null 이면 실패 → 빈 문자열 유지
	if (!SlotId.IsEmpty())
	{
		TArray<FSlotInfo> Slots; CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
		const FSlotInfo* FoundSlot = Slots.FindByPredicate([&](const FSlotInfo& X) { return X.Id == SlotId; });
		if (!FoundSlot) { OutError = FString::Printf(TEXT("주차면 없음: %s"), *SlotId); return false; }
		if (SlotOccupancy.Contains(SlotId)) { OutError = FString::Printf(TEXT("주차면 중복(스냅샷 안에 두 번): %s"), *SlotId); return false; }
		S.BaseTransform = FoundSlot->Xform;
		S.SlotId = SlotId;
	}
	else
	{
		const TSharedPtr<FJsonObject>* BaseObj;
		if (CarObj->TryGetObjectField(TEXT("baseTransform"), BaseObj)) { S.BaseTransform = ParseTransform(*BaseObj); }
		else { OutError = TEXT("slotId 도 baseTransform 도 없음"); return false; }
	}
	RecomposePlacement(S);

	AActor* Car = SpawnCarActor(S.Transform);
	if (!Car) { OutError = TEXT("차량 스폰 실패 (BP 로드 확인)"); return false; }
	S.Id = FString::Printf(TEXT("car-%02d"), ++CarSeq);
	S.Actor = Car;
	NormalizeKoreanPlate(S);
	ApplyToActor(Car, S);
	Cars.Add(S.Id, S);
	if (!S.SlotId.IsEmpty()) { SlotOccupancy.Add(S.SlotId, S.Id); }
	return true;
}

bool USceneControlSubsystem::HandleSnapshot(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// 씬 스냅샷(v0.1.13) — "살아있는 월드가 진실, 저장은 호출자" 철학은 유지한다: 서버는 파일을
	// 남기지 않고, GET 이 복원 가능한 JSON 을 주고 POST 가 그 JSON 을 그대로 받는다.
	// 범위: API 스폰 차량 전수 + 스폰 카메라(자동/config/API)의 설치 스펙. 레벨 저작 액터와
	// 현재 PTZ 상태(Hucoms 축)는 범위 밖이다. 차량 id 는 보존되지 않는다(순번 재부여).
	UHucomsServerSubsystem* Hu = GetWorld() ? GetWorld()->GetSubsystem<UHucomsServerSubsystem>() : nullptr;
	FString Level;
	if (UWorld* W = GetWorld()) { Level = W->GetMapName(); Level.RemoveFromStart(W->StreamingLevelsPrefix); }

	if (Req.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("level"), Level);
		FString PluginVer = TEXT("?");
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("baroCCTVSimulator"))) { PluginVer = Plugin->GetDescriptor().VersionName; }
		Root->SetStringField(TEXT("pluginVersion"), PluginVer);
		Root->SetStringField(TEXT("savedAtUtc"), FDateTime::UtcNow().ToIso8601());

		TArray<TSharedPtr<FJsonValue>> CarArr;
		for (TPair<FString, FSimCarState>& Pair : Cars)
		{
			TSharedRef<FJsonObject> O = CarToJson(Pair.Value);
			// 자유 배치 차량은 기준을 따로 실어야 복원이 정확하다(transform 은 offset 이 이미 합성된 값).
			if (Pair.Value.SlotId.IsEmpty()) { O->SetObjectField(TEXT("baseTransform"), TransformToJson(Pair.Value.BaseTransform)); }
			CarArr.Add(MakeShared<FJsonValueObject>(O));
		}
		Root->SetArrayField(TEXT("cars"), CarArr);

		TArray<TSharedPtr<FJsonValue>> CamArr;
		if (Hu)
		{
			TArray<FPTZCameraSpawnSpec> Specs; TArray<APTZCamera*> SpecCams;
			Hu->GetSpawnedCameraSpecs(Specs, &SpecCams);
			for (int32 i = 0; i < Specs.Num(); ++i)
			{
				TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("id"), SpecCams.IsValidIndex(i) ? SpecCams[i]->GetName() : TEXT(""));
				TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
				L->SetNumberField(TEXT("x"), Specs[i].Location.X);
				L->SetNumberField(TEXT("y"), Specs[i].Location.Y);
				L->SetNumberField(TEXT("z"), Specs[i].Location.Z);
				O->SetObjectField(TEXT("location"), L);
				O->SetNumberField(TEXT("yawDeg"), Specs[i].YawDeg);
				O->SetNumberField(TEXT("pitchDeg"), Specs[i].PitchDeg);
				O->SetNumberField(TEXT("httpPort"), Specs[i].HttpPort);
				O->SetNumberField(TEXT("mjpegPort"), Specs[i].MjpegPort);
				O->SetBoolField(TEXT("fixed"), Specs[i].bFixedMode);
				CamArr.Add(MakeShared<FJsonValueObject>(O));
			}
		}
		Root->SetArrayField(TEXT("cameras"), CamArr);
		OnComplete(JsonResp(Root));
		return true;
	}

	// POST = 복원. cars 는 전량 리셋 후 재배치, cameras 는 httpPort 를 키로 reconcile
	// (같은 포트 = 이동, 없는 포트 = 스폰, 스냅샷에 없는 스폰 카메라 = 제거 — 포트 재바인드 최소화).
	TSharedPtr<FJsonObject> Body = ParseBody(Req);
	if (!Body.IsValid()) { OnComplete(JsonError(EHttpServerResponseCodes::BadRequest, TEXT("JSON 파싱 실패"))); return true; }
	bool bForce = false; Body->TryGetBoolField(TEXT("force"), bForce);
	FString SnapLevel;
	if (Body->TryGetStringField(TEXT("level"), SnapLevel) && !SnapLevel.IsEmpty() && SnapLevel != Level && !bForce)
	{
		OnComplete(JsonError(EHttpServerResponseCodes::Conflict,
			FString::Printf(TEXT("스냅샷 레벨 불일치(%s ≠ %s) — 다른 월드의 좌표. 강행은 force:true"), *SnapLevel, *Level)));
		return true;
	}

	int32 CamSpawnedN = 0, CamMovedN = 0, CamRemovedN = 0;
	TArray<FString> Failures;
	if (Hu)
	{
		// 원하는 카메라 목록 파싱
		TArray<FPTZCameraSpawnSpec> Desired;
		const TArray<TSharedPtr<FJsonValue>>* CamArr;
		if (Body->TryGetArrayField(TEXT("cameras"), CamArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *CamArr)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				if (!O.IsValid()) { continue; }
				FPTZCameraSpawnSpec D;
				const TSharedPtr<FJsonObject>* LocObj;
				if (O->TryGetObjectField(TEXT("location"), LocObj))
				{
					double x = 0, y = 0, z = 0;
					(*LocObj)->TryGetNumberField(TEXT("x"), x); (*LocObj)->TryGetNumberField(TEXT("y"), y); (*LocObj)->TryGetNumberField(TEXT("z"), z);
					D.Location = FVector(x, y, z);
				}
				double Yaw = 0.0, Pitch = -20.0;
				O->TryGetNumberField(TEXT("yawDeg"), Yaw); O->TryGetNumberField(TEXT("pitchDeg"), Pitch);
				D.YawDeg = static_cast<float>(Yaw); D.PitchDeg = static_cast<float>(Pitch);
				O->TryGetNumberField(TEXT("httpPort"), D.HttpPort);
				O->TryGetNumberField(TEXT("mjpegPort"), D.MjpegPort);
				bool bFx = false; O->TryGetBoolField(TEXT("fixed"), bFx); D.bFixedMode = bFx;
				Desired.Add(D);
			}
		}

		// 1) 스냅샷에 없는(또는 mjpeg/fixed 가 달라진) 현재 스폰 카메라 제거
		{
			TArray<FPTZCameraSpawnSpec> CurSpecs; TArray<APTZCamera*> CurCams;
			Hu->GetSpawnedCameraSpecs(CurSpecs, &CurCams);
			for (int32 i = 0; i < CurSpecs.Num(); ++i)
			{
				const FPTZCameraSpawnSpec* D = Desired.FindByPredicate(
					[&](const FPTZCameraSpawnSpec& X) { return X.HttpPort == CurSpecs[i].HttpPort; });
				const bool bKeep = D && D->MjpegPort == CurSpecs[i].MjpegPort && D->bFixedMode == CurSpecs[i].bFixedMode;
				if (!bKeep && CurCams.IsValidIndex(i))
				{
					FString Err;
					if (Hu->RemoveCameraRuntime(CurCams[i], Err)) { ++CamRemovedN; }
					else { Failures.Add(FString::Printf(TEXT("카메라 제거 실패: %s"), *Err)); }
				}
			}
		}
		// 2) 원하는 목록을 이동(기존 포트) 또는 스폰(새 포트)
		for (const FPTZCameraSpawnSpec& D : Desired)
		{
			TArray<FPTZCameraSpawnSpec> NowSpecs; TArray<APTZCamera*> NowCams;
			Hu->GetSpawnedCameraSpecs(NowSpecs, &NowCams);
			const int32 Idx = NowSpecs.IndexOfByPredicate(
				[&](const FPTZCameraSpawnSpec& X) { return X.HttpPort == D.HttpPort; });
			FString Err;
			if (Idx != INDEX_NONE && NowCams.IsValidIndex(Idx))
			{
				const float Yaw = D.YawDeg, Pitch = D.PitchDeg;
				if (Hu->UpdateCameraPose(NowCams[Idx], &D.Location, &Yaw, &Pitch, Err)) { ++CamMovedN; }
				else { Failures.Add(FString::Printf(TEXT("카메라 이동 실패(:%d): %s"), D.HttpPort, *Err)); }
			}
			else
			{
				if (Hu->SpawnCameraRuntime(D, Err)) { ++CamSpawnedN; }
				else { Failures.Add(FString::Printf(TEXT("카메라 스폰 실패(:%d): %s"), D.HttpPort, *Err)); }
			}
		}
	}

	// 3) 차량: 전량 리셋 후 스냅샷 순서대로 재배치
	for (TPair<FString, FSimCarState>& Pair : Cars)
	{
		if (AActor* A = Pair.Value.Actor.Get()) { A->Destroy(); }
	}
	Cars.Reset();
	SlotOccupancy.Reset();
	int32 CarsRestoredN = 0;
	const TArray<TSharedPtr<FJsonValue>>* CarArr;
	if (Body->TryGetArrayField(TEXT("cars"), CarArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *CarArr)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) { continue; }
			FString Err;
			if (RestoreCarFromJson(O, Err)) { ++CarsRestoredN; }
			else { Failures.Add(FString::Printf(TEXT("차량 복원 실패: %s"), *Err)); }
		}
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("level"), Level);
	TSharedRef<FJsonObject> CarsR = MakeShared<FJsonObject>();
	CarsR->SetNumberField(TEXT("restored"), CarsRestoredN);
	Root->SetObjectField(TEXT("cars"), CarsR);
	TSharedRef<FJsonObject> CamsR = MakeShared<FJsonObject>();
	CamsR->SetNumberField(TEXT("spawned"), CamSpawnedN);
	CamsR->SetNumberField(TEXT("moved"), CamMovedN);
	CamsR->SetNumberField(TEXT("removed"), CamRemovedN);
	Root->SetObjectField(TEXT("cameras"), CamsR);
	TArray<TSharedPtr<FJsonValue>> FailArr;
	for (const FString& F : Failures) { FailArr.Add(MakeShared<FJsonValueString>(F)); }
	Root->SetArrayField(TEXT("failures"), FailArr);
	OnComplete(JsonResp(Root));
	return true;
}

bool USceneControlSubsystem::HandleHelp(const FHttpServerRequest& /*Req*/, const FHttpResultCallback& OnComplete)
{
	// 자기서술 — 소스·저장소·문서 없이 씬 포트 하나로 접속한 에이전트가 전체 계약을 발견하게 한다.
	// 산문은 C++ 가 아니라 플러그인 docs/scene-help.md 에 있다: 문구 수정 = 파일 저장이 전부다
	// (리빌드 불필요, 요청마다 다시 읽으므로 재시작도 불필요). C++ 는 로드 + 라이브 토큰 치환 +
	// 서빙만 한다. 패키징 빌드에 파일을 싣는 것은 Build.cs 의 RuntimeDependencies 가 담당한다.
	FString Doc;
	FString DocPath;
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("baroCCTVSimulator")))
	{
		DocPath = Plugin->GetBaseDir() / TEXT("docs/scene-help.md");
		FFileHelper::LoadFileToString(Doc, *DocPath);
	}
	if (Doc.IsEmpty())
	{
		// 파일이 없어도(스테이징 누락 등) 발견 가능성은 유지한다 — 최소 도움말로 응답.
		UE_LOG(LogSceneCtrl, Warning, TEXT("[Scene] scene-help.md 없음(%s) — 내장 최소 도움말로 응답."), *DocPath);
		Doc = TEXT("# /scene/* 씬 제어 REST\n\n"
			"상세 문서 파일(플러그인 docs/scene-help.md)이 이 배포에 없습니다.\n\n"
			"엔드포인트: GET /scene/catalog · GET /scene/slots · GET /scene/cameras · POST /scene/project ·\n"
			"GET|POST /scene/cars (POST: {slotId|transform, offset?, carType, color, plate?, force?}) ·\n"
			"GET|PATCH|DELETE /scene/cars/:id · POST /scene/reset · GET /scene/help\n\n"
			"라이브: 레벨 {{LEVEL}} · 플러그인 v{{PLUGIN_VERSION}} · 씬 포트 {{SCENE_PORT}} · "
			"주차면 {{SLOT_COUNT}} · 카메라 {{CAMERA_COUNT}} · 스폰 차량 {{SPAWNED_CAR_COUNT}}\n");
	}

	// 라이브 토큰 — 낡을 수 있는 수치는 문서에 박지 않고 서빙 시각에 치환한다.
	FString Level;
	if (UWorld* W = GetWorld()) { Level = W->GetMapName(); Level.RemoveFromStart(W->StreamingLevelsPrefix); }
	FString PluginVer = TEXT("?");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("baroCCTVSimulator"))) { PluginVer = Plugin->GetDescriptor().VersionName; }
	TArray<FSlotInfo> Slots; CollectSlots(GetWorld(), ParkingSlotClassPrefix, Slots);
	int32 CameraCount = 0;
	if (UWorld* W = GetWorld()) { for (TActorIterator<APTZCamera> It(W); It; ++It) { ++CameraCount; } }
	Doc.ReplaceInline(TEXT("{{LEVEL}}"), *Level, ESearchCase::CaseSensitive);
	Doc.ReplaceInline(TEXT("{{PLUGIN_VERSION}}"), *PluginVer, ESearchCase::CaseSensitive);
	Doc.ReplaceInline(TEXT("{{SCENE_PORT}}"), *FString::FromInt(ScenePort), ESearchCase::CaseSensitive);
	Doc.ReplaceInline(TEXT("{{SLOT_COUNT}}"), *FString::FromInt(Slots.Num()), ESearchCase::CaseSensitive);
	Doc.ReplaceInline(TEXT("{{CAMERA_COUNT}}"), *FString::FromInt(CameraCount), ESearchCase::CaseSensitive);
	Doc.ReplaceInline(TEXT("{{SPAWNED_CAR_COUNT}}"), *FString::FromInt(Cars.Num()), ESearchCase::CaseSensitive);

	// charset 은 FHttpServerResponse::Create 가 자동으로 붙인다(;charset=utf-8) — 여기 넣으면 중복된다.
	TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(Doc, TEXT("text/markdown"));
	R->Code = EHttpServerResponseCodes::Ok;
	OnComplete(MoveTemp(R));
	return true;
}
