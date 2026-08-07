도로이용 과금(개방형/폐쇄형/구간단속/주정차) 설계 논의 — 이슈 및 처리방향 정리
상세 흐름도/mermaid는 web/docs/charge-flow.html 참조. 이 문서는 논의 중 나온 이슈와 결론만 압축 기재.
※ 원격 DB 접속정보(host/dbname/user/pw)는 보안상 이 파일(git 추적됨)엔 기재 안 함 — Claude 메모리에 별도 기록.

A. [중요] 기존 ruc DB에 이미 완성된 마스터 스키마 존재 (2026-08-04 확인)
개방형/폐쇄형/구간단속/주정차용으로 새 테이블(PRIM_GATE_INFO 등)을 설계했었으나, 실제 운영 DB(스키마 ruc, roadnet 스키마와는 별개)에
이미 훨씬 성숙한 버전이 구축돼 있음을 확인함. 아래 B~H절 중 "기반정보 저장 위치"·"세션 필드" 관련 설계 방향(캐시 전략 등)은
개념적으로 유효하지만, 실제 테이블명/컬럼명은 전부 이 기존 스키마를 따라야 함 — roadnet/sql/prim_charge_create.sql은 폐기 대상.

공통코드 ZONEKIND (base_codesc, codelc='ZONEKIND') — 우리가 논의해온 유형 분류가 이미 코드화돼 있음:
  01 일반도로(거리기반과금) — 데이터 있음 1,525,177건
  02 유료도로(개방식)       — 데이터 있음 31,383건
  03 유료도로(폐쇄식)       — 코드만 정의, 데이터 0건 ← 테스트 대상
  04 과속·구간단속          — 코드만 정의, 데이터 0건 ← 테스트 대상
  05 주정차단속             — 코드만 정의, 데이터 0건 ← 테스트 대상
※ "임의 지역에 테스트 구역 설정"은 결국 03/04/05에 해당하는 데이터를 base_mapinfo(04·05) / base_road+base_tollgate+base_tollsection(03)에
  새로 등록하는 작업임. 새 테이블 설계는 불필요.

실제 마스터/결과 테이블 구조:
  base_road          도로 마스터. toll_type CHECK(O=개방식,C=폐쇄식,N=무료) — 285건. FK 피대상: base_tollgate.road_id
  base_roadlink       도로-형상 연결 (road_id, coords jsonb)
  base_tollgate       게이트. gate_div CHECK(M=본선/개방식 게이트, I=진입, O=진출, B=미사용) — 3건. road_id FK
  base_tollfare       게이트별 CARTYPE(1~6종)×fare_date(요금개정 이력) 정액요금 — 개방식(M) 게이트용
  base_tollsection    (entry_tollgate_id, exit_tollgate_id, car_type, fare_date)가 PK — 폐쇄식 구간요금.
                       ※ 우리가 만들려 했던 GATE_PAIR_ID 불필요 — (진입ID,진출ID) 조합 자체가 짝짓기 키
  base_mapinfo        ZONE 마스터 (zone_id PK, zone_kind, f_node/t_node/road_id/link_id, speed_limit_kmh,
                       geom_type+coords jsonb, color(지도표시용), min/max lon/lat(bbox 인덱스))
  road_link           표준노드링크 슬림 버전 (PRIM_LINK_INFO와 동급, link_id PK, f_node/t_node, coords jsonb)
  base_codelc/codesc  공통코드 (ZONEKIND, CARTYPE, USEYN 등)
  prim_rawgps         원시 GPS (기존에 알던 것과 동일 성격)
  prim_chargehand     원시 감지 로그 — 3유형 공통 1테이블. from_id/to_id/zone_id/dist_m/speed_kmh/speed_limit_kmh/
                       stay_seconds 컬럼을 유형 불문 다 가짐(=charge_type/charge_unit으로 분기). PK(trip_id,device_key,trip_seq)
  prim_tollcharge     TOLL 확정청구. vehicle_type/fare_amount/vat_amount/final_amount/app_send_yn(앱 발송상태)
  prim_vltncharge     SPEED·PARKING 위반 확정. violation_type/speed_limit_kmh/measured_speed_kmh/stay_minutes/fine_amount
  prim_tollpasshist / prim_roadpasshist   통과 이력
  proc_tripaudit / proc_tripaudithist     미종결(AUDIT) 처리
  proc_drivestatus    운행상태
  proc_dayfin*         일별 정산/수정/납부 집계

기존 설계 대비 이미 반영된 실무 요소(우리가 처음 설계에서 놓쳤던 것): 차종별 차등요금(CARTYPE), 요금개정 이력(fare_date),
앱 발송상태(app_send_yn), 공통코드 테이블 패턴(신규 유형 추가 시 코드 INSERT만 하면 됨).
geom 저장 방식은 PostGIS가 아니라 coords(jsonb) — 폴리곤 판정(포함여부 등)을 어떻게 하는지는 애플리케이션단 확인 필요(미확인).

B. 과금 유형 개념 정리 (여전히 유효 — 용어만 ZONEKIND 코드로 치환해서 이해)
점 이벤트: 개방형(ZONEKIND=02) — 게이트 통과 즉시 정액요금(base_tollfare) 확정. 진입/진출 짝짓기·세션 보류 불필요.
구간 짝 이벤트: 폐쇄형(ZONEKIND=03, base_tollsection) · 구간단속(ZONEKIND=04) — 메커니즘 동일("진입 저장 → 진출에서 계산"),
  목적만 다름(거리×단가=통행료 vs 거리÷시간=평균속도 위반판정).
구역 체류 이벤트: 주정차(ZONEKIND=05) — 정차 시작-재개 짝, 판정기준은 거리가 아니라 체류시간.
※ 최초엔 구간단속을 "개방형"으로 오해했으나, 진입-진출 짝이 필요한 메커니즘이라 폐쇄형과 동일 상태머신으로 재분류함.

C. 개방형(ZONEKIND=02) 재정의 관련 — 결론: 정액요금이 맞음
"개방형도 이용거리만큼 과금해야 하는 것 아니냐"는 의문 제기됨 → 웹 검색으로 확인.
국내 개방식 요금소는 "요금소별 사전 산정된 최단이용거리 기준 고정 금액"을 부과 — 개별 차량의 실측 누적거리를 실시간 계산하지 않음.
실측거리 비례는 폐쇄식의 역할. base_tollfare(게이트별 정액) vs base_tollsection(구간별 거리요금) 구조가 이 결론과 정확히 일치.
스마트톨링(2024~ 대왕판교/남해선 시범, 2026 확대)은 요금 "산정방식"이 아니라 "수납방식"(무정차, 번호판인식 후불) 혁신일 뿐,
개방식/폐쇄식 산정 로직 자체는 안 바뀜.
※ 참고: ZONEKIND=01(일반도로, 거리기반과금)이 데이터 152만건으로 이미 존재 — 이건 톨게이트와 무관한 "일반도로 자체에 대한
  거리기반 도로이용료" 개념으로 보임(프로젝트명 RUC=Road User Charge 추정). 개방형/폐쇄형 논의와는 별도 트랙. 상세 미확인 — 필요시 추가 조사.

D. 혼합식(HYBRID) 게이트 — 미해결 이슈로 남김
일부 고속도로 IC는 진입 통과 즉시 소액 정액 과금 + 이후 최종 진출까지 거리요금 추가 정산을 동시에 수행 —
"게이트 1개=유형 1개" 가정이 깨지는 케이스. base_tollgate.gate_div가 지금 M/I/O 중 하나만 갖는 구조라 이 케이스를 못 담음.
반영하려면 한 도로(road_id)에 대해 같은 지점이 gate_div=M 행과 I 행을 동시에 가지도록 허용하거나 별도 설계 필요. 재작업 시 검토.

E. 명명(RawLogWorker.h C++ 구조체 한정 — DB 컬럼명은 기존 스키마 그대로 사용, 건드리지 않음)
"구간 짝짓기 키"를 C++ 세션 필드로 새로 만들 때 SectionID라고 이름 붙이면 기존 VEHICLE_TRIP_SESSION의 "세션"(TTL 트립 세션)
개념과 스펠링/발음이 헷갈림 → GatePairID(또는 GateGroupID) 식으로 명명할 것. DB 쪽은 base_tollsection이 (entry_id,exit_id) 조합을
그대로 키로 쓰므로 이 이슈 자체가 없음 — C++ 세션 구조체에서 그 조합을 임시로 들고 있을 때만 해당.
기존 규칙 재확인: 미터 단위 접미사(_M) 남용 금지, config.ini 파라미터는 최대 밑줄 1개+짧은 접두사(별도 memory 참조).

F. 기반정보 조회 전략 — link.psf 바이너리 vs DB 직접조회, 결론은 유효 (대상 테이블만 교체)
결론: DB 테이블(base_tollgate/base_tollsection/base_tollfare/base_mapinfo) 직접 SELECT가 아니라 인메모리 캐시 경유 권장. psf 통합은 기각.

기각 사유(psf 방식):
 - DATA_FILE_HEAD(CreateData/src/DataFormat.h:121)에 reserved/버전 필드가 전혀 없음 — 섹션 추가 시 헤더 포맷 변경, 전량 재생성 필요.
 - 게이트는 고정크기 레코드로 가능하나, base_mapinfo의 폴리곤형 구역(주정차 등)은 가변길이 설계가 추가로 필요해 복잡도 증가.
 - 테스트 중 구역을 자주 바꿀 목적과 안 맞음 — 매번 CreateData 재실행 + MapMatchSvr 재기동 필요.

채택 사유(DB+캐시 방식):
 - DB는 PostgreSQL(MVCC) — 순수 SELECT는 다른 SELECT/INSERT/UPDATE를 블로킹하지 않음. 우려했던 "Lock 경합"은 근본적으로 낮음.
 - 진짜 리스크는 락이 아니라 PostgrePool의 작은 커넥션 풀(PostgrePool.cpp: 기본 min 3/max 5). getConnection()(PostgrePool.cpp:162)은
   min→max까지는 on-demand 자동 증가하지만 max 초과 시 타임아웃 없이 블로킹 대기, keepPoolAlive()(PostgrePool.cpp:264)가 주기적으로
   다시 min까지 축소 — 매 GPS마다 SELECT를 태우면 기존 배치 INSERT/UPDATE와 커넥션 경쟁으로 처리량 병목.
 - 해법: link.psf의 m_mapLinkInfoList(DataLoader.h:73) 패턴과 동일하게, base_tollgate/base_tollsection/base_tollfare/base_mapinfo
   전체를 프로세스 시작 시 한 번에 SELECT → 인메모리 해시맵 캐시. 조회는 항상 인메모리(DB 왕복 0회). 규모상 부담 없음
   (전국 링크 수십만~백만 건 대비 게이트 3건·구역 03/04/05는 현재 0건 — 앞으로 늘어도 훨씬 작은 규모).
 - 레코드 추가/삭제/수정 시 재기동 불필요하게 하려면: 별도 백그라운드 스레드가 N초(예 30~60초)마다 테이블 재조회 → 새 맵 완성 후
   뮤텍스로 짧게 잠그고 포인터만 교체(old 맵 폐기). PostgrePool.cpp:264-297 keepPoolAlive()와 동일한 기존 패턴 재사용.
 - 참고: 세션맵(m_vtTripSessions, RawLogWorker.h:168)은 스레드별 소유권 분리로 락 자체를 안 씀(RawLogWorker.cpp:140 주석) —
   게이트 캐시도 같은 철학(핫패스에서 락/DB 왕복 최소화) 계승.
 - base_mapinfo.coords가 PostGIS GEOMETRY가 아니라 jsonb임 — 인메모리 캐시로 올릴 때 "포함 여부" 판정 로직(ST_Contains 대체)을
   애플리케이션 코드에서 직접 구현해야 할 가능성 있음. 기존에 이 판정을 어디서 하는지 미확인 — 재작업 시 우선 확인.

G. VEHICLE_TRIP_SESSION 확장 필드 (안) — RawLogWorker.h, C++ 세션 구조체. DB 컬럼명과 무관, 판정 중간상태 보관용
개방형: 해당 없음(점 이벤트, 세션 보류 불필요).
폐쇄형·구간단속(공통 상태머신): 진입중여부, 진입게이트ID, 진입~진출 짝 임시보관용 GatePairID(E절 명명 참고),
  목적(TOLL/SPEED), 진입시각, 누적거리.
주정차: 정차중여부, 정차시작시각, 정차시작좌표.

H. 재작업 시 TODO (우선순위 순, 2026-08-04 갱신 — 새 테이블 설계 불필요로 전면 수정)
1. [부분 확인, I절 참고] base_mapinfo.coords(jsonb) 포함여부 판정 로직이 기존에 있는지 확인 (F절 마지막 항목).
   geom_type='LINE'(구간단속용)은 실데이터로 형식 확인됨(I-4절). geom_type='POLY'(주정차용)는 실사용 사례 0건 —
   point-in-polygon 판정 로직은 여전히 신규 구현 필요.
2. ZONEKIND=03(폐쇄식)/04(구간단속)/05(주정차) 임의 지역 테스트 데이터 등록
   - 03: base_road(toll_type=C) 1건 + base_tollgate(gate_div=I/O 짝) 2건 + base_tollsection(entry/exit/car_type/fare_date) 등록
   - 04/05: base_mapinfo(zone_kind=04 또는 05, f_node/t_node 또는 link_id, speed_limit_kmh 또는 체류허용시간 상당 컬럼 확인) 등록
     ※ base_mapinfo에 주정차 유예시간 컬럼은 여전히 안 보임(범칙금은 I-3절 base_fare로 확인됨) — "유예시간(10분)" 자체는
       하드코딩 여부 결정 필요. ※ zone_kind 실제 저장은 '04'/'05'가 아니라 '4'/'5'(앞자리0 없음) 가능성 큼 — 등록 전 확인(I-4절).
3. VEHICLE_TRIP_SESSION(RawLogWorker.h) 확장 필드 추가 (G절).
4. 게이트/구역 인메모리 캐시 클래스 + 백그라운드 재조회 스레드 구현 (F절, PostgrePool.keepPoolAlive 패턴 참고).
   base_fare/base_carinfo(I절)도 이 캐시 대상에 포함 필요 — 특히 base_carinfo는 device_key 단위 룩업이라 해시맵 적합.
5. RawLogWorker.cpp에 판정 흐름(charge-flow.html 참고, 필요 시 base_* 실제 컬럼명에 맞게 갱신) 구현,
   strChargeInsertSQL(RawLogWorker.h:92) 스텁 해소 — 대상은 prim_chargehand(원시) → prim_tollcharge/prim_vltncharge(확정) 2단계.
6. D절 혼합식(HYBRID) 게이트 케이스 반영 여부 결정.
7. [해결됨, I-2절 참고] C절 ZONEKIND=01(일반도로 거리기반과금, 152만건 기존 데이터)의 실제 용도/판정로직 조사.
   → 별도 트랙 아님. charge_type=0(NODE_STEP)으로 같은 파이프라인에 속하며 base_fare.new_fare를 참조해
     prim_tollcharge로 확정됨을 기존 데이터로 확인.
8. [신규, I-3절] SPEED·PARKING 범칙금 산정 기준을 base_fare로 확정할지 결정 — prim_vltncharge 기존 데이터에
   base_fare 구간표와 다른 고액(3만~9만원대) 패턴도 섞여있어 과금서버 담당자 확인 필요.
9. [신규, I-7절, 최우선] 개방형(OPEN) 모델 확정 — "게이트 통과 즉시 정액"(1절 설계 가정) vs "링크 hop마다
   누적거리"(prim_chargehand 293건 실측 패턴) 중 무엇이 맞는지 담당자 확인 없이는 1절 코드 착수 불가.
   prim_chargehand·prim_tollcharge 간 ID 정합성도 안 맞아 기존 데이터를 참고 삼아 넘겨짚으면 안 됨.
10. [신규, I-8절] 폐쇄형(CLOSED_ROAD) 진입정보 durable 기록 설계 확정 — DB 테이블 구조(prim_chargehand
    선기록 vs 별도 경량 테이블), CLOSED_ROAD 전용 장기 TTL 값, 배치 스윕 주기·임계값 결정. 수집앱이
    시동 ON/OFF를 TRIP_EVENT END/START로 처리하는지도 확인 필요(3절과 연동).

폐기됨: roadnet/sql/prim_charge_create.sql (PRIM_GATE_INFO/PRIM_PARKING_ZONE/PRIM_CHARGEHAND) — 기존 ruc 스키마와 중복 설계.
참고용으로만 남겨두고 실제 작업 대상에서 제외.

I. [2026-08-06] 운영 DB(59.11.91.162/ruc) 실데이터 직접 조회로 확인·발견한 사항
2026-08-04 최초 조사는 pg_description(코멘트) 스캔 위주였음. 오늘은 실제 로우 데이터까지 조회해 아래를 확인·발견함
(상세 흐름도는 web/docs/charge-flow.html 5절, 스키마는 web/docs/charge-tables.html base_fare/base_carinfo 절 참고).

I-1. car_type(차종) 출처 확인됨
  prim_chargehand.device_key = base_carinfo.device_key(UNIQUE) 로 1:1 조인 → base_carinfo.car_type(1~6) 획득.
  주의: tb_vhcl_info(회원 앱쪽 차량정보, PK=mbr_id+vhcl_sn, carmdl_type 컬럼)는 별개 코드체계 — 절대 혼용 금지.

I-2. TOLL 계열(OPEN·CLOSED_ROAD·NODE_STEP) 금액 공식 확인됨
  vat_amount = ROUND(fare_amount × 0.1), final_amount = fare_amount + vat_amount
  — 기존 prim_tollcharge 361건 중 352건(97.5%) 일치. 단 75건은 vat_amount=0 예외로 남아있어 100% 확정 규칙은 아님.
  charge_type=0(NODE_STEP, ZONEKIND=01 일반도로)도 이 파이프라인을 타며 base_fare.new_fare를 요금원으로 사용하는
  것으로 보임(H-7 해결).

I-3. SPEED·PARKING 범칙금 기준 테이블 base_fare 신규 발견 — 단, 확정은 아님
  base_fare(PK=car_type, 6건): new_speeding_fine_10km_down/20km_down/30km_down/30km_up(초과속도 4단계),
  new_parking_fine_10min_down/up(체류시간 2단계), new_fare(일반요금). base_farehist는 개정 이력.
  ⚠ prim_vltncharge 기존 111건 중 base_fare 구간표와 일치하는 소액(500~3,200원대) 패턴과, 이 표 어디에도 없는
  고액(30,000~90,000원대, 실제 도로교통법 과속 범칙금 규모와 유사) 패턴이 동일 테이블에 섞여 있고 등록일시로도
  분리 안 됨 — base_fare가 진짜 유일한 산정 기준인지 과금서버 담당자에게 재확인 필요(H-8).

I-4. base_mapinfo 관련 확인
  - 실제 zone_kind 저장값은 '01'~'05'가 아니라 앞자리0 없는 '1'~'5'(1글자) — 컬럼 코멘트와 다름. 04/05 등록 시 주의.
  - coords(jsonb)는 이미 LineString류([[lon,lat],...])/Polygon류(닫힌 링) 좌표 배열을 담는 용도로 설계돼 있어
    구간단속용 신규 컬럼 불필요. 단, geom_type='POLY'(주정차 폴리곤) 실사용 사례가 현재 0건 — 검증 안 된 경로.
  - base_mapinfo(zone_kind=01/02, 152만+3.1만 건)는 전국 도로를 링크 단위로 잘게 쪼갠 지도 표출용 데이터로 보임
    (예: "경부고속도로"/"강변북로" 등 노선명 그대로). base_tollgate.link_id가 전부 NULL이라 base_tollgate와
    실제로 연결되지 않음 — 개방형·폐쇄형 판정에는 base_mapinfo를 쓰지 않고 base_tollgate 계열만 사용.
  - 결론: base_mapinfo는 구간단속(04)·주정차(05) 판정 전용 기반 테이블. 개방형·폐쇄형은 base_tollgate 계열로 완전히 분리됨.

I-5. 유형별 필요 테이블 요약
  개방형(OPEN):       base_tollgate(M) + base_tollfare           → prim_chargehand(1) → prim_tollcharge
  폐쇄형(CLOSED_ROAD): base_tollgate(I/O) + base_tollsection      → prim_chargehand(2) → prim_tollcharge
  구간단속(SPEED):     base_mapinfo(zone_kind=04) + base_fare     → prim_chargehand(3) → prim_vltncharge(0)
  주정차(PARKING):     base_mapinfo(zone_kind=05) + base_fare     → prim_chargehand(4) → prim_vltncharge(1)
  공통: base_carinfo(device_key→car_type)는 네 유형 모두 요금/범칙금 산정에 필요(I-1).

I-6. 최초 조사(2026-08-04) 누락분
  base_fare, base_carinfo 2개 테이블이 당시 pg_description 스캔에서 빠져 있었음(2026-08-06 실데이터 조회로 발견).
  스키마 문서화 시 코멘트 스캔만으로는 부족 — 실데이터 조회를 병행할 것(교훈).

I-7. [정정 + 신규 미해결] 개방형(OPEN) 모델 자체가 실측과 안 맞음
  I-2에서 "prim_chargehand OPEN 행이 from_id=to_id=게이트ID 패턴으로 실측 확인됨"이라 적었는데, 이는 소수 샘플만
  보고 성급히 일반화한 오류로 정정함. prim_chargehand(charge_type=1) 293건을 전수 확인한 결과:
    - 전부 dist_m이 채워져 있고 from_id≠to_id(road_link.link_id 형식 10자리 숫자)
    - 같은 트립 내에서 이전 행의 to_id = 다음 행의 from_id로 체인처럼 연결됨
      (예: CAR000030_20260724070948 trip_seq 1→2→3, 779m→949m→950m, to_id/from_id 순차 연결)
    - 즉 "게이트 통과 즉시 1건"이 아니라 "맵매칭 링크가 바뀔 때마다(hop) 매번 1건씩" 쌓이는 구조로 보임
  그런데 같은 트립의 prim_tollcharge(2단계 확정)는 from_id=to_id인 점(point) 형태 행을 갖는데, ID 자체가
  prim_chargehand 쪽 링크체인과 전혀 다름(예: 위 트립 prim_chargehand 링크ID군 vs prim_tollcharge의
  2520013913/2520013912/2520013918 — 서로 무관). 두 테이블이 같은 trip_id를 가리키면서도 정합성이 안 맞음.
  결론: prim_chargehand와 prim_tollcharge가 서로 다른 시점에 서로 다른 스크립트로 독립 시딩된 것으로 추정
  (I-3 범칙금 이중패턴, I-2 vat_amount 예외와 같은 종류의 데이터 비정합). 개방형이 "게이트 점 이벤트"인지
  "링크 hop 누적거리"인지 현재 데이터만으로 확정 불가 — 코드 착수 전 담당자 확인 필수(H절에 반영).
  문서 반영: web/docs/charge-flow.html 1절·7절, web/docs/charge-issues.html 개방형 섹션.

I-8. [대응방안 설계] 폐쇄형(CLOSED_ROAD) 휴게소 장시간 정차(시동 OFF) 시 진입정보 유실 위험
  시나리오: 고속도로 폐쇄구간 진입 후 휴게소에서 시동을 끄고 한참(수 시간) 있다가 재시동 → 진출 게이트
  통과 시 요금 미부과 가능성. 원인은 두 가지가 복합적:
    1. 진입 게이트 정보가 인메모리 세션(VEHICLE_TRIP_SESSION, TRIP_ID 단위, m_vtTripSessions)에만 있고 DB엔
       없음 — 기본 TTL(worker.ttl_sec, CFG_DEF_TTL=3600초=1시간, ConfigDefaults.h:29) 초과 정차 시 세션 소실.
    2. 시동 재시작 시 수집앱이 TRIP_EVENT=END/START로 새 TRIP_ID를 발급할 가능성 — 맞다면 TTL을 늘려도
       무의미(애초에 다른 세션이 됨). 수집앱이 실제로 이렇게 동작하는지는 미확인 — 확인 필요.
    3. AppMain.cpp의 rawlog_recover(Config.h:38, "GPS 좀비 PROCESSING 복구 SQL")는 원시 GPS 배치 복구용이지
       세션/진입정보 복구와는 무관 — 현재 이 문제에 대한 기존 대응 메커니즘 없음.
  대응방안(web/docs/charge-flow.html 2절 플로우차트에 반영):
    ① 진입 감지 시점에 세션뿐 아니라 DB에도 즉시 기록(durable) — 진입 이벤트당 1회뿐이라 볼륨 부담 적음
       (README F절 "핫패스 DB 왕복 최소화" 원칙과 충돌 안 됨, 매 GPS 아님)
    ② 진출 게이트 판정 시 TRIP_ID 세션뿐 아니라 DEVICE_KEY 기준 미종결 진입기록(DB)도 함께 조회 — TRIP_ID가
       바뀌어도 DEVICE_KEY로 찾아 정산 가능하게
    ③ CLOSED_ROAD 전용 TTL을 트립 전체 TTL과 분리해 훨씬 길게 설정(예: 12~24시간)
    ④ 그래도 못 찾는 영구 미종결 건은 일 단위 배치로 스윕해 AUDIT 처리(ExpireTtlSessions는 프로세스 인메모리
       기준이라 프로세스 재시작·트립 재시작을 못 버팀 — DB 기반 배치 별도 필요)
  미해결로 남는 것: DB durable 기록의 테이블 구조(prim_chargehand 선기록 vs 별도 경량 테이블), 배치 스윕
  주기, "N시간" 임계값 확정, 그리고 위 2번(트립 경계가 시동 ON/OFF와 연동되는지) 확인.
