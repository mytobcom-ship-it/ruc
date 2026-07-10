-- ============================================================================
-- RUC 맵매칭 시뮬레이터 - DB 스키마 생성 스크립트
--   대상 DB   : roadnet (도로망 network.* 스키마와 동일 DB)
--   스키마    : roadnet (수집/맵매칭 인터페이스)
--   테이블    : roadnet.prim_rawgps (원시 GPS 로그)
--
-- 근거 : doc/RUC_위치검증서버_테이블설계서_v1.3.docx §2.1
--        MapMatchSvr/bin/query.sql 가 roadnet.prim_rawgps 를 조회하므로 동일 위치에 생성
--
-- TRIP_EVENT: 0=START, 1=NONE, 2=END
-- DRIVE_STATUS: 0=ON_ROAD, 1=IDLE, 2=PARKED, 3=TUNNELING
-- MATCH_STATUS: 0=PENDING, 1=MATCHED, 2=PROCESSING, 3=SKIP, 4=ERROR
--
-- 사용법 (postgres 슈퍼유저):
--   psql -U postgres -d roadnet -f Simulator/db/create_sim.sql
--   sudo -u postgres psql -d roadnet -f Simulator/db/create_sim.sql
-- ============================================================================

-- ---------------------------------------------------------------------------
-- [1] 스키마
-- ---------------------------------------------------------------------------
CREATE SCHEMA IF NOT EXISTS roadnet;
COMMENT ON SCHEMA roadnet IS '수집·맵매칭 인터페이스 (원시 GPS, 과금 대상 등)';

-- ---------------------------------------------------------------------------
-- [2] roadnet.prim_rawgps  (원시 GPS 로그 - 수집서버 소유)
--     - 모바일 APP GPS 좌표 적재. 위치검증서버가 PENDING 행을 읽어 맵매칭 후 MATCHED 로 갱신
--     - 시뮬레이터는 수집서버 역할로 PENDING 행을 INSERT 한다
--     - 컬럼 순서: 설계서 v1.3 §2.1
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS roadnet.prim_rawgps (
    device_key    varchar(36)    NOT NULL,
    gps_dt        timestamp      NOT NULL,
    gps_seq       bigserial      NOT NULL,
    trip_id       varchar(60),
    trip_event    smallint       NOT NULL DEFAULT 1,
    drive_status  smallint       NOT NULL DEFAULT 0,
    gps_lat       numeric(10,6),
    match_lat     numeric(10,6),
    gps_lon       numeric(10,6),
    match_lon     numeric(10,6),
    intersect_len integer        DEFAULT 0,
    raw_vld       boolean        DEFAULT true,
    speed_kmh     numeric(6,1),
    heading       numeric(5,1),
    altitude_m    numeric(7,1),
    accuracy_m    numeric(6,1),
    battery       smallint,
    recv_dt       timestamp      NOT NULL DEFAULT NOW(),
    match_status  smallint       NOT NULL DEFAULT 0,
    CONSTRAINT pk_prim_rawgps PRIMARY KEY (device_key, gps_dt, gps_seq)
);

COMMENT ON TABLE  roadnet.prim_rawgps              IS '원시 GPS 로그 (수집서버 INSERT, 위치검증서버 READ/UPDATE)';
COMMENT ON COLUMN roadnet.prim_rawgps.device_key   IS 'PK, 모바일 앱 인증키 (Client Key)';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_dt       IS 'PK, GPS 측정 시각 (KST)';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_seq      IS 'PK, 운행마다 1~N (시뮬: bigserial)';
COMMENT ON COLUMN roadnet.prim_rawgps.trip_event   IS '0:START, 1:NONE, 2:END';
COMMENT ON COLUMN roadnet.prim_rawgps.drive_status IS '0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_lat      IS '위도 (WGS-84). NUMERIC(10,6) 소수 6자리';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_lon      IS '경도 (WGS-84). NUMERIC(10,6) 소수 6자리';
COMMENT ON COLUMN roadnet.prim_rawgps.speed_kmh    IS '순간 속도 (km/h)';
COMMENT ON COLUMN roadnet.prim_rawgps.heading      IS '방향각 (0~359도)';
COMMENT ON COLUMN roadnet.prim_rawgps.altitude_m   IS '고도 (m)';
COMMENT ON COLUMN roadnet.prim_rawgps.accuracy_m   IS '수평 위치 오차 (m). 15m 초과 시 신뢰도 저하';
COMMENT ON COLUMN roadnet.prim_rawgps.battery      IS '배터리 잔량 (%, 0~100)';
COMMENT ON COLUMN roadnet.prim_rawgps.gps_dt       IS 'GPS 측정 시각 (KST)';
COMMENT ON COLUMN roadnet.prim_rawgps.recv_dt      IS '수집서버 수신 시각';
COMMENT ON COLUMN roadnet.prim_rawgps.match_status IS '0:PENDING, 1:MATCHED, 2:PROCESSING, 3:SKIP, 4:ERROR';
COMMENT ON COLUMN roadnet.prim_rawgps.trip_id      IS '맵매칭 후 채워지는 운행/항목 식별자 (초기 NULL)';

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'roadnet' AND table_name = 'prim_rawgps' AND column_name = 'lat'
    ) THEN
        ALTER TABLE roadnet.prim_rawgps RENAME COLUMN lat TO gps_lat;
    END IF;
    IF EXISTS (
        SELECT 1 FROM information_schema.columns
        WHERE table_schema = 'roadnet' AND table_name = 'prim_rawgps' AND column_name = 'lon'
    ) THEN
        ALTER TABLE roadnet.prim_rawgps RENAME COLUMN lon TO gps_lon;
    END IF;
END $$;

ALTER TABLE roadnet.prim_rawgps
    ALTER COLUMN gps_lat DROP NOT NULL,
    ALTER COLUMN gps_lon DROP NOT NULL;

ALTER TABLE roadnet.prim_rawgps
    ADD COLUMN IF NOT EXISTS match_lat numeric(10,6),
    ADD COLUMN IF NOT EXISTS match_lon numeric(10,6),
    ADD COLUMN IF NOT EXISTS intersect_len integer DEFAULT 0,
    ADD COLUMN IF NOT EXISTS raw_vld boolean DEFAULT true;

ALTER TABLE roadnet.prim_rawgps
    ALTER COLUMN gps_lat TYPE numeric(10, 6),
    ALTER COLUMN gps_lon TYPE numeric(10, 6),
    ALTER COLUMN match_lat TYPE numeric(10, 6),
    ALTER COLUMN match_lon TYPE numeric(10, 6);

-- ---------------------------------------------------------------------------
-- [3] 인덱스 (위치검증서버 PENDING 폴링 최적화)
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_prim_rawgps_status
    ON roadnet.prim_rawgps (match_status, gps_dt);
CREATE INDEX IF NOT EXISTS idx_prim_rawgps_device
    ON roadnet.prim_rawgps (device_key, gps_dt);

-- ---------------------------------------------------------------------------
-- [4] mytobcom 권한 (시뮬레이터 접속 계정)
--     - 시뮬레이터: INSERT, 시퀀스 사용
--     - 위치검증서버 역할(조회/갱신) 대비: SELECT, UPDATE 도 부여
-- ---------------------------------------------------------------------------
GRANT USAGE ON SCHEMA roadnet TO mytobcom;
GRANT SELECT, INSERT, UPDATE ON roadnet.prim_rawgps TO mytobcom;
GRANT USAGE, SELECT ON SEQUENCE roadnet.prim_rawgps_gps_seq_seq TO mytobcom;
ALTER DEFAULT PRIVILEGES IN SCHEMA roadnet
    GRANT SELECT, INSERT, UPDATE ON TABLES TO mytobcom;
