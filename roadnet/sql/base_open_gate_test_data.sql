-- ruc DB / 임시 개방식(OPEN) 게이트 테스트 데이터
--
-- 목적: base_roadlink + base_tollgate + base_tollfare 최소 데이터로 개방식 게이트 판정 흐름 테스트
-- (web/docs/charge-open-gate-test.html 참고). 톨게이트처럼 임의 도로에 게이트 ID를 부여해 사용하는 방식.
--
-- ⚠️ road_id='RL-Z900', tollgate_id='TG9000'은 실제 운영 데이터(2026-08 기준 RL-Znnn/TG0001~0003)와
--    번호대가 겹치지 않도록 임의로 큰 값을 고른 것뿐입니다. 실 DB에 반영하기 전에는
--    SELECT 1 FROM base_roadlink WHERE road_id='RL-Z900' 등으로 기존 채번과 충돌 여부를 재확인할 것.
-- 좌표: 강릉시 일대 임의 좌표(기존 base_mapinfo 데이터 지역과 맞춤, 실제 도로 형상과 무관한 테스트값).
--
-- 선행 조건: base_roadlink.sql, base_tollgate.sql, base_tollfare.sql 실행 완료 후 실행.
-- 정리(삭제): DELETE FROM base_tollfare  WHERE tollgate_id = 'TG9000';
--            DELETE FROM base_tollgate  WHERE tollgate_id = 'TG9000';
--            DELETE FROM base_roadlink  WHERE road_id     = 'RL-Z900';
--
-- 사용: psql -U postgres -d ruc -f roadnet/sql/base_open_gate_test_data.sql

\c ruc

-- 1) 임시 개방식 도로 1건 (road_kind='1' 유료 개방식)
INSERT INTO base_roadlink
    (road_id, road_kind, road_nm, geom_type, coords, speed_limit_kmh, use_yn, reg_dt, upd_dt, admin_id)
VALUES
    ('RL-Z900', '1', '[테스트] 임시 개방식 시험도로', 'LINE',
     '[[128.8761, 37.7519], [128.8802, 37.7563]]'::jsonb,
     NULL, 'Y', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), 'TEST')
ON CONFLICT (road_id) DO NOTHING;

-- 2) 임시 개방식 본선 게이트 1건 (도로 종점 부근에 배치, gate_div='M')
INSERT INTO base_tollgate
    (tollgate_id, road_id, tollgate_nm, gate_div, lon, lat, link_id, use_yn, reg_dt, upd_dt)
VALUES
    ('TG9000', 'RL-Z900', '[테스트] 임시 개방식 게이트', 'M', 128.8802, 37.7563, NULL, 'Y', NOW(), NOW())
ON CONFLICT (tollgate_id) DO NOTHING;

-- 3) 차종별(CARTYPE 1~6) 임시 요금 — 오늘 날짜부터 적용
INSERT INTO base_tollfare (tollgate_id, car_type, fare_date, toll_fare, admin_id, reg_dt, upd_dt)
VALUES
    ('TG9000', '1', TO_CHAR(NOW(), 'YYYYMMDD'), 1000, 'TEST', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS')),
    ('TG9000', '2', TO_CHAR(NOW(), 'YYYYMMDD'), 1200, 'TEST', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS')),
    ('TG9000', '3', TO_CHAR(NOW(), 'YYYYMMDD'), 1800, 'TEST', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS')),
    ('TG9000', '4', TO_CHAR(NOW(), 'YYYYMMDD'), 2200, 'TEST', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS')),
    ('TG9000', '5', TO_CHAR(NOW(), 'YYYYMMDD'), 2600, 'TEST', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS')),
    ('TG9000', '6', TO_CHAR(NOW(), 'YYYYMMDD'),  800, 'TEST', TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'), TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'))
ON CONFLICT (tollgate_id, car_type, fare_date) DO NOTHING;
