-- ============================================================================
-- RUC 맵매칭 시뮬레이터 - DB 스키마 생성 스크립트
--   대상 DB   : roadnet (도로망 network.* 스키마와 동일 DB)
--   스키마    : roadnet (수집/맵매칭 인터페이스)
--   테이블    : roadnet.prim_rawgps (원시 GPS 로그)
--
-- 근거 : doc/RUC_위치검증서버_테이블설계서_v1.3.docx §2.1
--        roadnet/sql/roadnet.sql 과 동일 PK·INDEX·컬럼 순서 (2026-07-10)
--
-- TRIP_EVENT: 0=START, 1=NONE, 2=END
-- DRIVE_STATUS: 0=ON_ROAD, 1=IDLE, 2=PARKED, 3=TUNNELING
-- MATCH_STATUS: 0=PENDING, 1=MATCHED, 2=PROCESSING, 3=SKIP, 4=ERROR
--
-- 사용법 (postgres 슈퍼유저):
--   psql -U postgres -d roadnet -f Simulator/db/create_sim.sql
--   sudo -u postgres psql -d roadnet -f Simulator/db/create_sim.sql
--
-- 기존 구 스키마 마이그레이션:
--   psql -U postgres -d roadnet -f roadnet/sql/prim_rawgps_pk_index_migrate.sql
-- ============================================================================

-- ---------------------------------------------------------------------------
-- [1] 스키마
-- ---------------------------------------------------------------------------
CREATE SCHEMA IF NOT EXISTS roadnet;
COMMENT ON SCHEMA roadnet IS '수집·맵매칭 인터페이스 (원시 GPS, 과금 대상 등)';

SET SEARCH_PATH TO roadnet, public;

-- ---------------------------------------------------------------------------
-- [2] roadnet.prim_rawgps  (원시 GPS 로그 - 수집서버 소유)
--     컬럼 순서: PK → INDEX 선두 → §2.1 속성 → 상태
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS roadnet.prim_rawgps (
    trip_id         varchar(60)     NOT NULL,
    gps_seq         bigint          NOT NULL DEFAULT 0,
    device_key      varchar(36)     NOT NULL,
    gps_dt          char(14)        NOT NULL,
    trip_event      smallint        NOT NULL DEFAULT 1,
    drive_status    smallint        NOT NULL DEFAULT 0,
    gps_lat         numeric(10, 6)  DEFAULT NULL,
    match_lat       numeric(10, 6)  DEFAULT NULL,
    gps_lon         numeric(10, 6)  DEFAULT NULL,
    match_lon       numeric(10, 6)  DEFAULT NULL,
    intersect_len   integer         DEFAULT 0,
    raw_vld         boolean         DEFAULT false,
    speed_kmh       smallint        DEFAULT NULL,
    heading         smallint        DEFAULT NULL,
    altitude_m      smallint        DEFAULT NULL,
    accuracy_m      smallint        DEFAULT NULL,
    battery         smallint        DEFAULT NULL,
    recv_dt         char(14)        NOT NULL DEFAULT to_char(now(), 'YYYYMMDDHH24MISS'),
    match_status    smallint        NOT NULL DEFAULT 0,
    CONSTRAINT pk_prim_rawgps PRIMARY KEY (trip_id, gps_seq),
    CONSTRAINT ck_prim_rawgps_trip_event CHECK (trip_event IN (0, 1, 2)),
    CONSTRAINT ck_prim_rawgps_drive_status CHECK (drive_status IN (0, 1, 2, 3)),
    CONSTRAINT ck_prim_rawgps_match_status CHECK (match_status IN (0, 1, 2, 3, 4))
);

COMMENT ON TABLE  roadnet.prim_rawgps              IS '원시 GPS 로그 (수집서버 INSERT, 위치검증서버 READ/UPDATE)';
COMMENT ON COLUMN roadnet.prim_rawgps.trip_id      IS 'PK-1, {DEVICE_KEY}_{YYYYMMDDHH24MISS}';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_seq      IS 'PK-2, 운행(TRIP_ID)마다 1~N';
COMMENT ON COLUMN roadnet.prim_rawgps.device_key   IS 'INDEX①·② 선두. ThreadPool sticky hash key';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_dt       IS 'INDEX①·②. GPS 측정 시각 KST CHAR(14)';
COMMENT ON COLUMN roadnet.prim_rawgps.trip_event   IS '0:START, 1:NONE, 2:END';
COMMENT ON COLUMN roadnet.prim_rawgps.drive_status IS '0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING';
COMMENT ON COLUMN roadnet.prim_rawgps.match_status IS '0:PENDING, 1:MATCHED, 2:PROCESSING, 3:SKIP, 4:ERROR';

-- ---------------------------------------------------------------------------
-- [3] 인덱스 (위치검증서버 PENDING 폴링 최적화)
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_prim_rawgps_pending
    ON roadnet.prim_rawgps (device_key, trip_id, gps_dt, gps_seq)
    WHERE match_status = 0
      AND drive_status <> 1
      AND raw_vld IS TRUE;

CREATE INDEX IF NOT EXISTS idx_prim_rawgps_device_time
    ON roadnet.prim_rawgps (device_key, gps_dt DESC);

-- ---------------------------------------------------------------------------
-- [4] mytobcom 권한 (시뮬레이터·맵매칭서버 접속 계정)
-- ---------------------------------------------------------------------------
GRANT USAGE ON SCHEMA roadnet TO mytobcom;
GRANT SELECT, INSERT, UPDATE, DELETE ON roadnet.prim_rawgps TO mytobcom;
ALTER DEFAULT PRIVILEGES IN SCHEMA roadnet
    GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO mytobcom;
