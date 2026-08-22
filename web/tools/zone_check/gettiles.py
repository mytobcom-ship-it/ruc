# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

import json, math, os, time, urllib.request
SC=WORK
TD=SC+'tiles/'; os.makedirs(TD,exist_ok=True)
Z=json.load(open(SC+'zonedata.json')); P=json.load(open(SC+'polydata.json')); LK=Z['links']
def flat(g):
    if not g: return []
    return g['coordinates'] if g['type']=='LineString' else [c for p in g['coordinates'] for c in p]
boxes=[]
for z in Z['zones']:
    pts=[]
    for lid in z['order']: pts+=flat(LK.get(lid,{}).get('g'))
    if pts: boxes.append(pts)
for q in P:
    pts=list(q['coords'])
    for l in q['links']: pts+=flat(l['g'])
    if q.get('prop'): pts+=q['prop']['coords']
    boxes.append(pts)
def t(lon,lat,z):
    n=2**z
    return (int((lon+180)/360*n),
            int((1-math.log(math.tan(math.radians(lat))+1/math.cos(math.radians(lat)))/math.pi)/2*n))
want=[]
seen=set()
for zoom in (15,16,17):
    for pts in boxes:
        lons=[p[0] for p in pts]; lats=[p[1] for p in pts]
        dx=(max(lons)-min(lons))*0.15 or 0.0006; dy=(max(lats)-min(lats))*0.15 or 0.0006
        x0,y1=t(min(lons)-dx,min(lats)-dy,zoom); x1,y0=t(max(lons)+dx,max(lats)+dy,zoom)
        for x in range(x0,x1+1):
            for y in range(y0,y1+1):
                k=(zoom,x,y)
                if k not in seen: seen.add(k); want.append(k)
print('내려받을 타일 %d개'%len(want))
UA='ruc-zone-doc/1.0 (internal verification document; contact mytobcom@gmail.com)'
ok=fail=skip=0; total=0
for i,(z,x,y) in enumerate(want,1):
    fn='%s%d_%d_%d.png'%(TD,z,x,y)
    if os.path.exists(fn) and os.path.getsize(fn)>0:
        skip+=1; total+=os.path.getsize(fn); continue
    url='https://tile.openstreetmap.org/%d/%d/%d.png'%(z,x,y)
    try:
        rq=urllib.request.Request(url,headers={'User-Agent':UA})
        d=urllib.request.urlopen(rq,timeout=20).read()
        open(fn,'wb').write(d); ok+=1; total+=len(d)
    except Exception as e:
        fail+=1
        if fail<=3: print('  실패 %d/%d/%d %s'%(z,x,y,e))
    time.sleep(0.06)
    if i%100==0: print('  %d/%d  (%.1fMB)'%(i,len(want),total/1048576))
print('완료: 신규 %d · 재사용 %d · 실패 %d · 합계 %.1fMB'%(ok,skip,fail,total/1048576))
