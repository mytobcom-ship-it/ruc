# -*- coding: utf-8 -*-

"""정답 기준선 대비 매칭 정확도 측정 (2026-08-23 최정우)

  build_groundtruth.py 가 만든 work/groundtruth_draft.json 의 truth 값과
  현재 prim_rawgps 의 match_link_id 를 대조한다.
  설정(maxstep·hoppenalty 등)을 바꿔 재매칭한 뒤 이 스크립트를 돌리면
  개선인지 악화인지 즉시 판별된다.

  ※ 000093_* 트립군은 지표에서 제외한다 (2026-08-23 검증 후 결정).
     10개 트립 전부 방위 편차 0.00~0.04도, 간격 편차 0.00~0.06m 인 "직선·등간격" 좌표열이다.
     주행 궤적이 아니라 출발점에서 한 방위로 일정 거리씩 찍은 합성 데이터라, 직선이 도로망을
     가로지르며 지난다 — 우연히 도로와 겹치면 이격 2m, 산지·고속도로 사이를 지나면 451m.
     미매칭 171건 중 122건(71%)이 radius=50 밖이고 평균 107m 떨어져 있는데, 이건 오차가
     아니라 애초에 도로를 따라가지 않는 선이므로 매칭 실패가 정상 동작이다.
     평행이동·좌표계 오류가 아니어서(트립별 ΔX/ΔY 편차 55~159m) 보정도 불가능하다.
     반경 검사·이상속도 검사의 시험 데이터로는 유효하므로 데이터 자체는 남겨 둔다.

  ※ 시뮬레이터 합성 트립(DEVICE_KEY 가 '9' 로 시작)도 제외한다 (2026-08-23 추가).
     실주행은 000093/000370/000376 처럼 0 으로 시작하고, 시뮬레이터는 9 로 시작한다.
     같은 테이블(ruc.prim_rawgps)에 들어가므로 걸러내지 않으면 실주행 지표가 오염된다 —
     000093 합성 좌표열이 몇 주간 매칭률을 끌어내렸던 것과 같은 함정이다.
     합성 표본의 검증은 ruc.sim_truth 를 쓰는 sim_verify.py 가 따로 한다.

  ※ 이 기준선의 성격 — "독립 정답"이 아니라 "회귀 감지용 스냅샷"이다 (2026-08-23 명시).
     truth 값은 사람이 지도를 보고 찍은 것이 아니라, 매칭 결과 중 위상적으로 깨끗한
     구간(ok/path)을 그대로 채택한 것이다. 따라서 기준선을 만든 그 시점의 엔진으로
     측정하면 정확도는 정의상 100% 가 나온다 — 그 숫자 자체에는 의미가 없다.
     의미가 있는 건 "설정·코드를 바꾼 뒤에도 100% 인가"다. 떨어지면 그 지점이 회귀다.
     사람이 직접 확인해 확정한 값만 CONFIRMED 로 따로 표시한다(build_groundtruth.py).

## 기준선 갱신 이력 (2026-09-06 최정우 추가, 사용자 지시)

  2026-09-06  build_groundtruth.py 재실행으로 기준선 갱신.
    갱신 시점의 엔진 상태 = ③ hoppenalty_lenratio 0.0->0.5 + ⓐ 클램프 브릿지 경로기반 확장.
    정답 모집단 958 -> 1025점(+67). 갱신 직후 정확도는 정의상 100%(자기 자신과 비교).
    갱신 전 마지막 측정: 매칭율 80.3% / 정확도 91.5%(구 기준선 기준).

## 보완 제안 — 정확도가 100%가 아닌 값으로 떨어졌을 때 확인할 것

  이 기준선은 "독립 정답"이 아니라 갱신 시점 엔진 출력 중 위상적으로 깨끗한 구간
  (ok+path)만 채택한 스냅샷이다. 그래서 불일치가 나오면 두 가지 가능성이 있고,
  **어느 쪽인지 먼저 가린 뒤에 조치해야 한다.**

    (A) 엔진이 나빠졌다(진짜 회귀)         -> 변경분을 되돌린다
    (B) 엔진이 좋아졌는데 기준선이 옛것이다 -> 기준선을 갱신한다

  가리는 방법 — 정답이 필요 없는 지표를 함께 본다:
    · 이격거리    : 달라진 지점에서 원시 GPS ~ 전/후 링크 수직거리. 가까워졌으면 개선
    · 연결 정상률 : 직전 링크에서 6홉 내 도달 가능한 전이 비율(quality.py 로직).
                    역방향 오매칭/위상 단절 건수도 함께 본다
    · 매칭 성공율 : match_rate.py

  **보정 가능한 경우 제안할 것** — 위 보조 지표가 모두 개선을 가리키는데 이 정확도만
  떨어졌다면, 그건 기준선이 뒤처진 것이므로 **기준선 갱신(build_groundtruth.py 재실행)을
  보완 방법의 하나로 제시한다.** 갱신 전 반드시 work/groundtruth_draft.json 을 백업하고,
  갱신 시점의 엔진 설정(config.ini 의 hoppenalty_lenratio 등)과 코드 상태를 이 이력에
  함께 적어 나중에 어느 엔진 기준인지 되짚을 수 있게 한다.

  주의: 갱신하면 그 시점 엔진 출력이 정답으로 굳는다. **잘못된 매칭이 섞인 채로 갱신하면
  그 오류가 정답이 되어 이후로는 영영 안 잡힌다.** 그래서 갱신 전에 위 보조 지표로
  "지금 상태가 이전보다 나은가"를 반드시 먼저 확인할 것.
"""

# 지표 산출 제외 대상 — 위 주석 참고
EXCLUDE_PREFIX = ('000093_', '9')
import os as _os, json, sys, collections, psycopg2
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'work')) + '/'
GT = json.load(open(WORK + 'groundtruth_draft.json'))
cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cn.set_session(readonly=True); cu = cn.cursor()

print('%-24s %6s %6s %6s %7s  %s' % ('trip_id', '정답점', '일치', '불일치', '미매칭', '정확도'))
T = collections.Counter(); wrong = []
for tid, pts in GT.items():
    if tid.startswith(EXCLUDE_PREFIX): continue
    truth = {p['seq']: p['truth'] for p in pts if p.get('truth')}
    if not truth: continue
    cu.execute("SELECT gps_seq, match_link_id, match_status FROM ruc.prim_rawgps WHERE trip_id=%s", (tid,))
    cur = {r[0]: (r[1], r[2]) for r in cu.fetchall()}
    hit = miss = un = 0
    for seq, want in truth.items():
        got, st = cur.get(seq, (None, None))
        if st != 1 or not got: un += 1
        elif got == want: hit += 1
        else:
            miss += 1; wrong.append((tid, seq, want, got))
    n = len(truth); T['n'] += n; T['hit'] += hit; T['miss'] += miss; T['un'] += un
    print('%-24s %6d %6d %6d %7d  %5.1f%%' % (tid, n, hit, miss, un, 100.0 * hit / n))
print('%-24s %6d %6d %6d %7d  %5.1f%%' % ('합계', T['n'], T['hit'], T['miss'], T['un'],
      100.0 * T['hit'] / max(1, T['n'])))
if wrong:
    print('\n── 불일치 지점 ──')
    for tid, seq, want, got in wrong[:20]:
        print('  %s G%-4d 정답 %s ↔ 현재 %s' % (tid, seq, want, got))
    if len(wrong) > 20: print('  ... 외 %d건' % (len(wrong) - 20))
