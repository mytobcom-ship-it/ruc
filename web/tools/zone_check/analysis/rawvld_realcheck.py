# -*- coding: utf-8 -*-
"""RAW_VLD=false 좌표를 실주행 데이터만으로 검증 (2026-08-23 최정우)

  묻는 것
    엔진은 raw_vld=false 행을 맵매칭에서 통째로 SKIP 한다(실주행 1,355점 중 210점, 15.5%).
    그 좌표들이 정말 못 쓸 좌표인가?

  왜 대용치로는 부족한가
    앞서 "최근접 링크까지 거리"로 재봤더니 210건 중 87.6% 가 검색반경 안이었다. 하지만
    이건 참값이 아니다 — 차가 주차장 안에 있으면 GPS 오차가 0이어도 거리가 크고, 반대로
    오차가 커도 우연히 다른 도로에 가까우면 작게 나온다. 방향만 알 뿐 결론으로 못 쓴다.

  여기서 쓰는 방법 — 경로 보간으로 추정 참값 만들기
    검증 대상 좌표(R)의 앞뒤에서 "신뢰할 수 있는" 점(raw_vld=true 이고 매칭 성공)을 찾는다.
    그 두 점은 각각 어느 링크 위 어디에 있는지 알고 있으므로, 두 지점 사이를 도로망을 따라
    (직선이 아니라 링크 형상을 따라) 이은 뒤 시각 비례로 보간하면 "R 시점에 차가 있었을
    위치"를 추정할 수 있다. 이 추정값은 R 자신의 좌표를 전혀 쓰지 않으므로 독립적이다.

    - 추정오차 = R 의 GPS 좌표 ↔ 보간 추정 위치 거리
    - 정답링크 = 보간 위치가 놓인 링크
    이렇게 하면 raw_vld 값과 무관하게 실측만으로 평면오차와 정답링크를 얻는다.

  정지 꼬리 구간
    검증 대상 대부분(159건)은 트립 끝 정차 구간에 몰려 있어 "뒤쪽 신뢰점"이 아예 없다.
    이 경우도 판정할 수 있다 — 마지막 신뢰점 이후 줄곧 정지(속도 0 근방)였다면 차는 그
    자리에 그대로 있는 것이므로, 마지막 신뢰 매칭 좌표가 곧 추정 참값이다. 트립 시작 쪽도
    같은 논리로 처리한다. 이 판정은 대상 좌표 자신을 쓰지 않으므로 여전히 독립적이다.

  신뢰 구간 제한
    앞뒤 신뢰점 간격이 벌어지면 보간 자체가 부정확하다. 다만 시간만으로 자르면 정차 구간이
    통째로 빠진다 — 차가 안 움직였으면 시간이 아무리 벌어져도 "그 자리"가 정답이기 때문이다.
    그래서 두 조건 중 하나만 만족하면 판정한다.
      · 시간 간격 GAP_SEC 이하 (주행 중 보간)
      · 앞뒤 신뢰점 사이 경로 길이 STILL_M 이하 (사실상 정지 — 시간 무관)
    어느 쪽도 아니면 '판정불가'로 분리한다.

  방법 자체의 신뢰도 — leave-one-out
    이미 신뢰하는 점(raw_vld=true·매칭 성공) 하나를 빼고 나머지로 보간해, 그 점의 실제
    매칭 결과와 얼마나 맞는지 잰다. 이 값이 보간법의 정확도 상한이다. 이걸 먼저 보지 않고
    raw_vld=false 쪽 숫자를 해석하면 안 된다.

  실행: python3 rawvld_realcheck.py
"""
import collections, math, statistics, psycopg2

GAP_SEC = 30            # 주행 중 보간을 허용할 시간 간격 상한
STILL_M = 20.0          # 앞뒤 신뢰점 경로 길이가 이 이하면 정지로 보고 시간 무관 허용
STILL_KMH = 2.0         # 이 속도 이하를 "정지"로 본다 (정지 꼬리 구간 판정용)
PATH_MAX_M = 400.0      # 앞뒤 신뢰점 사이 경로 길이 상한
MAX_HOP = 8             # 경로 탐색 depth
RADIUS_M = 50.0         # 엔진 검색반경(config radius)

r = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
n = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='roadnet')
r.set_session(readonly=True); n.set_session(readonly=True)
cr, cn = r.cursor(), n.cursor()

# ── 도로망 캐시 ────────────────────────────────────────────────────────────
_ends, _adj, _geom = {}, {}, {}


def ends(lid):
    if lid not in _ends:
        cn.execute("SELECT f_node, t_node FROM network.moct_link WHERE link_id=%s", (lid,))
        _ends[lid] = cn.fetchone() or (None, None)
    return _ends[lid]


def outs(node):
    if node not in _adj:
        cn.execute("SELECT link_id, t_node FROM network.moct_link WHERE f_node=%s", (node,))
        _adj[node] = cn.fetchall()
    return _adj[node]


def path_links(a, b):
    """a 에서 b 까지 링크 체인(a, ..., b). 못 찾으면 None"""
    if a == b:
        return [a]
    ta = ends(a)[1]
    if ta is None:
        return None
    frontier = {ta: [a]}
    seen = {ta}
    for _ in range(MAX_HOP):
        nxt = {}
        for node, chain in frontier.items():
            for lid, tnode in outs(node):
                if lid == b:
                    return chain + [b]
                if tnode not in seen:
                    seen.add(tnode)
                    nxt[tnode] = chain + [lid]
        if not nxt:
            return None
        frontier = nxt
    return None


def geom_pts(lid):
    """링크 형상 WGS84 [(lon,lat)]"""
    if lid not in _geom:
        cn.execute("""SELECT ST_X(p), ST_Y(p) FROM (
                        SELECT (ST_DumpPoints(ST_Transform(geom,4326))).geom p,
                               (ST_DumpPoints(geom)).path[1] i
                        FROM network.moct_link WHERE link_id=%s) d ORDER BY i""", (lid,))
        _geom[lid] = [(float(x), float(y)) for x, y in cn.fetchall()]
    return _geom[lid]


def hav(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a[1], a[0], b[1], b[0]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 2 * 6371000.0 * math.asin(math.sqrt(h))


def proj_frac(pts, pt):
    """형상 위에서 pt 에 가장 가까운 지점의 누적거리"""
    best = (1e18, 0.0); acc = 0.0
    for i in range(1, len(pts)):
        ax, ay = pts[i - 1]; bx, by = pts[i]
        seg = hav(pts[i - 1], pts[i])
        dx, dy = bx - ax, by - ay
        den = dx * dx + dy * dy
        t = 0.0 if den == 0 else max(0.0, min(1.0, ((pt[0] - ax) * dx + (pt[1] - ay) * dy) / den))
        q = (ax + t * dx, ay + t * dy)
        d = hav(pt, q)
        if d < best[0]:
            best = (d, acc + seg * t)
        acc += seg
    return best[1]


def build_path(prev_link, prev_pt, next_link, next_pt):
    """앞 신뢰점 -> 뒤 신뢰점 구간의 폴리라인과 각 점의 소속 링크. 못 만들면 None"""
    chain = path_links(prev_link, next_link)
    if chain is None:
        return None
    poly, owner = [], []
    for idx, lid in enumerate(chain):
        pts = geom_pts(lid)
        if not pts:
            return None
        if idx == 0:
            f = proj_frac(pts, prev_pt)
            acc = 0.0; keep = []
            for i in range(1, len(pts)):
                seg = hav(pts[i - 1], pts[i])
                if acc + seg >= f:
                    if not keep:
                        t = 0.0 if seg == 0 else (f - acc) / seg
                        keep.append((pts[i - 1][0] + t * (pts[i][0] - pts[i - 1][0]),
                                     pts[i - 1][1] + t * (pts[i][1] - pts[i - 1][1])))
                    keep.append(pts[i])
                acc += seg
            pts = keep or [pts[-1]]
        if idx == len(chain) - 1:
            f = proj_frac(geom_pts(lid), next_pt)
            full = geom_pts(lid); acc = 0.0; cut = None
            for i in range(1, len(full)):
                seg = hav(full[i - 1], full[i])
                if acc + seg >= f:
                    t = 0.0 if seg == 0 else (f - acc) / seg
                    cut = (full[i - 1][0] + t * (full[i][0] - full[i - 1][0]),
                           full[i - 1][1] + t * (full[i][1] - full[i - 1][1]))
                    break
                acc += seg
            if cut is not None:
                pts = [p for p in pts if proj_frac(full, p) <= f] + [cut]
                if len(pts) < 2:
                    pts = [pts[-1]] if pts else [cut]
        for p in pts:
            if poly and hav(poly[-1], p) < 0.05:
                continue
            poly.append(p); owner.append(lid)
    return (poly, owner) if len(poly) >= 2 else None


def interp(poly, owner, frac):
    """폴리라인을 거리 비례 frac 위치로 보간 → (좌표, 소속 링크)"""
    segs = [hav(poly[i - 1], poly[i]) for i in range(1, len(poly))]
    total = sum(segs)
    if total <= 0:
        return poly[0], owner[0], 0.0
    target = total * frac; acc = 0.0
    for i, seg in enumerate(segs, start=1):
        if acc + seg >= target:
            t = 0.0 if seg == 0 else (target - acc) / seg
            return ((poly[i - 1][0] + t * (poly[i][0] - poly[i - 1][0]),
                     poly[i - 1][1] + t * (poly[i][1] - poly[i - 1][1])), owner[i], total)
        acc += seg
    return poly[-1], owner[-1], total


# ── 실주행 좌표 로드 ───────────────────────────────────────────────────────
cr.execute("""SELECT trip_id, gps_seq, gps_lon, gps_lat, match_lon, match_lat, match_link_id,
                     match_status, raw_vld, drive_status, accuracy_m, speed_kmh, gps_dt
              FROM ruc.prim_rawgps
              WHERE trip_id NOT LIKE '000093%%' AND trip_id NOT LIKE '9%%'
                AND gps_lon IS NOT NULL AND gps_lat IS NOT NULL
              ORDER BY trip_id, gps_seq""")
trips = collections.defaultdict(list)
for row in cr.fetchall():
    trips[row[0]].append(row)


def secs(dt):
    return (int(dt[8:10]) * 3600 + int(dt[10:12]) * 60 + int(dt[12:14])) if dt else 0


res = collections.defaultdict(list)          # (raw_vld, drive_status) -> [(오차, 보간링크, 엔진링크)]
loo = []                                     # leave-one-out 검증 결과
undecided = collections.Counter()


def estimate(pts, trusted, i, skip=None):
    """i 번째 점의 참 위치를 앞뒤 신뢰점 보간으로 추정. (오차, 보간링크) 또는 (None, 사유)"""
    prev = next((j for j in reversed(trusted) if j < i and j != skip), None)
    nxt = next((j for j in trusted if j > i and j != skip), None)

    # 정지 꼬리 구간 — 한쪽 신뢰점만 있어도, 그 사이가 줄곧 정지였다면 차는 그 자리에 있다.
    #   마지막(또는 첫) 신뢰 매칭 좌표가 곧 추정 참값이다 (2026-08-23 최정우 추가)
    def still_between(lo, hi):
        for j in range(lo, hi + 1):
            sp = pts[j][11]
            if sp is None or float(sp) > STILL_KMH:
                return False
        return True

    if prev is not None and nxt is None:
        if not still_between(prev + 1, i):
            return None, '뒤쪽 신뢰점 없음(주행 중)'
        a = pts[prev]
        return (hav((float(pts[i][2]), float(pts[i][3])), (float(a[4]), float(a[5]))), a[6],
                (float(a[4]), float(a[5]))), None
    if prev is None and nxt is not None:
        if not still_between(i, nxt - 1):
            return None, '앞쪽 신뢰점 없음(주행 중)'
        b = pts[nxt]
        return (hav((float(pts[i][2]), float(pts[i][3])), (float(b[4]), float(b[5]))), b[6],
                (float(b[4]), float(b[5]))), None
    if prev is None or nxt is None:
        return None, '앞뒤 신뢰점 없음'
    a, b = pts[prev], pts[nxt]
    dt_all = secs(b[12]) - secs(a[12])
    if dt_all <= 0:
        return None, '시각 역전·동일'
    built = build_path(a[6], (float(a[4]), float(a[5])), b[6], (float(b[4]), float(b[5])))
    if built is None:
        return None, '경로 없음'
    poly, owner = built
    frac_t = max(0.0, min(1.0, (secs(pts[i][12]) - secs(a[12])) / float(dt_all)))
    est, est_link, total = interp(poly, owner, frac_t)
    if total > PATH_MAX_M:
        return None, '경로 길이 초과'
    # 주행 중이면 시간 간격 제한, 사실상 정지면 시간 무관 허용
    if total > STILL_M and dt_all > GAP_SEC:
        return None, '신뢰점 간격 초과'
    return (hav((float(pts[i][2]), float(pts[i][3])), est), est_link, est), None


for tid, pts in trips.items():
    trusted = [i for i, p in enumerate(pts) if p[7] == 1 and p[8] and p[6]]
    tset = set(trusted)
    # leave-one-out — 이미 신뢰하는 점으로 보간법 자체의 정확도를 잰다
    for k in trusted:
        got, why = estimate(pts, trusted, k, skip=k)
        if got is not None:
            loo.append((got[0], got[1], pts[k][6]))
    # 검증 대상 — 신뢰점이 아닌 모든 점 (raw_vld 무관)
    for i, p in enumerate(pts):
        if i in tset:
            continue
        got, why = estimate(pts, trusted, i)
        if got is None:
            undecided[why] += 1
            continue
        res[(bool(p[8]), p[9])].append((got[0], got[1], p[6], tid, p[1], p[10], p[11],
                                        (float(p[2]), float(p[3])), got[2]))


def qq(v, p):
    v = sorted(v)
    return v[min(int(len(v) * p), len(v) - 1)] if v else 0.0


print('경로 보간으로 추정한 실제 평면오차 — raw_vld 무관 (실주행 %d트립)' % len(trips))
print('  판정 조건: 앞뒤 신뢰점 간격 %ds 이내, 또는 경로 %dm 이하(사실상 정지)\n'
      % (GAP_SEC, int(STILL_M)))

# ── 0. 방법 자체의 신뢰도 ──────────────────────────────────────────────────
print('── 0. 보간법 신뢰도 (leave-one-out, 이미 신뢰하는 점 대상) ──')
if loo:
    e = [x[0] for x in loo]
    hit = sum(1 for x in loo if x[1] == x[2])
    print('  %d건 · 위치오차 중앙 %.1fm · 75분위 %.1fm · 90분위 %.1fm'
          % (len(loo), statistics.median(e), qq(e, 0.75), qq(e, 0.9)))
    print('  링크 일치 %d/%d = %.1f%%  ← 이 값이 보간법의 정확도 상한이다'
          % (hit, len(loo), 100.0 * hit / len(loo)))
else:
    print('  판정 가능한 표본 없음')

# ── 1. raw_vld 별 추정 평면오차 ────────────────────────────────────────────
print('\n── 1. raw_vld 별 추정 평면오차 ──')
print('%-9s %-8s %6s %9s %9s %9s %9s %9s'
      % ('raw_vld', '상태', '건수', '오차중앙', '75분위', '90분위', '최대', '반경내'))
NM = {0: 'ON_ROAD', 1: 'IDLE', 2: 'PARKED'}
for key in sorted(res, key=lambda k: (not k[0], k[1])):
    v = [x[0] for x in res[key]]
    print('%-9s %-8s %6d %8.1fm %8.1fm %8.1fm %8.1fm %8.1f%%'
          % ('true' if key[0] else 'FALSE', NM.get(key[1], key[1]), len(v),
             statistics.median(v), qq(v, 0.75), qq(v, 0.9), max(v),
             100.0 * sum(1 for d in v if d <= RADIUS_M) / len(v)))

tot_false = sum(len(v) for k, v in res.items() if not k[0])
print('\n판정 %d건 (raw_vld=false %d건) · 판정 불가: %s'
      % (sum(len(v) for v in res.values()), tot_false, dict(undecided)))


# ── 2. raw_vld=false 오차 상위 상세 (판정 근거 확인용) ─────────────────────
big = [x for k, v in res.items() if not k[0] for x in v]
if big:
    print('\n── 2. raw_vld=false 추정오차 상위 12건 ──')
    print('%-24s %5s %8s %-11s %6s %6s' % ('trip_id', 'seq', '추정오차', '보간링크', '정확도', '속도'))
    for x in sorted(big, key=lambda y: -y[0])[:12]:
        print('%-24s %5d %7.1fm %-11s %6s %6s' % (x[3], x[4], x[0], x[1], x[5], x[6]))


# ── 3. accuracy_m 구간별 추정오차 — raw_vld 임계(15)가 적정한가 ────────────
allpts = [x for v in res.values() for x in v] + [(e, l, m, '', 0, None, None) for e, l, m in []]
band = {}
for k, v in res.items():
    for x in v:
        acc = x[5]
        if acc is None:
            continue
        acc = int(acc)
        b = ('~15' if acc <= 15 else '16~25' if acc <= 25 else '26~50' if acc <= 50
             else '51~100' if acc <= 100 else '101~')
        band.setdefault(b, []).append(x[0])
print('\n── 3. accuracy_m 구간별 추정 평면오차 (raw_vld 무관) ──')
print('%-8s %6s %9s %9s %9s %9s' % ('정확도', '건수', '오차중앙', '75분위', '90분위', '50m이내'))
for b in ('~15', '16~25', '26~50', '51~100', '101~'):
    v = band.get(b)
    if not v:
        continue
    print('%-8s %6d %8.1fm %8.1fm %8.1fm %8.1f%%'
          % (b, len(v), statistics.median(v), qq(v, 0.75), qq(v, 0.9),
             100.0 * sum(1 for d in v if d <= 50.0) / len(v)))


# ── 4. 주정차 폴리곤 판정에 미치는 영향 ────────────────────────────────────
#   엔진은 raw_vld=false 행도 주정차 판정은 수행한다(2026-08-22 사용자 확정 — 차가 멈추면
#   GPS 가 나빠지는데 하필 그때 판정이 필요하기 때문). 그런데 고오차 좌표가 실제 위치에서
#   수백 m 떨어져 동결되면, 그 좌표로 폴리곤 포함을 판정하는 순간 허위 진입/누락이 난다.
#   GPS 좌표와 보간 추정 참위치의 폴리곤 포함 여부를 각각 구해 비교한다.
import json as _json
cr.execute("SELECT road_id, road_nm, coords FROM ruc.base_roadlink WHERE road_kind='4'")
_zones = []
for rid, nm, co in cr.fetchall():
    co = co if isinstance(co, list) else _json.loads(co)
    poly = co[0] if (co and isinstance(co[0][0], list)) else co
    if poly[0] != poly[-1]:
        poly = poly + [poly[0]]
    _zones.append((rid, nm, 'POLYGON((%s))' % ','.join('%f %f' % (q[0], q[1]) for q in poly)))


def in_zone(wkt, pt):
    cr.execute("SELECT ST_Contains(ST_GeomFromText(%s,4326), ST_SetSRID(ST_MakePoint(%s,%s),4326))",
               (wkt, pt[0], pt[1]))
    return bool(cr.fetchone()[0])


print('\n── 4. 주정차 폴리곤 판정 영향 (GPS 좌표 vs 보간 추정 참위치) ──')
for rid, nm, wkt in _zones:
    stat = collections.Counter()
    detail = []
    for k, v in res.items():
        for x in v:
            g_in = in_zone(wkt, x[7])
            t_in = in_zone(wkt, x[8])
            if not g_in and not t_in:
                continue
            band = '~15' if (x[5] or 0) <= 15 else '16~50' if (x[5] or 0) <= 50 \
                   else '51~100' if (x[5] or 0) <= 100 else '101~'
            if g_in and t_in:
                stat[(band, '일치')] += 1
            elif g_in and not t_in:
                stat[(band, '허위진입')] += 1; detail.append((band, '허위진입', x))
            else:
                stat[(band, '누락')] += 1; detail.append((band, '누락', x))
    if not stat:
        print('  [%s] %s — 해당 좌표 없음' % (rid, nm)); continue
    print('  [%s] %s' % (rid, nm))
    print('    %-8s %8s %10s %8s' % ('정확도', '일치', '허위진입', '누락'))
    for band in ('~15', '16~50', '51~100', '101~'):
        row = [stat[(band, t)] for t in ('일치', '허위진입', '누락')]
        if sum(row):
            print('    %-8s %8d %10d %8d' % (band, row[0], row[1], row[2]))
    if detail:
        print('    ── 불일치 상세(상위 10) ──')
        print('    %-24s %5s %-9s %8s %6s' % ('trip_id', 'seq', '유형', '추정오차', '정확도'))
        for band, typ, x in sorted(detail, key=lambda y: -y[2][0])[:10]:
            print('    %-24s %5d %-9s %7.1fm %6s' % (x[3], x[4], typ, x[0], x[5]))
