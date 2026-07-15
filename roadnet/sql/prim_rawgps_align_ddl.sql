-- PRIM_RAWGPS 라이브 테이블을 설계 DDL(prim_rawgps_create.sql)에 정합화 (2026-07-15 최정우)
--   변경점(참고):
--     · PK        : (device_key, gps_dt, gps_seq) → (TRIP_ID, GPS_SEQ)
--     · INDEX     : 라이브 4종 → 설계 INDEX①(DEVICE_KEY,GPS_DT DESC)·INDEX②(PENDING partial)
--     · CHECK     : drive_status (0,1,2,3) → (0,1,2,3,4)  [4=OFF_ROAD 포함]
--     · 컬럼순서  : device_key 선두 → TRIP_ID,GPS_SEQ 선두 (설계 컬럼순서)
--   동일 구조 신규 테이블 재생성 후 데이터 복사(40행)·권한 보존.
--   실행: cat prim_rawgps_align_ddl.sql | sudo -u postgres psql -d roadnet -v ON_ERROR_STOP=1
BEGIN;

-- 설계 DDL 컬럼순서로 신규 테이블 (PK/인덱스는 이름충돌 방지 위해 스왑 후 부여)
CREATE TABLE roadnet.prim_rawgps_new (
    trip_id         varchar(60)     NOT NULL,
    gps_seq         bigint          NOT NULL DEFAULT 0,
    device_key      varchar(36)     NOT NULL,
    gps_dt          char(14)        NOT NULL,
    trip_event      smallint        NOT NULL DEFAULT 1,
    drive_status    smallint        NOT NULL DEFAULT 0,
    gps_lat         numeric(10,6)   DEFAULT NULL,
    match_lat       numeric(10,6)   DEFAULT NULL,
    gps_lon         numeric(10,6)   DEFAULT NULL,
    match_lon       numeric(10,6)   DEFAULT NULL,
    intersect_len   integer         DEFAULT 0,
    raw_vld         boolean         DEFAULT FALSE,
    speed_kmh       smallint        DEFAULT NULL,
    heading         smallint        DEFAULT NULL,
    altitude_m      smallint        DEFAULT NULL,
    accuracy_m      smallint        DEFAULT NULL,
    battery         smallint        DEFAULT NULL,
    recv_dt         char(14)        NOT NULL DEFAULT TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'),
    match_link_id   varchar(20)     DEFAULT NULL,
    match_status    smallint        NOT NULL DEFAULT 0,
    CONSTRAINT ck_prim_rawgps_trip_event   CHECK (trip_event IN (0, 1, 2)),
    CONSTRAINT ck_prim_rawgps_drive_status CHECK (drive_status IN (0, 1, 2, 3, 4)),
    CONSTRAINT ck_prim_rawgps_match_status CHECK (match_status IN (0, 1, 2, 3, 4))
);

INSERT INTO roadnet.prim_rawgps_new
    (trip_id, gps_seq, device_key, gps_dt, trip_event, drive_status,
     gps_lat, match_lat, gps_lon, match_lon, intersect_len, raw_vld,
     speed_kmh, heading, altitude_m, accuracy_m, battery,
     recv_dt, match_link_id, match_status)
SELECT
     trip_id, gps_seq, device_key, gps_dt, trip_event, drive_status,
     gps_lat, match_lat, gps_lon, match_lon, intersect_len, raw_vld,
     speed_kmh, heading, altitude_m, accuracy_m, battery,
     recv_dt, match_link_id, match_status
FROM roadnet.prim_rawgps;

DROP TABLE roadnet.prim_rawgps;
ALTER TABLE roadnet.prim_rawgps_new RENAME TO prim_rawgps;

-- PK (설계: TRIP_ID, GPS_SEQ)
ALTER TABLE roadnet.prim_rawgps
    ADD CONSTRAINT pk_prim_rawgps PRIMARY KEY (trip_id, gps_seq);

-- INDEX① 단말별 GPS 이력 (DEVICE_KEY, GPS_DT DESC)
CREATE INDEX idx_prim_rawgps_device_time
    ON roadnet.prim_rawgps (device_key, gps_dt DESC);

-- INDEX② 맵매칭 Feeder PENDING partial
CREATE INDEX idx_prim_rawgps_pending
    ON roadnet.prim_rawgps (device_key, trip_id, gps_dt, gps_seq)
    WHERE match_status = 0
      AND drive_status <> 1
      AND raw_vld IS TRUE;

-- 주석 (설계 DDL 기준)
COMMENT ON TABLE  roadnet.prim_rawgps IS '모바일 APP 원시 GPS. 위치검증서버가 PENDING(0) 조회 후 MATCHED(1) 갱신';
COMMENT ON COLUMN roadnet.prim_rawgps.trip_id IS 'PK-1, {DEVICE_KEY}_{YYYYMMDDHH24MISS} (수집서버 START 시 적재)';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_seq IS 'PK-2, 운행(TRIP_ID)마다 1~N 초기화';
COMMENT ON COLUMN roadnet.prim_rawgps.device_key IS 'INDEX①·② 선두. BASE_CUSTINFO 조인키(단말·회원 식별). ThreadPool sticky hash key';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_dt IS 'INDEX①·② 구성컬럼. GPS 측정 시각 KST CHAR(14) YYYYMMDDHH24MISS';
COMMENT ON COLUMN roadnet.prim_rawgps.trip_event IS '0:START, 1:NONE, 2:END';
COMMENT ON COLUMN roadnet.prim_rawgps.drive_status IS '0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING, 4:OFF_ROAD';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_lat IS '위도 WGS-84. NULL 허용. NUMERIC(10,6) 소수 6자리';
COMMENT ON COLUMN roadnet.prim_rawgps.match_lat IS '맵매칭 위도 (위치검증서버 처리). NUMERIC(10,6) 소수 6자리';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_lon IS '경도 WGS-84. NULL 허용. NUMERIC(10,6) 소수 6자리';
COMMENT ON COLUMN roadnet.prim_rawgps.match_lon IS '맵매칭 경도 (위치검증서버 처리). NUMERIC(10,6) 소수 6자리';
COMMENT ON COLUMN roadnet.prim_rawgps.intersect_len IS 'GPS 좌표와 세그먼트 교차점까지 거리(m)';
COMMENT ON COLUMN roadnet.prim_rawgps.raw_vld IS 'BOOLEAN. TRUE=맵매칭 대상. INDEX② PENDING partial 조건';
COMMENT ON COLUMN roadnet.prim_rawgps.heading IS '방위각(정북 기준 시계방향 0~359도). SMALLINT 정수, NULL 허용';
COMMENT ON COLUMN roadnet.prim_rawgps.recv_dt IS '수집서버 수신 시각 CHAR(14) YYYYMMDDHH24MISS';
COMMENT ON COLUMN roadnet.prim_rawgps.match_link_id IS '맵매칭된 링크 ID (위치검증서버 처리). PRIM_LINK_INFO.LINK_ID 조인키. 매칭前/ERROR NULL';
COMMENT ON COLUMN roadnet.prim_rawgps.match_status IS '0:PENDING, 1:MATCHED, 2:PROCESSING, 3:SKIP, 4:ERROR. INDEX② PENDING partial 조건';

-- 소유자·권한
ALTER TABLE roadnet.prim_rawgps OWNER TO postgres;
GRANT SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON TABLE roadnet.prim_rawgps TO mytobcom;

COMMIT;
