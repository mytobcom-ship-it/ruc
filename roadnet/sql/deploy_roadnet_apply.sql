-- roadnet.sql 실행 전 기존 동일 테이블 제거 (roadnet.sql 본문은 변경하지 않음)
-- 사용: psql -U postgres -f roadnet/sql/deploy_roadnet_apply.sql
-- 이후: psql -U postgres -f roadnet/sql/roadnet.sql

SELECT 'CREATE DATABASE ROADNET'
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = 'roadnet')\gexec

\c roadnet

SET SEARCH_PATH TO ROADNET, PUBLIC;

DROP TABLE IF EXISTS ROADNET.PRIM_RAWGPS CASCADE;
