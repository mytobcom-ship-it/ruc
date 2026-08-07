-- ruc DB / base_roadlink — 운영 설정 도로/구역 마스터 (재구성 DDL)
--
-- ⚠️ 이 DDL은 실제 pg_dump가 아니라 doc/RUC_과금DB_스키마_기준선_v1.0.md(2026-08-07)에 정리된
--    pg_description(테이블·컬럼 코멘트) 조사 결과를 근거로 재구성한 것입니다.
--    타입 정밀도가 문서에 명시 안 된 컬럼(speed_limit_kmh 등)은 유사 컬럼(base_mapinfo.speed_limit_kmh)
--    기준으로 추정했습니다 — ⚠️ 표시된 부분은 실제 DDL 대조검증 전까지 참고용으로만 사용할 것.
-- 용도: 원격 DB(59.11.91.162) 접속 불가 시 로컬에 동일 구조의 테스트용 ruc DB를 구성하기 위함.
-- 다음 단계: doc/RUC_과금DB_스키마_기준선_v1.0.md 5절 pg_dump 대조검증 후 v1.1 델타 반영.
--
-- 사용: psql -U postgres -d ruc -f roadnet/sql/base_roadlink.sql

\c ruc

-- ⚠️ 실제 시퀀스명 미확인 — road_id(RL-Znnn) 채번용으로 추정 명명
CREATE SEQUENCE IF NOT EXISTS seq_road_id;

CREATE TABLE IF NOT EXISTS base_roadlink (
    road_id          VARCHAR(20)   NOT NULL DEFAULT ('RL-Z' || LPAD(NEXTVAL('seq_road_id')::text, 3, '0')),
    road_kind        VARCHAR(2)    NOT NULL,
    road_nm          VARCHAR(100)  DEFAULT NULL,
    geom_type        VARCHAR(4)    DEFAULT NULL,
    coords           JSONB         DEFAULT NULL,
    speed_limit_kmh  NUMERIC(6,1)  DEFAULT NULL,
    use_yn           CHAR(1)       NOT NULL DEFAULT 'Y',
    reg_dt           VARCHAR(14)   NOT NULL DEFAULT TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'),
    upd_dt           VARCHAR(14)   NOT NULL DEFAULT TO_CHAR(NOW(), 'YYYYMMDDHH24MISS'),
    admin_id         VARCHAR(20)   DEFAULT NULL,
    CONSTRAINT pk_base_roadlink PRIMARY KEY (road_id),
    -- ⚠️ 아래 두 CHECK는 실제 운영 DB에 존재하는지 미확인 — 로컬 개발용 안전장치로 추가
    CONSTRAINT ck_base_roadlink_geom_type CHECK (geom_type IS NULL OR geom_type IN ('LINE', 'POLY')),
    CONSTRAINT ck_base_roadlink_use_yn CHECK (use_yn IN ('Y', 'N'))
);

COMMENT ON TABLE base_roadlink IS
    '운영 설정 도로/구역 — 지도관리에서 직접 그려 등록. road_kind: ZONEKIND(01 일반/02 유료 개방식/03 유료 폐쇄식/04 과속·구간단속/05 주정차단속)';
COMMENT ON COLUMN base_roadlink.road_id IS 'PK. 도로 ID — RL-Znnn 자동 채번';
COMMENT ON COLUMN base_roadlink.road_kind IS
    'ROADKIND(0-base, base_codesc): 0 일반/1 유료(개방식)/2 유료(폐쇄식)/3 과속·구간단속/4 주정차단속. base_codelc 부모행 누락 — 하드 FK 없음(소프트 참조)';
COMMENT ON COLUMN base_roadlink.road_nm IS '도로명(표시용)';
COMMENT ON COLUMN base_roadlink.geom_type IS 'LINE(선: 도로/구간) / POLY(면: 단속범위)';
COMMENT ON COLUMN base_roadlink.coords IS '좌표열 [[경도,위도],...] WGS84. 지도 클릭으로 찍은 순서';
COMMENT ON COLUMN base_roadlink.speed_limit_kmh IS '제한속도(km/h) — road_kind=3(과속·구간단속) 판정 기준. 그 외 유형은 참고 보관';
COMMENT ON COLUMN base_roadlink.use_yn IS 'Y 사용 / N 미사용(지도·판정에서 제외)';
COMMENT ON COLUMN base_roadlink.reg_dt IS '등록일시 YYYYMMDDHH24MISS';
COMMENT ON COLUMN base_roadlink.upd_dt IS '수정일시 YYYYMMDDHH24MISS';
COMMENT ON COLUMN base_roadlink.admin_id IS '최종 수정 관리자 근무번호(base_workerinfo.worker_no)';

-- ⚠️ 아래 인덱스는 실제 운영 DB에 존재하는지 미확인 — road_kind별 조회 최적화를 위한 제안
CREATE INDEX IF NOT EXISTS idx_base_roadlink_road_kind ON base_roadlink (road_kind) WHERE use_yn = 'Y';
