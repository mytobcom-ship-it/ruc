# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

import json, math, psycopg2, collections
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
cu.execute("SELECT road_id,road_nm,road_kind,link_ids,coords FROM ruc.base_roadlink ORDER BY road_id")
zones=cu.fetchall()
def parse(v):
    if v is None: return []
    if isinstance(v,list): return [str(x) for x in v]
    s=str(v).strip()
    if s.startswith('['): return [str(x) for x in json.loads(s)]
    return [t.strip() for t in s.split(',') if t.strip()]

allids=set()
for z in zones: allids|=set(parse(z[3]))
def load(ids):
    if not ids: return {}
    cu.execute("""SELECT link_id,f_node,t_node,length,ST_AsGeoJSON(ST_Transform(geom,4326),7)
                  FROM network.moct_link WHERE link_id=ANY(%s)""",(list(ids),))
    return {r[0]:{'f':r[1],'t':r[2],'len':float(r[3] or 0),'g':json.loads(r[4])} for r in cu.fetchall()}
LK=load(allids)

# ── 체인 재구성 (이전과 동일) ───────────────────────────────
def outadj(nodes):
    if not nodes: return {}
    cu.execute("SELECT link_id,f_node,t_node FROM network.moct_link WHERE f_node=ANY(%s)",(list(nodes),))
    d=collections.defaultdict(list)
    for lid,f,t in cu.fetchall(): d[f].append((lid,t))
    return d
def bfs(src,dst,maxdepth=4):
    if src==dst: return []
    fr={src:[]}; seen={src}
    for _ in range(maxdepth):
        adj=outadj(fr.keys()); nx={}
        for n,p in fr.items():
            for lid,t in adj.get(n,[]):
                if t==dst: return p+[lid]
                if t not in seen: seen.add(t); nx[t]=p+[lid]
        if not nx: break
        fr=nx
    return None
def segments(ids):
    have=[i for i in ids if i in LK]
    byf=collections.defaultdict(list)
    for i in have: byf[LK[i]['f']].append(i)
    tset={LK[i]['t'] for i in have}
    used=set(); segs=[]
    for h in [i for i in have if LK[i]['f'] not in tset]+[i for i in have if LK[i]['f'] in tset]:
        if h in used: continue
        seg=[]; cur=h
        while cur and cur not in used:
            used.add(cur); seg.append(cur)
            nx=[x for x in byf.get(LK[cur]['t'],[]) if x not in used]
            cur=nx[0] if len(nx)==1 else None
        segs.append(seg)
    return segs
def build(ids):
    segs=segments(ids)
    if not segs: return ids,[],[]
    order=list(segs[0]); missing=[]; breaks=[]; rest=segs[1:]
    while rest:
        tail=LK[order[-1]]['t']; best=None
        for k,s in enumerate(rest):
            p=bfs(tail,LK[s[0]]['f'])
            if p is not None and (best is None or len(p)<len(best[1])): best=(k,p)
        if best is None:
            head=LK[order[0]]['f']; bh=None
            for k,s in enumerate(rest):
                p=bfs(LK[s[-1]]['t'],head)
                if p is not None and (bh is None or len(p)<len(bh[1])): bh=(k,p)
            if bh is None: break
            k,p=bh; s=rest.pop(k); breaks.append({'after':s[-1],'before':order[0],'fill':p})
            missing+=p; order=s+p+order
        else:
            k,p=best; s=rest.pop(k); breaks.append({'after':order[-1],'before':s[0],'fill':p})
            missing+=p; order=order+p+s
    for s in rest:
        breaks.append({'after':order[-1],'before':s[0],'fill':None}); order+=s
    return order,missing,breaks

# ── 좌표 기하 유틸 (국지 평면 근사, m) ──────────────────────
def mk(lat0):
    kx=111320.0*math.cos(math.radians(lat0)); ky=110540.0
    return lambda p:(p[0]*kx, p[1]*ky)
def seglen(a,b): return math.hypot(b[0]-a[0], b[1]-a[1])
def pt2seg(p,a,b):
    vx,vy=b[0]-a[0],b[1]-a[1]; wx,wy=p[0]-a[0],p[1]-a[1]
    L2=vx*vx+vy*vy
    t=0.0 if L2==0 else max(0.0,min(1.0,(wx*vx+wy*vy)/L2))
    return math.hypot(p[0]-(a[0]+t*vx), p[1]-(a[1]+t*vy)), t
def locate(p, poly, cum):
    """poly 위 최근접점까지의 누적거리(m), 최근접거리(m)"""
    bd=1e18; bs=0.0
    for i in range(len(poly)-1):
        d,t=pt2seg(p,poly[i],poly[i+1])
        if d<bd: bd=d; bs=cum[i]+t*seglen(poly[i],poly[i+1])
    return bs,bd

def flat(gj):
    return gj['coordinates'] if gj['type']=='LineString' else [c for part in gj['coordinates'] for c in part]

TOL=12.0   # 좌표선이 링크를 덮었다고 볼 최대 이격(m)
CHAIN={}
for rid,nm,kind,lids,co in zones:
    CHAIN[rid]=build(parse(lids))
_need=set()
for o,m,b in CHAIN.values(): _need|=set(m)
LK.update(load([i for i in _need if i not in LK]))

RES=[]
for rid,nm,kind,lids,co in zones:
    ids=parse(lids)
    order,missing,breaks=CHAIN[rid]
    reordered=[x for x in order if x not in missing]!=ids
    cxy=co if isinstance(co,list) else json.loads(co)
    z={'road_id':rid,'road_nm':nm,'road_kind':kind,'orig':ids,'order':order,
       'missing':missing,'breaks':breaks,'reordered':reordered,
       'coords':cxy,'coord':{'n':len(cxy)},'links':[]}
    if not order or len(cxy)<2:
        z['coord']['verdict']='좌표 없음' if len(cxy)<2 else '링크 미등록'
        RES.append(z); continue
    lat0=cxy[0][1]; P=mk(lat0)
    poly=[P(c) for c in cxy]
    cum=[0.0]
    for i in range(len(poly)-1): cum.append(cum[-1]+seglen(poly[i],poly[i+1]))
    rpoly=poly[::-1]; rcum=[0.0]
    for i in range(len(rpoly)-1): rcum.append(rcum[-1]+seglen(rpoly[i],rpoly[i+1]))
    fwd=[]; rev=[]; per=[]
    for lid in order:
        vs=[P(c) for c in flat(LK[lid]['g'])]
        mx=max(locate(v,poly,cum)[1] for v in vs)
        s,_=locate(vs[0],poly,cum); e,_=locate(vs[-1],poly,cum)
        rs,_=locate(vs[0],rpoly,rcum); re_,_=locate(vs[-1],rpoly,rcum)
        fwd.append((s,e)); rev.append((rs,re_))
        per.append({'link_id':lid,'miss_link':lid in missing,'dev':round(mx,1),
                    'covered':mx<=TOL,'s':round(s,1),'e':round(e,1)})
    covered=[p for p in per if p['covered']]
    def mono(pairs, idx):
        seq=[pairs[i][0] for i in idx]
        inc=sum(1 for a,b in zip(seq,seq[1:]) if b>a)
        return inc, max(1,len(seq)-1)
    ci=[i for i,p in enumerate(per) if p['covered']]
    fi,ft=mono(fwd,ci); ri,rt=mono(rev,ci)
    # 링크 자체 방향 일치 (s<e 여야 정방향)
    fdir=sum(1 for i in ci if fwd[i][1]>fwd[i][0]); rdir=sum(1 for i in ci if rev[i][1]>rev[i][0])
    clen=cum[-1]; llen=sum(LK[l]['len'] for l in order)
    rlen=sum(LK[l]['len'] for l in order if l not in missing)
    z['coord'].update({'len':round(clen,1),'link_len':round(llen,1),'reg_len':round(rlen,1),
        'uncovered':[p['link_id'] for p in per if not p['covered']],
        'max_dev':round(max(p['dev'] for p in per),1),
        'fwd_ratio':'%d/%d'%(fi,ft),'rev_ratio':'%d/%d'%(ri,rt),
        'fdir':fdir,'rdir':rdir,'ncov':len(ci)})
    z['links']=per
    # ── 판정 ──
    v=[]
    reversed_ = len(ci)>=1 and (ri+rdir) > (fi+fdir)
    if reversed_: v.append('좌표 역순')
    unc=[p for p in per if not p['covered']]
    unc_reg=[p for p in unc if not p['miss_link']]
    unc_mis=[p for p in unc if p['miss_link']]
    if unc_reg: v.append('좌표 누락(등록링크 %d개 미포함)'%len(unc_reg))
    if unc_mis: v.append('좌표 누락(미등록링크 %d개 구간)'%len(unc_mis))
    if not v and clen < rlen*0.9: v.append('좌표 부족(총연장 %.0f%%)'%(clen/rlen*100))
    z['coord']['verdict']=' · '.join(v) if v else '정상'
    RES.append(z)
    print('%-10s %-30s 링크%2d 누락%d %-4s | 좌표%3d개 %7.1fm  이격max %5.1fm  정방향점수 %s / 역방향점수 %s  → %s'%(
        rid,(nm or '')[:28],len(ids),len(missing),'순서Y' if reordered else '',
        len(cxy),clen,z['coord']['max_dev'],'%d+%d'%(fi,fdir),'%d+%d'%(ri,rdir),z['coord']['verdict']))

json.dump({'zones':RES,'links':LK},open(WORK+'zonedata.json','w'),ensure_ascii=False)
