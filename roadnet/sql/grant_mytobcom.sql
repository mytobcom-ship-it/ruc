-- roadnet 스키마 PRIM 테이블 — 소유자·권한을 mytobcom(앱 계정)으로 통일
-- 사용: sudo -u postgres psql -d roadnet -f roadnet/sql/grant_mytobcom.sql
--
-- · 테이블 OWNER → mytobcom (postgres DDL 실행 후에도 앱 계정이 owner)
-- · DB CONNECT, 스키마 USAGE

\c roadnet

GRANT CONNECT ON DATABASE roadnet TO mytobcom;
GRANT USAGE ON SCHEMA roadnet TO mytobcom;

-- PRIM 핵심 테이블 (명시)
ALTER TABLE IF EXISTS roadnet.prim_rawgps OWNER TO mytobcom;
ALTER TABLE IF EXISTS roadnet.prim_link_info OWNER TO mytobcom;

-- roadnet 스키마 내 기타 테이블 일괄 (있을 때만)
DO $$
DECLARE
    r RECORD;
BEGIN
    FOR r IN
        SELECT tablename
        FROM pg_tables
        WHERE schemaname = 'roadnet'
    LOOP
        EXECUTE format('ALTER TABLE roadnet.%I OWNER TO mytobcom', r.tablename);
    END LOOP;
END
$$;

-- postgres 가 DDL 로 만든 객체 → 이후에도 mytobcom 이 owner (CREATE 스크립트와 병행)
ALTER DEFAULT PRIVILEGES FOR ROLE postgres IN SCHEMA roadnet
    GRANT SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON TABLES TO mytobcom;
