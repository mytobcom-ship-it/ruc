-- =============================================================================
--  RUC 위치검증서버 — 2026-09-17 배포 동반 DB 변경
--
--  대상 : 실서버 ruc DB (59.11.91.162), 스키마 ruc
--  작성 : 2026-09-17, 위치검증서버(MapMatchSvr) 담당
--
--  ※ 이 스크립트는 MapMatchSvr 바이너리 배포와 **함께** 적용해야 한다.
--
--  실행 방법
--    PGPASSWORD=... psql -h 59.11.91.162 -U <user> -d ruc -v ON_ERROR_STOP=1 \
--        -f doc/deploy_2026-09-17.sql
--
--  ※ ON_ERROR_STOP=1 을 반드시 붙일 것 — 사전 점검(0절)에서 걸리면 즉시 멈춘다.
--  ※ 전체가 하나의 트랜잭션이다. 중간 실패 시 아무것도 반영되지 않는다.
--  ※ 재실행해도 안전하다(멱등). 이미 적용된 항목은 건너뛴다.
--
--  변경 요약 — 컬럼 추가·타입 변경·데이터 변경은 **없다**
--    1) prim_chargehand.non_charge_reason : DEFAULT 0 설정              ... DDL(기본값만)
--
--  ※ NOT NULL 승격은 이 스크립트에 **넣지 않았다** — 2절의 설명 참고.
--
--  이 스크립트를 가리키는 곳 (배포 누락 방지용 역참조)
--    · MapMatchSvr/bin/query.sql 최상단 "★ 실서버 배포 시 반드시 함께 적용할 DB 변경" 섹션
--    · MapMatchSvr/src/DataDefine.h   NCR_NORMAL 정의부 주석
--    · MapMatchSvr/src/RawLogWorker.h CHARGE_INSERT_ROW::strNonChargeReason 주석
--    ※ doc/*.md 문서는 .gitignore 로 저장소에 올라가지 않는다. 그래서 다른 PC 에서도 보이는
--      위 세 곳(소스·SQL)에 안내를 남겼다 — 문서만 믿고 배포하면 누락된다.
-- =============================================================================

\set ON_ERROR_STOP on
BEGIN;


-- -----------------------------------------------------------------------------
--  0) 사전 점검 — 대상 컬럼이 예상한 모습인지 확인하고, 아니면 즉시 중단
-- -----------------------------------------------------------------------------
DO $$
DECLARE
    v_missing text := '';
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema='ruc' AND table_name='prim_chargehand'
                      AND column_name='non_charge_reason') THEN
        v_missing := '컬럼 prim_chargehand.non_charge_reason (smallint) 이 없음 — '
                  || 'deploy_2026-09-15.sql 가 먼저 적용돼야 한다';
    END IF;

    IF v_missing <> '' THEN
        RAISE EXCEPTION '[사전 점검 실패] %', v_missing;
    END IF;

    RAISE NOTICE '[0절] 사전 점검 통과';
END $$;


-- -----------------------------------------------------------------------------
--  1) prim_chargehand.non_charge_reason — DEFAULT 0 설정
--
--  왜 필요한가
--    NON_CHARGE_REASON 은 "CHARGE_YN/STATUS 가 Y/0 이 아닐 때 그 사유" 를 남기는 컬럼이고,
--    코드표상 0 = NCR_NORMAL(정상 과금) 이다(DataDefine.h). 그런데 DB 쪽에는 DEFAULT 도
--    NOT NULL 도 없어서, **값을 보장하는 주체가 애플리케이션 하나뿐**이다 —
--    query.sql [charge_insert] 의
--        CASE WHEN U.NON_CHARGE_REASON <> '' THEN U.NON_CHARGE_REASON::SMALLINT ELSE 0 END
--    이 유일한 방어선이다. 이 경로를 타지 않는 INSERT(수작업 보정, 연계 앱, 데이터 이관)가
--    컬럼을 생략하면 그대로 NULL 이 된다.
--
--  실측 (2026-09-17, 로컬 검증 DB 기준 63행)
--    · non_charge_reason IS NULL          : 60행
--    · 그중 CHARGE_YN='N' 인데 사유 없음  : 18행  ← 심사 큐에 올라갔는데 사유 추적 불가
--    NULL 행은 전부 20260907165527~29 에 적재된 것으로, [charge_insert] 에 ELSE 0 변환이
--    들어가기(2026-09-15) 전 바이너리가 넣은 값이다.
--
--  이 변경이 안전한 이유
--    [charge_insert] 는 $31 을 항상 명시적으로 넘기므로 **DEFAULT 는 발동하지 않는다.**
--    즉 엔진의 현재 동작은 조금도 바뀌지 않고, 위에 적은 "다른 경로" 에만 방어선이 생긴다.
--    기존 행의 값도 건드리지 않는다(UPDATE 아님).
-- -----------------------------------------------------------------------------
ALTER TABLE ruc.prim_chargehand
    ALTER COLUMN non_charge_reason SET DEFAULT 0;

COMMENT ON COLUMN ruc.prim_chargehand.non_charge_reason IS
    '과금 제외 사유 코드 — 0=정상 과금. 도로유형별 10단위 대역: '
    '1~10 일반도로 / 11~20 개방식 / 21~30 폐쇄식 / 31~40 구간단속 / '
    '41~50 주정차 / 51~60 면제도로 / 61~70 공통(강제마감). '
    '판정(charge_yn/charge_status)에는 영향을 주지 않는 부가 설명 컬럼이다. '
    '값 정의는 MapMatchSvr/src/DataDefine.h 의 NCR_* 상수가 정본.';


-- -----------------------------------------------------------------------------
--  2) NOT NULL 승격 — 이번에는 적용하지 않는다 (의도적 보류)
--
--  지금 걸 수 없는 이유
--    기존 NULL 행(로컬 기준 60행)이 남아 있어 ALTER ... SET NOT NULL 이 실패한다.
--
--  그 NULL 행을 UPDATE 로 메우지 않는 이유
--    Y/0 행은 0 으로 채우면 되지만, **N/3 행(18건)은 사후에 사유를 판정할 수 없다.**
--    일괄로 61(강제마감)을 넣으면 실제로는 12(게이트 통과 미확정)·23(출구 미확인) 이었을
--    행에 **틀린 사유를 심게 된다** — 추적성을 얻으려다 잘못된 근거를 남기는 셈이다.
--    prim_chargehand 는 prim_rawgps 에서 재생성되는 파생 테이블이므로(원본 보존 확인:
--    21트립·1,616행), **재매칭하면 현재 코드가 각 행에 정확한 사유를 붙여 준다.**
--    어차피 바이너리 배포에는 전체 재매칭이 따라오므로 별도 백필 작업이 필요 없다.
--
--  그래서 순서는 이렇게 간다
--    ① (지금) 위 1절 DEFAULT 0 적용
--    ② (배포 후) 전체 재매칭 — 기존 행이 재생성되며 NULL 이 사라진다
--    ③ (재매칭 후) 아래 문장을 별도로 실행해 NOT NULL 승격
--
--    -- 재매칭 완료 후 실행할 것. 먼저 NULL 이 0 건인지 반드시 확인한다.
--    --   SELECT COUNT(*) FROM ruc.prim_chargehand WHERE non_charge_reason IS NULL;
--    -- ALTER TABLE ruc.prim_chargehand ALTER COLUMN non_charge_reason SET NOT NULL;
-- -----------------------------------------------------------------------------


-- -----------------------------------------------------------------------------
--  3) 적용 결과 확인
-- -----------------------------------------------------------------------------
SELECT
    column_name                                   AS "컬럼",
    data_type                                     AS "타입",
    is_nullable                                   AS "NULL허용",
    COALESCE(column_default, '(없음)')            AS "기본값"
FROM information_schema.columns
WHERE table_schema = 'ruc'
  AND table_name   = 'prim_chargehand'
  AND column_name  = 'non_charge_reason';

-- 참고 — 남아 있는 NULL 행 현황(위 2절의 ②·③ 판단용)
SELECT
    COUNT(*)                                                      AS "전체행",
    COUNT(*) FILTER (WHERE non_charge_reason IS NULL)             AS "사유NULL",
    COUNT(*) FILTER (WHERE charge_yn = 'N'
                       AND COALESCE(non_charge_reason, 0) = 0)    AS "N판정인데사유없음"
FROM ruc.prim_chargehand;

COMMIT;

-- =============================================================================
--  롤백이 필요하면 (DEFAULT 제거)
--    ALTER TABLE ruc.prim_chargehand ALTER COLUMN non_charge_reason DROP DEFAULT;
-- =============================================================================
