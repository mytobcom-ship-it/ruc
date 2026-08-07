-- ruc DB / base_tollgate — 영업소(게이트) 마스터 (재구성 DDL)
--
-- ⚠️ 이 DDL은 실제 pg_dump가 아니라 doc/RUC_과금DB_스키마_기준선_v1.0.md(2026-08-07)에 정리된
--    pg_description 조사 결과를 근거로 재구성한 것입니다. lon/lat 정밀도는 명시돼 있지 않아
--    prim_chargehand.from_lat/lon(numeric(10,6)) 기준으로 추정했습니다 — ⚠️ 표시 부분은 대조검증 전 참고용.
-- 선행 조건: base_roadlink.sql을 먼저 실행할 것 (road_id 하드 FK 대상).
-- 용도: 로컬 테스트용 ruc DB 구성. 다음 단계: 기준선 문서 5절 pg_dump 대조검증 후 v1.1 반영.
--
-- 사용: psql -U postgres -d ruc -f roadnet/sql/base_tollgate.sql

\c ruc

-- ⚠️ 실제 시퀀스명 미확인 — tollgate_id(TG+4자리) 채번용으로 추정 명명
CREATE SEQUENCE IF NOT EXISTS seq_tollgate_id;

CREATE TABLE IF NOT EXISTS base_tollgate (
    tollgate_id  VARCHAR(20)    NOT NULL DEFAULT ('TG' || LPAD(NEXTVAL('seq_tollgate_id')::text, 4, '0')),
    road_id      VARCHAR(20)    NOT NULL,
    tollgate_nm  VARCHAR(100)   DEFAULT NULL,
    gate_div     VARCHAR(1)     NOT NULL,
    lon          NUMERIC(10,6)  DEFAULT NULL,
    lat          NUMERIC(10,6)  DEFAULT NULL,
    link_id      VARCHAR(20)    DEFAULT NULL,
    use_yn       VARCHAR(1)     NOT NULL DEFAULT 'Y',
    reg_dt       TIMESTAMP      NOT NULL DEFAULT NOW(),
    upd_dt       TIMESTAMP      NOT NULL DEFAULT NOW(),
    CONSTRAINT pk_base_tollgate PRIMARY KEY (tollgate_id),
    -- 확인됨: base_tollgate.road_id → base_roadlink.road_id 하드 FK (기준선 문서 명시)
    CONSTRAINT fk_base_tollgate_road FOREIGN KEY (road_id) REFERENCES base_roadlink (road_id),
    -- ⚠️ 아래 두 CHECK는 실제 운영 DB에 존재하는지 미확인 — 로컬 개발용 안전장치로 추가
    CONSTRAINT ck_base_tollgate_gate_div CHECK (gate_div IN ('M', 'I', 'O', 'B')),
    CONSTRAINT ck_base_tollgate_use_yn CHECK (use_yn IN ('Y', 'N'))
);

COMMENT ON TABLE base_tollgate IS '영업소 마스터 - 개방식 본선(M)/폐쇄식 입구(I)·출구(O)·겸용(B)';
COMMENT ON COLUMN base_tollgate.tollgate_id IS 'PK. 영업소 ID (TG+4자리, seq_tollgate_id 채번)';
COMMENT ON COLUMN base_tollgate.road_id IS 'FK → base_roadlink.road_id (하드 FK)';
COMMENT ON COLUMN base_tollgate.tollgate_nm IS '영업소명';
COMMENT ON COLUMN base_tollgate.gate_div IS
    'M 개방식 본선 / I 폐쇄식 입구 / O 폐쇄식 출구 / B 겸용 (실데이터는 M/I/O만 관측, B 미관측)';
COMMENT ON COLUMN base_tollgate.lon IS '경도(WGS84)';
COMMENT ON COLUMN base_tollgate.lat IS '위도(WGS84)';
COMMENT ON COLUMN base_tollgate.link_id IS
    '기준 링크 — 소프트 참조(base_mapinfo.link_id, road_link.link_id 아님). 실데이터 3건 전부 NULL — 좌표거리 임계값 매칭 필요(추정)';
COMMENT ON COLUMN base_tollgate.use_yn IS '사용 여부';
COMMENT ON COLUMN base_tollgate.reg_dt IS '등록일시';
COMMENT ON COLUMN base_tollgate.upd_dt IS '수정일시';

-- ⚠️ 아래 인덱스는 실제 운영 DB에 존재하는지 미확인 — FK/게이트유형 조회 최적화를 위한 제안
CREATE INDEX IF NOT EXISTS idx_base_tollgate_road_id ON base_tollgate (road_id);
CREATE INDEX IF NOT EXISTS idx_base_tollgate_gate_div ON base_tollgate (gate_div) WHERE use_yn = 'Y';
