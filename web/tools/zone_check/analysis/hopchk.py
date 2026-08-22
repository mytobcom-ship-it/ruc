# -*- coding: utf-8 -*-
"""연속 매칭점의 링크 전이를 전수 조사 — 실제 hop 수 vs 현재 탐색 depth"""
import math, psycopg2, collections
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
cu.execute("""
  SELECT trip_id, gps_seq, match_link_id, match_lon, match_lat,
         LAG(match_link_id) OVER (PARTITION BY trip_id ORDER BY gps_seq),
         LAG(match_lon)     OVER (PARTITION BY trip_id ORDER BY gps_seq),
         LAG(match_lat)     OVER (PARTITION BY trip_id ORDER BY gps_seq)
  FROM ruc.prim_rawgps WHERE match_status=1 AND match_link_id IS NOT NULL
  ORDER BY trip_id, gps_seq""")
rows=[r for r in cu.fetchall() if r[5] and r[5]!=r[2]]
print('링크 전이 %d건'%len(rows))

# 인접 인덱스 (필요한 노드만 그때그때)
adjc={}
def out(nodes):
    todo=[n for n in nodes if n not in adjc]
    if todo:
        cu.execute("SELECT f_node,link_id,t_node FROM network.moct_link WHERE f_node=ANY(%s)",(todo,))
        d=collections.defaultdict(list)
        for f,l,t in cu.fetchall(): d[f].append((l,t))
        for n in todo: adjc[n]=d.get(n,[])
    return {n:adjc[n] for n in nodes}

ends={}
def node_of(lid):
    if lid not in ends:
        cu.execute("SELECT f_node,t_node,length FROM network.moct_link WHERE link_id=%s",(lid,))
        r=cu.fetchone(); ends[lid]=r if r else (None,None,0)
    return ends[lid]

def hops(a,b,maxd=6):
    """a 링크에서 b 링크까지 몇 hop (a→b 직접 인접이면 1)"""
    fa,ta,_=node_of(a)
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
    R=6378137.0
    p1,p2=math.radians(a[1]),math.radians(b[1])
    dp=p2-p1; dl=math.radians(b[0]-a[0])
    h=math.sin(dp/2)**2+math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
    return 2*R*math.asin(min(1.0,math.sqrt(h)))

MAXSTEP=2; EXT_DIST=50.0; EXT_MAX=3
dist_hop=collections.Counter(); over=[]; unreach=0
for trip,seq,cur,clon,clat,prev,plon,plat in rows:
    h=hops(prev,cur)
    mv=hav((float(plon),float(plat)),(float(clon),float(clat)))
    depth=MAXSTEP+min(EXT_MAX,int(mv/EXT_DIST))
    if h is None: unreach+=1; dist_hop['도달불가(>6hop)']+=1; continue
    dist_hop[h]+=1
    if h>depth: over.append((trip,seq,prev,cur,h,round(mv,1),depth))

print('\n── 실제 hop 분포 ──')
for k in sorted(dist_hop, key=lambda x:(isinstance(x,str),x)):
    print('  %-14s %3d건'%(('%d hop'%k) if isinstance(k,int) else k, dist_hop[k]))
print('\n── 현재 탐색 depth 로 못 닿는 전이: %d건 (%.1f%%) ──'%(len(over),100.0*len(over)/max(1,len(rows))))
print('  trip_id                    G    이전링크      현재링크      hop  이동m  depth')
for t,s,p,c,h,mv,d in over[:25]:
    print('  %-26s %-4d %-12s %-12s %2d   %5.1f   %d'%(t,s,p,c,h,mv,d))
if len(over)>25: print('  ... 외 %d건'%(len(over)-25))

# depth 를 올렸을 때 커버율
print('\n── maxstep 상향 시 커버율 ──')
for ms in (2,3,4,5,6):
    ok=0
    for trip,seq,cur,clon,clat,prev,plon,plat in rows:
        h=hops(prev,cur)
        if h is None: continue
        mv=hav((float(plon),float(plat)),(float(clon),float(clat)))
        if h <= ms+min(EXT_MAX,int(mv/EXT_DIST)): ok+=1
    print('  maxstep=%d → %d/%d (%.1f%%)'%(ms,ok,len(rows),100.0*ok/len(rows)))
