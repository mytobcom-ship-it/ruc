# -*- coding: utf-8 -*-
"""크로스오버 지표 한 줄 요약 — hop_sweep.sh 가 반복 호출한다 (2026-08-23 최정우)
   ① 직진 통과 중 지선으로 튄 비율   ② 지선 진입을 놓친 비율   ③ 우회 매칭(실주행) 건수"""
import os as _os, json, collections, psycopg2
W = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'work')) + '/'
CR = json.load(open(W + 'crossover.json'))
BSET = {r['B'] for r in CR}
AC = {}
for r in CR:
    AC.setdefault((r['A'], r['C']), r)

r = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
r.set_session(readonly=True); cr = r.cursor()
cr.execute("""SELECT t.trip_id, t.gps_seq, t.true_link_id, p.match_status, p.match_link_id
              FROM ruc.sim_truth t JOIN ruc.prim_rawgps p USING (trip_id, gps_seq)
              ORDER BY t.trip_id, t.gps_seq""")
tr = collections.defaultdict(list)
for x in cr.fetchall():
    tr[x[0]].append(x)

pas = bad = stub = lost = 0
for tid, pts in tr.items():
    for i in range(1, len(pts)):
        a, b = pts[i - 1], pts[i]
        if a[2] != b[2]:
            info = AC.get((a[2], b[2]))
            if info:
                pas += 1
                if any(p[4] == info['B'] for p in pts[max(0, i - 2):i + 3] if p[3] == 1):
                    bad += 1
        if b[2] in BSET:
            stub += 1
            if b[3] == 1 and b[4] != b[2]:
                lost += 1
print('  ① 직진통과 %3d건 중 지선오매칭 %2d건 (%.1f%%)' % (pas, bad, 100.0 * bad / pas if pas else 0))
print('  ② 지선진입 %3d건 중 누락       %2d건 (%.1f%%)' % (stub, lost, 100.0 * lost / stub if stub else 0))
