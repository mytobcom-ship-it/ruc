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
--                           1회 실행, 결과를 인메모리 캐시 (2026-08-12 최정우 추가)
--   4) zone_select         : BASE_ROADLINK 전량 조회 — CChargeDataLoader::LoadZones() 가 gate_select 와
--                           동일 주기로 실행, 결과를 인메모리 캐시(road_id 키) (2026-08-12 최정우 추가)
--   5) charge_insert      : 개방형·폐쇄형·구간단속·주정차 4유형 공용 bulk INSERT —
--                           CRawLogWorker::BulkInsertCharges() 가 배치 종료 시(rawgps_update 성공 후) 실행
--                           (2026-08-13 최정우 수정 — 주정차 구현으로 4유형 전부 완료)
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
-- 제외 조건 없음 — RAW_VLD 도 값과 무관하게 전부 선택 대상(2026-08-19 최정우 수정). 이전엔
--   WHERE 에 RAW_VLD IS TRUE 를 둬서 RAW_VLD=false 행은 애초에 조회·예약이 안 됐음 — 그 결과
--   RawLogWorker::ShouldSkipGpsInput() 이 RAW_VLD=false 를 SKIP(3) 처리하는 로직이 이미
--   있는데도 그 행들이 이 함수까지 도달하지 못해 죽은 코드였고, 해당 행은 영원히 PENDING(0)에
--   갇혀 trip 이 완료돼도(END 도달) "진행중"으로 잘못 표시되는 원인이었음(실측: END 행 자체가
--   RAW_VLD=false 인 trip 은 세션 종료 처리조차 못 받음). 이제 RAW_VLD=false 행도 선택해와서
--   ShouldSkipGpsInput() 이 정상적으로 SKIP 처리하게 함.
-- DRIVE_STATUS 는 0(ON_ROAD)/1(IDLE)/2(PARKED)/3(TUNNELING)/4(OFF_ROAD) 전부 맵매칭 대상
--   (2026-08-13 최정우 수정 — 이전엔 DRIVE_STATUS=1(IDLE) 만 제외했었음. 서행 구간(IDLE)이
--   맵매칭·주정차 판정에서 통째로 안 보이는 문제가 있어 사용자 지시로 제외 조건 삭제 —
--   "모든 맵 매칭 시 DRIVE_STATUS 에서 0/1/2 는 포함되어야 한다")
-- $1 = LIMIT
-- 2026-08-18 최정우 수정 — 한때 TRIP_ID를 DEVICE_KEY_{시각} 형식으로 여기서 보정했었으나,
--   RawLogWorker::IsValidTripIdForDevice() 의 검증 기준 자체가 CAR_SEQ_NO(6자리 숫자)_{시각}
--   형식을 요구하도록 바뀌어(같은 날 수정) 방향이 반대가 됨 — 그 보정 로직을 남겨두면 유효한
--   CAR_SEQ_NO 형식을 오히려 무효한 DEVICE_KEY 형식으로 바꿔버리는 역효과가 나서 제거함.
--   원본(보정 없는) 형태로 되돌림.
[rawgps_select]
UPDATE RUC.PRIM_RAWGPS AS U
SET
	MATCH_STATUS = 2
FROM (
	SELECT TRIP_ID, GPS_SEQ
	FROM RUC.PRIM_RAWGPS
	WHERE MATCH_STATUS = 0
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
--   3:GEOM_TYPE 4:SPEED_LIMIT_KMH 5:USE_YN 6:LINK_IDS 7:COORDS 8:LENGTH_M
--   9:FIRST_LON 10:FIRST_LAT 11:LAST_LON 12:LAST_LAT
-- LENGTH_M: coords 배열의 연속 정점 사이 하버사인 거리 합산(폴리라인 실거리) — 폐쇄형·구간단속
--   dist_m 산출에 사용(실측 운영 데이터와 거의 일치 확인됨, 2026-08-12 최정우 추가).
--   PostGIS 없어 직접 구현: 정점을 LEAD() 로 다음 정점과 짝지어 각 구간 하버사인 거리 계산 후 SUM.
--   주의: BASE_ROADLINK.coords 는 road_link.coords 와 중첩 구조가 다름 — [[lon,lat],...] 형태로
--   바로 좌표쌍 배열이라 coords->0 로 한 번 더 까면 안 됨(road_link 는 [[[lon,lat],...]] 라 ->0 필요.
--   이 차이를 못 보고 처음에 ->0 을 붙였다가 전부 대척점 거리(20037508m)로 잘못 나온 걸 실측으로 확인·수정함).
-- FIRST/LAST: coords 배열의 첫/마지막 정점 — 구간단속 from_lat/lon·to_lat/lon 에 사용
--   (실측 확인: 게이트 좌표가 아니라 구역 폴리라인 자체의 시작·끝점, 2026-08-12 최정우 추가)
[zone_select]
SELECT
	ROAD_ID,
	ROAD_KIND,
	ROAD_NM,
	GEOM_TYPE,
	SPEED_LIMIT_KMH,
	USE_YN,
	LINK_IDS,
	COORDS,
	(
		SELECT COALESCE(SUM(
			2 * 6378137.0 * asin(least(1.0, sqrt(
				sin(radians((nxt->>1)::float8 - (cur->>1)::float8) / 2) ^ 2 +
				cos(radians((cur->>1)::float8)) * cos(radians((nxt->>1)::float8)) *
				sin(radians((nxt->>0)::float8 - (cur->>0)::float8) / 2) ^ 2
			)))
		), 0)
		FROM (
			SELECT elem AS cur, LEAD(elem) OVER (ORDER BY ord) AS nxt
			FROM jsonb_array_elements(COORDS) WITH ORDINALITY AS t(elem, ord)
		) pairs
		WHERE nxt IS NOT NULL
	) AS LENGTH_M,
	COORDS->0->>0 AS FIRST_LON,
	COORDS->0->>1 AS FIRST_LAT,
	COORDS->-1->>0 AS LAST_LON,
	COORDS->-1->>1 AS LAST_LAT
FROM RUC.BASE_ROADLINK
WHERE USE_YN = 'Y'
ORDER BY ROAD_ID ASC;

-- ── 5. 과금 판정 결과 bulk INSERT ─────────────────────────────────────
-- [charge_insert] PRIM_CHARGEHAND — CRawLogWorker::BulkInsertCharges() 가 실행 (2026-08-12 최정우 추가)
-- 개방형·폐쇄형·구간단속·주정차 4유형 공용 — 폐쇄형은 DIST_M/ENTRY_TOLLGATE_ID/EXIT_TOLLGATE_ID 도 채움,
-- 주정차는 DIST_M(누적거리)/SPEED_KMH(평균속도) 채우고 CHARGE_YN='Y'/CHARGE_STATUS='0' 고정 (2026-08-13 최정우 수정)
-- 2026-08-20 최정우 수정 — FROM_ID/TO_ID/FROM_LAT/FROM_LON/TO_LAT/TO_LON 6개 컬럼 NOT NULL 해제
-- (사용자 지시 — 폐쇄형/구간단속 미완료(AUDIT) 건은 출구 쪽 아는 값이 없는데 억지로 ''/0 을 넣던
-- 것을 진짜 NULL로 저장하도록). RawLogWorker.cpp 는 모르는 값이면 빈 문자열("")을 넘기고, 여기서
-- NULLIF(...,'')로 그 빈 문자열을 NULL로 변환 — FROM_LAT 등 NUMERIC 컬럼은 캐스팅 전에 먼저
-- NULLIF 를 걸어야 함(빈 문자열을 바로 ::NUMERIC 캐스팅하면 에러가 남).
-- 파라미터 순서(RawLogWorker.cpp CHARGE_INSERT_ROW/BulkInsertCharges() 와 반드시 일치):
--   $1=TRIP_ID[] $2=DEVICE_KEY[] $3=TRIP_SEQ[] $4=CHARGE_TYPE[](1=OPEN_ROAD/2=CLOSED_ROAD) $5=CHARGE_UNIT[](0=NODE/1=LINK,실측 확인)
--   $6=LINK_ID[](실측상 항상 빈값) $7=FROM_ID[](모르면 NULL) $8=TO_ID[](모르면 NULL) $9=FROM_LAT[](모르면 NULL) $10=FROM_LON[](모르면 NULL)
--   $11=TO_LAT[](모르면 NULL) $12=TO_LON[](모르면 NULL) $13=ZONE_ID[] $14=ZONE_NAME[] $15=DIST_M[](개방형은 빈값) $16=SPEED_KMH[](계산 불가 시 빈값)
--   $17=SPEED_LIMIT_KMH[] $18=OCCUR_DT[] $19=TRIP_START_DT[] $20=TOLLGATE_ID[](실측상 항상 빈값)
--   $21=ENTRY_TOLLGATE_ID[](폐쇄형 전용, 개방형은 빈값) $22=EXIT_TOLLGATE_ID[](폐쇄형 전용) $23=REG_DT[] $24=UPD_DT[](REG_DT와 항상 동일)
--   $25=CHARGE_YN[](빈값=DB기본 Y, 폐쇄형 입/출구 게이트 이상 시 "N") $26=CHARGE_STATUS[](빈값=DB기본 0, 이상 시 "4"=SKIP)
--   $27=STAY_SECONDS[](주정차 전용 — 체류시간 초. 컬럼 코멘트: "체류 시간(초). 주정차 위반 판단". 다른 3종은 빈값=DB기본 0) (2026-08-13 최정우 추가)
--   $28=TRIP_END_DT[](주정차 TTL 만료 강제마감 전용 — 더 이상 GPS 수신 불가로 판단한 시각. 그 외는
--     빈값=NULL 유지, 실제 TRIP_EVENT=2 수신 시 [trip_end] UPDATE 가 별도로 채움) (2026-08-13 최정우 추가)
-- ON CONFLICT: 배치 release 후 재시도되는 케이스의 중복 INSERT 방어(정상 흐름에서는 trip_seq 가 매번 새 값)
[charge_insert]
INSERT INTO RUC.PRIM_CHARGEHAND (
	TRIP_ID, DEVICE_KEY, TRIP_SEQ, CHARGE_TYPE, CHARGE_UNIT, LINK_ID, FROM_ID, TO_ID,
	FROM_LAT, FROM_LON, TO_LAT, TO_LON, ZONE_ID, ZONE_NAME, DIST_M, SPEED_KMH, SPEED_LIMIT_KMH,
	OCCUR_DT, TRIP_START_DT, TOLLGATE_ID, ENTRY_TOLLGATE_ID, EXIT_TOLLGATE_ID, REG_DT, UPD_DT,
	CHARGE_YN, CHARGE_STATUS, STAY_SECONDS, TRIP_END_DT
)
SELECT
	U.TRIP_ID,
	U.DEVICE_KEY,
	U.TRIP_SEQ::INTEGER,
	U.CHARGE_TYPE::SMALLINT,
	U.CHARGE_UNIT::SMALLINT,
	NULLIF(U.LINK_ID, ''),
	NULLIF(U.FROM_ID, ''),
	NULLIF(U.TO_ID, ''),
	NULLIF(U.FROM_LAT, '')::NUMERIC,
	NULLIF(U.FROM_LON, '')::NUMERIC,
	NULLIF(U.TO_LAT, '')::NUMERIC,
	NULLIF(U.TO_LON, '')::NUMERIC,
	NULLIF(U.ZONE_ID, ''),
	NULLIF(U.ZONE_NAME, ''),
	CASE WHEN U.DIST_M <> '' THEN U.DIST_M::INTEGER ELSE 0 END,
	CASE WHEN U.SPEED_KMH <> '' THEN U.SPEED_KMH::SMALLINT ELSE 0 END,
	CASE WHEN U.SPEED_LIMIT_KMH <> '' THEN U.SPEED_LIMIT_KMH::SMALLINT ELSE 0 END,
	U.OCCUR_DT,
	U.TRIP_START_DT,
	NULLIF(U.TOLLGATE_ID, ''),
	NULLIF(U.ENTRY_TOLLGATE_ID, ''),
	NULLIF(U.EXIT_TOLLGATE_ID, ''),
	U.REG_DT,
	U.UPD_DT,
	CASE WHEN U.CHARGE_YN <> '' THEN U.CHARGE_YN ELSE 'Y' END,
	CASE WHEN U.CHARGE_STATUS <> '' THEN U.CHARGE_STATUS::SMALLINT ELSE 0 END,
	CASE WHEN U.STAY_SECONDS <> '' THEN U.STAY_SECONDS::INTEGER ELSE 0 END,
	NULLIF(U.TRIP_END_DT, '')
FROM UNNEST(
	$1::TEXT[], $2::TEXT[], $3::TEXT[], $4::TEXT[], $5::TEXT[], $6::TEXT[], $7::TEXT[],
	$8::TEXT[], $9::TEXT[], $10::TEXT[], $11::TEXT[], $12::TEXT[], $13::TEXT[], $14::TEXT[],
	$15::TEXT[], $16::TEXT[], $17::TEXT[], $18::TEXT[], $19::TEXT[], $20::TEXT[], $21::TEXT[],
	$22::TEXT[], $23::TEXT[], $24::TEXT[], $25::TEXT[], $26::TEXT[], $27::TEXT[], $28::TEXT[]
) AS U(
	TRIP_ID, DEVICE_KEY, TRIP_SEQ, CHARGE_TYPE, CHARGE_UNIT, LINK_ID, FROM_ID, TO_ID,
	FROM_LAT, FROM_LON, TO_LAT, TO_LON, ZONE_ID, ZONE_NAME, DIST_M, SPEED_KMH, SPEED_LIMIT_KMH,
	OCCUR_DT, TRIP_START_DT, TOLLGATE_ID, ENTRY_TOLLGATE_ID, EXIT_TOLLGATE_ID, REG_DT, UPD_DT,
	CHARGE_YN, CHARGE_STATUS, STAY_SECONDS, TRIP_END_DT
)
ON CONFLICT (trip_id, device_key, trip_seq) DO NOTHING;

-- ── 6. 트립 종료 시 trip_end_dt 반영 ───────────────────────────────────────
-- [trip_end] TRIP_EVENT=2 감지 시 CRawLogWorker::UpdateTripEndDt() 가 실행 (2026-08-12 최정우 추가)
-- 과금 INSERT 는 게이트 통과 즉시(트립 종료 전) 발생하므로 trip_end_dt 는 그 시점에 알 수
-- 없음 — 트립이 실제로 끝날 때 해당 trip_id 의 PRIM_CHARGEHAND 전 행에 반영.
-- 2026-08-20 최정우 수정 — 같은 trip_id 에 END 신호가 두 번 이상 와서 $1~$3 배열에 같은
-- TRIP_ID 가 여러 번 들어오면(원본 GPS에 END 이벤트가 중복·순서뒤섞임으로 두 번 온 실측 케이스로
-- 확인됨), 원래 쿼리는 UPDATE...FROM 에 같은 T 행에 매칭되는 V 행이 여러 개일 때 어느 게
-- 반영될지 명세상 정해져 있지 않아(PostgreSQL 공식 문서 — "unspecified which one is used"),
-- 배치 타이밍에 따라 더 이른 END 값이 남는 경우가 실측됨(나중 값이 이긴다는 보장이 없었음).
-- 아래 서브쿼리로 (1) 배열 안에서부터 trip_id 별 가장 늦은 TRIP_END_DT 만 골라 쓰고(DISTINCT ON),
-- (2) 이미 저장된 TRIP_END_DT 보다 늦을 때만 반영하도록(WHERE) 이중으로 보장 — 배치가 나뉘어
-- 여러 번 호출돼도, 어떤 순서로 오든 항상 "가장 늦은 종료 시각"만 최종 반영됨.
-- $1=TRIP_ID[] $2=TRIP_END_DT[] $3=UPD_DT[]
[trip_end]
UPDATE RUC.PRIM_CHARGEHAND AS T
SET
	TRIP_END_DT = V.TRIP_END_DT,
	UPD_DT = V.UPD_DT
FROM (
	SELECT DISTINCT ON (TRIP_ID) TRIP_ID, TRIP_END_DT, UPD_DT
	FROM UNNEST($1::TEXT[], $2::TEXT[], $3::TEXT[]) AS U(TRIP_ID, TRIP_END_DT, UPD_DT)
	ORDER BY TRIP_ID, TRIP_END_DT DESC
) AS V
WHERE T.TRIP_ID = V.TRIP_ID
	AND (T.TRIP_END_DT IS NULL OR V.TRIP_END_DT > T.TRIP_END_DT);

-- ── 7. 세션 TTL 만료(비정상 종료) 시 미확정 레코드 마감(5유형 공용) ─────────
-- [trip_abnormal_end] CRawLogWorker::UpdateAbnormalTripEnd() 가 ExpireTtlSessions() 안에서
-- 실행(2026-08-13 최정우 추가, 2026-08-13 수정 — CHARGE_TYPE 제한 제거해 4유형 전부로 확대).
-- "그 trip_id로 마지막 GPS 이후 TTL 넘게 신호가 없었는데(=트립이 정상 종료됐는지 끝내 확인 못함)
-- 이미 INSERT된 레코드가 아직 TRIP_END_DT 를 못 받은 상태"를 charge_type 무관하게 "확정 데이터
-- 아님"으로 표시(사용자 지시, 2026-08-13). CHARGE_STATUS=3(AUDIT=심사대상) — 완전 배제(SKIP=4)가
-- 아니라 사람이 재확인하도록 표시(사용자 지시, 2026-08-13 — 원래 4였다가 3으로 정정).
-- 2026-08-14 최정우 수정 — CHARGE_TYPE=5(면제도로)는 예외: 애초에 과금 대상이 아니라 재확인이
-- 필요없다는 설계 의도로 CHARGE_STATUS=4(SKIP) 고정이 맞음(사용자 확인) — 이 UPDATE가 무조건
-- 3으로 덮어쓰면 그 의도가 깨지므로, 면제도로만 4를 유지하도록 분기.
-- TRIP_END_DT IS NULL 조건으로 이미 [trip_end] 로 정상 마감된 레코드는 건드리지 않음. 주정차의
-- "지금 막 열려있는 세션"은 이 UPDATE 가 아니라 AppendExpiredParkingCharge() 가 별도로 처음부터
-- N/3 로 INSERT하므로(이미 TRIP_END_DT 채워진 채로 들어감) 이 UPDATE 와 안 겹침 — 주정차 중
-- "이전에 이미 정상 종료(Y/0)됐지만 그 트립 자체가 아직 안 끝난" 레코드만 이 UPDATE 대상이 됨
-- (다른 유형과 동일 취급).
-- $1=TRIP_ID[] $2=TRIP_END_DT[] $3=UPD_DT[]
[trip_abnormal_end]
UPDATE RUC.PRIM_CHARGEHAND AS T
SET
	TRIP_END_DT = V.TRIP_END_DT,
	CHARGE_YN = 'N',
	CHARGE_STATUS = CASE WHEN T.CHARGE_TYPE = 5 THEN 4 ELSE 3 END,
	UPD_DT = V.UPD_DT
FROM UNNEST(
	$1::TEXT[], $2::TEXT[], $3::TEXT[]
) AS V(TRIP_ID, TRIP_END_DT, UPD_DT)
WHERE T.TRIP_ID = V.TRIP_ID AND T.TRIP_END_DT IS NULL;

-- [server_status_update] CServer::UpdateServerStatus() 가 [server] status_interval(기본
--   600초=10분) 주기로 실행 — 이 서버(SERVER_ID=[server] id, 기본 "location") 1행만 갱신.
--   $1=SERVER_ID, $2=CPU_PCT, $3=MEM_PCT, $4=MEM_USED_MB, $5=MEM_TOTAL_MB
--   (2026-08-20 최정우 추가)
[server_status_update]
UPDATE RUC.PROC_SERVERSTATUS
SET
	CPU_PCT = $2::NUMERIC,
	MEM_PCT = $3::NUMERIC,
	MEM_USED_MB = $4::INTEGER,
	MEM_TOTAL_MB = $5::INTEGER,
	HB_DT = NOW(),
	UPD_DT = NOW()
WHERE SERVER_ID = $1;
