// Copyright gbox3d. All Rights Reserved.
//
// USceneControlSubsystem — 시뮬레이터 "씬 편집" 런타임 서버 (월드 전역, 카메라와 별개 축)
//
// baro_calory 의 /api/simulator/* 가 프록시하는 JSON REST 계약(/scene/*)을 UE 안에서 이행한다.
// 배치된 BP_ParkingSlot 액터를 슬롯으로 노출하고, /Game/BP/BP_Car 를 슬롯에 스폰하며,
// 차종/색상/번호판종류/번호판글자를 BP 함수(Change_Car/Change_Color/Change_Plate/Change_Text)를
// ProcessEvent 로 호출해 반영한다(리플렉션 직접설정 대신 BP 로직 재사용).
//
// HucomsServerSubsystem(카메라별 N포트, PTZ, Tick)과 관심사가 다르므로(월드 전역 1포트, CRUD,
// 비틱) 별도 UWorldSubsystem 으로 분리한다. 포트 8095 는 baro_calory config.simulator.port 와 일치.
//
// 계약(엔드포인트):
//   GET    /scene/catalog        차종목록(index·name·asset·boundsCm)·색상·번호판종류·한글목록
//   GET    /scene/slots          주차면(id·label·type·transform·occupied·carId)
//   GET    /scene/cameras        카메라 광학 포즈·포트·화각표·슬롯 기준면/높이 (현재 PTZ/FOV는 Hucoms)
//   POST   /scene/cameras        카메라 런타임 스폰 {location, yawDeg?, pitchDeg?, httpPort, mjpegPort, fixed?, note?}
//   PATCH|DELETE /scene/cameras/:id  스폰 카메라 이동/삭제 (레벨 저작 카메라는 403)
//   GET    /scene/snapshot       씬 스냅샷(차량 전수 + 스폰 카메라 스펙) — 복원 가능한 JSON
//   POST   /scene/snapshot       스냅샷 복원(차량 리셋 후 재배치 + 카메라 reconcile)
//   POST   /scene/project        월드점→픽셀 그라운드-트루스(UE 뷰·투영행렬; 웹 오버레이 정합 검증 오라클)
//   GET    /scene/cars           배치된 차량
//   POST   /scene/cars           스폰 {slotId|transform, offset?, carType, color, plate}
//   GET|PATCH|DELETE /scene/cars/:id
//   POST   /scene/reset          전체 삭제
//   GET    /scene | /scene/help  자기서술(전 엔드포인트·규약·라이브 상태 — 소스 없이 사용 가능하게)

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "IHttpRouter.h"          // IHttpRouter, FHttpRequestHandler, FHttpRouteHandle
#include "HttpResultCallback.h"   // FHttpResultCallback
#include "SceneControlSubsystem.generated.h"

struct FHttpServerRequest;

/** 씬에 배치된 차량 1대의 논리 상태 — 응답 재구성 + 스폰 액터 참조. */
struct FSimCarState
{
	FString Id;
	FString SlotId;                 // 빈 문자열 = 자유 좌표 배치(슬롯 없음)
	int32 CarType = 0;              // selected_Car 0..22
	int32 Color = 0;               // selected_Color 0..7
	int32 PlateType = 0;           // selected_Plate 0..2
	FString City, Prefix, Kor, Number;
	// 배치는 기준(Base)과 그 로컬 변형(Offset)으로 나눠 들고 있다. Transform 은 둘을 합성한 결과이고
	// 액터에 실제로 적용되는 값이다. 나눠 두면 주차면을 옮겨도 변형이 따라가고, 같은 offset 을 두 번
	// 보내도 결과가 같다(값 의미 — 누적 델타가 아님).
	FTransform BaseTransform = FTransform::Identity;  // 주차면 트랜스폼(슬롯 배치) 또는 요청 transform(자유 배치)
	// Base 로컬 변형 — 좌우로 비껴·틀기·180도 반대. FTransform 이 아니라 받은 그대로 들고 있는 이유는
	// 응답 에코 때문이다: FTransform 에 넣으면 사분원수로 접혔다 펴지면서 yaw 10 이 9.999999999999998 로
	// 돌아온다. 배치에는 무해하지만 "보낸 값이 그대로 돌아온다"가 깨지고, Fake 와도 어긋난다.
	FVector OffsetLocation = FVector::ZeroVector;
	FRotator OffsetRotation = FRotator::ZeroRotator;
	FTransform Transform = FTransform::Identity;      // 최종 월드 배치 = Offset * BaseTransform
	TWeakObjectPtr<AActor> Actor;  // 스폰된 BP_Car (딴 경로로 파괴돼도 dangling 안 됨)
};

/**
 * 배치 합성. Offset 을 Base 의 로컬 축에서 먼저 적용한 뒤 Base 로 월드에 옮긴다 — 그래서
 * offset.location.y 는 월드 Y 가 아니라 주차면이 향한 방향 기준의 좌우이고, offset.rotation.yaw 는
 * 주차면 방위에 더해지는 상대 각이다(180 = 정확히 반대로 주차).
 * 곱 순서를 뒤집으면 월드축 기준이 돼 주차면 방위가 무시된다 — 그 순서를 잠그는 게 SceneControlPlacementTest.
 */
inline void RecomposePlacement(FSimCarState& S)
{
	S.Transform = FTransform(S.OffsetRotation, S.OffsetLocation) * S.BaseTransform;
}

UCLASS(config = Game)
class BAROCCTVSIMULATOR_API USceneControlSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** 씬 제어 HTTP 리스너 포트. baro_calory config.simulator.port 와 일치시킬 것. */
	UPROPERTY(config, EditAnywhere, Category = "Scene")
	int32 ScenePort = 8095;

	/** 스폰할 차량 블루프린트의 generated class 경로. */
	UPROPERTY(config, EditAnywhere, Category = "Scene")
	FString CarBlueprintPath = TEXT("/Game/BP/BP_Car.BP_Car_C");

	/** 주차면 액터를 식별하는 클래스명 접두(변종 BP_ParkingSlot_5m/6m/BUS/... 공통). */
	UPROPERTY(config, EditAnywhere, Category = "Scene")
	FString ParkingSlotClassPrefix = TEXT("BP_ParkingSlot");

	//==================================================================================
	// UWorldSubsystem
	//==================================================================================
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	bool bStarted = false;
	int32 CarSeq = 0;

	/** /Game/BP/BP_Car.BP_Car_C 캐시 (UPROPERTY 로 GC 보호). */
	UPROPERTY()
	TSubclassOf<AActor> CarClass;

	/** BP_Car.Mesh_List 에서 읽은 차종 에셋명. 배열 인덱스 = selected_Car. 최초 요청 시 1회 채운다. */
	TArray<FString> CarAssetNames;
	bool bCarAssetNamesResolved = false;   // 실패해도 재시도·경고 반복을 막는다

	/** 차종별 최종 표시 메시 aggregate actor-local bounds(cm). CarAssetNames 와 같은 인덱스, 최초 요청 시 1회 계산. */
	TArray<FBox> CarBoundsCm;
	bool bCarBoundsResolved = false;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;

	TMap<FString, FSimCarState> Cars;      // carId  -> 상태
	TMap<FString, FString> SlotOccupancy;  // slotId -> 점유 carId

	void StartServer();
	void StopServer();

	// --- 라우트 핸들러 ---
	bool HandleCatalog(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSlots(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCameras(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // GET 목록 / POST 런타임 스폰
	bool HandleCameraById(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete); // PATCH 이동 / DELETE 삭제
	bool HandleSnapshot(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);  // GET 저장 / POST 복원
	bool HandleProject(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // 월드점→픽셀 오라클
	bool HandleCars(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);      // GET list / POST spawn
	bool HandleCarById(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // GET / PATCH / DELETE
	bool HandleReset(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleHelp(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);   // GET /scene, /scene/help — 자기서술

	// --- 내부 ---
	UClass* ResolveCarClass();
	/** 차종 에셋명(BP_Car.Mesh_List). 실패하면 빈 배열 — 호출부는 인덱스만 노출한다. */
	const TArray<FString>& GetCarAssetNames();
	/** 임시 BP_Car 한 대에 차종을 순차 적용해 최종 표시 메시 aggregate actor-local bounds를 계산·캐시한다. */
	const TArray<FBox>& GetCarBoundsCm();
	/** carType 을 실제 Mesh_List 길이로 클램프. Mesh_List 를 못 읽으면 과거 범위(0..22). */
	int32 ClampCarType(int32 InCarType);
	AActor* SpawnCarActor(const FTransform& Xform);
	void ApplyToActor(AActor* Car, const FSimCarState& S);   // BP setter 호출(Change_Car/Color/Plate/Text)
	/** 슬롯의 현재 점유 차량을 축출한다(액터 파괴 + Cars/SlotOccupancy 정리). force 덮어쓰기 전에 호출해 슬롯당 1대를 보장한다. */
	void EvictSlotOccupant(const FString& SlotId);
	/** 스냅샷의 차량 1대를 복원(스폰+상태 등록). 실패 시 false + OutError(주차면 소실 등). */
	bool RestoreCarFromJson(const TSharedPtr<FJsonObject>& CarObj, FString& OutError);
};
