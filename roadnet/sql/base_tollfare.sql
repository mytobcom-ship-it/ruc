-- ruc DB / base_tollfare — 개방식 요금표 (재구성 DDL)
--
-- ⚠️ 이 DDL은 실제 pg_dump가 아니라 doc/RUC_과금DB_스키마_기준선_v1.0.md(2026-08-07)에 정리된
--    pg_description 조사 결과를 근거로 재구성한 것입니다. 대조검증 전 참고용으로만 사용할 것.
-- 선행 조건: base_roadlink.sql, base_tollgate.sql을 먼저 실행할 것 (tollgate_id 하드 FK 대상).
--
-- 사용: psql -U postgres -d ruc -f roadnet/sql/base_tollfare.sql

\c ruc

CREATE TABLE IF NOT EXISTS base_tollfare (
    tollgate_id  VARCHAR(20)  NOT NULL,
    car_type     VARCHAR(1)   NOT NULL,
    fare_date    VARCHAR(8)   NOT NULL,
    toll_fare    INTEGER      DEFAULT NULL,
    admin_id     VARCHAR(20)  DEFAULT NULL,
    reg_dt       VARCHAR(14)  NOT NULL DEFAULT TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'),
    upd_dt       VARCHAR(14)  NOT NULL DEFAULT TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'),
    CONSTRAINT pk_base_tollfare PRIMARY KEY (tollgate_id, car_type, fare_date),
    -- 확인됨: base_tollfare.tollgate_id → base_tollgate.tollgate_id 하드 FK (기준선 문서 명시)
    CONSTRAINT fk_base_tollfare_tollgate FOREIGN KEY (tollgate_id) REFERENCES base_tollgate (tollgate_id),
    -- ⚠️ 실제 운영 DB에 존재하는지 미확인 — 로컬 개발용 안전장치로 추가
    CONSTRAINT ck_base_tollfare_car_type CHECK (car_type IN ('1', '2', '3', '4', '5', '6'))
);

COMMENT ON TABLE base_tollfare IS
    '개방식 요금표 - 본선 영업소x차종x적용일. 개정 = 새 fare_date 행 추가(행 자체가 이력)';
COMMENT ON COLUMN base_tollfare.tollgate_id IS 'PK-1. FK → base_tollgate (gate_div=M 게이트 사용 권장)';
COMMENT ON COLUMN base_tollfare.car_type IS 'PK-2. 차종(CARTYPE 1~6)';
COMMENT ON COLUMN base_tollfare.fare_date IS 'PK-3. 적용 시작일(YYYYMMDD) — 행 자체가 요금개정 이력';
COMMENT ON COLUMN base_tollfare.toll_fare IS '통행요금(원), NULL=미정';
COMMENT ON COLUMN base_tollfare.admin_id IS '저장 관리자';
COMMENT ON COLUMN base_tollfare.reg_dt IS '등록일시';
COMMENT ON COLUMN base_tollfare.upd_dt IS '수정일시';

-- ⚠️ 실제 운영 DB에 존재하는지 미확인 — 차종별 조회 최적화를 위한 제안
CREATE INDEX IF NOT EXISTS idx_base_tollfare_car_type ON base_tollfare (car_type);
