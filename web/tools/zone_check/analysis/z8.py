import json, psycopg2, collections
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
cu.execute("SELECT link_ids FROM ruc.base_roadlink WHERE road_id='RL-Z00008'")
v=cu.fetchone()[0]
ids=[str(x) for x in (v if isinstance(v,list) else json.loads(v) if str(v).strip().startswith('[') else str(v).split(','))]
cu.execute("SELECT link_id,f_node,t_node,length FROM network.moct_link WHERE link_id=ANY(%s)",(ids,))
LK={r[0]:(r[1],r[2],float(r[3] or 0)) for r in cu.fetchall()}
tset={LK[i][1] for i in ids if i in LK}
fset={LK[i][0] for i in ids if i in LK}
print('링크 %d개'%len(ids))
print('시작후보(f_node가 어느 t_node도 아님):', [i for i in ids if i in LK and LK[i][0] not in tset])
print('끝후보  (t_node가 어느 f_node도 아님):', [i for i in ids if i in LK and LK[i][1] not in fset])
print()
for i in ids:
    f,t,l=LK[i]
    nxt=[j for j in ids if j in LK and LK[j][0]==t]
    print('%s  %s -> %s  %6.1fm   다음후보=%s'%(i,f,t,l,','.join(nxt) or '없음'))
# RL-Z00009 (반대방향)와 비교
cu.execute("SELECT link_ids FROM ruc.base_roadlink WHERE road_id='RL-Z00009'")
v9=cu.fetchone()[0]
ids9=[str(x) for x in (v9 if isinstance(v9,list) else json.loads(v9))]
print('\nRL-Z00009 링크수 %d, RL-Z00008 링크수 %d'%(len(ids9),len(ids)))
cu.execute("SELECT link_id,opposite_link_id FROM network.moct_link WHERE link_id=ANY(%s)",(ids9,)) if False else None
