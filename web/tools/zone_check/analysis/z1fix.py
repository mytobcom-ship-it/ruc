# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

import json, psycopg2
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
cu.execute("SELECT coords FROM ruc.base_roadlink WHERE road_id='RL-Z00001'")
co=cu.fetchone()[0]; co=co if isinstance(co,list) else json.loads(co)
DROP=[1,14,15,16,17,21]                       # 접속 링크 중심선 위에 얹힌 좌표(1-based)
kept=[c for i,c in enumerate(co,1) if i not in DROP]

def wkt(pts):
    r=pts+[pts[0]]
    return 'SRID=4326;POLYGON((%s))'%', '.join('%.10f %.10f'%(p[0],p[1]) for p in r)

MAIN=['2040425301','2040425801','2040425201']
CONN=['2040424302','2040424803','2040425202','2040425302','2040425303','2040425802']
OPP =['2040425401']                            # 건너편(반대방향) 본선

def report(tag, pts):
    cu.execute("""
      WITH p AS (SELECT ST_GeomFromEWKT(%s) AS g)
      SELECT ST_IsValid(p.g), ST_IsSimple(ST_Boundary(p.g)),
             round(ST_Area(p.g::geography)::numeric,0),
             round(ST_Perimeter(p.g::geography)::numeric,0),
             (SELECT count(*) FROM network.moct_node n, p
               WHERE ST_Contains(p.g, ST_Transform(n.geom,4326)))
      FROM p""",(wkt(pts),))
    ok,simple,area,peri,nin=cu.fetchone()
    print('\n[%s] 좌표 %d개 · 유효 %s · 자기교차없음 %s · 면적 %s㎡ · 둘레 %sm · 내부노드 %d개'
          %(tag,len(pts),ok,simple,area,peri,nin))
    cu.execute("""
      WITH p AS (SELECT ST_GeomFromEWKT(%s) AS g)
      SELECT l.link_id,
             round((ST_Length(ST_Intersection(ST_Transform(l.geom,4326),p.g)::geography)
                   / nullif(ST_Length(ST_Transform(l.geom,4326)::geography),0)*100)::numeric,0)
      FROM network.moct_link l, p WHERE l.link_id = ANY(%s)""",(wkt(pts),MAIN+CONN+OPP))
    r=dict(cu.fetchall())
    print('  본선   %s'%'  '.join('%s %s%%'%(k,r.get(k,0) or 0) for k in MAIN))
    print('  접속   %s'%'  '.join('%s %s%%'%(k,r.get(k,0) or 0) for k in CONN))
    print('  건너편 %s'%'  '.join('%s %s%%'%(k,r.get(k,0) or 0) for k in OPP))

report('현재 (21점)', co)
report('6점 제거 후 (15점)', kept)
json.dump(kept,open(WORK+'z1_kept.json','w'))
