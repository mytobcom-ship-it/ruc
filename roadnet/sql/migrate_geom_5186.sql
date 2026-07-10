-- network.moct_* geom SRID 5179 → 5186 (신규 표준노드링크)
-- 사용: psql -U postgres -d roadnet -f roadnet/sql/migrate_geom_5186.sql
-- 이후: bash roadnet/scripts/roadnet.sh import

\c roadnet
SET search_path TO network, public;

-- FK·자식 테이블 정리 후 도로망 비우기
TRUNCATE TABLE network.turn_info;
TRUNCATE TABLE network.multilink;
TRUNCATE TABLE network.moct_link CASCADE;
TRUNCATE TABLE network.moct_node CASCADE;

ALTER TABLE network.moct_node
    ALTER COLUMN geom TYPE geometry(Point, 5186);

ALTER TABLE network.moct_link
    ALTER COLUMN geom TYPE geometry(LineString, 5186);

COMMENT ON COLUMN network.moct_node.geom IS '노드 좌표 (EPSG:5186, GRS80 TM Central Belt 2010)';
COMMENT ON COLUMN network.moct_link.geom IS '링크 형상 (EPSG:5186, LineString)';
