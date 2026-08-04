# Dev Log

## 목차

- [Entries](#entries)

## Entries

- 2026-08-03: **v0.1.13 — 카메라 런타임 생명주기 + 씬 스냅샷. BEV 데이터셋 루프의 두 병목 제거.**
  - 동기(이교수님): BEV(BEVHeight류) 자율 파인튜닝 — 학습 성능은 카메라 기하 다양성에서 나온다
    (`paper_works/object3d_model_review` 실측: 학습에 없던 5.75m 높이에서 공개 체크포인트 0%).
    기존 config 스포너는 포즈 변경마다 ini 수정 + 재시작이라 에이전트 루프가 못 돈다.
  - **카메라 API**: `POST /scene/cameras`(런타임 스폰 — 포트 명시 필수, 채널·리스너 런타임 증설),
    `PATCH /scene/cameras/:id`(location/yawDeg/pitchDeg — pitch 는 tilt 이관 규약 그대로),
    `DELETE`(in-flight flush→라우트 unbind→MJPEG 정지→캡처 반납→파괴). 목록에 `spawned` 필드.
    **레벨 저작 카메라는 403** — 스폰 출자(`SpawnedCameras` set: 자동/config/API)만 이동·삭제 가능.
  - 구조: BuildChannels/StartServers/StopServers 의 채널 단위 로직을 `CreateChannelForCamera`/
    `StartChannelServers`/`StopChannel` 로 추출해 부팅 경로와 런타임 경로가 같은 코드를 쓴다.
  - **스냅샷 API**: `GET /scene/snapshot`(차량 전수 + 스폰 카메라 스펙 — 파일 저장 없음, 호출자 보관),
    `POST`(차량 전량 리셋 후 재배치, 카메라는 httpPort 키 reconcile: 이동/스폰/제거).
    자유 배치 차량은 `baseTransform` 을 따로 싣는다 — 합성된 transform 을 기준으로 재사용하면
    offset 이 이중 적용되기 때문. 레벨 불일치 409(force 강행), 부분 실패는 failures[] 로 보고.
  - 검증(standalone -game, `tools/scene-test/camera-snapshot-contract.mjs` **33개 전수 통과**):
    스폰 즉시 CGI 응답(pitch -30 → tiltpos 3000)·실렌더 JPEG, 이동 반영(tilt 4500), 저작 카메라
    403, 삭제 후 포트 닫힘, 스냅샷 왕복에서 차량 배치(슬롯·transform·차종·색·번호판) 완전 일치 +
    카메라 reconcile(이동 1·제거 1) 정확. offset-contract 회귀 통과.
  - 함정 하나 정정: scene-help.md 의 Hucoms 경로가 `/cgi-bin/ptz/getptzfpos` 로 잘못 적혀 있었다
    (실제는 `/cgi-bin/control/ptzf_status.cgi?action=getptzfpos`, 응답은 `panpos = N` 줄 형식) —
    계약 테스트가 잡아냈다. 자기서술이 틀리면 소스 없는 소비자에겐 그대로 사고다.
- 2026-08-03: **v0.1.12 — 자기서술 `GET /scene`·`/scene/help`. 소스 없이 포트 하나로 계약 발견.**
  - 이교수님 제기: 다른 세션이 소스·참고자료·저장소 없이 sim 을 쓰려면 help API 가 있어야 한다
    (baro_calory `GET /api/help` 와 같은 원칙 — 라우트 사용 규약이 저장소 문서 안에만 있으면
    외부 에이전트가 볼 수 없다. 패키징된 sim 은 docs/ 조차 없으니 더 심하다).
  - **산문은 C++ 가 아니라 `docs/scene-help.md` 텍스트 파일이다(이교수님 지적으로 방향 수정 —
    처음엔 C++ 에 JSON 으로 박았었다).** 문구 수정 = 파일 저장이 전부: 리빌드·버전범프 사슬 없음,
    요청마다 다시 읽어 재시작도 불필요(마커 추가→즉시 서빙→제거→즉시 사라짐 실검증).
    C++(`HandleHelp`)는 로드 + `{{LEVEL}}` 등 라이브 토큰 6종 치환 + text/markdown 서빙만.
  - 패키징: `Build.cs` `RuntimeDependencies`(`$(PluginDir)/docs/scene-help.md`, NonUFS)로 원본
    텍스트가 pak 밖 루즈 파일로 실린다 — 배포 후에도 편집 가능. 파일이 없으면 404 대신 내장
    최소 도움말로 응답(발견 가능성 유지, 경고 로그).
  - 문서 내용: 라이브 상태·공통 규약(전송/좌표/에러/번호판/값범위)·배치 모델(offset)·엔드포인트
    전수(요청/응답 shape)·Hucoms CGI 조인(포트·단위·tiltpos 부호)·curl 빠른 시작. 유지 규칙:
    **API 표면 변경 시 scene-help.md 와 호스트 docs/scene-control-api.md 를 같이 고친다**.
  - 검증(standalone -game): `/scene`·`/scene/help` 동일 서빙, 토큰 실치환(레벨·v0.1.12·24면·6대),
    무빌드 수정 실증, offset-contract 전 항목 재통과. charset 중복(Create 가 자동 부가) 수정.
- 2026-08-03: **v0.1.11 — 차량 배치 변형(`offset`). 주차면에 붙인 채로 비껴·틀어·반대로 세운다.**
  - 이교수님 요구: "차량 배치할 때 조금씩 위치를 변형 — 좌우로 비껴, 10도쯤 틀어, 180도 반대로도."
    기존 RPC 로도 `transform` 자유 좌표로 같은 렌더는 만들 수 있었지만 그 경로는 **주차면 소속을
    버린다**(slotId=null, 점유 미등록, 409 방어·carId 조인 소멸). 그래서 자유 좌표가 아니라
    "기준 + 변형" 으로 쪼갠다.
  - 계약: `POST/PATCH /scene/cars` 에 `offset {location, rotation}` 추가, 응답에도 에코.
    최종 배치 = `Offset * Base`(UE FTransform 곱) — **변형의 축은 월드축이 아니라 기준의 로컬축**이다.
    sim_01 주차면 yaw 가 -87.51 이라 곱 순서를 뒤집으면 그대로 오배치다. `SceneControlPlacementTest`
    가 그 순서를 잠근다.
  - 상태 분리: `FSimCarState` 가 `BaseTransform`(주차면 or 자유좌표) / `OffsetLocation`+`OffsetRotation`
    / `Transform`(합성 결과) 셋을 따로 든다. 덕분에 ① offset 은 누적 델타가 아니라 **값**(같은 값을
    다시 PATCH 해도 제자리) ② 주차면을 옮기면 변형이 따라간다.
  - **offset 을 FTransform 으로 안 들고 있는 이유**: FTransform 에 넣으면 사분원수로 접혔다 펴지면서
    yaw 10 이 9.999999999999998 로 에코된다. 배치엔 무해하지만 "보낸 값이 그대로 돌아온다"가 깨지고
    JS Fake 와도 어긋난다. 그래서 받은 FVector/FRotator 를 그대로 보관하고 합성 때만 조립한다.
  - 함께 정리한 두 가지(둘 다 조용한 무시를 없애는 쪽):
    `slotId`+`transform` 동시 지정은 이제 **400**(예전엔 transform 을 말없이 버렸다. 게다가 JS Fake 는
    반대로 transform 을 살려서 실 sim 과 계약이 갈려 있었다). PATCH 의 `transform` 도 무시하지 않고
    **자유 좌표 기준 교체**로 동작한다(붙어 있던 주차면은 점유 해제).
  - 검증: UE 자동화 5/5 Success(신규 배치 테스트 포함). standalone `-game` 실기동에서 JS
    `composePlacement` 와 10개 자세 대조 — **위치 오차 0cm, 회전 최대 5.6e-12°**. 다축(pitch/roll/yaw
    동시)과 짐벌 임계 밴드 안팎(89.5 / 89.9 / 89.99 / ±90)까지 포함. 렌더 A/B(같은 차·같은 자리,
    offset 만 차이)로 실제로 비뚤게 서는 것까지 눈으로 확인.
  - **UE 짐벌 락 규약(JS 포팅에서 알아낸 것)**: 피치 ±90 에서는 일반식의 `YawY`·`YawX` 가 둘 다 0 이라
    `atan2` 가 잡음이다. UE 는 그 구간에서 roll 을 yaw 로 접고(`roll=0`) `yaw = ±2*atan2(X, W)` 를 쓴다.
    임계는 `SingularityTest > 0.4999995`(피치 약 89.94° 부터). 흔한 공개 구현들과 다르므로 UE 출력과
    맞춰야 하는 코드는 이 규칙을 따라야 한다.
  - 검증 하네스(같은 날): 소비 저장소(baro_calory)를 같이 고치지 않는다(이교수님 확정 — 처음에
    고쳤다가 전량 복원). 대신 호스트 저장소 `tools/scene-test/offset-contract.mjs` 가 UE 회전 규약의
    JS 참조 구현을 들고 실행 중 sim 에 수치·동작 계약을 전수 대조한다(의존성 0). 소비자 연동
    (라우터 필드 통과 등)은 별도 결정 사항으로 남아 있다.
- 2026-07-24: **v0.1.10 — 연속 스트림 캡처를 비동기 GPU 리드백으로(품질 무손실). 6대 카메라당 +73%.**
  - 발단: 6대 동시 MJPEG 카메라당 3.3fps·게임 틱 2.5fps. 이교수님 "품질 양보 불가, 구조적으로 개선".
  - 4각도 적대적 검증(wf_4a5a001e)으로 오칭 정정: 병목은 `FImageUtils::GetRenderTargetImage`→
    `ReadPixels` 내부의 **디바이스 전역 GPU 드레인**(`SubmitAndBlockUntilGPUIdle`)이 게임 스레드를
    총 GPU 부하 비례로 블로킹(1대 7.9→6대 26.8ms+, 진짜 정상상태에선 432ms). "gpuwait 불변"은
    `FlushRenderingCommands` 가 GPU 안 기다려서였고(오칭), GPU 실측은 6대 3D 80%·VRAM 94%.
  - 구현: `FRHIGPUTextureReadback`(플러시·스톨 없음) + 워커 스레드 JPEG 인코딩. PTZCaptureComponent
    를 PrepareCapture(뷰세팅+RT 반환)/RenderOnce 로 분해(동기 CaptureJpeg 는 스냅샷 전용 유지).
    RT 를 크기별 분리(스트림 720p/스냅샷 1440p — 왕복 재할당 히치 제거). 서브시스템 Tick 3단
    파이프라인: drain(완료 큐→UpdateFrame)/collect(IsReady→렌더스레드 Lock+de-pitch→워커 인코딩)/
    submit(EnqueueCopy). 채널당 in-flight 1개(상태머신이 소비-후-재사용 보장). 완료 큐는
    TSharedPtr<TQueue Mpsc>(워커는 포트·시퀀스·값복사만 참조 — UObject/Stream 무참조). 종료 규율:
    in-flight 있으면 채널 파괴 전 FlushRenderingCommands. `bAsyncStreamCapture`(기본 True) 킬스위치.
  - 안전 근거(검증): RT/ViewState 해제는 렌더커맨드 FIFO 로 EnqueueCopy 뒤 실행(crash 없음),
    readback 객체는 게임스레드 즉시 delete 금지(flush 후), ImageWrapper 모듈 StartServers 선로딩,
    화질 동일(BGRA8+sRGB, 감마는 태깅뿐 — 바이트 동일). v0.1.9 축출/유휴해제에 in-flight 가드 추가.
  - 실측(RTX 5060, standalone): 1대 30fps·화질 육안 동일, **6대 3.3→5.7fps/대·19.7→34.2캡처/s(+73%,
    검증 예측 +25% 초과)**, 게임 틱 2.5→24~60fps, 스트림 중 QHD 스냅샷 0.1s(히치 0), 6대 in-flight
    중 정상종료 크래시 0. 계측(임시 FlushRenderingCommands 프로파일)은 제거함.
  - 넘을 수 없는 벽: GPU 렌더 자체(6대 all-warm). 6×30fps 는 이 품질·이 GPU 에선 불가 — 후속은
    캡처당 GPU 39ms 초선형 기전(VRAM eviction vs 뷰별 Lumen) 규명 시 추가 가능.
- 2026-07-24: **v0.1.9 — 캡처 렌더 자원을 수요 기반 생명주기로 전환("쓰는 카메라만 켠다").**
  - 증상: 패키지 실행에서 클라이언트 0·캡처 0 인데 게임 틱 2.5 fps, GPU 3D 47.7%, RT 지오메트리 상주 1.245 GiB(예산 400 MiB) 초과, 텍스처 스트리밍 풀 초과.
  - 원인 ①: **release 경로가 `EndPlay` 뿐이었다.** 한 번이라도 캡처된 카메라는 SceneCapture2D + persistent ViewState(HWRT 가드로 on) + RT 를 종료까지 영구 상주. acquire 는 이미 지연 생성이라 "켜기만 하고 끄지 않는" 반쪽 구조였다.
  - 원인 ②: `Tick` 이 `IStreamingManager::AddViewInformation` 을 **전 채널 무조건 매 틱** 호출 — 아무도 안 보는 카메라의 원거리 텍스처까지 고해상도 mip 상주.
  - 원인 ③(진짜 방아쇠): **DGX(192.168.0.220) 의 uvicorn 이 6대 jpeg.cgi 를 HTTP keep-alive 로 상시 순회 폴링**하고 있었다. HUD 는 MJPEG 클라이언트만 세어 "클라이언트 없음, 캡처 0" 으로 보였다 — 유휴로 오인한 상태가 실제로는 6대 전부 사용 중이었다. keep-alive 라 연결의 원격 포트가 안 바뀌어 "연결만 남았다" 로 오판하기 쉽다(원격 포트 불변 ≠ 요청 없음).
  - 수정: `UPTZCaptureComponent::ReleaseCaptureResources()`/`HasCaptureResources()` 신설(`DestroyComponent` → `OnUnregister` 가 ViewState 파괴, `RenderTarget->ReleaseResource()` 로 GC 대기 없이 VRAM 반납, `RtWidth/Height` 무효화 → 다음 `EnsureSetup` 이 재생성). `FHucomsChannel::LastDemandTime` + `StampDemand()`/`ReleaseChannelCapture()`. 캡처 진입점이 정확히 2곳(스트림 분기, `RenderSnapshotJpeg`)뿐임을 전수 확인해 그 2곳 + PTZ 이동 4핸들러에만 스탬프. `AddViewInformation` 을 "켜진 카메라"로 게이트. HUD 3상태(▶스트리밍/켜짐/꺼짐).
  - config: `MaxActiveCameras=1`(0=레거시) / `MinWarmSeconds=5` / `IdleReleaseSeconds=30` / `RecreateWarmupFrames=4`. 상태 폴링(`getptzfpos` 등)은 **수요가 아니다**(헬스체크가 전 채널을 켜 두면 무효화).
  - **`MinWarmSeconds` 유예는 필수다** — 없이 `MaxActiveCameras=1` 만 넣었더니 6대 순회 폴링과 충돌해 **1분에 173회 재생성** churn(원래보다 나쁨). 유예 적용 후 0회.
  - 실측(standalone -game, RTX 5060 8GB): GPU 3D **47.7% → 2.6%**, WorkingSet **21.6 → 12.9 GB**, Private **23.0 → 15.9 GB**. 유휴 30초 후 `카메라 끔 — 유휴 30초` 6줄. 카메라 전환 시 이전 1대만 `다른 카메라 사용` 으로 해제. warm 재요청 0.091s vs 콜드 0.191s.
  - **콜드 재시작 비용 실측(워밍업을 깎지 않기로 한 근거)**: 꺼진 카메라 첫 스냅샷 약 3.0초 / warm 0.24~0.34초. `RecreateWarmupFrames=0` 으로 워밍업을 없애도 **2.25초** — 즉 워밍업(0.75초)이 아니라 **SceneCapture2D + 2560x1440 RT 할당과 첫 Lumen/셰이더 셋업(약 2.2초)이 주범**이다. 0.75초 벌자고 콜드 프레임 품질을 걸 이유가 없어 `RecreateWarmupFrames=4` 유지(이교수님 판단: "무리하게 줄일 필요 없다, 왜 그런지 문서화"). 콜드가 부담되면 `IdleReleaseSeconds` 를 폴 간격보다 크게 올리거나 `MaxActiveCameras` 를 늘린다.
  - `MaxActiveCameras=2` 동작 검증(패키지, 커맨드라인 오버라이드만으로): 8083→8085 사용 시 축출 0건(둘 다 warm, 8083 재요청 0.343초로 확인), 세 번째 8086 사용 시 **가장 오래 안 쓴 8085 하나만** 해제 — LRU 정확. 앱 ini 는 `IdleReleaseSeconds=10`(이교수님 요청, 30초는 길다) 으로 확정.
  - 잔여: RT 지오메트리 always-resident 는 뷰와 무관한 별도 레버(`r.RayTracing.NumAlwaysResidentLODs`)로 남아 있다 — 필요 시 A/B 후 ini 반영.
- 2026-07-20: **v0.1.6 — persistent ViewState 를 HWRT Lumen 가용 시에만 허용(UE5.8 SW Lumen 캡처 누수 근본 대응).**
  - UE 5.8 엔진 결함: 소프트웨어 Lumen(SDF 트레이싱) + persistent ViewState 를 가진 SceneCapture 조합에서 캡처 프레임마다 CPU 할당이 회수되지 않는다(LLM 태그 `DistanceFields`, 실측 +1.9~2.1MB/s @30fps 1280x720 — 35시간 가동 시 가상 72GiB OOM, 2026-07-16 현장). 라디언스캐시·스크린프로브 템포럴·서피스캐시 피드백·GDF 재캐시 억제·브릭 아틀라스 확대 cvar 전부 무효(A/B 10회 실측). Lumen GI off 또는 ViewState 제거 시에만 소멸.
  - v0.1.5(persist 무조건 off)는 누수는 잡지만 ViewState 가 없으면 캡처에서 Lumen 자체가 비활성이라 암부가 뭉개진다(clipLo 15.2%→18.4% 실측) — 화질 회귀로 대체.
  - 수정: `baro.Capture.PersistRenderingState` cvar(기본 1) + 가드 — `GRHISupportsRayTracing && r.Lumen.HardwareRayTracing` 일 때만 `bAlwaysPersistRenderingState=true`(HWRT 경로는 누수 원천인 SDF/GDF 프레임 갱신을 쓰지 않음). `bUseRayTracingIfEnabled=true` 필수(`r.RayTracing.SceneCaptures` 기본 -1 은 컴포넌트 설정을 따름). Build.cs 에 `RHI` Private 의존 추가.
  - 실측(호스트 baro_unreal Development 패키지, RTX): HWRT+persist **+0.053MB/s**(누수 소멸) + 암부 clipLo 14.8%(구 SW persist 15.2% 대비 개선, 선명도 동등). SW 폴백(가드 발동, persist auto-off) **-0.343MB/s**. 소비 호스트는 `DefaultEngine.ini` 에 `r.Lumen.HardwareRayTracing=True` 를 설정해야 화질 모드가 켜진다 — RT 미지원 GPU 는 자동으로 안전 모드(v0.1.5 동작)로 강등.
- 2026-07-20: **v0.1.5 — 캡처 persistent ViewState 비활성(누수 1차 대응 — v0.1.6 으로 대체).** `bAlwaysPersistRenderingState=false`. 누수는 제거되나 캡처 Lumen 소실로 암부 화질이 저하되어 같은 날 v0.1.6 가드 방식으로 대체됐다.
- 2026-07-14: **v0.1.4 — setcenter 조준을 tan+구면 기하로 정정.** (소급 기록. 실기 텔레메트리 역산으로 줌별 초점거리 배율 정합 — 호스트 baro_calory 센터링 오차 실측 종결 작업의 sim 측 반영.)
- 2026-07-10: **v0.1.3 — 캡처 VT 페이지 스로틀 해제(주차면 라인 데칼 미렌더 해결).**
  - `PTZCaptureComponent::EnsureSetup`의 캡처 컴포넌트에 `bOverrideVirtualTextureThrottle=true`. 이 플러그인의 표준 구조(메인 뷰포트 렌더 OFF + SceneCapture 전용)에서는 SVT 텍스처의 VT 피드백이 스로틀에 막혀 VT 샘플링 머티리얼(Megascans _VT 데칼 등)이 쿡 빌드에서 부팅 복불복/오프스크린 상시로 투명 렌더된다. Windows D3D12 완치(클린부팅 6/6). Vulkan 오프스크린은 VT 피드백 자체가 무동작이라 이 플래그로도 미해결(호스트 프로젝트 dev_log 2026-07-10 플랜 B 참조).
  - v0.1.2(같은 날 오전): `HandleCatalog`가 BP_Car.Mesh_List를 CDO 리플렉션으로 읽어 차종 `cars[]{index,name,asset}` 반환. carType 클램프도 배열 길이 기반(`ClampCarType`).
- 2026-07-08: **Scene Control 슬롯 라벨 계약 + v0.1.1.**
  - `/scene/slots`가 주차면 액터의 안정 ID(`GetName()`)와 에디터 표시명(`GetActorLabel()`)을 함께 반환하도록 정리. `id`는 RPC/점유 추적용, `label`은 웹 UI 표시용이다.
  - `baroCCTVSimulator.uplugin` `VersionName`을 **0.1.1**로 올림. 이 값은 `/scene/catalog.pluginVersion`과 `BaroSimHUD` 제목줄에 그대로 노출된다.
  - `baro_unrealEditor Win64 Development` 풀 빌드로 WorldSubsystem/플러그인 변경 반영 확인.
