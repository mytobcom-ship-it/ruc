# -*- coding: utf-8 -*-
"""정답 기준선 대비 매칭 정확도 측정 (2026-08-23 최정우)

  build_groundtruth.py 가 만든 work/groundtruth_draft.json 의 truth 값과
  현재 prim_rawgps 의 match_link_id 를 대조한다.
  설정(maxstep·hoppenalty 등)을 바꿔 재매칭한 뒤 이 스크립트를 돌리면
  개선인지 악화인지 즉시 판별된다.
"""
import os as _os, json, sys, collections, psycopg2
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'work')) + '/'
GT = json.load(open(WORK + 'groundtruth_draft.json'))
cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cn.set_session(readonly=True); cu = cn.cursor()

print('%-24s %6s %6s %6s %7s  %s' % ('trip_id', '정답점', '일치', '불일치', '미매칭', '정확도'))
T = collections.Counter(); wrong = []
for tid, pts in GT.items():
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
