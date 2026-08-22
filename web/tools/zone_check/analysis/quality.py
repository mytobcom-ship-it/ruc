# -*- coding: utf-8 -*-
"""정답 없이 쓸 수 있는 매칭 품질 지표를 이전/이후로 비교"""
import math, psycopg2, collections
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

def measure(tbl,label):
    cu.execute("""SELECT trip_id,gps_seq,match_link_id,
                    LAG(match_link_id) OVER (PARTITION BY trip_id ORDER BY gps_seq)
                  FROM %s WHERE match_status=1 AND match_link_id IS NOT NULL
                  ORDER BY trip_id,gps_seq"""%tbl)
    tr=[r for r in cu.fetchall() if r[3] and r[3]!=r[2]]
    conn=rev=iso=0
    for _,_,cur_,prev in tr:
        h=hops(prev,cur_)
        if h is not None: conn+=1
        elif hops(cur_,prev) is not None: rev+=1     # 역방향으로만 닿음 = 역행/건너편 오매칭
        else: iso+=1
    print('%-8s 전이 %3d  연결됨 %3d(%.1f%%)  역방향만 %2d(%.1f%%)  단절 %2d(%.1f%%)'
          %(label,len(tr),conn,100.0*conn/len(tr),rev,100.0*rev/len(tr),iso,100.0*iso/len(tr)))
    return len(tr),conn,rev,iso

print('── 매칭 경로의 물리적 타당성 (정답 불요 지표) ──')
b=measure('ruc.bak_rawgps_maxstep2','이전')
c=measure('ruc.prim_rawgps','이후')
print()
print('── 반대방향 오매칭(역행) 건수: %d → %d ──'%(b[2],c[2]))
print('── 위상 단절(설명 불가) 건수 : %d → %d ──'%(b[3],c[3]))
