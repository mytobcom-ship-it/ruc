# -*- coding: utf-8 -*-
"""base_roadlink 의 link_ids(주행순·누락 보완)와 coords(역순 교정)를 실제로 반영한다.

  근거는 zonechk2.py 가 만든 work/zonedata.json — 각 구역의 'order'(주행순 링크, 누락 보완 포함)와
  'coord.verdict'('좌표 역순' 포함 여부)를 그대로 쓴다.
  coords 는 링크 위 이격이 전 구역 0.0~0.1m 로 확인됐으므로 재생성하지 않고 순서만 뒤집는다.
  실행: python3 apply_zone_fix.py [--apply]      (--apply 없으면 변경안만 출력)
"""
import os as _os, sys, json, psycopg2
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'

APPLY = '--apply' in sys.argv
D = json.load(open(WORK+'zonedata.json'))
cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cu = cn.cursor()

plan = []
for z in D['zones']:
    if not z['order']:
        continue
    rid = z['road_id']
    cu.execute("SELECT link_ids, coords FROM ruc.base_roadlink WHERE road_id=%s", (rid,))
    cur_links, cur_coords = cu.fetchone()
    cur_links = cur_links or []
    new_links = z['order']                                  # 주행순 + 누락 링크 포함
    rev = '역순' in (z.get('coord') or {}).get('verdict', '')
    new_coords = list(reversed(cur_coords)) if rev else cur_coords
    if cur_links == new_links and not rev:
        continue
    plan.append((rid, z['road_nm'], cur_links, new_links, z['missing'], rev, new_coords))

print('%-10s %-30s %-6s %-6s %-8s %s' % ('road_id', '구역', '기존', '변경', '누락보완', 'coords'))
for rid, nm, cur, new, miss, rev, _ in plan:
    print('%-10s %-30s %-6d %-6d %-8s %s' % (rid, (nm or '')[:28], len(cur), len(new),
          (','.join(miss) if miss else '-'), '역순 → 뒤집음' if rev else '유지'))

if not APPLY:
    print('\n(변경안만 출력 — 실제 반영하려면 --apply)')
    sys.exit(0)

cu.execute("DROP TABLE IF EXISTS ruc.bak_roadlink_20260823")
cu.execute("CREATE TABLE ruc.bak_roadlink_20260823 AS SELECT * FROM ruc.base_roadlink")
for rid, nm, cur, new, miss, rev, new_coords in plan:
    cu.execute("""UPDATE ruc.base_roadlink
                     SET link_ids = %s::jsonb, coords = %s::jsonb,
                         upd_dt = to_char(now(),'YYYYMMDDHH24MISS')
                   WHERE road_id = %s""",
               (json.dumps(new), json.dumps(new_coords), rid))
cn.commit()
print('\n%d개 구역 반영 완료 (백업: ruc.bak_roadlink_20260823)' % len(plan))
