# -*- coding: utf-8 -*-
"""연속 매칭 3점(A,B,C)에서 B 를 거치면 hop 이 크게 늘어나는 '우회' 패턴 탐지"""
import psycopg2, collections
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
adjc={}; ends={}
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
        cu.execute("SELECT f_node,t_node FROM network.moct_link WHERE link_id=%s",(lid,))
        ends[lid]=cu.fetchone() or (None,None)
    return ends[lid]
def hops(a,b,maxd=6):
    if a==b: return 0
    _,ta=info(a)
    if ta is None: return None
    fr={ta:0}; seen={ta}
    for d in range(1,maxd+1):
        adj=out(list(fr.keys())); nx={}
        for n in fr:
            for lid,t in adj.get(n,[]):
                if lid==b: return d
                if t not in seen: seen.add(t); nx[t]=d
        if not nx: break
        fr=nx
    return None

cu.execute("""SELECT trip_id, gps_seq, match_link_id FROM ruc.prim_rawgps
              WHERE match_status=1 AND match_link_id IS NOT NULL ORDER BY trip_id, gps_seq""")
rows=cu.fetchall()
by=collections.defaultdict(list)
for t,s,l in rows: by[t].append((s,l))

tot=0; detour=[]
for t,pts in by.items():
    for i in range(len(pts)-2):
        (s1,a),(s2,b),(s3,c)=pts[i],pts[i+1],pts[i+2]
        if s2!=s1+1 or s3!=s2+1: continue      # 연속 3점만
        if a==b or b==c: continue              # 링크가 바뀌는 경우만
        tot+=1
        hab=hops(a,b); hbc=hops(b,c); hac=hops(a,c)
        if hab is None or hbc is None or hac is None: continue
        via=hab+hbc
        if via >= hac+3:                       # B 를 거치면 3 hop 이상 더 듦
            detour.append((t,s2,a,b,c,hab,hbc,hac))
print('연속 3점(링크 변경 포함) %d건 중 우회 의심 %d건 (%.1f%%)'%(tot,len(detour),100.0*len(detour)/max(1,tot)))
print('\ntrip_id                    G     A(직전)      B(현재)      C(다음)      A→B  B→C  A→C')
for t,s,a,b,c,x,y,z in detour[:20]:
    print('%-26s %-5d %-12s %-12s %-12s %2d   %2d   %2d'%(t,s,a,b,c,x,y,z))
if len(detour)>20: print('... 외 %d건'%(len(detour)-20))
