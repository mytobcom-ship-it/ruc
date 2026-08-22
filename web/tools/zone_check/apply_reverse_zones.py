# -*- coding: utf-8 -*-
"""제2판교 개방식·폐쇄식 구역의 반대방향 짝 구역과 게이트를 생성한다 (2026-08-23 최정우)

  왜 필요한가
    등록된 12개 LINE 구역은 모두 한 방향 차로만 담는다. 강릉 구역들은 그래서 방향별로
    짝 구역을 함께 등록해 뒀다 — 폐쇄식 (안목해변~강문)/(강문~안목해변), 개방식 (상행)/(하행),
    구간단속 (상행)/(하행). 반면 제2판교 개방식·폐쇄식은 짝이 없어, 반대방향으로 지나간
    실주행이 무과금으로 빠진다(실측: 2040423701 이 4트립·56좌표, 2040423501 이 4트립·24좌표).
    유료도로는 양방향 과금이 실제 운영 형태이므로 강릉과 같은 형태로 맞춘다.

  일반도로(RL-Z00002)·구간단속(RL-Z00003)은 대상이 아니다 — 두 구역이 같은 도로의
  서로 다른 방향에 걸려 있어(2040424401 / 2040424301) 테스트 배치 의도가 뚜렷하다.

  만드는 것
    RL-Z00015  제2판교 개방식 구역(북행)   링크 2040423501 -> 2040423503
               TG00026 M  — 기존 TG00009(본선) 위치를 반대편 링크에 투영
    RL-Z00016  제2판교 폐쇄식 구역(동행)   링크 2040423701
               TG00027 I  — 기존 TG00008(출구) 위치를 투영. 방향이 반대라 입구/출구가 뒤바뀐다
               TG00028 O  — 기존 TG00007(입구) 위치를 투영

  coords 는 기존 구역과 같은 방식으로 링크 형상(WGS84 경위도)을 주행순으로 이어 붙인다.
  기존 두 구역의 road_nm 에도 방향 표기를 더한다 — 짝이 생기면 방향 없는 이름은 못 읽는다.

  실행: python3 apply_reverse_zones.py [--apply]     (--apply 없으면 변경안만 출력)
"""
import sys, json, psycopg2

APPLY = '--apply' in sys.argv

# (신규 road_id, road_kind, road_nm, 원본 road_id, 주행순 링크)
ZONES = [
    ('RL-Z00015', '1', '제2판교 개방식 구역(북행)', 'RL-Z00004', ['2040423501', '2040423503']),
    ('RL-Z00016', '2', '제2판교 폐쇄식 구역(동행)', 'RL-Z00005', ['2040423701']),
]
# 기존 구역 이름에 방향 표기 추가
RENAME = [
    ('RL-Z00004', '제2판교 개방식 구역(남행)'),
    ('RL-Z00005', '제2판교 폐쇄식 구역(서행)'),
]
# (신규 tollgate_id, 신규 road_id, tollgate_nm, gate_div, 위치를 따올 기존 게이트, 올려놓을 링크)
GATES = [
    ('TG00026', 'RL-Z00015', '제2판교 개방식(북행) 본선', 'M', 'TG00009', '2040423501'),
    ('TG00027', 'RL-Z00016', '제2판교 폐쇄식(동행) 입구', 'I', 'TG00008', '2040423701'),
    ('TG00028', 'RL-Z00016', '제2판교 폐쇄식(동행) 출구', 'O', 'TG00007', '2040423701'),
]

rc = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
nc = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='roadnet')
nc.set_session(readonly=True)
cr, cn = rc.cursor(), nc.cursor()


def link_coords(link_ids):
    """주행순 링크 형상을 WGS84 [lon,lat] 목록으로 이어 붙인다 (노드 중복 제거)"""
    out = []
    for lid in link_ids:
        cn.execute("""SELECT ST_X(p), ST_Y(p) FROM (
                        SELECT (ST_DumpPoints(ST_Transform(geom,4326))).geom p,
                               (ST_DumpPoints(geom)).path[1] i
                        FROM network.moct_link WHERE link_id=%s) d ORDER BY i""", (lid,))
        pts = [[float(x), float(y)] for x, y in cn.fetchall()]
        if not pts:
            raise SystemExit('링크 형상을 찾을 수 없습니다: %s' % lid)
        if out and out[-1] == pts[0]:
            pts = pts[1:]
        out.extend(pts)
    return out


def project_on_link(lon, lat, link_id):
    """기존 게이트 좌표를 반대편 링크 위로 투영한 지점(WGS84)"""
    cn.execute("""SELECT ST_X(g), ST_Y(g),
                         ST_Distance(ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),5186), geom)
                  FROM network.moct_link,
                       LATERAL (SELECT ST_Transform(ST_ClosestPoint(geom,
                           ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),5186)), 4326) g) q
                  WHERE link_id=%s""", (lon, lat, lon, lat, link_id))
    row = cn.fetchone()
    if row is None:
        raise SystemExit('링크를 찾을 수 없습니다: %s' % link_id)
    return float(row[0]), float(row[1]), float(row[2])


# ── 변경안 산출 ────────────────────────────────────────────────────────────
plan_zones, plan_gates = [], []
for rid, kind, nm, src, links in ZONES:
    cr.execute("SELECT speed_limit_kmh, use_yn FROM ruc.base_roadlink WHERE road_id=%s", (src,))
    row = cr.fetchone()
    if row is None:
        raise SystemExit('원본 구역이 없습니다: %s' % src)
    spd, use = row
    coords = link_coords(links)
    plan_zones.append((rid, kind, nm, links, coords, spd, use, src))

for tg, rid, nm, div, srctg, link in GATES:
    cr.execute("SELECT lon, lat, use_yn FROM ruc.base_tollgate WHERE tollgate_id=%s", (srctg,))
    row = cr.fetchone()
    if row is None:
        raise SystemExit('원본 게이트가 없습니다: %s' % srctg)
    lon, lat, use = row
    nlon, nlat, gap = project_on_link(float(lon), float(lat), link)
    plan_gates.append((tg, rid, nm, div, link, nlon, nlat, use, srctg, gap))

print('── 신규 구역 ──')
print('%-11s %-4s %-28s %-24s %5s %s' % ('road_id', '유형', '이름', '링크(주행순)', '좌표', '원본'))
for rid, kind, nm, links, coords, spd, use, src in plan_zones:
    print('%-11s %-4s %-28s %-24s %5d %s' % (rid, kind, nm, '->'.join(links), len(coords), src))

print('\n── 신규 게이트 ──')
print('%-9s %-11s %-28s %-4s %-11s %12s %11s %7s %s' %
      ('gate_id', 'road_id', '이름', '구분', '링크', 'lon', 'lat', '투영이격', '원본'))
for tg, rid, nm, div, link, lon, lat, use, srctg, gap in plan_gates:
    print('%-9s %-11s %-28s %-4s %-11s %12.7f %11.7f %6.1fm %s' %
          (tg, rid, nm, div, link, lon, lat, gap, srctg))

print('\n── 기존 구역 이름 변경 ──')
for rid, nm in RENAME:
    cr.execute("SELECT road_nm FROM ruc.base_roadlink WHERE road_id=%s", (rid,))
    print('  %s  %s -> %s' % (rid, cr.fetchone()[0], nm))

# 중복 확인
cr.execute("SELECT road_id FROM ruc.base_roadlink WHERE road_id = ANY(%s)", ([z[0] for z in ZONES],))
dup_z = [r[0] for r in cr.fetchall()]
cr.execute("SELECT tollgate_id FROM ruc.base_tollgate WHERE tollgate_id = ANY(%s)", ([g[0] for g in GATES],))
dup_g = [r[0] for r in cr.fetchall()]
if dup_z or dup_g:
    raise SystemExit('\n이미 존재합니다 — 중단합니다: %s %s' % (dup_z, dup_g))

if not APPLY:
    print('\n(변경안만 출력 — 실제 반영하려면 --apply)')
    sys.exit(0)

# ── 반영 ───────────────────────────────────────────────────────────────────
cr.execute("DROP TABLE IF EXISTS ruc.bak_roadlink_before_reverse")
cr.execute("CREATE TABLE ruc.bak_roadlink_before_reverse AS SELECT * FROM ruc.base_roadlink")
cr.execute("DROP TABLE IF EXISTS ruc.bak_tollgate_before_reverse")
cr.execute("CREATE TABLE ruc.bak_tollgate_before_reverse AS SELECT * FROM ruc.base_tollgate")

for rid, kind, nm, links, coords, spd, use, src in plan_zones:
    cr.execute("""INSERT INTO ruc.base_roadlink
                    (road_id, road_kind, road_nm, geom_type, coords, speed_limit_kmh,
                     use_yn, reg_dt, upd_dt, admin_id, link_ids)
                  VALUES (%s,%s,%s,'LINE',%s::jsonb,%s,%s,
                          to_char(now(),'YYYYMMDDHH24MISS'), to_char(now(),'YYYYMMDDHH24MISS'),
                          'reverse-pair', %s::jsonb)""",
               (rid, kind, nm, json.dumps(coords), spd, use, json.dumps(links)))

for tg, rid, nm, div, link, lon, lat, use, srctg, gap in plan_gates:
    cr.execute("""INSERT INTO ruc.base_tollgate
                    (tollgate_id, road_id, tollgate_nm, gate_div, lon, lat, link_id, use_yn, reg_dt, upd_dt)
                  VALUES (%s,%s,%s,%s,%s,%s,%s,%s,now(),now())""",
               (tg, rid, nm, div, round(lon, 7), round(lat, 7), link, use))

for rid, nm in RENAME:
    cr.execute("""UPDATE ruc.base_roadlink
                  SET road_nm=%s, upd_dt=to_char(now(),'YYYYMMDDHH24MISS') WHERE road_id=%s""", (nm, rid))

rc.commit()
print('\n반영 완료 — 구역 %d개·게이트 %d개 추가, 이름 %d건 변경 (백업: ruc.bak_roadlink_before_reverse, ruc.bak_tollgate_before_reverse)'
      % (len(plan_zones), len(plan_gates), len(RENAME)))
