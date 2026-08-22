# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

import json,math,psycopg2
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
D=json.load(open(WORK+'zonedata.json'))
Z={z['road_id']:z for z in D['zones']}; LK=D['links']
def mk(lat0):
    kx=111320.0*math.cos(math.radians(lat0)); ky=110540.0
    return lambda p:(p[0]*kx,p[1]*ky)
def pt2seg(p,a,b):
    vx,vy=b[0]-a[0],b[1]-a[1]; wx,wy=p[0]-a[0],p[1]-a[1]; L2=vx*vx+vy*vy
    t=0.0 if L2==0 else max(0.0,min(1.0,(wx*vx+wy*vy)/L2))
    return math.hypot(p[0]-(a[0]+t*vx),p[1]-(a[1]+t*vy)),t
def flat(g): return g['coordinates'] if g['type']=='LineString' else [c for pt in g['coordinates'] for c in pt]
def score(coords, order):
    P=mk(coords[0][1]); poly=[P(c) for c in coords]
    cum=[0.0]
    for i in range(len(poly)-1): cum.append(cum[-1]+math.dist(poly[i],poly[i+1]))
    def loc(p):
        bd,bs=1e18,0.0
        for i in range(len(poly)-1):
            d,t=pt2seg(p,poly[i],poly[i+1])
            if d<bd: bd,bs=d,cum[i]+t*math.dist(poly[i],poly[i+1])
        return bs,bd
    dev=0; ok=0; ss=[]
    for lid in order:
        vs=[P(c) for c in flat(LK[lid]['g'])]
        dev=max(dev,max(loc(v)[1] for v in vs))
        s=loc(vs[0])[0]; e=loc(vs[-1])[0]; ss.append(s)
        if e>s: ok+=1
    inc=sum(1 for a,b in zip(ss,ss[1:]) if b>a)
    return dev, ok, inc, len(order)
for a,b in (('RL-Z00010','RL-Z00011'),('RL-Z00008','RL-Z00009')):
    for cz in (a,b):
        for lz in (a,b):
            dev,ok,inc,n=score(Z[cz]['coords'], Z[lz]['order'])
            print('%s 좌표 × %s 링크  이격max %5.1fm  링크방향일치 %2d/%2d  순서증가 %2d/%2d %s'
                  %(cz,lz,dev,ok,n,inc,n-1,'  ← 일치' if ok==n and inc==n-1 and dev<12 else ''))
    print()
