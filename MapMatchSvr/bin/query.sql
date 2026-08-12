-- MapMatchSvr query.sql (PostgreSQL / libpq: $1, $2, ...)
-- 근거: doc/RUC_위치검증서버_테이블설계서_v1.3.docx §2.1, §3.1
--
-- TRIP_EVENT   : 0=START, 1=NONE, 2=END
-- DRIVE_STATUS : 0=ON_ROAD, 1=IDLE, 2=PARKED, 3=TUNNELING, 4=OFF_ROAD(비과금 구역)
-- MATCH_STATUS : 0=PENDING, 1=MATCHED, 2=PROCESSING, 3=SKIP, 4=ERROR
--
-- 처리 흐름
--   0) rawgps_recover     : 기동 시 PROCESSING(2) → PENDING(0) 복구
--   1) rawgps_select      : PENDING 예약(Reserve) + RETURNING
--   2) rawgps_update      : 맵매칭 결과 일괄 갱신 (MATCHED/SKIP/ERROR)
--   3) gate_select         : BASE_TOLLGATE 전량 조회 — CChargeDataLoader 기동 시(또는 주기 재조회 시)
--                           1회 실행, 결과를 인메모리 캐시. 아직 CChargeDataLoader 배선 전(2026-08-12 최정우 추가)
--   4) zone_select         : BASE_ROADLINK 전량 조회 — CChargeDataLoader::LoadZones() 가 gate_select 와
--                           동일 주기로 실행, 결과를 인메모리 캐시(road_id 키) (2026-08-12 최정우 추가)
--   5) charge_insert      : 개방형(OPEN_ROAD) 게이트 통과 bulk INSERT — CRawLogWorker::BulkInsertCharges()
--                           가 배치 종료 시(rawgps_update 성공 후) 실행. 폐쇄형/구간단속/주정차는 미구현
--                           (2026-08-12 최정우 수정 — 개방형 한정 구현)
--
-- DRIVE_STATUS=4 : 맵매칭·결과 저장은 0/2/3 과 동일, 과금 판별·CHARGE_TARGET 적재만 생략 (2026-07-10 최정우 수정)
--
-- rawgps_select RETURNING 컬럼 순서 (= RUC.PRIM_RAWGPS 컬럼 순서 — 로컬 ruc DB 전환, 2026-08-11 최정우 수정)
--   0:TRIP_ID 1:GPS_SEQ 2:DEVICE_KEY 3:GPS_DT 4:TRIP_EVENT 5:DRIVE_STATUS
--   6:GPS_LAT 7:MATCH_LAT 8:GPS_LON 9:MATCH_LON 10:INTERSECT_LEN(GPS↔세그먼트 교차점 거리,m) 11:RAW_VLD
--   12:SPEED_KMH 13:OBD_SPEED_KMH(차량 OBD 순간속도 — NULL 아니면 SPEED_KMH 보다 우선) 14:HEADING
--   15:ALTITUDE_M 16:ACCURACY_M 17:BATTERY 18:RECV_DT 19:MATCH_STATUS

-- ── 0. 좀비 PROCESSING 복구 ────────────────────────────────────────────────
[rawgps_recover]
UPDATE RUC.PRIM_RAWGPS
SET
	MATCH_STATUS = 0
WHERE
	MATCH_STATUS = 2;

-- ── 1. 조회 + 예약(Reserve) ────────────────────────────────────────────────
-- [rawgps_select] PENDING(0) → PROCESSING(2) 예약, RETURNING 으로 행 반환
-- 제외: DRIVE_STATUS=1(IDLE), RAW_VLD<>TRUE. 4(OFF_ROAD)는 맵매칭 대상 포함 (2026-07-10 최정우 수정)
-- $1 = LIMIT
[rawgps_select]
UPDATE RUC.PRIM_RAWGPS AS U
SET
	MATCH_STATUS = 2
FROM (
	SELECT TRIP_ID, GPS_SEQ
	FROM RUC.PRIM_RAWGPS
	WHERE MATCH_STATUS = 0
	  AND DRIVE_STATUS <> 1
	  AND RAW_VLD IS TRUE
	ORDER BY DEVICE_KEY ASC, TRIP_ID ASC, GPS_DT ASC, GPS_SEQ ASC
	LIMIT $1
	FOR UPDATE SKIP LOCKED
) AS S
WHERE U.TRIP_ID = S.TRIP_ID
  AND U.GPS_SEQ = S.GPS_SEQ
RETURNING
	U.TRIP_ID,
	U.GPS_SEQ,
	U.DEVICE_KEY,
	U.GPS_DT,
	U.TRIP_EVENT,
	U.DRIVE_STATUS,
	U.GPS_LAT,
	U.MATCH_LAT,
	U.GPS_LON,
	U.MATCH_LON,
	U.INTERSECT_LEN,
	U.RAW_VLD,
	U.SPEED_KMH,
	U.OBD_SPEED_KMH,
	U.HEADING,
	U.ALTITUDE_M,
	U.ACCURACY_M,
	U.BATTERY,
	U.RECV_DT,
	U.MATCH_STATUS;

-- ── 2. 결과 갱신 ──────────────────────────────────────────────────────────
-- [rawgps_update] PROCESSING(2) → MATCHED(1)/SKIP(3)/ERROR(4). DRIVE_STATUS=4 동일 갱신 (2026-07-10 최정우 수정)
-- 파라미터는 PRIM_RAWGPS 컬럼 순서대로 배열 (2026-07-15 최정우 재정렬)
-- $1=TRIP_ID[], $2=GPS_SEQ[], $3=MATCH_LAT[], $4=MATCH_LON[], $5=INTERSECT_LEN[](GPS↔세그먼트 교차점 거리,m), $6=MATCH_LINK_ID[], $7=MATCH_STATUS[]
[rawgps_update]
UPDATE RUC.PRIM_RAWGPS AS T
SET
	MATCH_LAT = CASE
		WHEN V.MATCH_LAT <> '' THEN V.MATCH_LAT::NUMERIC
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LAT
	END,
	MATCH_LON = CASE
		WHEN V.MATCH_LON <> '' THEN V.MATCH_LON::NUMERIC
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LON
	END,
	INTERSECT_LEN = CASE
		WHEN V.INTERSECT_LEN <> '' THEN V.INTERSECT_LEN::INTEGER
		WHEN V.MATCH_STATUS IN (3, 4) THEN 0
		ELSE T.INTERSECT_LEN
	END,
	MATCH_LINK_ID = CASE
		WHEN V.MATCH_LINK_ID <> '' THEN V.MATCH_LINK_ID
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LINK_ID
	END,
	MATCH_STATUS = V.MATCH_STATUS
FROM (
	SELECT
		U.TRIP_ID,
		U.GPS_SEQ::BIGINT AS GPS_SEQ,
		U.MATCH_LAT,
		U.MATCH_LON,
		U.INTERSECT_LEN,
		U.MATCH_LINK_ID,
		U.MATCH_STATUS::SMALLINT AS MATCH_STATUS
	FROM UNNEST(
		$1::TEXT[],
		$2::TEXT[],
		$3::TEXT[],
		$4::TEXT[],
		$5::TEXT[],
		$6::TEXT[],
		$7::TEXT[]
	) AS U(
		TRIP_ID,
		GPS_SEQ,
		MATCH_LAT,
		MATCH_LON,
		INTERSECT_LEN,
		MATCH_LINK_ID,
		MATCH_STATUS
	)
) AS V
WHERE T.TRIP_ID = V.TRIP_ID
  AND T.GPS_SEQ = V.GPS_SEQ
  AND T.MATCH_STATUS = 2;

-- ── 3. 과금 게이트 전량 조회 ─────────────────────────────────────────────
-- [gate_select] BASE_TOLLGATE 전량 — CChargeDataLoader::LoadGates() 가 실행 (2026-08-12 최정우 추가)
-- link_id 없는 행(좌표거리 폴백만 가능한 게이트)은 일단 제외 — CChargeDataLoader.cpp LoadGates() TODO 참고
-- 컬럼 순서(ChargeDataLoader.cpp LoadGates() 와 반드시 일치): 0:TOLLGATE_ID 1:ROAD_ID 2:GATE_DIV 3:LON 4:LAT 5:LINK_ID
[gate_select]
SELECT
	TOLLGATE_ID,
	ROAD_ID,
	GATE_DIV,
	LON,
	LAT,
	LINK_ID
FROM RUC.BASE_TOLLGATE
WHERE USE_YN = 'Y'
ORDER BY TOLLGATE_ID ASC;

-- ── 4. 과금 구역(도로/링크) 전량 조회 ────────────────────────────────────────
-- [zone_select] BASE_ROADLINK 전량 — CChargeDataLoader::LoadZones() 가 실행 (2026-08-12 최정우 추가)
-- 컬럼 순서(ChargeDataLoader.cpp LoadZones() 와 반드시 일치): 0:ROAD_ID 1:ROAD_KIND 2:ROAD_NM
--   3:GEOM_TYPE 4:SPEED_LIMIT_KMH 5:USE_YN 6:LINK_IDS 7:COORDS
[zone_select]
SELECT
	ROAD_ID,
	ROAD_KIND,
	ROAD_NM,
	GEOM_TYPE,
	SPEED_LIMIT_KMH,
	USE_YN,
	LINK_IDS,
	COORDS
FROM RUC.BASE_ROADLINK
WHERE USE_YN = 'Y'
ORDER BY ROAD_ID ASC;

-- ── 5. 개방형 게이트 통과 bulk INSERT ─────────────────────────────────────
-- [charge_insert] PRIM_CHARGEHAND — CRawLogWorker::BulkInsertCharges() 가 실행 (2026-08-12 최정우 추가)
-- 파라미터 순서(RawLogWorker.cpp CHARGE_INSERT_ROW/BulkInsertCharges() 와 반드시 일치):
--   $1=TRIP_ID[] $2=DEVICE_KEY[] $3=TRIP_SEQ[] $4=CHARGE_TYPE[](1=OPEN_ROAD 고정) $5=CHARGE_UNIT[](1=LINK 고정)
--   $6=LINK_ID[] $7=FROM_ID[](링크 시작노드) $8=TO_ID[](링크 종료노드) $9=FROM_LAT[] $10=FROM_LON[]
--   $11=TO_LAT[] $12=TO_LON[] $13=ZONE_ID[] $14=ZONE_NAME[] $15=OCCUR_DT[] $16=TRIP_START_DT[] $17=TOLLGATE_ID[]
-- ON CONFLICT: 배치 release 후 재시도되는 케이스의 중복 INSERT 방어(정상 흐름에서는 trip_seq 가 매번 새 값)
[charge_insert]
INSERT INTO RUC.PRIM_CHARGEHAND (
	TRIP_ID, DEVICE_KEY, TRIP_SEQ, CHARGE_TYPE, CHARGE_UNIT, LINK_ID, FROM_ID, TO_ID,
	FROM_LAT, FROM_LON, TO_LAT, TO_LON, ZONE_ID, ZONE_NAME, OCCUR_DT, TRIP_START_DT, TOLLGATE_ID
)
SELECT
	U.TRIP_ID,
	U.DEVICE_KEY,
	U.TRIP_SEQ::INTEGER,
	U.CHARGE_TYPE::SMALLINT,
	U.CHARGE_UNIT::SMALLINT,
	NULLIF(U.LINK_ID, ''),
	U.FROM_ID,
	U.TO_ID,
	U.FROM_LAT::NUMERIC,
	U.FROM_LON::NUMERIC,
	U.TO_LAT::NUMERIC,
	U.TO_LON::NUMERIC,
	NULLIF(U.ZONE_ID, ''),
	NULLIF(U.ZONE_NAME, ''),
	U.OCCUR_DT,
	U.TRIP_START_DT,
	NULLIF(U.TOLLGATE_ID, '')
FROM UNNEST(
	$1::TEXT[], $2::TEXT[], $3::TEXT[], $4::TEXT[], $5::TEXT[], $6::TEXT[], $7::TEXT[],
	$8::TEXT[], $9::TEXT[], $10::TEXT[], $11::TEXT[], $12::TEXT[], $13::TEXT[], $14::TEXT[],
	$15::TEXT[], $16::TEXT[], $17::TEXT[]
) AS U(
	TRIP_ID, DEVICE_KEY, TRIP_SEQ, CHARGE_TYPE, CHARGE_UNIT, LINK_ID, FROM_ID, TO_ID,
	FROM_LAT, FROM_LON, TO_LAT, TO_LON, ZONE_ID, ZONE_NAME, OCCUR_DT, TRIP_START_DT, TOLLGATE_ID
)
ON CONFLICT (trip_id, device_key, trip_seq) DO NOTHING;
