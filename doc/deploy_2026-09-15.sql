-- =============================================================================
--  RUC 위치검증서버 — 2026-09-15 배포 동반 DB 변경
--
--  대상 : 실서버 ruc DB (59.11.91.162), 스키마 ruc
--  작성 : 2026-09-15, 위치검증서버(MapMatchSvr) 담당
--
--  ※ 이 스크립트는 MapMatchSvr 바이너리 배포와 **함께** 적용해야 한다.
--    로컬 검증 DB(127.0.0.1/ruc)에는 이미 적용·검증 완료된 내용이다.
--
--  실행 방법
--    PGPASSWORD=... psql -h 59.11.91.162 -U <user> -d ruc -v ON_ERROR_STOP=1 \
--        -f doc/deploy_2026-09-15.sql
--
--  ※ ON_ERROR_STOP=1 을 반드시 붙일 것 — 사전 점검(0절)에서 걸리면 즉시 멈춘다.
--  ※ 전체가 하나의 트랜잭션이다. 중간 실패 시 아무것도 반영되지 않는다.
--  ※ 재실행해도 안전하다(멱등). 이미 적용된 항목은 건너뛴다.
--
--  변경 요약 — 컬럼 추가나 타입 변경은 **없다**
--    1) base_codelc      : 행 1건 INSERT (ROADKIND 대분류 누락 보완)      ... DML
--    2) base_codesc      : 행 1건 INSERT (ROADKIND=5 면제도로 등록)        ... DML
--    3) base_roadlink    : CHECK 제약 1개 ADD (speed_limit_kmh 범위 제한)  ... DDL
--    4) prim_chargehand  : 컬럼 코멘트 2건 갱신 (OCCUR_DT, NON_CHARGE_REASON)
--                          ... 메타데이터만, 데이터·구조 변화 없음
-- =============================================================================

\set ON_ERROR_STOP on
BEGIN;


-- -----------------------------------------------------------------------------
-- 0. 사전 점검 — 3) 제약을 걸 수 있는 상태인지 먼저 확인한다.
--
--    실서버 데이터는 로컬과 다를 수 있다. 범위 밖(0 미만 또는 255 초과) 값이
--    하나라도 있으면 ALTER 가 실패하므로, 여기서 먼저 멈추고 데이터를 정리한 뒤
--    다시 실행해야 한다. 아래 쿼리가 0 이 아니면 RAISE 로 중단된다.
-- -----------------------------------------------------------------------------
DO $$
DECLARE
    v_bad_cnt   integer;
    v_bad_list  text;
BEGIN
    SELECT count(*),
           string_agg(road_id || '=' || speed_limit_kmh::text, ', ' ORDER BY road_id)
      INTO v_bad_cnt, v_bad_list
      FROM ruc.base_roadlink
     WHERE speed_limit_kmh IS NOT NULL
       AND (speed_limit_kmh < 0 OR speed_limit_kmh > 255);

    IF v_bad_cnt > 0 THEN
        RAISE EXCEPTION
            'speed_limit_kmh 범위 밖 데이터 %건 — 제약 추가 불가. 먼저 정리할 것: %',
            v_bad_cnt, v_bad_list;
    END IF;

    RAISE NOTICE '[사전점검] speed_limit_kmh 범위 위반 0건 — 진행 가능';
END $$;


-- -----------------------------------------------------------------------------
-- 0-2. 사전 점검 — 엔진이 요구하는 선행 스키마 6종이 모두 있는지 확인
--
--      ※ 아래 6종은 **이번 배포분의 변경사항이 아니다.** 2026-08-28 ~ 09-01 에 걸쳐
--        엔진이 쓰기 시작한 것들로, MapMatchSvr/bin/query.sql 헤더의 "이 파일의 SQL 이
--        전제하는 DB 스키마" 블록에 DDL 원문이 있다. 실서버에 이미 적용돼 있다면 이
--        점검은 그냥 통과한다.
--
--      여기서 막는 이유: 없으면 **기동은 되는데 해당 SQL 만 런타임에 실패**한다.
--      2026-08-29 실측 사례 — prim_chargehand.start_gps_seq 가 없어 과금 bulk INSERT 가
--      전건 실패했고, 같은 트랜잭션이라 그 배치의 맵매칭 결과까지 통째로 되돌아갔다.
--      배포 후에 발견하면 이미 데이터가 밀린 뒤다.
--
--      빠진 항목이 있으면 **전부 나열하고** 중단한다. 그때는 query.sql 헤더의 DDL 을
--      먼저 적용하고(전부 IF NOT EXISTS 라 재실행 안전) 이 스크립트를 다시 실행할 것.
-- -----------------------------------------------------------------------------
DO $$
DECLARE
    v_missing text[] := ARRAY[]::text[];
BEGIN
    -- (1~4) 컬럼 4종
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema='ruc' AND table_name='prim_chargehand'
                      AND column_name='start_gps_seq') THEN
        v_missing := v_missing || '컬럼 prim_chargehand.start_gps_seq (bigint NOT NULL DEFAULT 0)'::text;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema='ruc' AND table_name='prim_chargehand'
                      AND column_name='end_gps_seq') THEN
        v_missing := v_missing || '컬럼 prim_chargehand.end_gps_seq (bigint NOT NULL DEFAULT 0)'::text;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema='ruc' AND table_name='prim_chargehand'
                      AND column_name='non_charge_reason') THEN
        v_missing := v_missing || '컬럼 prim_chargehand.non_charge_reason (smallint) — 과금 INSERT $31'::text;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema='ruc' AND table_name='prim_rawgps'
                      AND column_name='match_rsv_dt') THEN
        v_missing := v_missing || '컬럼 prim_rawgps.match_rsv_dt (char(14)) — 없으면 맵매칭 자체가 진행 안 됨'::text;
    END IF;

    -- (5) 인덱스 1종 — 없어도 동작은 하나 좀비 PROCESSING 회수가 풀스캔이 된다
    IF NOT EXISTS (SELECT 1 FROM pg_indexes
                    WHERE schemaname='ruc' AND indexname='ix_prim_rawgps_stale_proc') THEN
        v_missing := v_missing || '인덱스 ix_prim_rawgps_stale_proc — prim_rawgps(match_rsv_dt) WHERE match_status=2'::text;
    END IF;

    -- (6) 테이블 1종 — 없으면 주정차 임계가 0 이 되어 모든 정차가 위반 등록된다
    IF NOT EXISTS (SELECT 1 FROM pg_tables
                    WHERE schemaname='ruc' AND tablename='base_parking_fine') THEN
        v_missing := v_missing || '테이블 base_parking_fine — 없으면 주정차 단속 임계가 0분이 되어 과다 등록'::text;
    END IF;

    IF array_length(v_missing, 1) > 0 THEN
        RAISE EXCEPTION
            E'엔진 선행 스키마 %건 누락 — 배포 중단.\n누락 목록:\n  - %\n\nMapMatchSvr/bin/query.sql 헤더의 "이 파일의 SQL 이 전제하는 DB 스키마" 블록 DDL 을 먼저 적용한 뒤 재실행할 것.',
            array_length(v_missing, 1), array_to_string(v_missing, E'\n  - ');
    END IF;

    RAISE NOTICE '[사전점검] 엔진 선행 스키마 6종(컬럼 4·인덱스 1·테이블 1) 모두 존재 — 진행 가능';
END $$;


-- -----------------------------------------------------------------------------
-- 1. base_codelc — ROADKIND 대분류 행 추가                              [DML]
--
--    배경: 공통코드 부모 테이블에 ROADKIND 대분류 행 자체가 없었다(하위 코드
--    base_codesc 만 존재). 두 테이블 사이에 FK 제약이 없어 동작 오류는 없었으나,
--    코드 마스터를 codelc -> codesc 로 순회하는 화면·리포트에서 ROADKIND 그룹이
--    통째로 누락된다.
-- -----------------------------------------------------------------------------
INSERT INTO ruc.base_codelc (codelc, codelc_nm, codelc_note, codelc_use_yn)
VALUES ('ROADKIND', '과금 도로 유형', 'base_roadlink.road_kind 가 참조', 'Y')
ON CONFLICT (codelc) DO NOTHING;


-- -----------------------------------------------------------------------------
-- 2. base_codesc — ROADKIND = 5 (면제도로) 추가                         [DML]
--
--    배경: base_roadlink 에 road_kind='5'(면제도로) 구역이 실제로 운영 중인데
--    코드 마스터에는 0~4 만 있어, 조인 시 명칭이 NULL 로 빠졌다.
--    명칭·note 는 기존 항목 관례를 따랐다(dist/tollOpen/tollClosed/section/parking
--    -> exempt). 엔진도 ROAD_KIND=5 를 "면제도로"로 지칭한다.
-- -----------------------------------------------------------------------------
INSERT INTO ruc.base_codesc (codelc, codesc, codesc_nm, codesc_note, ary_ordr, codesc_use_yn)
VALUES ('ROADKIND', '5', '면제도로', 'exempt', 6, 'Y')
ON CONFLICT (codelc, codesc) DO NOTHING;


-- -----------------------------------------------------------------------------
-- 3. base_roadlink — speed_limit_kmh 범위 CHECK 제약 추가                [DDL]
--
--    ※ 컬럼 타입·NULL 허용·기본값은 **전혀 바뀌지 않는다.** 허용 값 범위만 제한한다.
--       (numeric / NULL 허용 / 기본값 없음 — 그대로)
--
--    배경: 이 컬럼은 precision 도 CHECK 도 없는 numeric 이라 지도관리 화면에서
--    임의 값이 들어갈 수 있었다. 값은 prim_chargehand.speed_limit_kmh(SMALLINT)로
--    캐스팅되므로 범위 밖 1건이 그 배치의 과금 INSERT 전건을 실패시킨다.
--    음수는 더 위험하다 — 평균속도가 항상 제한속도 이상이 되어 **모든 통행이
--    위반으로 적재**된다(결함 주입으로 실증).
--
--    상한 255 근거: 엔진의 링크 제한속도 표현이 uint8(LINK_INFO.nMaxSpeed)이라
--    이미 같은 천장을 전제한다. 법정 상한(120 등)으로 잡으면 향후 상향 시 정상
--    데이터를 막게 되므로 쓰지 않았다.
--    NULL 은 "해당 없음"(구간단속 외 유형은 전부 NULL)이라 그대로 허용한다.
--
--    ※ 엔진 측 클램프(BulkInsertCharges)와 역할이 다르다 — 클램프는 배치 전멸을
--      막는 소비자 측 방어, 이 제약은 입력 단계 차단. 둘 다 유지한다.
--
--    ※ 운영 영향: 이 제약 추가 후 지도관리 화면에서 범위 밖 값을 저장하면 DB 가
--      거부한다. 화면이 DB 오류를 어떻게 표시하는지 배포 전 확인 권장.
-- -----------------------------------------------------------------------------
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM pg_constraint
         WHERE conrelid = 'ruc.base_roadlink'::regclass
           AND conname  = 'base_roadlink_speed_limit_chk'
    ) THEN
        ALTER TABLE ruc.base_roadlink
          ADD CONSTRAINT base_roadlink_speed_limit_chk
          CHECK (speed_limit_kmh IS NULL
                 OR (speed_limit_kmh >= 0 AND speed_limit_kmh <= 255));
        RAISE NOTICE '[3] base_roadlink_speed_limit_chk 추가됨';
    ELSE
        RAISE NOTICE '[3] base_roadlink_speed_limit_chk 이미 존재 — 건너뜀';
    END IF;
END $$;


-- -----------------------------------------------------------------------------
-- 4. prim_chargehand — 컬럼 코멘트 갱신                          [메타데이터]
--
--    ※ 데이터도 구조도 바뀌지 않는다. 조회·정산 담당자가 psql \d+ 나 ERD 도구에서
--      보는 설명문만 갱신한다. 로컬 검증 DB에는 이미 반영돼 있어, 실서버만 맞추면
--      두 환경의 스키마 메타데이터가 다시 동일해진다.
--
--    4-1) OCCUR_DT — 기준 시각이 과금유형별로 다르다는 사실이 어디에도 적혀 있지
--         않아, 조회측이 6유형 전부를 "진입 시각"으로 오해할 소지가 있었다.
--         일반도로만 진출 시각인 것은 2026-08-14 사용자 지시로 확정된 설계다.
--    4-2) NON_CHARGE_REASON — 10단위 대역 체계를 컬럼 코멘트만 보고 해석할 수
--         있게 한다. 이번 배포분부터 NULL 이 아니라 항상 채워진다(정상 과금 = 0).
-- -----------------------------------------------------------------------------
COMMENT ON COLUMN ruc.prim_chargehand.occur_dt IS
    '과금/위반/해당구간 발생 시각. 기준이 과금유형별로 다름 — 일반도로(CHARGE_TYPE=0)는 구간 진출 시각, 나머지 5유형(개방형/폐쇄형/구간단속/주정차/면제)은 구간 진입 시각. 따라서 구간 시간범위는 일반도로가 (OCCUR_DT-STAY_SECONDS ~ OCCUR_DT), 그 외는 (OCCUR_DT ~ OCCUR_DT+STAY_SECONDS)';

COMMENT ON COLUMN ruc.prim_chargehand.non_charge_reason IS
    '과금/비과금 사유 코드(위치검증서버에서 적재). 0: 정상 과금, 1~10: 일반도로, 11~20: 개방형, 21~30: 폐쇄형, 31~40: 구간단속, 41~50: 주정차, 51~60: 면제도로, 61~70: 공통(TTL/강제종료)';


COMMIT;


-- =============================================================================
--  적용 결과 확인 (COMMIT 이후 조회)
-- =============================================================================
\echo ''
\echo '=== 1) ROADKIND 대분류 (1건이어야 함) ==='
SELECT codelc, codelc_nm, codelc_use_yn
  FROM ruc.base_codelc WHERE codelc = 'ROADKIND';

\echo '=== 2) ROADKIND 하위 코드 (0~5, 6건이어야 함) ==='
SELECT codesc, codesc_nm, codesc_note, ary_ordr
  FROM ruc.base_codesc WHERE codelc = 'ROADKIND' ORDER BY ary_ordr;

\echo '=== 3) CHECK 제약 (1건이어야 함) ==='
SELECT conname, pg_get_constraintdef(oid) AS 정의
  FROM pg_constraint WHERE conname = 'base_roadlink_speed_limit_chk';

\echo '=== 4) 코드 마스터 미매칭 구역 (0건이어야 함) ==='
SELECT count(*) AS 미등록_road_kind
  FROM ruc.base_roadlink b
 WHERE NOT EXISTS (SELECT 1 FROM ruc.base_codesc c
                    WHERE c.codelc = 'ROADKIND' AND c.codesc = b.road_kind);

\echo '=== 5) 컬럼 코멘트 (2건 모두 내용이 채워져 있어야 함) ==='
SELECT a.attname AS 컬럼, d.description AS 코멘트
  FROM pg_attribute a
  LEFT JOIN pg_description d ON d.objoid = a.attrelid AND d.objsubid = a.attnum
 WHERE a.attrelid = 'ruc.prim_chargehand'::regclass
   AND a.attname IN ('occur_dt', 'non_charge_reason')
 ORDER BY a.attnum;


-- =============================================================================
--  롤백 (필요 시에만 수동 실행)
-- =============================================================================
--  BEGIN;
--    ALTER TABLE ruc.base_roadlink DROP CONSTRAINT base_roadlink_speed_limit_chk;
--    DELETE FROM ruc.base_codesc WHERE codelc = 'ROADKIND' AND codesc = '5';
--    DELETE FROM ruc.base_codelc WHERE codelc = 'ROADKIND';
--  COMMIT;
--
--  ※ 2)를 롤백하면 road_kind='5' 구역의 명칭이 다시 NULL 로 빠진다(과금에는 영향 없음).
--  ※ 1)만 단독 롤백하면 하위 코드가 부모 없이 남는다 — 원래 상태로 돌아갈 뿐이다.
--  ※ 4)는 롤백 대상이 아니다 — 설명문일 뿐 동작에 관여하지 않는다. 굳이 되돌리려면
--    COMMENT ON COLUMN ... IS NULL 로 지울 수 있으나, 되돌릴 이유가 없다.
-- =============================================================================
