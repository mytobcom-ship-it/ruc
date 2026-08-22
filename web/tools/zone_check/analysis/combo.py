# -*- coding: utf-8 -*-
import math, psycopg2, collections
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
cu.execute("""SELECT trip_id,gps_seq,match_link_id,match_lon,match_lat,
                LAG(match_link_id) OVER w, LAG(match_lon) OVER w, LAG(match_lat) OVER w
              FROM ruc.prim_rawgps WHERE match_status=1 AND match_link_id IS NOT NULL
              WINDOW w AS (PARTITION BY trip_id ORDER BY gps_seq) ORDER BY trip_id,gps_seq""")
rows=[r for r in cu.fetchall() if r[5] and r[5]!=r[2]]
adjc={}; ends={}; lens={}
def out(nodes):
    todo=[n for n in nodes if n not in adjc]
    if todo:
        cu.execute("SELECT f_node,link_id,t_node FROM network.moct_link WHERE f_node=ANY(%s)",(todo,))
        d=collections.defaultdict(list)
        for f,l,t in cu.fetchall(): d[f].append((l,t))
        for n in todo: adjc[n]=d.get(n,[])
    return {n:adjc[n] for n in nodes}
def info(lid):
    if lid not in ends:
        cu.execute("SELECT f_node,t_node,length FROM network.moct_link WHERE link_id=%s",(lid,))
        r=cu.fetchone() or (None,None,0); ends[lid]=(r[0],r[1]); lens[lid]=float(r[2] or 0)
    return ends[lid]
def hops(a,b,maxd=8):
    _,ta=info(a)
    if ta is None: return None
    fr={ta:0}; seen={ta}
    for d in range(1,maxd+1):
        adj=out(list(fr.keys())); nxt={}
        for n in fr:
            for lid,t in adj.get(n,[]):
                if lid==b: return d
                if t not in seen: seen.add(t); nxt[t]=d
        if not nxt: break
        fr=nxt
    return None
def hav(a,b):
    R=6378137.0; p1,p2=math.radians(a[1]),math.radians(b[1])
    h=math.sin((p2-p1)/2)**2+math.cos(p1)*math.cos(p2)*math.sin(math.radians(b[0]-a[0])/2)**2
    return 2*R*math.asin(min(1.0,math.sqrt(h)))

data=[]
for trip,seq,cur,clon,clat,prev,plon,plat in rows:
    h=hops(prev,cur); info(prev)
    mv=hav((float(plon),float(plat)),(float(clon),float(clat)))
    data.append((h,mv,lens.get(prev,0.0)))
reach=[d for d in data if d[0] is not None]
print('전이 %d건 (도달가능 %d, 불가 %d)\n'%(len(data),len(reach),len(data)-len(reach)))
print('maxstep  EXT_DIST  커버(도달가능 기준)   최대depth')
for ms in (2,3,4,5):
    for ed in (50.0,25.0):
        ok=sum(1 for h,mv,pl in reach if h <= ms+min(3,int(mv/ed)))
        print('   %d       %4.0fm     %3d/%d  (%5.1f%%)        %d'
              %(ms,ed,ok,len(reach),100.0*ok/len(reach),ms+3))
print('\n※ 참고: "직전 링크가 짧으면 +1" 규칙 적용 시')
for ms in (2,4):
    ok=sum(1 for h,mv,pl in reach if h <= ms+min(3,int(mv/50.0))+(1 if 0<pl<20 else 0))
    print('   maxstep=%d + 짧은직전링크(+1) → %d/%d (%.1f%%)'%(ms,ok,len(reach),100.0*ok/len(reach)))
