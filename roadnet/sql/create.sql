-- roadnet DB 생성 + 스키마 + 테이블 + 권한 (통합)
-- 사용법 (postgres 슈퍼유저, postgres DB 접속 상태):
--   psql -U postgres -f roadnet/sql/create.sql
--   sudo -u postgres psql -f roadnet/sql/create.sql

-- ---------------------------------------------------------------------------
-- [1] 데이터베이스 생성
-- ---------------------------------------------------------------------------
SELECT 'CREATE DATABASE roadnet'
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'roadnet')\gexec

\c roadnet

-- ---------------------------------------------------------------------------
-- [2] PostGIS + 스키마
-- ---------------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS postgis;

CREATE SCHEMA IF NOT EXISTS network;
COMMENT ON SCHEMA network IS '표준 노드·링크 도로 네트워크 (MOCT Shapefile)';

SET search_path TO network, public;

-- ---------------------------------------------------------------------------
-- network.moct_node  (MOCT_NODE.shp / .dbf)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS network.moct_node (
    node_id     varchar(10)  NOT NULL,
    node_type   varchar(3),
    node_name   varchar(50),
    turn_p      varchar(1),
    updatedate  varchar(8),
    remark      varchar(30),
    hist_type   varchar(8),
    histremark  varchar(30),
    geom        geometry(Point, 5186),
    CONSTRAINT pk_moct_node PRIMARY KEY (node_id)
);

COMMENT ON TABLE network.moct_node IS '표준 노드 (교차점·분기점 등)';
COMMENT ON COLUMN network.moct_node.node_id    IS '노드 ID (10자리, PK)';
COMMENT ON COLUMN network.moct_node.node_type  IS '노드 유형 코드 (101:교차로, 102:JC, 104:IC 등)';
COMMENT ON COLUMN network.moct_node.node_name  IS '노드 명칭';
COMMENT ON COLUMN network.moct_node.turn_p     IS '회전 가능 여부 (0/1)';
COMMENT ON COLUMN network.moct_node.updatedate IS '데이터 갱신일 (YYYYMMDD)';
COMMENT ON COLUMN network.moct_node.remark     IS '비고';
COMMENT ON COLUMN network.moct_node.hist_type  IS '이력 유형 (LINK0001:신규, LINK0003:변경, LINK1007:유지 등)';
COMMENT ON COLUMN network.moct_node.histremark IS '이력 비고';
COMMENT ON COLUMN network.moct_node.geom       IS '노드 좌표 (EPSG:5186, GRS80 TM Central Belt 2010)';

-- ---------------------------------------------------------------------------
-- network.moct_link  (MOCT_LINK.shp / .dbf)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS network.moct_link (
    link_id     varchar(10)  NOT NULL,
    f_node      varchar(10),
    t_node      varchar(10),
    lanes       integer,
    road_rank   varchar(3),
    road_type   varchar(3),
    road_no     varchar(5),
    road_name   varchar(30),
    road_use    varchar(1),
    multi_link  varchar(1),
    connect     varchar(3),
    max_spd     integer,
    rest_veh    varchar(3),
    rest_w      integer,
    rest_h      integer,
    c_its       varchar(1),
    length      numeric(18, 12),
    updatedate  varchar(8),
    remark      varchar(30),
    hist_type   varchar(8),
    histremark  varchar(30),
    geom        geometry(LineString, 5186),
    CONSTRAINT pk_moct_link PRIMARY KEY (link_id),
    CONSTRAINT fk_moct_link_f_node FOREIGN KEY (f_node)
        REFERENCES network.moct_node (node_id) DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT fk_moct_link_t_node FOREIGN KEY (t_node)
        REFERENCES network.moct_node (node_id) DEFERRABLE INITIALLY DEFERRED
);

COMMENT ON TABLE network.moct_link IS '표준 링크 (도로 구간)';
COMMENT ON COLUMN network.moct_link.link_id    IS '링크 ID (10자리, PK)';
COMMENT ON COLUMN network.moct_link.f_node     IS '시작 노드 ID → moct_node.node_id';
COMMENT ON COLUMN network.moct_link.t_node     IS '종료 노드 ID → moct_node.node_id';
COMMENT ON COLUMN network.moct_link.lanes      IS '차로 수';
COMMENT ON COLUMN network.moct_link.road_rank  IS '도로 등급 (101:고속, 103:국도, 106:지방도, 107:시군도 등)';
COMMENT ON COLUMN network.moct_link.road_type  IS '도로 유형 (000:일반, 001~004:터널·교량 등)';
COMMENT ON COLUMN network.moct_link.road_no    IS '대표 노선번호';
COMMENT ON COLUMN network.moct_link.road_name  IS '도로명';
COMMENT ON COLUMN network.moct_link.road_use   IS '도로 사용 구분 (0:일반, 1:전용)';
COMMENT ON COLUMN network.moct_link.multi_link IS '다중 노선 여부 (0:단일, 1:복수 노선 → multilink 참조)';
COMMENT ON COLUMN network.moct_link.connect    IS '연결로 여부 (0:아님, 1:연결로)';
COMMENT ON COLUMN network.moct_link.max_spd    IS '최고 제한속도 (km/h)';
COMMENT ON COLUMN network.moct_link.rest_veh   IS '통행 제한 차량 코드';
COMMENT ON COLUMN network.moct_link.rest_w     IS '요일별 통행 제한';
COMMENT ON COLUMN network.moct_link.rest_h     IS '시간대별 통행 제한';
COMMENT ON COLUMN network.moct_link.c_its      IS 'C-ITS 구간 여부';
COMMENT ON COLUMN network.moct_link.length     IS '링크 길이 (m)';
COMMENT ON COLUMN network.moct_link.updatedate IS '데이터 갱신일 (YYYYMMDD)';
COMMENT ON COLUMN network.moct_link.remark     IS '비고';
COMMENT ON COLUMN network.moct_link.hist_type  IS '이력 유형 (LINK0001:신규, LINK0003:변경, LINK1007:유지 등)';
COMMENT ON COLUMN network.moct_link.histremark IS '이력 비고';
COMMENT ON COLUMN network.moct_link.geom       IS '링크 형상 (EPSG:5186, LineString)';

-- ---------------------------------------------------------------------------
-- network.multilink  (MULTILINK.dbf, geometry 없음)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS network.multilink (
    link_id     varchar(10)  NOT NULL,
    multi_id    smallint     NOT NULL,
    road_rank   varchar(3),
    road_type   varchar(3),
    road_no     varchar(5),
    road_name   varchar(30),
    remark      varchar(30),
    CONSTRAINT pk_multilink PRIMARY KEY (link_id, multi_id),
    CONSTRAINT fk_multilink_link FOREIGN KEY (link_id)
        REFERENCES network.moct_link (link_id) DEFERRABLE INITIALLY DEFERRED
);

COMMENT ON TABLE network.multilink IS '다중 노선 링크 부가 속성 (한 구간에 노선번호 2개 이상)';
COMMENT ON COLUMN network.multilink.link_id   IS '링크 ID → moct_link.link_id';
COMMENT ON COLUMN network.multilink.multi_id  IS '다중 노선 순번 (1, 2, 3 …)';
COMMENT ON COLUMN network.multilink.road_rank IS '도로 등급';
COMMENT ON COLUMN network.multilink.road_type IS '도로 유형';
COMMENT ON COLUMN network.multilink.road_no   IS '추가 노선번호';
COMMENT ON COLUMN network.multilink.road_name IS '도로명';
COMMENT ON COLUMN network.multilink.remark    IS '비고';

-- ---------------------------------------------------------------------------
-- network.turn_info  (TURNINFO.dbf, geometry 없음)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS network.turn_info (
    node_id     varchar(10)  NOT NULL,
    turn_id     smallint     NOT NULL,
    st_link     varchar(10)  NOT NULL,
    ed_link     varchar(10)  NOT NULL,
    turn_type   varchar(3),
    turn_oper   varchar(1),
    remark      varchar(30),
    CONSTRAINT pk_turn_info PRIMARY KEY (node_id, turn_id)
);

COMMENT ON TABLE network.turn_info IS '노드 회전 정보 (진입링크→진출링크 회전 제한/허용)';
COMMENT ON COLUMN network.turn_info.node_id   IS '노드 ID (→ moct_node.node_id)';
COMMENT ON COLUMN network.turn_info.turn_id   IS '노드 내 회전 정보 순번';
COMMENT ON COLUMN network.turn_info.st_link   IS '진입(시작) 링크 ID (→ moct_link.link_id)';
COMMENT ON COLUMN network.turn_info.ed_link   IS '진출(종료) 링크 ID (→ moct_link.link_id)';
COMMENT ON COLUMN network.turn_info.turn_type IS '회전 유형 코드 (011:직진, 101:좌회전, 102:우회전, 103:유턴 등)';
COMMENT ON COLUMN network.turn_info.turn_oper IS '회전 허용 여부 (0:허용, 1:제한)';
COMMENT ON COLUMN network.turn_info.remark    IS '비고';

-- ---------------------------------------------------------------------------
-- [3] 인덱스
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_moct_link_f_node ON network.moct_link (f_node);
CREATE INDEX IF NOT EXISTS idx_moct_link_t_node ON network.moct_link (t_node);
CREATE INDEX IF NOT EXISTS idx_moct_link_road_rank ON network.moct_link (road_rank);
CREATE INDEX IF NOT EXISTS idx_moct_link_multi_link ON network.moct_link (multi_link);
CREATE INDEX IF NOT EXISTS idx_moct_link_geom ON network.moct_link USING GIST (geom);
CREATE INDEX IF NOT EXISTS idx_moct_node_geom ON network.moct_node USING GIST (geom);
CREATE INDEX IF NOT EXISTS idx_multilink_link_id ON network.multilink (link_id);
CREATE INDEX IF NOT EXISTS idx_turn_info_node_id ON network.turn_info (node_id);
CREATE INDEX IF NOT EXISTS idx_turn_info_st_link ON network.turn_info (st_link);
CREATE INDEX IF NOT EXISTS idx_turn_info_ed_link ON network.turn_info (ed_link);
CREATE INDEX IF NOT EXISTS idx_turn_info_st_ed ON network.turn_info (st_link, ed_link);

-- ---------------------------------------------------------------------------
-- [4] mytobcom 사용자 권한
-- ---------------------------------------------------------------------------
GRANT CONNECT ON DATABASE roadnet TO mytobcom;
GRANT USAGE ON SCHEMA network TO mytobcom;
GRANT SELECT ON ALL TABLES IN SCHEMA network TO mytobcom;
ALTER DEFAULT PRIVILEGES IN SCHEMA network GRANT SELECT ON TABLES TO mytobcom;
