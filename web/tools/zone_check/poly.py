# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

import json, psycopg2
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
POLY_SQL = """
  SELECT ST_MakePolygon(ST_AddPoint(ST_MakeLine(ARRAY(
           SELECT ST_SetSRID(ST_MakePoint((e->>0)::float8,(e->>1)::float8),4326)
           FROM jsonb_array_elements(coords) e)),
           ST_SetSRID(ST_MakePoint((coords->0->>0)::float8,(coords->0->>1)::float8),4326)))
  FROM ruc.base_roadlink WHERE road_id=%s"""
cu.execute("SELECT road_id,road_nm,road_kind,coords FROM ruc.base_roadlink WHERE geom_type='POLY' ORDER BY road_id")
zs=cu.fetchall()
out=[]
for rid,nm,kind,co in zs:
    co = co if isinstance(co,list) else json.loads(co)
    cu.execute("WITH p AS (%s) SELECT ST_IsValid(g), ST_IsSimple(ST_Boundary(g)),"
               " ST_Area(g::geography), ST_Perimeter(g::geography) FROM p AS t(g)"%POLY_SQL,(rid,))
    valid,simple,area,peri=cu.fetchone()

    # ── 링크: 노드 내부 여부 + 포함률 + 기하 ──
    cu.execute("""
      WITH p(g) AS (%s), p5 AS (SELECT ST_Transform(g,5186) AS g5 FROM p),
      nd AS (SELECT n.node_id FROM network.moct_node n, p5 WHERE ST_Contains(p5.g5, n.geom))
      SELECT l.link_id, l.f_node, l.t_node, l.road_name, l.length,
             (l.f_node IN (SELECT node_id FROM nd)) AS fin,
             (l.t_node IN (SELECT node_id FROM nd)) AS tin,
             ST_Length(ST_Intersection(ST_Transform(l.geom,4326), p.g)::geography) AS inlen,
             ST_AsGeoJSON(ST_Transform(l.geom,4326),7),
             ST_AsGeoJSON(ST_Intersection(ST_Transform(l.geom,4326), p.g),7),
             ST_AsGeoJSON(ST_Difference(ST_Transform(l.geom,4326), p.g),7)
      FROM network.moct_link l, p, p5
      WHERE l.f_node IN (SELECT node_id FROM nd) OR l.t_node IN (SELECT node_id FROM nd)
         OR ST_Intersects(l.geom, p5.g5)
      ORDER BY 8 DESC"""%POLY_SQL,(rid,))
    links=[]
    for lid,f,t,rn,ln,fin,tin,inl,g,gin,gout in cu.fetchall():
        ln=float(ln or 0); inl=float(inl or 0)
        kind_ = '양끝 내부' if (fin and tin) else ('한쪽 내부' if (fin or tin) else '양끝 외부')
        links.append({'link_id':lid,'f':f,'t':t,'name':rn,'len':round(ln,1),
                      'inlen':round(inl,1),'ratio':round(inl/ln*100,1) if ln else 0,
                      'fin':fin,'tin':tin,'kind':kind_,
                      'excl':(not fin and not tin),      # 노드가 모두 밖 = 관통만 → 제외 대상
                      # 대상 링크(노드 하나라도 내부)인데 밖으로 삐져나온 구간 = 경계가 링크를 자른 것
                      'cut':((fin or tin) and inl < ln-0.5),
                      'outlen':round(max(ln-inl,0),1),
                      'g':json.loads(g),'gin':json.loads(gin),'gout':json.loads(gout)})

    # ── 좌표 점검: 최근접 노드 / 링크 중심선 / 링크 정점 ──
    cu.execute("""
      WITH p(g) AS (%s), p5 AS (SELECT ST_Transform(g,5186) AS g5 FROM p),
      c AS (SELECT ord, ST_Transform(ST_SetSRID(ST_MakePoint((e->>0)::float8,(e->>1)::float8),4326),5186) AS pt
            FROM ruc.base_roadlink r, LATERAL jsonb_array_elements(r.coords) WITH ORDINALITY AS x(e,ord)
            WHERE r.road_id=%%s),
      nl AS (SELECT l.link_id,l.geom FROM network.moct_link l, p5 WHERE ST_DWithin(l.geom,p5.g5,40)),
      v  AS (SELECT nl.link_id,(dp).path[1] vi,(dp).geom vp FROM nl, LATERAL ST_DumpPoints(nl.geom) dp)
      SELECT c.ord,
             (SELECT n.node_id FROM network.moct_node n ORDER BY n.geom <-> c.pt LIMIT 1),
             (SELECT ST_Distance(n.geom,c.pt) FROM network.moct_node n ORDER BY n.geom <-> c.pt LIMIT 1),
             (SELECT nl.link_id FROM nl ORDER BY nl.geom <-> c.pt LIMIT 1),
             (SELECT ST_Distance(nl.geom,c.pt) FROM nl ORDER BY nl.geom <-> c.pt LIMIT 1),
             (SELECT ST_Distance(v.vp,c.pt) FROM v ORDER BY v.vp <-> c.pt LIMIT 1)
      FROM c ORDER BY c.ord"""%POLY_SQL,(rid,rid))
    cps=[]
    for ord_,nid,ndist,lid,ldist,vdist in cu.fetchall():
        cps.append({'ord':ord_,'node':nid,'nd':round(float(ndist),2),
                    'link':lid,'ld':round(float(ldist),2),'vd':round(float(vdist),2)})
    out.append({'road_id':rid,'road_nm':nm,'road_kind':kind,'coords':co,
                'valid':valid,'simple':simple,'area':float(area),'peri':float(peri),
                'links':links,'cpts':cps})
    ni=sum(1 for l in links if l['kind']=='양끝 내부'); nh=sum(1 for l in links if l['kind']=='한쪽 내부')
    ne=sum(1 for l in links if l['excl'])
    onl=sum(1 for c in cps if c['ld']<=1.5); onn=sum(1 for c in cps if c['nd']<=1.0)
    print('%-10s %-30s 좌표%2d  링크 %d개(양끝내부 %d/한쪽내부 %d/관통 %d)  중심선위 좌표 %d  노드일치 좌표 %d'
          %(rid,(nm or '')[:28],len(co),len(links),ni,nh,ne,onl,onn))
json.dump(out,open(WORK+'polydata.json','w'),ensure_ascii=False)
