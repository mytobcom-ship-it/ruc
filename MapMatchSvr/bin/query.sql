-- MapMatchSvr query.sql (PostgreSQL / libpq: $1, $2, ...)
-- 근거: doc/RUC_위치검증서버_테이블설계서_v1.3.docx §2.1, §3.1
--
-- TRIP_EVENT (SMALLINT): 0=START, 1=NONE, 2=END
-- DRIVE_STATUS (SMALLINT): 0=ON_ROAD, 1=IDLE, 2=PARKED, 3=TUNNELING
-- MATCH_STATUS (SMALLINT): 0=PENDING, 1=MATCHED, 2=PROCESSING, 3=SKIP, 4=ERROR
--
-- 처리 흐름
--   0) rawgps_recover : 기동 시 1회 좀비 PROCESSING → PENDING 전량 복구 (안전망)
--   1) rawgps_select  : PENDING(0) 만 예약(Reserve) (UPDATE ... RETURNING)
--                       → PROCESSING 재예약 없음 (RESERVE_DT 없이 중복 예약 방지)
--                       → 런타임 좀비는 재기동 시 recover 후 재처리
--   2) rawgps_update  : TRIP_ID batch 맵매칭 완료 후 MATCH_STATUS(1/3/4) 일괄 갱신
--   3) charge_insert  : 맵매칭+위반탐지 후 과금 대상 INSERT (§3.1 ④)
--                       → #10 보류: CHARGE_TARGET 테이블 재설계 후 Worker INSERT 배선 예정
--
-- ThreadPool: hash(DEVICE_KEY) % N → Enqueue(worker_id), batch = 동일 TRIP_ID 묶음
-- Feeder backpressure (config.ini [feeder]):
--   queue_pause_count 이상 → DB 조회 중단, queue_busy_min~queue_busy_max 적응 대기
--   fetch_interval → 큐 여유 시 DB 조회 간격 (연속 조회 방지)
-- TRIP_ID: {DEVICE_KEY}_{YYYYMMDDHH24MISS} (수집서버 START 시 적재)
-- 좌표 컬럼 (GPS_LAT/LON, MATCH_LAT/LON): NUMERIC(10,6) — 소수 6자리 (설계서 v1.3 §2.1)
--
-- rawgps_select RETURNING 컬럼 순서 (설계서 v1.3 §2.1)
--   0:DEVICE_KEY 1:GPS_DT 2:GPS_SEQ 3:TRIP_ID 4:TRIP_EVENT 5:DRIVE_STATUS
--   6:GPS_LAT 7:MATCH_LAT 8:GPS_LON 9:MATCH_LON 10:INTERSECT_LEN 11:RAW_VLD
--   12:SPEED_KMH 13:HEADING 14:ALTITUDE_M 15:ACCURACY_M 16:BATTERY
--   17:RECV_DT 18:MATCH_STATUS

-- ── 0. 좀비 PROCESSING 복구 (기동 시 1회 안전망) ──────────────────────────
-- [rawgps_recover]
-- 서버가 PROCESSING 으로 예약한 뒤 완료 전에 종료되면 그 행이 PROCESSING 에 갇힌다.
-- 기동 시 워커가 없으므로 PROCESSING 전량을 PENDING 으로 되돌린다 (재기동 후 재처리).
-- 런타임 poll(rawgps_select) 에서는 PROCESSING 재예약을 하지 않는다.
[rawgps_recover]
UPDATE MATCHING.RAW_GPS_LOG
SET
	MATCH_STATUS = 0
WHERE
	MATCH_STATUS = 2;

-- ── 1. 조회 + 예약(Reserve) ─────────────────────────────────────────────────
-- [rawgps_select]  (UPDATE ... RETURNING : PENDING 조회와 동시에 PROCESSING 으로 예약)
-- 내부 SELECT 로 처리 대상(정렬·LIMIT·SKIP LOCKED)을 고르고, 바깥 UPDATE 가 예약하며
-- RETURNING 으로 그 행 데이터를 반환한다. 단일 statement 라 원자적이다.
-- 대상 = PENDING(0) 만. PROCESSING(2) 재예약 없음 (RECV_DT 기반 타임아웃 미사용).
-- DRIVE_STATUS=1(IDLE) 행은 조회·예약 제외 (WHERE DRIVE_STATUS <> 1).
-- $1 = LIMIT
[rawgps_select]
UPDATE MATCHING.RAW_GPS_LOG AS U
SET
	MATCH_STATUS = 2
FROM (
	SELECT DEVICE_KEY, GPS_DT, GPS_SEQ
	FROM MATCHING.RAW_GPS_LOG
	WHERE MATCH_STATUS = 0
	  AND DRIVE_STATUS <> 1
	ORDER BY DEVICE_KEY ASC, TRIP_ID ASC, GPS_DT ASC, GPS_SEQ ASC
	LIMIT $1
	FOR UPDATE SKIP LOCKED
) AS S
WHERE U.DEVICE_KEY = S.DEVICE_KEY
  AND U.GPS_DT = S.GPS_DT
  AND U.GPS_SEQ = S.GPS_SEQ
RETURNING
	DEVICE_KEY,
	TO_CHAR(GPS_DT, 'YYYYMMDDHH24MISS')  AS GPS_DT,
	GPS_SEQ,
	TRIP_ID,
	TRIP_EVENT,
	DRIVE_STATUS,
	GPS_LAT,
	MATCH_LAT,
	GPS_LON,
	MATCH_LON,
	INTERSECT_LEN,
	RAW_VLD,
	SPEED_KMH,
	HEADING,
	ALTITUDE_M,
	ACCURACY_M,
	BATTERY,
	TO_CHAR(RECV_DT, 'YYYYMMDDHH24MISS') AS RECV_DT,
	MATCH_STATUS;

-- ── 2. 결과 갱신 ──────────────────────────────────────────────────────────
-- [rawgps_update] TRIP_ID batch 맵매칭 결과 일괄 갱신 (UNNEST 배열)
-- TRIP_ID 는 수집서버가 적재하므로 위치검증서버는 갱신하지 않는다.
-- 처리 중(PROCESSING=2) → MATCHED(1) / SKIP(3) / ERROR(4) 로 전이
-- $1=DEVICE_KEY[], $2=GPS_DT[](YYYYMMDDHH24MISS), $3=GPS_SEQ[](TEXT→BIGINT)
-- $4=MATCH_STATUS[](SMALLINT), $5=INTERSECT_LEN[](''=미갱신)
-- $6=MATCH_LAT[](NUMERIC(10,6)), $7=MATCH_LON[](NUMERIC(10,6))
-- PK (DEVICE_KEY, GPS_DT, GPS_SEQ) 로 일괄 갱신. 대상 행은 MATCH_STATUS=2(PROCESSING) 만
[rawgps_update]
UPDATE MATCHING.RAW_GPS_LOG AS T
SET
	MATCH_STATUS = V.MATCH_STATUS,
	MATCH_LAT = CASE
		WHEN V.MATCH_STATUS = 1 AND V.MATCH_LAT <> '' THEN V.MATCH_LAT::NUMERIC
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LAT
	END,
	MATCH_LON = CASE
		WHEN V.MATCH_STATUS = 1 AND V.MATCH_LON <> '' THEN V.MATCH_LON::NUMERIC
		WHEN V.MATCH_STATUS IN (3, 4) THEN NULL
		ELSE T.MATCH_LON
	END,
	INTERSECT_LEN = CASE
		WHEN V.INTERSECT_LEN = '' THEN T.INTERSECT_LEN
		ELSE V.INTERSECT_LEN::INTEGER
	END
FROM (
	SELECT
		U.DEVICE_KEY,
		U.GPS_DT,
		U.GPS_SEQ::BIGINT AS GPS_SEQ,
		U.MATCH_STATUS::SMALLINT AS MATCH_STATUS,
		U.INTERSECT_LEN,
		U.MATCH_LAT,
		U.MATCH_LON
	FROM UNNEST(
		$1::TEXT[],
		$2::TEXT[],
		$3::TEXT[],
		$4::TEXT[],
		$5::TEXT[],
		$6::TEXT[],
		$7::TEXT[]
	) AS U(
		DEVICE_KEY,
		GPS_DT,
		GPS_SEQ,
		MATCH_STATUS,
		INTERSECT_LEN,
		MATCH_LAT,
		MATCH_LON
	)
) AS V
WHERE T.DEVICE_KEY = V.DEVICE_KEY
  AND T.GPS_DT = TO_TIMESTAMP(V.GPS_DT, 'YYYYMMDDHH24MISS')
  AND T.GPS_SEQ = V.GPS_SEQ
  AND T.MATCH_STATUS = 2;

-- ── 3. 과금 대상 INSERT (C++ 호출 보류 — 테이블 재설계 후 연동) ─────────────
-- [charge_insert]
-- §2.2 CHARGE_TARGET, §3.1 ④ CHARGE_STATUS=PENDING
-- #10: SQL 정의만 유지. RawLogWorker INSERT 호출은 테이블 재설계 이후 예정.
-- $1  = TRIP_ID
-- $2  = DEVICE_KEY
-- $3  = TRIP_SEQ
-- $4  = CHARGE_TYPE      (NODE_STEP / OPEN_ROAD / CLOSED_ROAD / SPEED / PARKING)
-- $5  = CHARGE_UNIT      (NODE / LINK)
-- $6  = LINK_ID          (LINK 일 때, NODE 면 '')
-- $7  = FROM_ID
-- $8  = TO_ID
-- $9  = FROM_LAT
-- $10 = FROM_LON
-- $11 = TO_LAT
-- $12 = TO_LON
-- $13 = DIST_M
-- $14 = SPEED_KMH
-- $15 = SPEED_LIMIT_KMH
-- $16 = STAY_SECONDS
-- $17 = ZONE_ID          (없으면 '')
-- $18 = ZONE_NAME        (없으면 '')
-- $19 = OCCUR_DT         ('YYYYMMDDHH24MISS')
-- $20 = TRIP_START_DT    ('YYYYMMDDHH24MISS')
-- $21 = TRIP_END_DT      ('YYYYMMDDHH24MISS', 없으면 '')
-- $22 = CHARGE_STATUS    (예: 'PENDING')
-- REG_DT, UPD_DT 는 NOW() 자동 적재 ($23/$24 없음)
[charge_insert]
INSERT INTO MATCHING.CHARGE_TARGET (
	TRIP_ID,
	DEVICE_KEY,
	TRIP_SEQ,
	CHARGE_TYPE,
	CHARGE_UNIT,
	LINK_ID,
	FROM_ID,
	TO_ID,
	FROM_LAT,
	FROM_LON,
	TO_LAT,
	TO_LON,
	DIST_M,
	SPEED_KMH,
	SPEED_LIMIT_KMH,
	STAY_SECONDS,
	ZONE_ID,
	ZONE_NAME,
	OCCUR_DT,
	TRIP_START_DT,
	TRIP_END_DT,
	CHARGE_STATUS,
	REG_DT,
	UPD_DT
) VALUES (
	$1, 
	$2, 
	$3::INTEGER, 
	$4, 
	$5,
	NULLIF($6, ''), 
	$7, 
	$8,
	$9::NUMERIC, 
	$10::NUMERIC,
	$11::NUMERIC, 
	$12::NUMERIC,
	$13::INTEGER, 
	$14::NUMERIC,
	$15::NUMERIC, 
	$16::INTEGER,
	NULLIF($17, ''), 
	NULLIF($18, ''),
	TO_TIMESTAMP($19, 'YYYYMMDDHH24MISS'),
	TO_TIMESTAMP($20, 'YYYYMMDDHH24MISS'),
	CASE 
		WHEN $21 = '' THEN NULL
		ELSE TO_TIMESTAMP($21, 'YYYYMMDDHH24MISS') 
	END,
	$22, 
	NOW(), 
	NOW()
);
