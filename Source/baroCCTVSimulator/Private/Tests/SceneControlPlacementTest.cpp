// 차량 배치 합성(Offset * Base) 규약 테스트.
//
// offset 은 "주차면 기준" 변형이다 — 좌우로 비껴 세우기, 조금 틀기, 180도 반대로 주차.
// 곱 순서를 뒤집으면(Base * Offset) 변형이 월드축 기준이 되고, 주차면 방위가 0 이 아닌 실제 레벨에서는
// 그게 곧 오배치다. LV_Park_sim_01 의 주차면 yaw 는 -87.51 같은 값이라 두 순서의 차이가 미터 단위로 벌어진다.
// 이 테스트가 잠그는 것은 "곱 순서"와 "offset 은 누적 델타가 아니라 값" 이 두 가지다.

#include "Misc/AutomationTest.h"
#include "SceneControlSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FSimCarState PlacedCar(const FTransform& Base, const FTransform& Offset)
	{
		FSimCarState S;
		S.BaseTransform = Base;
		S.OffsetLocation = Offset.GetLocation();
		S.OffsetRotation = Offset.Rotator();
		RecomposePlacement(S);
		return S;
	}

	// LV_Park_sim_01 의 실제 주차면 하나(BP_ParkingSlot_C_1) — 방위가 축에 정렬돼 있지 않아
	// 곱 순서를 뒤집으면 바로 틀어진다.
	const FTransform SlantedSlot(FRotator(0.0, -87.51, 0.0), FVector(-842.88, -1181.21, 10.0));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSceneControlPlacementOffset,
	"baroCCTVSimulator.SceneControl.Car placement offset composes in slot local axes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSceneControlPlacementOffset::RunTest(const FString&)
{
	constexpr double Tolerance = 1e-3;

	// offset 을 안 주면 예전과 같은 자리 — 기존 호출자는 영향을 받지 않는다.
	{
		const FSimCarState S = PlacedCar(SlantedSlot, FTransform::Identity);
		TestTrue(TEXT("항등 offset 은 기준 배치를 그대로 둔다"), S.Transform.Equals(SlantedSlot, Tolerance));
	}

	// 좌우로 비껴 세우기는 주차면이 향한 방향 기준이다.
	// yaw=90 주차면에서 로컬 +Y(우측) 100cm 는 월드로는 -X 100cm.
	{
		const FTransform Base(FRotator(0.0, 90.0, 0.0), FVector(1000.0, 2000.0, 10.0));
		const FSimCarState S = PlacedCar(Base, FTransform(FRotator::ZeroRotator, FVector(0.0, 100.0, 0.0)));
		TestTrue(TEXT("로컬 우측 변형이 주차면 방위를 따른다"),
			S.Transform.GetLocation().Equals(FVector(900.0, 2000.0, 10.0), Tolerance));
	}

	// 임의 방위에서도 월드 이동량 = 주차면 회전을 offset 위치에 적용한 것이고, yaw 는 상대 각으로 더해진다.
	{
		const FVector LocalNudge(-35.0, 18.0, 0.0);
		const FSimCarState S = PlacedCar(SlantedSlot, FTransform(FRotator(0.0, 10.0, 0.0), LocalNudge));
		const FVector Expected = SlantedSlot.GetLocation() + SlantedSlot.GetRotation().RotateVector(LocalNudge);
		TestTrue(TEXT("월드 이동량 = 주차면 회전 x offset 위치"),
			S.Transform.GetLocation().Equals(Expected, Tolerance));
		TestTrue(TEXT("yaw 는 주차면 방위에 더해진다"),
			FMath::IsNearlyEqual(
				FRotator::NormalizeAxis(S.Transform.Rotator().Yaw - SlantedSlot.Rotator().Yaw), 10.0, Tolerance));
	}

	// 180도 = 정확히 반대로 주차. 자리는 그대로여야 한다.
	{
		const FSimCarState S = PlacedCar(SlantedSlot, FTransform(FRotator(0.0, 180.0, 0.0), FVector::ZeroVector));
		TestTrue(TEXT("180도 반대 주차는 위치를 바꾸지 않는다"),
			S.Transform.GetLocation().Equals(SlantedSlot.GetLocation(), Tolerance));
		TestTrue(TEXT("180도 반대 주차의 월드 yaw"),
			FMath::IsNearlyEqual(FRotator::NormalizeAxis(S.Transform.Rotator().Yaw), 92.49, Tolerance));
	}

	// offset 은 값이지 누적 델타가 아니다 — PATCH 로 같은 값을 다시 보내도 차가 더 밀리지 않는다.
	{
		const FTransform Off(FRotator(0.0, 12.0, 0.0), FVector(5.0, 7.0, 0.0));
		FSimCarState S = PlacedCar(FTransform(FRotator(0.0, 33.0, 0.0), FVector(10.0, -20.0, 10.0)), Off);
		const FTransform Once = S.Transform;
		S.OffsetLocation = Off.GetLocation();
		S.OffsetRotation = Off.Rotator();
		RecomposePlacement(S);
		TestTrue(TEXT("같은 offset 재적용은 멱등"), S.Transform.Equals(Once, Tolerance));
	}

	// 주차면을 옮겨도 변형은 따라간다(PATCH slotId 이동이 기준만 갈아끼우는 구조의 근거).
	{
		const FTransform Off(FRotator(0.0, 180.0, 0.0), FVector(0.0, 25.0, 0.0));
		FSimCarState S = PlacedCar(SlantedSlot, Off);
		const FTransform OtherSlot(FRotator(0.0, 12.4, 0.0), FVector(300.0, -50.0, 10.0));
		S.BaseTransform = OtherSlot;
		RecomposePlacement(S);
		TestTrue(TEXT("주차면 이동 후에도 같은 상대 변형"),
			S.Transform.GetLocation().Equals(
				OtherSlot.GetLocation() + OtherSlot.GetRotation().RotateVector(FVector(0.0, 25.0, 0.0)), Tolerance));
		TestTrue(TEXT("주차면 이동 후에도 같은 상대 yaw"),
			FMath::IsNearlyEqual(
				FRotator::NormalizeAxis(S.Transform.Rotator().Yaw - OtherSlot.Rotator().Yaw), 180.0, Tolerance));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
