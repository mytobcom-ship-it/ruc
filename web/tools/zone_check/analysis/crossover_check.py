# -*- coding: utf-8 -*-
"""크로스오버(중앙 지선) 1틱 오매칭 측정 (2026-08-23 최정우)

  무엇을 재는가
    노드 N 에서 짧은 지선 B(15m 미만)와 본선 C 가 함께 갈라질 때, 직진 통과하는 차량이
    그 한 틱만 B 에 붙는 현상이다. 미해결 이슈 3(우회 매칭)의 정체로 실주행에서 확인됐다
    (000376_20260819093337 seq119 · 000376_20260821094609 seq54 · 000376_20260821095603 seq35).
    실주행 표본이 3건뿐이라 임계값을 정할 수 없었고, 그래서 시뮬레이터로 수백 건을 만든다.

  왜 시뮬레이터인가
    ruc.sim_truth 에 참 링크가 들어 있어 "차가 실제로 어디 있었는지"를 안다. 정답이
    엔진 출력과 독립이라 순환 논리가 없다.

  판정
    통과누락  참 링크는 C(또는 A)인데 엔진이 B 로 붙음  <- 잡아야 할 오매칭
    지선정상  참 링크가 B 이고 엔진도 B                  <- 건드리면 안 되는 정상
    지선누락  참 링크가 B 인데 엔진이 다른 링크          <- 과교정 위험 지표
    정상      그 외

  실행: python3 crossover_check.py
"""
import os as _os, json, collections, psycopg2

WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'work')) + '/'
CROSS = json.load(open(WORK + 'crossover.json'))

BSET = {r['B'] for r in CROSS}
BINFO = {}
for r in CROSS:
    BINFO.setdefault(r['B'], []).append(r)

r = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
r.set_session(readonly=True); cr = r.cursor()
cr.execute("""SELECT t.trip_id, t.gps_seq, t.true_link_id, t.offset_m, t.speed_kmh,
                     p.match_status, p.match_link_id
              FROM ruc.sim_truth t JOIN ruc.prim_rawgps p USING (trip_id, gps_seq)
              ORDER BY t.trip_id, t.gps_seq""")
rows = cr.fetchall()
if not rows:
    raise SystemExit('ruc.sim_truth 가 비어 있습니다 — 시뮬레이터를 먼저 돌리세요')

trips = collections.defaultdict(list)
for x in rows:
    trips[x[0]].append(x)

stat = collections.Counter()
miss = []          # 통과누락 상세
lost = []          # 지선누락 상세
for tid, pts in trips.items():
    for i, p in enumerate(pts):
        truth, mlink = p[2], p[6]
        if p[5] != 1 or not mlink:
            continue
        if mlink in BSET and truth != mlink:
            stat['통과누락'] += 1
            prev = pts[i - 1][6] if i > 0 else None
            nxt = pts[i + 1][6] if i + 1 < len(pts) else None
            ang = BINFO[mlink][0]['ang']; blen = BINFO[mlink][0]['blen']
            miss.append((tid, p[1], truth, mlink, prev, nxt, ang, blen, float(p[3])))
        elif truth in BSET and mlink == truth:
            stat['지선정상'] += 1
        elif truth in BSET and mlink != truth:
            stat['지선누락'] += 1
            lost.append((tid, p[1], truth, mlink, float(p[3])))
        else:
            stat['정상'] += 1

tot = sum(stat.values())
print('합성 표본 %d건 · 매칭 성공 %d건 (트립 %d개)\n' % (len(rows), tot, len(trips)))
print('── 크로스오버 판정 ──')
for k in ('정상', '통과누락', '지선정상', '지선누락'):
    if stat[k]:
        print('  %-8s %6d  (%.2f%%)' % (k, stat[k], 100.0 * stat[k] / tot))

if miss:
    byang = collections.Counter(('180도형' if m[6] > 150 else '90도형' if 60 <= m[6] <= 120 else '기타')
                                for m in miss)
    print('\n── 통과누락(잡아야 할 오매칭) %d건 ──' % len(miss))
    print('  교차로 유형별: %s' % dict(byang))
    print('  %-24s %5s %-11s %-11s %-11s %-11s %5s %6s' %
          ('trip_id', 'seq', '참링크', '오매칭B', '직전', '다음', '각차', 'B길이'))
    for m in sorted(miss, key=lambda y: -y[8])[:15]:
        print('  %-24s %5d %-11s %-11s %-11s %-11s %4d° %5.1fm' %
              (m[0], m[1], m[2], m[3], m[4] or '-', m[5] or '-', m[6], m[7]))

if lost:
    print('\n── 지선누락(과교정하면 안 되는 정상 진입) %d건 ──' % len(lost))
    for m in lost[:10]:
        print('  %-24s %5d 참=%-11s 매칭=%-11s 오차 %.1fm' % (m[0], m[1], m[2], m[3], m[4]))
