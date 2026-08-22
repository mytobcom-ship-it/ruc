# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

import json, psycopg2
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
MAIN=['2520119804','2520164100','2520084000','2520120000','2520119801']   # 본선 + 중간 연결
ALL =MAIN+['2520164101','2520084001']
for buf,simp in ((15.0,2.0),(15.0,3.0),(14.0,2.0)):
    cu.execute("""
      WITH u AS (SELECT ST_Union(geom) g FROM network.moct_link WHERE link_id=ANY(%s)),
      b AS (SELECT ST_SimplifyPreserveTopology(
                     ST_Buffer((SELECT g FROM u), %s, 'endcap=round join=round'), %s) AS g)
      SELECT ST_NPoints(g), round(ST_Area(g)::numeric,0), round(ST_Perimeter(g)::numeric,0),
             ST_NumInteriorRings(g), ST_IsValid(g),
             ST_AsGeoJSON(ST_Transform(g,4326),7)
      FROM b""",(MAIN,buf,simp))
    n,area,peri,rings,ok,gj=cu.fetchone()
    print('버퍼 %.0fm / 단순화 %.0fm → 좌표 %d점, 면적 %s㎡, 둘레 %sm, 내부구멍 %d, 유효 %s'
          %(buf,simp,n,area,peri,rings,ok))
    if (buf,simp)==(15.0,2.0):
        g=json.loads(gj)
        ring=g['coordinates'][0] if g['type']=='Polygon' else g['coordinates'][0][0]
        cand=[[round(x,7),round(y,7)] for x,y in ring[:-1]]      # 닫는 중복점 제외
        json.dump(cand,open(WORK+'z14_new.json','w'))

# 제안 폴리곤으로 링크 포함률 재계산
cand=json.load(open(WORK+'z14_new.json'))
wkt='SRID=4326;POLYGON((%s))'%', '.join('%.7f %.7f'%(p[0],p[1]) for p in cand+[cand[0]])
cu.execute("""
  WITH p AS (SELECT ST_GeomFromEWKT(%s) g), p5 AS (SELECT ST_Transform(g,5186) g5 FROM p),
  nd AS (SELECT n.node_id FROM network.moct_node n, p5 WHERE ST_Contains(p5.g5,n.geom))
  SELECT l.link_id, l.road_name, round(l.length::numeric,0),
         round((ST_Length(ST_Intersection(ST_Transform(l.geom,4326),p.g)::geography)
               /nullif(ST_Length(ST_Transform(l.geom,4326)::geography),0)*100)::numeric,0),
         (l.f_node IN (SELECT node_id FROM nd)), (l.t_node IN (SELECT node_id FROM nd))
  FROM network.moct_link l, p, p5
  WHERE l.link_id=ANY(%s) ORDER BY 4 DESC""",(wkt,ALL))
print('\n제안 폴리곤(버퍼15m/단순화2m) 기준 포함률')
for lid,nm,ln,r,fi,ti in cu.fetchall():
    print('  %-12s %-8s %4sm  포함 %3s%%  노드 %s%s'%(lid,(nm or '-')[:8],ln,r,
          '내부' if fi else '외부','/내부' if ti else '/외부'))
cu.execute("""WITH p AS (SELECT ST_GeomFromEWKT(%s) g)
  SELECT (SELECT count(*) FROM network.moct_link l, p
          WHERE ST_Intersects(ST_Transform(l.geom,4326),p.g) AND NOT l.link_id = ANY(%s))
  """,(wkt,ALL))
print('\n대상 외 링크 중 새 폴리곤에 걸리는 것: %d개'%cu.fetchone()[0])
