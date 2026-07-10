-- mytobcom 앱 계정 권한 (TRUNCATE 포함)
-- 사용: psql -U postgres -d roadnet -f roadnet/sql/grant_mytobcom.sql

\c roadnet

GRANT CONNECT ON DATABASE roadnet TO mytobcom;
GRANT USAGE ON SCHEMA roadnet TO mytobcom;
GRANT SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON ALL TABLES IN SCHEMA roadnet TO mytobcom;
ALTER DEFAULT PRIVILEGES IN SCHEMA roadnet
    GRANT SELECT, INSERT, UPDATE, DELETE, TRUNCATE ON TABLES TO mytobcom;
