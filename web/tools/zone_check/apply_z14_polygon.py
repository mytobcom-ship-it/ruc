# -*- coding: utf-8 -*-
"""RL-Z00014(강릉 농산물 새벽시장) 폴리곤을 제방길 본선 중심선 10m 버퍼 안으로 교체한다.

  기존은 좌표 4점 사각형이라 도로 형상을 따라가지 못하고 본선을 비스듬히 잘랐다
  (본선 5개 포함률 16~75%). 제안안은 work/z14_new.json — analysis/z14.py 산출물.
  실행: python3 apply_z14_polygon.py [--apply]
"""
import os as _os, sys, json, psycopg2
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
APPLY = '--apply' in sys.argv
RID = 'RL-Z00014'
MAIN = ['2520119804','2520164100','2520084000','2520120000','2520119801']

new = json.load(open(WORK+'z14_new.json'))
cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cu = cn.cursor()
cu.execute("SELECT coords FROM ruc.base_roadlink WHERE road_id=%s", (RID,))
cur = cu.fetchone()[0]

def report(tag, pts):
    wkt = 'SRID=4326;POLYGON((%s))' % ', '.join('%.7f %.7f' % (p[0], p[1]) for p in pts + [pts[0]])
    cu.execute("""
      WITH p AS (SELECT ST_GeomFromEWKT(%s) g), p5 AS (SELECT ST_Transform(g,5186) g5 FROM p),
      nd AS (SELECT n.node_id FROM network.moct_node n, p5 WHERE ST_Contains(p5.g5, n.geom))
      SELECT (SELECT ST_IsValid(g) FROM p), (SELECT round(ST_Area(g::geography)::numeric,0) FROM p),
             (SELECT count(*) FROM nd),
             (SELECT count(*) FROM network.moct_link l
               WHERE l.f_node IN (SELECT node_id FROM nd) OR l.t_node IN (SELECT node_id FROM nd))
      """, (wkt,))
    ok, area, nnode, nlink = cu.fetchone()
    cu.execute("""
      WITH p AS (SELECT ST_GeomFromEWKT(%s) g)
      SELECT l.link_id, round((ST_Length(ST_Intersection(ST_Transform(l.geom,4326),p.g)::geography)
             / nullif(ST_Length(ST_Transform(l.geom,4326)::geography),0)*100)::numeric,0)
      FROM network.moct_link l, p WHERE l.link_id = ANY(%s)""", (wkt, MAIN))
    r = dict(cu.fetchall())
    print('[%s] %d점 · 유효 %s · 면적 %s㎡ · 내부노드 %d개 · 관련링크 %d개'
          % (tag, len(pts), ok, area, nnode, nlink))
    print('   본선 포함률: %s' % '  '.join('%s %s%%' % (k, r.get(k, 0)) for k in MAIN))

report('현재', cur if isinstance(cur, list) else json.loads(cur))
report('제안', new)

if not APPLY:
    print('\n(변경안만 출력 — 반영하려면 --apply)')
    sys.exit(0)
cu.execute("DROP TABLE IF EXISTS ruc.bak_roadlink_z14")
cu.execute("CREATE TABLE ruc.bak_roadlink_z14 AS SELECT * FROM ruc.base_roadlink WHERE road_id=%s", (RID,))
cu.execute("""UPDATE ruc.base_roadlink SET coords=%s::jsonb,
                     upd_dt=to_char(now(),'YYYYMMDDHH24MISS') WHERE road_id=%s""",
           (json.dumps(new), RID))
cn.commit()
print('\n반영 완료 (백업: ruc.bak_roadlink_z14)')
