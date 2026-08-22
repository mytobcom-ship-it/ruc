# -*- coding: utf-8 -*-
"""정답 경로 기준선 초안 생성 (2026-08-23 최정우)

  대표 트립의 매칭 결과에서 '위상적으로 깨끗한 구간'만 확신(confident)으로 표시하고,
  판단이 필요한 지점은 review 목록으로 뽑는다. 매칭 결과를 그대로 정답으로 삼으면
  순환 논리가 되므로, 확신 구간과 확인 대상을 반드시 분리한다.

  판정
    ok       직전 매칭 링크에서 1 hop 이내로 이어지고 GPS↔링크 이격이 작음
    path     2~4 hop 이지만 경로가 존재 — 경유 링크를 보완하면 이어짐
    detour   B 를 거치면 hop 이 A→C 보다 크게 늘어남(우회 의심)
    reverse  정방향으로는 못 가고 반대방향으로만 닿음(역행 오매칭 의심)
    gap      경로 자체가 없음(중간 미매칭 등)
    far      이격이 크고(기본 25m 초과) 더 가까운 링크가 따로 있음 — 진짜 검토 대상
    offnet   이격이 크지만 매칭 링크가 곧 최근접 링크 — 차량이 도로망 밖(주차장·건물 등)에
             있다는 뜻이라 경로 정답으로 판단할 수 없음. 검토 대상이 아니라 제외 대상
    unmatched 미매칭 좌표
"""
import os as _os, json, math, collections, psycopg2
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

# 정답 기준선 대상 — 실주행 트립만 쓴다. 000093_* 는 직선·등간격 합성 좌표열이라 제외
#   (2026-08-23 검증: 방위 편차 0.00~0.04도, 간격 편차 0.00~0.06m). 자세한 근거는 check_accuracy.py
#   2026-08-23 확대 — 3트립 218점(실주행의 16%)에서 실주행 11트립 전량으로 넓혔다.
TRIPS = ['000370_20260819093236', '000376_20260819093337', '000376_20260819094414',
         '000376_20260819140532', '000376_20260819140856', '000376_20260819141002',
         '000376_20260819171654', '000376_20260819175124', '000376_20260821094609',
         '000376_20260821095239', '000376_20260821095603']

# 사람이 확인해 확정한 정답 — 자동 판정(detour/reverse 등)을 덮어쓴다.
#   000376_20260819140856 G18/G20 : 매칭은 2040424401(반대방향 차로)로 갔으나 오매칭이다.
#     · 두 링크는 9.9m 떨어진 왕복분리 쌍이고 방위가 74° 대 254° 로 정반대
#     · GPS heading 이 G14~G22 내내 72~87° 로 2040424301 방향과 일치
#     · 이격 차이는 4.3m 인데 ACCURACY_M 이 9m 라 거리로는 가릴 수 없는 수준
#     · 2040424401 로 두면 왕복분리 도로를 두 번 건너뛰는 경로가 됨
#   (2026-08-23 확인)
CONFIRMED = {
    '000376_20260819140856': {18: '2040424301', 20: '2040424301'},
}
FAR_M = 25.0
DETOUR_TH = 3

cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cn.set_session(readonly=True); cu = cn.cursor()

adjc = {}; ends = {}
def out(nodes):
    todo = [n for n in nodes if n not in adjc]
    if todo:
        cu.execute("SELECT f_node,link_id,t_node FROM network.moct_link WHERE f_node=ANY(%s)", (todo,))
        d = collections.defaultdict(list)
        for f, l, t in cu.fetchall(): d[f].append((l, t))
        for n in todo: adjc[n] = d.get(n, [])
    return {n: adjc[n] for n in nodes}
def info(lid):
    if lid not in ends:
        cu.execute("SELECT f_node,t_node FROM network.moct_link WHERE link_id=%s", (lid,))
        ends[lid] = cu.fetchone() or (None, None)
    return ends[lid]
def hops(a, b, maxd=4):
    if a == b: return 0
    _, ta = info(a)
    if ta is None: return None
    fr = {ta: []}; seen = {ta}
    for d in range(1, maxd + 1):
        adj = out(list(fr.keys())); nx = {}
        for n, p in fr.items():
            for lid, t in adj.get(n, []):
                if lid == b: return d
                if t not in seen: seen.add(t); nx[t] = p + [lid]
        if not nx: break
        fr = nx
    return None

NEAR_MARGIN_M = 2.0			# 최근접 링크가 이만큼 더 가까워야 '더 가까운 링크가 있다'로 본다

def nearer(tid, seq, dev):
    """매칭 링크보다 뚜렷하게 가까운 링크가 따로 있으면 True"""
    cu.execute("""
      SELECT MIN(ST_Distance(ST_SetSRID(ST_MakePoint(p.gps_lon,p.gps_lat),4326)::geography,
                             ST_Transform(l.geom,4326)::geography))
      FROM ruc.prim_rawgps p
      JOIN network.moct_link l
        ON ST_DWithin(ST_Transform(ST_SetSRID(ST_MakePoint(p.gps_lon,p.gps_lat),4326),5186), l.geom, 300)
      WHERE p.trip_id=%s AND p.gps_seq=%s""", (tid, seq))
    row = cu.fetchone()
    return (row is not None) and (row[0] is not None) and (float(row[0]) < dev - NEAR_MARGIN_M)

result = {}
for tid in TRIPS:
    cu.execute("""
      SELECT p.gps_seq, p.match_status, p.match_link_id, p.speed_kmh, p.gps_dt,
             CASE WHEN p.match_link_id IS NULL THEN NULL ELSE
               ST_Distance(ST_SetSRID(ST_MakePoint(p.gps_lon,p.gps_lat),4326)::geography,
                           ST_Transform(l.geom,4326)::geography) END
      FROM ruc.prim_rawgps p LEFT JOIN network.moct_link l ON l.link_id=p.match_link_id
      WHERE p.trip_id=%s ORDER BY p.gps_seq""", (tid,))
    pts = [{'seq': r[0], 'st': r[1], 'link': r[2], 'spd': r[3], 'dt': r[4],
            'dev': None if r[5] is None else round(float(r[5]), 1)} for r in cu.fetchall()]
    ml = [p for p in pts if p['st'] == 1 and p['link']]
    for i, p in enumerate(pts):
        if p['st'] != 1 or not p['link']:
            p['verdict'] = 'unmatched'; continue
        if p['dev'] is not None and p['dev'] > FAR_M:
            # 더 가까운 링크가 실제로 있는지 확인한다. 없으면(=매칭 링크가 곧 최근접) 차량이
            #   도로망 밖에 있는 것이라 오매칭이 아니다 — 실측 57건 중 대부분이 정차 중
            #   주차장/부지 안이었다(속도 0, 같은 자리에서 이격 31~66m 고정) (2026-08-23 추가)
            p['verdict'] = 'far' if nearer(tid, p['seq'], p['dev']) else 'offnet'
            continue
        idx = ml.index(p)
        prev = ml[idx - 1] if idx > 0 else None
        nxt = ml[idx + 1] if idx + 1 < len(ml) else None
        if prev is None:
            p['verdict'] = 'ok'; continue
        h = hops(prev['link'], p['link'])
        if h is None:
            p['verdict'] = 'reverse' if hops(p['link'], prev['link']) is not None else 'gap'
        elif nxt is not None:
            hbc = hops(p['link'], nxt['link']); hac = hops(prev['link'], nxt['link'])
            if hbc is not None and hac is not None and (h + hbc) - hac >= DETOUR_TH:
                p['verdict'] = 'detour'
            else:
                p['verdict'] = 'ok' if h <= 1 else 'path'
        else:
            p['verdict'] = 'ok' if h <= 1 else 'path'
    # 사람이 확정한 값으로 덮어쓴다
    for q in pts:
        fix = CONFIRMED.get(tid, {}).get(q['seq'])
        if fix:
            q['truth'] = fix
            q['verdict'] = 'confirmed'
            q['note'] = '사람 확인 — 매칭값 %s 은 반대방향 오매칭' % (q['link'] or '(미매칭)')
        elif q['verdict'] in ('ok', 'path'):
            q['truth'] = q['link']
        else:
            q['truth'] = None
    result[tid] = pts

json.dump(result, open(WORK + 'groundtruth_draft.json', 'w'), ensure_ascii=False)

print('%-24s %5s %5s %5s %9s %6s %7s %4s %4s %6s %9s' % ('trip_id','좌표','ok','path','confirmed','detour','reverse','gap','far','offnet','unmatched'))
tot = collections.Counter()
for tid, pts in result.items():
    c = collections.Counter(p['verdict'] for p in pts); tot.update(c)
    print('%-24s %5d %5d %5d %9d %6d %7d %4d %4d %6d %9d' % (tid, len(pts), c['ok'], c['path'],
          c['confirmed'], c['detour'], c['reverse'], c['gap'], c['far'], c['offnet'], c['unmatched']))
print('%-24s %5d %5d %5d %9d %6d %7d %4d %4d %6d %9d' % ('합계', sum(len(v) for v in result.values()),
      tot['ok'], tot['path'], tot['confirmed'], tot['detour'], tot['reverse'], tot['gap'], tot['far'],
      tot['offnet'], tot['unmatched']))
need = tot['detour'] + tot['reverse'] + tot['gap'] + tot['far']
print('\n확인 필요: %d곳 · 정답 확정: %d점 (ok+path+confirmed) · 판단 제외: %d점 (offnet+unmatched)'
      % (need, tot['ok'] + tot['path'] + tot['confirmed'], tot['offnet'] + tot['unmatched']))
