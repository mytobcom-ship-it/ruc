# -*- coding: utf-8 -*-
"""6 hop 내 도달 불가 52건의 성격 분류"""
import math, psycopg2, collections
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
cu.execute("""
  SELECT trip_id, gps_seq, match_link_id, match_lon, match_lat, gps_dt,
         LAG(match_link_id) OVER w, LAG(match_lon) OVER w, LAG(match_lat) OVER w,
         LAG(gps_dt) OVER w, LAG(gps_seq) OVER w
  FROM ruc.prim_rawgps WHERE match_status=1 AND match_link_id IS NOT NULL
  WINDOW w AS (PARTITION BY trip_id ORDER BY gps_seq)
  ORDER BY trip_id, gps_seq""")
rows=[r for r in cu.fetchall() if r[6] and r[6]!=r[2]]
adjc={}; ends={}
def out(nodes):
    todo=[n for n in nodes if n not in adjc]
    if todo:
        cu.execute("SELECT f_node,link_id,t_node FROM network.moct_link WHERE f_node=ANY(%s)",(todo,))
        d=collections.defaultdict(list)
        for f,l,t in cu.fetchall(): d[f].append((l,t))
        for n in todo: adjc[n]=d.get(n,[])
    return {n:adjc[n] for n in nodes}
def node_of(lid):
    if lid not in ends:
        cu.execute("SELECT f_node,t_node FROM network.moct_link WHERE link_id=%s",(lid,))
        ends[lid]=cu.fetchone() or (None,None)
    return ends[lid]
def hops(a,b,maxd=6):
    _,ta=node_of(a)
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
def secs(a,b):
    import datetime
    f='%Y%m%d%H%M%S'
    return (datetime.datetime.strptime(b.strip(),f)-datetime.datetime.strptime(a.strip(),f)).total_seconds()

cat=collections.Counter(); ex=collections.defaultdict(list)
for trip,seq,cur,clon,clat,gdt,prev,plon,plat,pdt,pseq in rows:
    if hops(prev,cur) is not None: continue
    mv=hav((float(plon),float(plat)),(float(clon),float(clat)))
    dt=secs(pdt,gdt)
    rev = hops(cur,prev) is not None            # 역방향으로는 닿는가 = 역주행/반대편 링크
    gap = (seq-pseq) > 1                        # 사이에 미매칭 좌표가 있었는가
    if rev:   k='반대방향(역행/건너편 링크)'
    elif gap: k='중간 미매칭 좌표 존재(공백)'
    elif dt>10: k='시간공백 %.0fs'%dt
    elif mv>150: k='장거리 점프 %.0fm'%mv
    else:     k='근거리인데 위상 단절'
    cat[k]+=1; ex[k].append((trip,seq,prev,cur,round(mv),int(dt)))
print('── 6hop 내 도달불가 52건 분류 ──')
for k,v in cat.most_common():
    print('  %-28s %2d건'%(k,v))
    for t,s,p,c,mv,dt in ex[k][:4]:
        print('      %s G%-4d %s → %s  %sm %ss'%(t,s,p,c,mv,dt))
