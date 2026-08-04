# 씬 제어 API 도움말 (/scene/*)

> 이 문서는 실행 중인 시뮬레이터가 `GET /scene/help`(또는 `GET /scene`)로 직접 서빙하는
> 자기서술이다. 원본은 플러그인 `docs/scene-help.md` 텍스트 파일 — **문구 수정은 파일 저장이
> 전부다**(리빌드·재시작 불필요, 요청마다 다시 읽는다). `{{…}}` 토큰은 서빙 시각에 라이브
> 값으로 치환된다. 사람용 심화 레퍼런스(투영 수식·검증 이력)는 호스트 저장소
> `docs/scene-control-api.md` 에 있다 — 이 문서만으로도 API 사용에는 충분하다.

## 목차

1. [지금 이 sim (라이브)](#지금-이-sim-라이브)
2. [개요](#개요)
3. [공통 규약](#공통-규약)
4. [차량 배치 모델 — 기준 + 변형(offset)](#차량-배치-모델--기준--변형offset)
5. [엔드포인트](#엔드포인트) — 카탈로그·주차면·카메라(목록/스폰/이동/삭제)·투영·차량 CRUD·스냅샷·리셋
6. [카메라 제어·영상은 씬 API 밖이다 (Hucoms CGI)](#카메라-제어영상은-씬-api-밖이다-hucoms-cgi)
7. [빠른 시작 (curl)](#빠른-시작-curl)

## 지금 이 sim (라이브)

- 레벨 **{{LEVEL}}** · 플러그인 **v{{PLUGIN_VERSION}}** · 씬 포트 **{{SCENE_PORT}}**
- 주차면 **{{SLOT_COUNT}}** 면 · 카메라 **{{CAMERA_COUNT}}** 대 · API 스폰 차량 **{{SPAWNED_CAR_COUNT}}** 대

## 개요

UE 주차장 CCTV 시뮬레이터의 씬(주차면·차량·번호판·카메라 파라미터)을 **실행 중에** 조회·편집하는
HTTP REST API 다. 상태의 진실의 출처는 살아있는 UE 월드다 — 차량 스폰은 실제 액터 생성이고,
서버가 상태 파일을 따로 저장하지 않으므로 프로세스를 껐다 켜면 API 로 스폰한 차량은 사라진다.
재현이 필요하면 스폰 요청(응답의 `car` 전체)을 호출자가 기록해 두었다가 다시 보내면 된다.

## 공통 규약

- **전송**: 요청/응답 본문은 JSON(UTF-8). 빈 본문 POST 는 `{}` 로 취급. 실패 시 `{"error":"메시지"}`.
- **좌표**: UE 월드 좌표(왼손, +Z 위). 위치 **cm**, 각도 **deg**.
  `transform = { "location": {x,y,z}, "rotation": {pitch,yaw,roll} }`.
- **에러**: `400` 잘못된 입력(JSON 파싱 실패 · 배치 기준 없음 · slotId+transform 동시 지정) /
  `404` 주차면·차량·카메라 없음 / `409` 주차면 점유(`force:true` 면 기존 차를 파괴하고 진행) /
  `500` 차량 BP 로드·스폰 실패.
- **번호판**: 한국 신형으로 정규화 — `prefix` 숫자 3 + `kor` 한글 1 + `number` 숫자 4 (예 `123가4567`).
  `city` 는 API 상태로 저장·에코만 되고 **렌더에는 반영되지 않는다**.
- **값 범위**: `carType` 0..`catalog.carCount`-1 (라이브 카탈로그가 진실 — 하드코딩 금지),
  `color` 0..7, `plate.type` 0..2. 범위 밖 값은 서버가 클램프한다.

## 차량 배치 모델 — 기준 + 변형(offset)

차 한 대의 배치는 **기준(Base)** 과 **로컬 변형(offset)** 두 조각이고, 월드에 서는 자리는 둘의 합성이다:

```text
최종 transform = Offset * Base   (UE FTransform 곱 — offset 을 기준의 로컬축에서 먼저 적용)
```

- **기준**: `slotId`(그 주차면의 트랜스폼) 또는 자유 `transform` 중 **하나**. 동시에 주면 `400`.
- **offset 의 축은 월드축이 아니라 기준의 로컬축**: `location.x` = 전방 cm, `location.y` = 우측 cm,
  `rotation.yaw` = 기준 방위에 더해지는 상대 각(`10` = 살짝 틀어 주차, `180` = 정확히 반대로 주차).
- **offset 은 누적 델타가 아니라 값**: 같은 값을 PATCH 로 다시 보내도 차는 더 밀리지 않는다.
- **기준이 바뀌면 변형이 따라간다**: PATCH 로 주차면을 옮기면 비껴 선 정도·틀어진 각을 유지한 채
  새 주차면에서 같은 상대 자세로 선다.
- **주차면 소속은 유지된다**: offset 을 써도 점유(`occupied`·`carId`)와 409 방어는 그대로 산다 —
  자유 좌표로 빼내면 끊기는 것들이다.
- **겹침은 서버가 막지 않는다**(스폰이 밀려나면 GT 라벨과 실위치가 어긋나므로 의도된 동작).
  허용 변형 폭은 `catalog.cars[].boundsCm.size` 와 주차면 규격으로 호출자가 판단한다.

## 엔드포인트

### GET /scene/catalog

정적 카탈로그 + 레벨/플러그인 버전. →
`{ level, pluginVersion, carCount, cars:[{index, name, asset, class(car|truck|van), boundsCm{center,size} cm}], colors[8], plateTypes[3], korList }`

### GET /scene/slots

주차면 목록. `id` 가 안정 키(스폰의 `slotId` 로 사용), `transform` 은 월드 배치. →
`{ slots:[{id, label, type, transform, occupied, carId}] }`

### GET /scene/cameras

카메라 **설치 외부 파라미터와 화각표**. `spawned`(v0.1.13) = API/config 스폰 여부(true 면 이동·삭제 가능). →
`{ cameras:[{id, hucomsPort, mjpegPort, fixed, mount{location, baseYaw}, wideHFovDeg, projection:"pinhole", distortion:null, rollDeg, groundReference, heightAboveReferenceGroundCm, intrinsics{zoomHfov:[{zoomPos,hfovDeg}]}}] }`

- **현재 PTZ 는 여기 없다** — 실기와 동일하게 카메라별 Hucoms CGI(`getptzfpos`)에서 읽는다.
- 현재 화각 = `zoompos` 를 `intrinsics.zoomHfov` 로 환산(두 앵커 사이 선형 보간, 표 밖은 양끝 클램프).
- 광학 yaw = `mount.baseYaw + pan`. 시뮬 광학은 이상적 핀홀(왜곡 0, 주점 = 프레임 중앙, roll 0).

### POST /scene/project

월드점 → 화면 픽셀 **그라운드-트루스**(UE 가 실제 렌더에 쓰는 뷰·투영행렬). 클라이언트측 투영
구현의 검증 오라클로 쓴다.
요청 `{ cameraId|hucomsPort, points:[{x,y,z}], resolution?{width,height} (기본 1920x1080) }` →
`{ cameraId, fovDeg, resolution, points:[{x, y, visible, behind}] }` (요청 순서 1:1, 프레임 밖 점도 좌표 유지)

### GET /scene/cars

배치된 차량 목록 → `{ cars:[{id, slotId, transform, offset, carType, color, plate}] }`.
`?visibility=<cameraId|hucomsPort>` 를 붙이면 각 차량에 `visibleRatio`(0=완전가림 … 1=완전노출,
그 카메라 광학중심에서의 라인트레이스 가림 GT)가 실리고 `visibilityCamera` 가 에코된다.

### POST /scene/cars

차량 스폰. 요청:

```json
{ "slotId": "BP_ParkingSlot_C_15",
  "offset": { "location": {"x": -16, "y": 12, "z": 0}, "rotation": {"pitch": 0, "yaw": 7, "roll": 0} },
  "carType": 3, "color": 4,
  "plate": { "type": 0, "city": "서울", "prefix": "123", "kor": "가", "number": "4567" },
  "force": false }
```

배치 기준은 `slotId` 또는 `transform` 중 하나(동시 지정 `400`), `offset`·`plate`·`force` 는 선택.
점유 주차면이면 `409`, `force:true` 면 기존 차를 파괴 후 교체(주차면당 항상 1대). →
`{ car: {id, slotId, transform, offset, carType, color, plate} }` (`id` 는 `car-01`, `car-02`… 순번)

### GET·PATCH·DELETE /scene/cars/:id

- GET → `{ car: {...} }`.
- PATCH = 부분 갱신(넘긴 필드만): `carType` · `color` · `plate`(필드 단위 병합) ·
  `slotId`(주차면 이동 — 변형 유지, `""` = 분리·자리 유지) ·
  `transform`(자유 좌표로 기준 교체 — 붙어 있던 주차면 점유 해제) · `offset`(변형 교체) · `force`.
  배치 필드를 하나도 안 넘기면 차는 움직이지 않는다.
- DELETE → 액터 파괴 + 주차면 해제, `{ "removed": "car-01" }`.

### POST /scene/reset

API 로 스폰한 차량 전부 삭제(레벨에 저작된 액터는 불변) → `{ "cleared": N }`

### POST /scene/cameras

카메라 **런타임 스폰**(v0.1.13) — 레벨·ini 수정 없이 새 시점을 만든다(BEV 데이터셋용 포즈 다양화).

```json
{ "location": { "x": 73, "y": -2015, "z": 1000 }, "yawDeg": 90, "pitchDeg": -30,
  "httpPort": 8287, "mjpegPort": 8297, "fixed": false, "note": "16m-test" }
```

- `location` = 광학중심 월드 cm(레버암 0 이라 액터 위치 = 광학중심), `yawDeg` = 설치 방위(기본 0),
  `pitchDeg` = 설치 하향각(기본 -20, 음수 = 아래 — 내부적으로 tilt 로 이관되어 짐벌에 롤이 안 생긴다).
- **포트는 명시 필수**(자동 부여 없음 — 비결정적). 씬 포트·기존 채널과 겹치면 `400` 에 원인을 싣는다.
- 스폰 즉시 그 포트의 Hucoms CGI·MJPEG 가 살아난다. 응답 = `{ camera: {...} }` (GET /scene/cameras 항목과 동일).

### PATCH·DELETE /scene/cameras/:id

`:id` = 카메라 id 또는 hucomsPort. **스폰 카메라만** 허용 — 레벨 저작 카메라는 `403`
(GET /scene/cameras 의 `spawned` 필드로 구분).

- PATCH = 설치 자세 갱신(넘긴 필드만): `location` / `yawDeg` / `pitchDeg`. 다음 캡처부터 새 시점.
- DELETE = 채널 정리(CGI·MJPEG 포트 닫힘) 후 액터 파괴 → `{ "removed": "PTZCamera_N" }`.

### GET · POST /scene/snapshot

**씬 스냅샷**(v0.1.13) — 서버는 파일을 저장하지 않는다. GET 이 복원 가능한 JSON 을 주고,
POST 가 그 JSON 을 그대로 받아 복원한다(호출자가 저장 책임 — 재현 실험의 단위).

- GET → `{ level, pluginVersion, savedAtUtc, cars:[...(자유 배치 차량엔 baseTransform 포함)],
  cameras:[스폰 카메라의 {id, location, yawDeg, pitchDeg, httpPort, mjpegPort, fixed}] }`
- POST(위 JSON 그대로) → 차량은 전량 리셋 후 재배치(id 는 재부여), 카메라는 `httpPort` 를 키로
  reconcile(같은 포트 = 이동, 새 포트 = 스폰, 스냅샷에 없는 스폰 카메라 = 제거).
  레벨이 다르면 `409`(`force:true` 로 강행). 응답 = `{ cars:{restored}, cameras:{spawned,moved,removed}, failures:[] }`.
- 범위 밖: 레벨 저작 액터, 현재 PTZ 상태(Hucoms 축 — 필요하면 goptzfpos 로 별도 복원).

### GET /scene · GET /scene/help

이 문서.

## 카메라 제어·영상은 씬 API 밖이다 (Hucoms CGI)

PTZ 제어와 영상은 씬 API 가 아니라 **카메라별 Hucoms CGI** 다. 각 카메라의 실효 포트는
`/scene/cameras` 의 `hucomsPort`(CGI)·`mjpegPort`(스트림)로 조인한다.

| 목적 | 호출 |
|---|---|
| 현재 PTZ 조회 | `GET :{hucomsPort}/cgi-bin/control/ptzf_status.cgi?action=getptzfpos` (응답 `panpos = N` 줄 형식) |
| 절대 이동 | `GET :{hucomsPort}/cgi-bin/control/ptzf_status.cgi?action=goptzfpos&panpos=&tiltpos=&zoompos=` |
| 클릭 센터링 | `GET :{hucomsPort}/cgi-bin/control/ptz_centering.cgi?action=setcenter&x=&y=` (1920x1080 프레임 기준) |
| 스냅샷 1장 | `GET :{hucomsPort}/cgi-bin/image/jpeg.cgi` |
| 연속 영상 | `:{mjpegPort}` 의 MJPEG 스트림 |

단위: `panpos`/`tiltpos` 는 1/100 deg 정수(**tiltpos 증가 = 아래를 봄**), `zoompos` 는 불투명
눈금이라 반드시 `/scene/cameras` 의 화각표로만 해석한다.

## 빠른 시작 (curl)

```bash
P=http://127.0.0.1:{{SCENE_PORT}}

curl $P/scene/catalog                  # 차종·색·번호판 어휘 + 버전
curl $P/scene/slots                    # 주차면 id 목록
# 주차면에 스폰 — 우측 12cm·뒤 16cm 비껴, 7도 틀어서
curl -X POST $P/scene/cars -H "content-type: application/json" \
  -d '{"slotId":"<slots 의 id>","carType":3,"color":4,
       "offset":{"location":{"x":-16,"y":12},"rotation":{"yaw":7}}}'
# 반대 방향으로 돌려 세우기 (자리는 그대로)
curl -X PATCH $P/scene/cars/car-01 -H "content-type: application/json" \
  -d '{"offset":{"rotation":{"yaw":180}}}'
curl -X POST $P/scene/reset            # 전부 정리
```
