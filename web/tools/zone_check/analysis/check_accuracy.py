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
"""

# 지표 산출 제외 대상 — 위 주석 참고
EXCLUDE_PREFIX = ('000093_',)
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
