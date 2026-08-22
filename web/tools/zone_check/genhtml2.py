# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)
# web/docs 는 이 스크립트(web/tools/zone_check/) 기준 상대경로로 찾는다
DOCS = _os.path.normpath(_os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'docs')) + '/'

import json, io
SC=WORK
D=json.load(open(SC+'zonedata.json'))
POLY=json.load(open(SC+'polydata.json'))
# 제안 폴리곤 대조 표시용 — 반영이 끝나면 비운다 (2026-08-23 RL-Z00014 반영 완료로 제거)
PROPOSED={}
for q in POLY: q['prop']=PROPOSED.get(q['road_id'])
# road_kind 코드 — base_roadlink 실제 값 기준 (2026-08-23 정정: 4/5 가 뒤바뀌어 있었음)
KIND={'0':'일반도로','1':'개방식','2':'폐쇄식','3':'구간단속','4':'주정차단속','5':'면제도로'}
Z=[z for z in D['zones'] if z['order']]

# 조치 이력 — 무엇을 언제 고쳤는지는 DB 만 봐서는 알 수 없으므로 여기에 축적한다.
#   현재 상태(정상/이상) 판정에는 관여하지 않고, 참고 줄로만 표시한다.
#   2026-08-23 정정: 예전엔 이력이 있으면 그 구역을 '링크 누락' 에러로 되돌려 표시했는데,
#   이미 고친 구역이 계속 빨갛게 남아 오해를 불렀다. 이력과 현재 상태를 분리한다.
FIXED = {
    'RL-Z00002': ['2026-08-22 12:49 — 누락 링크 2040425702 추가, coords 를 링크 주행순으로 재생성'],
    'RL-Z00004': ['2026-08-23 — link_ids 주행순 정렬, coords 역순 교정'],
    'RL-Z00006': ['2026-08-23 — 누락 링크 2040403001 추가'],
    'RL-Z00008': ['2026-08-23 — 누락 링크 4개 추가(2520179301·2520081707·2520081705·2520081706), '
                  'link_ids 주행순 정렬, coords 역순 교정 → 입구/출구 게이트 위치 정상화(99.6%→0.4%, 2.3%→97.7%)'],
    'RL-Z00009': ['2026-08-23 — link_ids 주행순 정렬'],
    'RL-Z00010': ['2026-08-23 — link_ids 주행순 정렬, coords 역순 교정'],
    'RL-Z00011': ['2026-08-23 — link_ids 주행순 정렬, coords 역순 교정'],
    'RL-Z00014': ['2026-08-23 — 좌표 4점 사각형을 제방길 본선 중심선 10m 버퍼(30점)로 교체 '
                  '→ 본선 5개 포함률 16~75% 에서 전부 100% 로, 면적 14,362㎡ → 12,964㎡'],
}
for z in Z:
    z['fixed'] = FIXED.get(z['road_id'], [])

# ── 원인 문구 ────────────────────────────────────────────
for z in Z:
    c=[]
    if z['missing']:
        c.append(('bad','링크 누락 %d개 — link_ids 에 중간 연결 링크가 빠짐'%len(z['missing'])))
    if z['reordered']:
        c.append(('warn','link_ids 순서 불일치 — 배열 순서가 주행순이 아님'))
    v=z['coord'].get('verdict','')
    if '역순' in v:
        c.append(('bad','좌표 거꾸로 입력 — coords 가 주행 반대방향으로 저장됨 (링크 이격 %.1fm 로 위치는 정확)'%z['coord'].get('max_dev',0)))
    unc=[p for p in z['links'] if not p['covered']]
    if unc:
        ur=[p for p in unc if not p['miss_link']]; um=[p for p in unc if p['miss_link']]
        if ur: c.append(('bad','좌표 누락 — 등록 링크 %d개 구간을 coords 가 지나지 않음'%len(ur)))
        if um: c.append(('bad','좌표 누락 — 미등록(누락) 링크 %d개 구간이 coords 에도 없음'%len(um)))
    elif z['missing']:
        c.append(('ok','좌표 범위는 온전 — 누락 링크 구간까지 coords 는 이미 포함(이격 %.1fm)라 link_ids 만 보완하면 됨'%z['coord'].get('max_dev',0)))
    for h in z['fixed']:
        c.append(('fix','조치 이력 — %s'%h))
    if not [1 for k,_ in c if k in ('bad','warn')]:
        c.append(('ok','현재 정상 — 링크 연결·순서·좌표 방향 모두 일치'))
    z['cause']=c
    z['bad']=any(k=='bad' for k,_ in c)
    z['fixonly']=(not z['bad']) and bool(z['fixed'])

def esc(s): return (s or '').replace('&','&amp;').replace('<','&lt;').replace('>','&gt;')

rows=[]
for z in Z:
    tags=''.join('<span class="tag %s">%s</span>'%(k,esc(t.split(' — ')[0])) for k,t in z['cause'])
    rows.append('<tr data-z="{rid}"><td class="mono">{rid}</td><td>{nm}</td><td class="num">{n}</td>'
                '<td class="num {mc}">{m}</td><td class="mono red">{ml}</td><td>{tg}</td></tr>'.format(
        rid=z['road_id'], nm=esc(z['road_nm']), n=len(z['orig']),
        mc='bad' if z['missing'] else '', m=len(z['missing']),
        ml=', '.join(z['missing']) or '—', tg=tags))

navs=''.join('<a href="#{rid}"><span class="mono">{rid}</span><small>{nm}</small>{dot}</a>'.format(
    rid=z['road_id'], nm=esc(z['road_nm']),
    dot='<b class="dot bad"></b>' if z['bad'] else '<b class="dot ok"></b>') for z in Z)

secs=[]
for z in Z:
    miss=set(z['missing'])
    tr=[]
    for k,p in enumerate(z['links'],1):
        lid=p['link_id']; L=D['links'][lid]
        cls=' class="mrow"' if p['miss_link'] else ('' if p['covered'] else ' class="crow"')
        note=[]
        if p['miss_link']: note.append('<em class="bad">링크 누락</em>')
        if not p['covered']: note.append('<em class="bad">좌표 미포함</em>')
        tr.append('<tr{cls}><td class="num">{k}</td><td class="mono">{lid}</td><td class="mono">{f}</td>'
                  '<td class="mono">{t}</td><td class="num">약 {ln:,d}m</td><td class="num">{dv:.1f}</td>'
                  '<td>{nt}</td></tr>'.format(cls=cls,k=k,lid=lid,f=L['f'],t=L['t'],ln=int(round(L['len'])),
                                              dv=p['dev'],nt=' '.join(note) or '—'))
    cd=z['coord']
    causes=''.join('<li class="%s">%s</li>'%(k,esc(t)) for k,t in z['cause'])
    secs.append('''<section id="{rid}"><h2>{rid} <small>{nm}</small></h2>
<p class="meta">road_kind {rk} · 등록 링크 {n}개 · 누락 링크 <b class="{mc}">{m}개</b> ·
링크 총연장 {ll:.1f}m · coords {cn}개 / {cl:.1f}m · 링크–좌표 최대이격 {dv:.1f}m</p>
<ul class="cause">{cs}</ul>
<div class="mapwrap"><div class="map" data-z="{rid}"></div>
<div class="mctl"><label><input type="checkbox" class="ckC" checked> 좌표선(coords)</label>
<label><input type="checkbox" class="ckN" checked> 노드</label>
<label><input type="checkbox" class="ckL" checked> 링크 ID</label><span class="hint">마우스 휠 = 확대·축소</span></div></div>
<div class="wrap"><table class="lk"><thead><tr><th class="num">주행순</th><th>link_id</th><th>f_node</th>
<th>t_node</th><th class="num">길이</th><th class="num">좌표이격(m)</th><th>비고</th></tr></thead>
<tbody>{tb}</tbody></table></div></section>'''.format(
        rid=z['road_id'], nm=esc(z['road_nm']), rk='%s (%s)'%(z['road_kind'],KIND.get(str(z['road_kind']),'?')),
        n=len(z['orig']), mc='bad' if z['missing'] else 'ok', m=len(z['missing']),
        ll=cd.get('link_len',0), cn=cd.get('n',0), cl=cd.get('len',0), dv=cd.get('max_dev',0),
        cs=causes, tb=''.join(tr)))

# ── POLY(주정차단속) 구역 ────────────────────────────────
# 판정 기준: 링크 양끝 노드가 폴리곤 안에 있는지. 노드가 안에 있으면 그 링크는 대상이며,
# 경계가 링크를 가로질러 포함률이 100% 미만이어도 제외 대상이 아니다 (2026-08-22 확정).
# 두 노드가 모두 밖인데 폴리곤을 관통만 하는 링크가 실제 제외 대상이다.
psecs=[]
for q in POLY:
    both=[l for l in q['links'] if l['kind']=='양끝 내부']
    half=[l for l in q['links'] if l['kind']=='한쪽 내부']
    thru=[l for l in q['links'] if l['excl']]
    tr=[]
    for l in q['links']:
        cls=' class="mrow"' if (l['excl'] or l['cut']) else ''
        tr.append('<tr{c}><td class="mono">{lid}</td><td>{nm}</td><td class="mono">{nd}</td>'
                  '<td>{kd}</td><td class="num">약 {ln:,d}m</td><td class="num">약 {il:,d}m</td>'
                  '<td class="num">{r:.0f}%</td><td>{nt}</td></tr>'.format(
            c=cls,lid=l['link_id'],nm=esc(l['name'] or '—'),
            nd='%s%s → %s%s'%(l['f'],'●' if l['fin'] else '○',l['t'],'●' if l['tin'] else '○'),
            kd=l['kind'],ln=int(round(l['len'])),il=int(round(l['inlen'])),r=l['ratio'],
            nt='<em class="bad">폴리곤 관통만 — 제외 대상</em>' if l['excl'] else
               ('<em class="bad">경계가 링크 중간을 자름 — 밖 구간 약 %dm 미포함</em>'%round(l['outlen'])
                if l['cut'] else
                ('단속 대상 (전 구간)' if l['kind']=='양끝 내부' else '단속 대상 (교차로 진출입)'))))
    # ── 좌표 점검표 ──
    ctr=[]
    for c in q['cpts']:
        onnode = c['nd']<=1.0
        online = c['ld']<=1.5
        onvtx  = c['vd']<=0.5
        bad = onnode or onvtx
        ctr.append('<tr{cl}><td class="num">{o}</td><td class="mono">{nid}</td><td class="num">{nd:.2f}</td>'
                   '<td class="mono">{lid}</td><td class="num">{ld:.2f}</td><td class="num">{vd:.2f}</td>'
                   '<td>{v}</td></tr>'.format(
            cl=' class="mrow"' if bad else (' class="crow"' if online else ''),
            o=c['ord'],nid=c['node'],nd=c['nd'],lid=c['link'],ld=c['ld'],vd=c['vd'],
            v='<em class="bad">노드 좌표 혼입 — 제거 필요</em>' if onnode else
              ('<em class="bad">링크 정점 복사 — 제거 필요</em>' if onvtx else
               ('교차로 경계 통과점' if online else '도로 이격 경계점'))))
    onnode_n=sum(1 for c in q['cpts'] if c['nd']<=1.0)
    onvtx_n =sum(1 for c in q['cpts'] if c['vd']<=0.5)
    online_n=sum(1 for c in q['cpts'] if c['ld']<=1.5)
    cause=[]
    if not q['valid']: cause.append(('bad','폴리곤이 유효하지 않음'))
    if not q['simple']: cause.append(('bad','폴리곤 경계가 자기교차함'))
    if thru: cause.append(('bad','양끝 노드가 모두 폴리곤 밖인데 관통만 하는 링크 %d개 — 제외 대상'%len(thru)))
    if onnode_n: cause.append(('bad','노드 좌표가 그대로 들어간 폴리곤 좌표 %d개 — 제거 필요'%onnode_n))
    if onvtx_n:  cause.append(('bad','링크 정점을 복사한 폴리곤 좌표 %d개 — 제거 필요'%onvtx_n))
    if len(q['coords'])<=4:
        cause.append(('bad','좌표 %d점 — 사각형 하나라 도로 형상을 따라가지 못하고 링크를 비스듬히 자름'%len(q['coords'])))
    if q.get('prop'):
        cause.append(('fix','제안 폴리곤(노란색)을 지도에 함께 표시 — %s'%q['prop']['note']))
    if online_n:
        cause.append(('ok','링크 중심선 위 좌표 %d개는 경계가 교차로 접속 링크를 지나는 통과점 — '
                           '제거하면 폴리곤이 무너져 유지해야 함'%online_n))
    cut=[l for l in q['links'] if l['cut']]
    if cut:
        cause.append(('bad','양끝 노드가 모두 내부인 링크 %d개가 경계 밖으로 삐져나감 — 폴리곤이 도로를 '
                            '제대로 감싸지 못함 (밖 구간 합계 약 %dm, 지도에 빨간 점선)'
                            %(len(cut),round(sum(l['outlen'] for l in cut)))))
    halfcut=[l for l in q['links'] if (not l['cut']) and l['kind']=='한쪽 내부' and l['outlen']>0.5]
    if halfcut:
        cause.append(('ok','교차로 진출입 링크 %d개는 경계를 한 번 가로지름 (밖 구간 합계 약 %dm) — '
                           '블록을 감싸는 닫힌 폴리곤은 블록을 떠나는 모든 도로를 자를 수밖에 없어 정상'
                           %(len(halfcut),round(sum(l['outlen'] for l in halfcut)))))
    if half: cause.append(('ok','한쪽 노드가 내부인 링크 %d개는 교차로 진출입 구간으로 단속 대상 (포함률 100%% 미만이어도 제외 아님)'%len(half)))
    if both: cause.append(('ok','양끝 노드가 내부인 링크 %d개는 전 구간 단속 대상'%len(both)))
    psecs.append('''<section id="{rid}"><h2>{rid} <small>{nm}</small></h2>
<p class="meta">road_kind {rk} · geom_type POLY · 좌표 {cn}점 · 면적 약 {ar:,d}㎡ · 둘레 약 {pe:,d}m ·
유효 {vd} · 자기교차 {sp} · 링크 {nl}개(양끝내부 {nb} / 한쪽내부 {nh} / 관통 {nt})</p>
<ul class="cause">{cs}</ul>
<div class="mapwrap"><div class="pmap" data-z="{rid}"></div>
<div class="mctl"><label><input type="checkbox" class="ckP" checked> 현재 폴리곤</label>
{pk}<label><input type="checkbox" class="ckN" checked> 노드</label>
<label><input type="checkbox" class="ckL" checked> 링크 ID</label>
<span class="hint">마우스 휠 = 확대·축소</span></div></div>
<h3>링크 판정 <small>노드 표기 ● 폴리곤 내부 / ○ 외부</small></h3>
<div class="wrap"><table><thead><tr><th>link_id</th><th>도로명</th><th>노드</th><th>노드 위치</th>
<th class="num">링크 길이</th><th class="num">폴리곤 내</th><th class="num">포함률</th><th>판정</th>
</tr></thead><tbody>{tb}</tbody></table></div>
<h3>폴리곤 좌표 점검 <small>노드·링크 정점이 잘못 섞여 들어갔는지</small></h3>
<div class="wrap"><table><thead><tr><th class="num">좌표#</th><th>최근접 노드</th>
<th class="num">노드거리(m)</th><th>최근접 링크</th><th class="num">중심선거리(m)</th>
<th class="num">링크정점거리(m)</th><th>판정</th></tr></thead><tbody>{ct}</tbody></table></div>
</section>'''.format(
        rid=q['road_id'],nm=esc(q['road_nm']),rk='%s (%s)'%(q['road_kind'],KIND.get(str(q['road_kind']),'?')),
        cn=len(q['coords']),ar=int(round(q['area'])),pe=int(round(q['peri'])),
        vd='O' if q['valid'] else 'X', sp='없음' if q['simple'] else '있음',
        nl=len(q['links']),nb=len(both),nh=len(half),nt=len(thru),
        pk=('<label><input type="checkbox" class="ckR" checked> 제안 폴리곤</label>'
            if q.get('prop') else ''),
        cs=''.join('<li class="%s">%s</li>'%(k,esc(t)) for k,t in cause),
        tb=''.join(tr),ct=''.join(ctr)))
    q['cause']=cause

pnav=''.join('<a href="#{rid}"><span class="mono">{rid}</span><small>{nm}</small>{dot}</a>'.format(
    rid=q['road_id'],nm=esc(q['road_nm']),
    dot='<b class="dot bad"></b>' if any(k=='bad' for k,_ in q['cause']) else
        ('<b class="dot warn"></b>' if any(k=='warn' for k,_ in q['cause']) else '<b class="dot ok"></b>'))
    for q in POLY)

payload={'zones':[{k:v for k,v in z.items() if k in
          ('road_id','road_nm','road_kind','orig','order','missing','coords','links','cause','bad','fixed')} for z in Z],
         'links':D['links'],
         'polys':[{k:v for k,v in q.items() if k in
                   ('road_id','road_nm','coords','links','area','peri','cpts','prop')} for q in POLY]}
data=json.dumps(payload,ensure_ascii=False,separators=(',',':'))

HTML=r'''<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>과금구역 링크·좌표 점검</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
<style>
  :root { --bg:#f8fafc; --panel:#fff; --text:#1e293b; --muted:#64748b; --border:#e2e8f0;
          --accent:#1e88e5; --code-bg:#f1f5f9; --ok:#2e7d32; --del:#c62828; --warn:#ef6c00; --fix:#00838f; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#0f172a; --panel:#1e293b; --text:#e2e8f0; --muted:#94a3b8; --border:#334155;
            --accent:#60a5fa; --code-bg:#0b1220; --ok:#66bb6a; --del:#ef5350; --warn:#ffa726; --fix:#26c6da; }
  }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--text); line-height:1.6; font-size:17px;
         font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Malgun Gothic",sans-serif; }
  #layout { display:flex; min-height:100vh; }
  nav { width:250px; flex:none; background:var(--panel); border-right:1px solid var(--border);
        padding:22px 14px; position:sticky; top:0; height:100vh; overflow-y:auto; }
  nav h2 { font-size:13px; margin:0 0 12px; color:var(--muted); text-transform:uppercase; letter-spacing:.05em; }
  nav a { display:block; font-size:13.5px; color:var(--text); text-decoration:none; padding:6px 9px;
          border-radius:6px; line-height:1.35; position:relative; }
  nav a small { display:block; font-size:12px; color:var(--muted); }
  nav a:hover { background:var(--code-bg); }
  .dot { position:absolute; right:8px; top:12px; width:9px; height:9px; border-radius:50%; }
  .dot.bad { background:var(--del); } .dot.ok { background:var(--ok); }
  .dot.fix { background:var(--fix); }
  main { flex:1; padding:30px 38px 90px; max-width:1220px; }
  h1 { font-size:30px; margin:0 0 6px; }
  .subtitle { color:var(--muted); font-size:15.5px; margin-bottom:22px; max-width:80ch; }
  section { margin-bottom:52px; scroll-margin-top:14px; }
  section h2 { font-size:23px; border-bottom:3px solid var(--accent); padding-bottom:7px; margin:0 0 8px; }
  section h2 small { font-size:15px; color:var(--muted); font-weight:400; margin-left:8px; }
  .meta { color:var(--muted); font-size:14px; margin:0 0 10px; }
  ul.cause { margin:0 0 14px; padding-left:20px; font-size:14.5px; }
  ul.cause li.bad { color:var(--del); font-weight:600; }
  ul.cause li.warn { color:var(--warn); font-weight:600; }
  ul.cause li.ok { color:var(--ok); }
  ul.cause li.fix { color:var(--fix); font-weight:600; }
  table { border-collapse:collapse; width:100%; font-size:14px; background:var(--panel); }
  th,td { border:1px solid var(--border); padding:6px 10px; text-align:left; vertical-align:middle; }
  td.num { text-align:right; font-variant-numeric:tabular-nums; }
  th, th.num { background:var(--code-bg); font-weight:700; text-align:center; vertical-align:middle; }
  .mono { font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; font-size:13px; }
  .red { color:var(--del); } .ok { color:var(--ok); font-weight:700; } .bad { color:var(--del); font-weight:700; }
  tr.mrow td { background:rgba(198,40,40,.12); }
  tr.crow td { background:rgba(239,108,0,.10); }
  tr.frow td { background:rgba(0,131,143,.11); }
  tr.mrow em, tr.crow em, tr.frow em { font-style:normal; font-weight:700; font-size:12px; }
  em.fix, td.fix { color:var(--fix); }
  .tag { display:inline-block; font-size:11.5px; font-weight:700; border-radius:11px;
         padding:1px 9px; margin:1px 3px 1px 0; color:#fff; white-space:nowrap; }
  .tag.bad { background:#c62828; } .tag.warn { background:#ef6c00; } .tag.ok { background:#2e7d32; }
  .tag.fix { background:#00838f; }
  #sum tbody tr { cursor:pointer; } #sum tbody tr:hover td { background:var(--code-bg); }
  .mapwrap { margin:0 0 14px; }
  .map, .pmap { height:430px; border:1px solid var(--border); border-radius:8px; background:var(--code-bg); }
  .mctl { display:flex; gap:16px; font-size:13px; color:var(--muted); padding:7px 3px 0; flex-wrap:wrap; }
  .mctl label { cursor:pointer; user-select:none; }
  .mctl .hint { margin-left:auto; font-size:12.5px; opacity:.8; }
  .legend { display:flex; gap:18px; flex-wrap:wrap; align-items:center; font-size:13.5px;
            color:var(--muted); margin:12px 0 26px; padding:11px 14px; background:var(--panel);
            border:1px solid var(--border); border-radius:8px; }
  .legend i { display:inline-block; width:28px; height:6px; border-radius:3px; vertical-align:middle; margin-right:7px; }
  .legend i.nd  { width:15px; height:15px; border-radius:50%; border:3px solid #0d47a1; background:#fff; }
  .legend i.ndr { width:15px; height:15px; border-radius:50%; border:3px solid #b71c1c; background:#ef9a9a; }
  .legend i.nds { width:17px; height:17px; border-radius:50%; border:4px solid #1b5e20; background:#66bb6a; }
  .legend i.nde { width:17px; height:17px; border-radius:50%; border:4px solid #1a237e; background:#7986cb; }
  .leaflet-tooltip.lt { font-family:ui-monospace,Menlo,Consolas,monospace; font-size:12px; }
  .leaflet-tooltip.lid { background:rgba(13,71,161,.92); border:0; color:#fff; font-weight:700;
      font-family:ui-monospace,Menlo,Consolas,monospace; font-size:11.5px; padding:2px 6px;
      box-shadow:none; white-space:nowrap; }
  .leaflet-tooltip.lid.m { background:rgba(183,28,28,.94); }
  .leaflet-tooltip.lid.f { background:rgba(0,131,143,.94); }
  .leaflet-tooltip.lid:before { display:none; }
  .leaflet-tooltip.lid { pointer-events:none; }
  .wrap { overflow-x:auto; }
</style>
</head>
<body>
<div id="layout">
<nav><h2>선형 구역 (LINE)</h2>@@NAV@@
<h2 style="margin-top:20px">면 구역 (POLY)</h2>@@PNAV@@</nav>
<main>
<h1>과금구역 링크·좌표 점검</h1>
<p class="subtitle">ruc.base_roadlink 의 <b>link_ids</b>(링크 연결)와 <b>coords</b>(구간 좌표)를 주행 방향 기준으로
교차 검증했습니다. 링크의 t_node 가 다음 링크의 f_node 와 맞지 않는 지점은 network.moct_link 에서 최단 경로를 찾아
<span class="bad">누락 링크</span>로 표시하고, coords 는 그 주행 체인 위에 투영해 <b>덮는 범위</b>와 <b>진행 방향</b>을 확인했습니다.</p>

<div class="legend">
  <span><i style="background:#1e88e5"></i>등록된 링크</span>
  <span><i style="background:#d32f2f"></i>누락 링크(미등록)</span>
  <span><i style="background:#fb8c00"></i>coords 좌표선</span>
  <span><i class="nd"></i>노드</span>
  <span><i class="ndr"></i>누락 링크 인접 노드</span>
  <span><i class="nds"></i>주행 시작</span>
  <span><i class="nde"></i>주행 종료</span>
  <span><b style="color:#e53935;font-size:16px">➤</b> 빨간 화살표 = 링크 주행방향</span>
  <span><b style="color:#e65100;font-size:16px">➤</b> 주황 화살표 = coords 입력방향</span>
</div>

<section id="summary"><h2>요약</h2>
<p class="meta">행을 클릭하면 해당 구역으로 이동합니다.</p>
<div class="wrap"><table id="sum"><thead><tr><th>road_id</th><th>road_nm</th><th class="num">등록</th>
<th class="num">누락</th><th>누락 link_id</th><th>원인</th></tr></thead><tbody>@@ROWS@@</tbody></table></div>
</section>
@@SECS@@

<section id="polyhead"><h2>면(POLY) 구역 — 주정차단속</h2>
<p class="meta">주정차단속 구역은 주행 체인이 아니라 <b>면 안에 머무는지</b>로 판정하므로 link_ids 가 없습니다.
coords 를 닫아 폴리곤을 만든 뒤 <b>링크 양끝 노드가 폴리곤 안에 있는지</b>로 대상을 판정했습니다.
노드가 안에 있으면 경계가 링크를 가로질러 포함률이 100% 미만이어도 단속 대상입니다.
아울러 폴리곤 좌표에 노드나 링크 정점이 잘못 섞여 들어갔는지도 좌표 단위로 대조했습니다.</p>
<div class="legend">
  <span><i style="background:#f9a825"></i>폴리곤 경계 (RL-Z00001)</span>
  <span><i style="background:#7b1fa2"></i>RL-Z00014 현재 폴리곤 (보라 점선)</span>
  <span><i style="background:#f9a825"></i>RL-Z00014 제안 폴리곤 (노란 실선, DB 미반영)</span>
  <span><i style="background:#1565c0"></i>양끝 노드 내부 — 전 구간 단속 대상</span>
  <span><i style="background:#42a5f5"></i>한쪽 노드 내부 — 교차로 진출입, 단속 대상</span>
  <span><i style="background:#d32f2f"></i>양끝 노드 외부, 관통만 — 제외 대상</span>
  <span><i style="background:#d32f2f"></i>대상 링크인데 경계 밖으로 잘린 구간</span>
  <span><i style="background:#9e9e9e"></i>무관한 링크의 폴리곤 밖 구간</span>
  <span><i class="nd" style="border-color:#1b5e20;background:#66bb6a"></i>폴리곤 내부 노드</span>
  <span><i class="nd"></i>폴리곤 외부 노드</span>
  <span><b style="color:#e53935;font-size:16px">➤</b> 빨간 화살표 = 링크 주행방향</span>
</div></section>
@@PSECS@@
</main></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script src="https://unpkg.com/leaflet-polylinedecorator@1.6.0/dist/leaflet.polylineDecorator.js"></script>
<script>
const DATA = @@DATA@@;
const DIR_COLOR = '#e53935';   // 주행방향 화살표 (전 지도 공통)
// 폴리곤 색은 구역별로 지정 가능. 지정이 없으면 기본 노랑.
const POLY_COLOR = { 'RL-Z00014': { line:'#7b1fa2', fill:'#ce93d8' } };
const POLY_COLOR_DEF = { line:'#f9a825', fill:'#ffee58' };
const ZM = {}; DATA.zones.forEach(z => ZM[z.road_id] = z);

function partsOf(lid){
  const L = DATA.links[lid]; if(!L) return [];
  const gj = L.g;
  const ps = gj.type === 'MultiLineString' ? gj.coordinates : [gj.coordinates];
  return ps.map(p => p.map(c => [c[1], c[0]]));
}
// 폴리라인 길이 기준 중앙점과, 그 지점의 진행방향 세그먼트를 함께 돌려준다
function midPoint(cs){
  const ln = cs[0], seg = []; let tot = 0;
  for(let i = 0; i < ln.length - 1; i++){
    const d = Math.hypot(ln[i+1][0] - ln[i][0], ln[i+1][1] - ln[i][1]);
    seg.push(d); tot += d;
  }
  let acc = 0;
  for(let i = 0; i < seg.length; i++){
    if(acc + seg[i] >= tot / 2){
      const t = seg[i] ? (tot / 2 - acc) / seg[i] : 0;
      return { p: [ln[i][0] + (ln[i+1][0] - ln[i][0]) * t,
                   ln[i][1] + (ln[i+1][1] - ln[i][1]) * t], a: ln[i], b: ln[i+1] };
    }
    acc += seg[i];
  }
  return { p: ln[0], a: ln[0], b: ln[ln.length - 1] };
}
// 링크 진행방향에 수직으로 라벨을 밀어내 노드 마커를 가리지 않게 한다.
// 웹메르카토르는 등각사상이라 방향은 줌과 무관하므로 한 번만 계산하면 된다.
function labelAt(map, cs){
  const m = midPoint(cs);
  const pa = map.project(L.latLng(m.a), 18), pb = map.project(L.latLng(m.b), 18);
  const dx = pb.x - pa.x, dy = pb.y - pa.y, n = Math.hypot(dx, dy) || 1;
  let ox = -dy / n, oy = dx / n;
  if(oy > 0){ ox = -ox; oy = -oy; }      // 항상 위쪽으로 몰아 라벨끼리도 덜 겹치게
  return { ll: m.p, dir: [ox, oy] };
}

// ── 링크 ID 라벨 배치 ─────────────────────────────────────
// 링크가 짧으면 라벨 상자가 양끝 노드를 덮는다. 링크에 수직인 방향으로 거리를
// 늘려가며 노드 원·기존 라벨과 겹치지 않는 첫 자리를 고르고, 멀어지면 연결선을 긋는다.
// 픽셀 기하는 줌에 따라 달라지므로 zoomend 마다 다시 계산한다.
function makeLabeler(map, gLead){
  const nodes = [], labels = [];
  const STEPS = [24, 34, 46, 60, 76, 94, 114, 136];
  function hitNode(b, np){
    return np.some(n => {
      const qx = Math.max(b.x, Math.min(n.p.x, b.x + b.w));
      const qy = Math.max(b.y, Math.min(n.p.y, b.y + b.h));
      return Math.hypot(n.p.x - qx, n.p.y - qy) < n.r;
    });
  }
  function hitBox(b, ps){
    return ps.some(o => b.x < o.x + o.w && b.x + b.w > o.x && b.y < o.y + o.h && b.y + b.h > o.y);
  }
  function relayout(){
    const np = nodes.map(n => ({ p: map.latLngToLayerPoint(L.latLng(n.ll)), r: n.r }));
    const placed = []; gLead.clearLayers();
    labels.slice().sort((a, c) => c.len - a.len).forEach(it => {   // 긴 링크부터 자리 배정
      const el = it.tt.getElement();
      const w = (el ? el.offsetWidth : 76) + 8, h = (el ? el.offsetHeight : 18) + 6;
      const base = map.latLngToLayerPoint(L.latLng(it.ll));
      let best = null;
      for(const d of STEPS){
        for(const sg of [1, -1]){
          const cx = base.x + it.dx * d * sg, cy = base.y + it.dy * d * sg;
          const box = { x: cx - w / 2, y: cy - h / 2, w, h };
          if(hitNode(box, np) || hitBox(box, placed)) continue;
          best = { cx, cy, box }; break;
        }
        if(best) break;
      }
      if(!best){
        const d = STEPS[STEPS.length - 1];
        const cx = base.x + it.dx * d, cy = base.y + it.dy * d;
        best = { cx, cy, box: { x: cx - w / 2, y: cy - h / 2, w, h } };
      }
      placed.push(best.box);
      it.tt.options.offset = L.point(Math.round(best.cx - base.x), Math.round(best.cy - base.y));
      it.tt.update();
      if(Math.hypot(best.cx - base.x, best.cy - base.y) > 30){
        L.polyline([it.ll, map.layerPointToLatLng(L.point(best.cx, best.cy))],
          { color: it.col, weight:1.5, opacity:.6, dashArray:'2,4', interactive:false }).addTo(gLead);
      }
    });
  }
  return {
    node: (ll, r) => nodes.push({ ll, r }),
    label: (ll, dir, tt, col, len) => labels.push({ ll, dx:dir[0], dy:dir[1], tt, col, len }),
    relayout,
    start(){ map.whenReady(() => setTimeout(relayout, 0)); map.on('zoomend', relayout); }
  };
}

function buildMap(el, rid){
  const z = ZM[rid];
  const map = L.map(el, { preferCanvas:true, scrollWheelZoom:true, wheelDebounceTime:30, wheelPxPerZoomLevel:80 });
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    { maxZoom:20, maxNativeZoom:19, attribution:'&copy; OpenStreetMap' }).addTo(map);
  const gCo   = L.layerGroup().addTo(map);   // 좌표선 (최하단)
  const gLink = L.layerGroup().addTo(map);   // 링크
  const gNode = L.layerGroup().addTo(map);   // 노드 (최상단)
  const gLead = L.layerGroup().addTo(map);   // 라벨 연결선
  const gLab  = L.layerGroup().addTo(map);   // 링크 ID 라벨(DOM)
  const LB = makeLabeler(map, gLead);        // 라벨 충돌 회피
  const miss = new Set(z.missing), b = [];

  // ── coords 좌표선 (주황) ──
  if(z.coords && z.coords.length > 1){
    const cl = z.coords.map(c => [c[1], c[0]]);
    const pc = L.polyline(cl, { color:'#fb8c00', weight:4, opacity:.85, dashArray:'2,7',
                                lineCap:'round' }).addTo(gCo);
    pc.bindTooltip('coords · ' + z.coords.length + '점', { className:'lt', sticky:true });
    L.polylineDecorator(pc, { patterns:[{ offset:'4%', repeat:'90px',
      symbol: L.Symbol.arrowHead({ pixelSize:11, headAngle:50,
        pathOptions:{ color:'#e65100', fillOpacity:.95, weight:0 } }) }] }).addTo(gCo);
    L.circleMarker(cl[0], { radius:6, color:'#e65100', weight:3, fillColor:'#fff3e0',
      fillOpacity:1 }).addTo(gCo).bindTooltip('coords 시작', { className:'lt' });
  }

  // ── 링크 ──
  const nodeSeen = {};
  z.order.forEach((lid, i) => {
    const L0 = DATA.links[lid], isM = miss.has(lid);
    const col = isM ? '#d32f2f' : '#1e88e5';
    const cov = (z.links[i] || {}).covered !== false;
    partsOf(lid).forEach(cs => {
      const pl = L.polyline(cs, { color:col, weight: isM ? 9 : 6,
                  opacity: isM ? .95 : .8,
                  dashArray: isM ? '12,7' : null }).addTo(gLink);
      pl.bindTooltip('#' + (i+1) + '  ' + lid + (isM ? '  [링크 누락]' : '') +
        (cov ? '' : '  [좌표 미포함]') + '<br>' + L0.f + ' → ' + L0.t +
        '<br>약 ' + Math.round(L0.len) + 'm · 좌표이격 ' + (z.links[i] ? z.links[i].dev : '?') + 'm',
        { className:'lt', sticky:true });
      L.polylineDecorator(pl, { patterns:[{ offset:'12%', repeat:'95px',
        symbol: L.Symbol.arrowHead({ pixelSize:14, headAngle:50,
          pathOptions:{ color:DIR_COLOR, fillOpacity:1, weight:0 } }) }] }).addTo(gLink);
      cs.forEach(p => b.push(p));
    });
    const cs0 = partsOf(lid);
    if(!cs0.length || !cs0[0].length) return;
    const la = labelAt(map, cs0);
    const tt = L.tooltip({ permanent:true, direction:'center', interactive:false,
        className:'lid' + (isM ? ' m' : '') }).setLatLng(la.ll)
        .setContent(lid).addTo(gLab);
    LB.label(la.ll, la.dir, tt, col, L0.len);
    // 노드
    const ends = [[L0.f, cs0[0][0], i], [L0.t, cs0[cs0.length-1].slice(-1)[0], i]];
    ends.forEach(([nid, ll]) => {
      if(nodeSeen[nid]) return; nodeSeen[nid] = 1;
      const adj = isM || (i > 0 && miss.has(z.order[i-1])) ||
                  (i < z.order.length-1 && miss.has(z.order[i+1]));
      // 흰 헤일로를 먼저 깔아 링크 위에서도 원이 또렷하게 보이도록 한다
      L.circleMarker(ll, { radius:10, color:'#ffffff', weight:3, opacity:.95,
        fillColor:'#ffffff', fillOpacity:1 }).addTo(gNode);
      L.circleMarker(ll, { radius:7,
        color: adj ? '#b71c1c' : ('#0d47a1'), weight:3,
        fillColor: adj ? '#ef9a9a' : ('#ffffff'), fillOpacity:1 }).addTo(gNode)
        .bindTooltip('node ' + nid + (adj ? '<br>누락 링크 인접' :
          ''), { className:'lt' });
      LB.node(ll, 13);
    });
  });
  // 시작 / 종료
  function endNode(ll, ring, fill, tip){
    L.circleMarker(ll, { radius:14, color:'#ffffff', weight:3, opacity:.95,
      fillColor:'#ffffff', fillOpacity:1 }).addTo(gNode);
    L.circleMarker(ll, { radius:11, color:ring, weight:4, fillColor:fill,
      fillOpacity:1 }).addTo(gNode).bindTooltip(tip, { className:'lt' });
    LB.node(ll, 18);
  }
  const a0 = partsOf(z.order[0]), aN = partsOf(z.order[z.order.length-1]);
  const nf = DATA.links[z.order[0]].f, nt = DATA.links[z.order[z.order.length-1]].t;
  if(a0.length) endNode(a0[0][0], '#1b5e20', '#66bb6a', '주행 시작<br>node ' + nf);
  if(aN.length){ const lc = aN[aN.length-1];
    endNode(lc[lc.length-1], '#1a237e', '#7986cb', '주행 종료<br>node ' + nt); }
  if(b.length) map.fitBounds(L.latLngBounds(b).pad(0.06), { maxZoom:18 });
  LB.start();

  const w = el.closest('.mapwrap');
  w.querySelector('.ckC').onchange = e => e.target.checked ? map.addLayer(gCo) : map.removeLayer(gCo);
  w.querySelector('.ckN').onchange = e => {
    if(e.target.checked) map.addLayer(gNode); else map.removeLayer(gNode);
  };
  w.querySelector('.ckL').onchange = e => {
    if(e.target.checked){ map.addLayer(gLab); map.addLayer(gLead); LB.relayout(); }
    else { map.removeLayer(gLab); map.removeLayer(gLead); }
  };
  return map;
}

function buildPoly(el, rid){
  const q = (DATA.polys || []).find(x => x.road_id === rid);
  const map = L.map(el, { preferCanvas:true, scrollWheelZoom:true,
                          wheelDebounceTime:30, wheelPxPerZoomLevel:80 });
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    { maxZoom:20, maxNativeZoom:19, attribution:'&copy; OpenStreetMap' }).addTo(map);
  const gOut = L.layerGroup().addTo(map);   // 폴리곤 밖 링크
  const gPoly= L.layerGroup().addTo(map);   // 폴리곤
  const gIn  = L.layerGroup().addTo(map);   // 폴리곤 안 링크
  const gNode= L.layerGroup().addTo(map);   // 링크 노드
  const gLead= L.layerGroup().addTo(map);   // 라벨 연결선
  const gLab = L.layerGroup().addTo(map);   // 링크 ID
  const LB   = makeLabeler(map, gLead);
  const ring = q.coords.map(c => [c[1], c[0]]);

  const pc = POLY_COLOR[rid] || POLY_COLOR_DEF;
  const hasProp = !!(q.prop && q.prop.coords && q.prop.coords.length > 2);
  const poly = L.polygon(ring, { color:pc.line, weight:4, opacity:1,
      fillColor:pc.fill, fillOpacity: hasProp ? .12 : .22,
      dashArray: hasProp ? '10,6' : null }).addTo(gPoly);
  poly.bindTooltip(rid + ' 현재<br>' + q.coords.length + '점 · 약 ' +
      Math.round(q.area).toLocaleString() + '㎡', { className:'lt', sticky:true });
  // 제안 폴리곤 — 노란색. DB 에는 반영하지 않은 안이다.
  const gProp = L.layerGroup().addTo(map);
  if(hasProp){
    const pr = L.polygon(q.prop.coords.map(c => [c[1], c[0]]),
        { color:'#f9a825', weight:4, opacity:1, fillColor:'#ffee58',
          fillOpacity:.22 }).addTo(gProp);
    pr.bindTooltip(rid + ' 제안<br>' + q.prop.coords.length + '점<br>' + q.prop.note,
        { className:'lt', sticky:true });
  }

  const parts = g => !g ? [] : (g.type === 'MultiLineString' ? g.coordinates :
                     (g.type === 'LineString' ? [g.coordinates] : []));
  const nodeSeen = {};
  q.links.forEach(l => {
    const full = parts(l.g).map(c => c.map(x => [x[1], x[0]]));
    // 대상 링크(노드 하나라도 내부)인데 경계 밖으로 삐져나온 구간은 빨간 점선으로 경고한다.
    // 무관한 링크의 밖 구간은 회색 그대로.
    const cut = !!l.cut;
    parts(l.gout).forEach(c => {
      const ll = c.map(x => [x[1], x[0]]);
      const pg = L.polyline(ll, { color: cut ? '#d32f2f' : '#9e9e9e',
                  weight: cut ? 6 : 4, opacity: cut ? .95 : .55,
                  dashArray: cut ? '4,7' : null }).addTo(gOut);
      if(cut) pg.bindTooltip(l.link_id + ' — 폴리곤 밖 구간 약 ' + Math.round(l.outlen) + 'm' +
        '<br><b>경계가 링크 중간을 자름 — 폴리곤 미포함</b>', { className:'lt', sticky:true });
    });
    // 진행방향 화살표는 링크 전체에 얹는다
    full.forEach(ll => {
      const po = L.polyline(ll, { color:'#9e9e9e', weight:1, opacity:0 }).addTo(gOut);
      L.polylineDecorator(po, { patterns:[{ offset:'10%', repeat:'95px',
        symbol: L.Symbol.arrowHead({ pixelSize:13, headAngle:50,
          pathOptions:{ color:DIR_COLOR, fillOpacity:.8, weight:0 } }) }] }).addTo(gOut);
    });
    // 폴리곤 안 구간은 파란 굵은 선 + 같은 방향 화살표
    // 노드 기준 판정: 양끝 내부=진파랑 실선, 한쪽 내부=연파랑 파선(교차로 진출입),
    // 양끝 외부인데 관통만=빨강(제외 대상)
    const ex = !!l.excl, half = !ex && l.kind === '한쪽 내부';
    const cIn = ex ? '#d32f2f' : (half ? '#42a5f5' : '#1565c0');
    const cAr = ex ? '#b71c1c' : (half ? '#1e88e5' : '#0d47a1');
    parts(l.gin).forEach(c => {
      const ll = c.map(x => [x[1], x[0]]);
      const pl = L.polyline(ll, { color:cIn, weight: ex ? 9 : 7, opacity:.92,
                  dashArray: ex ? '12,7' : (half ? '14,8' : null) }).addTo(gIn);
      pl.bindTooltip(l.link_id + (l.name ? '  ' + l.name : '') +
        '<br>' + l.f + ' → ' + l.t +
        '<br>링크 약 ' + Math.round(l.len) + 'm 중 폴리곤 안 약 ' + Math.round(l.inlen) +
        'm (' + Math.round(l.ratio) + '%)' +
        '<br>노드 위치: ' + l.kind +
        (ex ? '<br><b>폴리곤 관통만 — 제외 대상</b>'
            : (half ? '<br>교차로 진출입 — 단속 대상' : '<br>전 구간 단속 대상')),
        { className:'lt', sticky:true });
      L.polylineDecorator(pl, { patterns:[{ offset:'12%', repeat:'95px',
        symbol: L.Symbol.arrowHead({ pixelSize:14, headAngle:50,
          pathOptions:{ color:DIR_COLOR, fillOpacity:1, weight:0 } }) }] }).addTo(gIn);
    });
    // 링크 ID 라벨 — 폴리곤 안 구간(없으면 전체)의 중앙에, 노드를 피해 배치
    const base = parts(l.gin).length ? parts(l.gin).map(c => c.map(x => [x[1], x[0]]))
                                     : full;
    if(base.length){
      const la = labelAt(map, base);
      const tt = L.tooltip({ permanent:true, direction:'center', interactive:false,
          className:'lid' + (ex ? ' m' : '') }).setLatLng(la.ll).setContent(l.link_id).addTo(gLab);
      LB.label(la.ll, la.dir, tt, cAr, l.inlen || l.len);
    }
    // 링크 양끝 노드
    if(full.length){
      const ends = [[l.f, full[0][0], l.fin], [l.t, full[full.length-1].slice(-1)[0], l.tin]];
      ends.forEach(([nid, ll, isin]) => {
        if(nodeSeen[nid]) return; nodeSeen[nid] = 1;
        L.circleMarker(ll, { radius:10, color:'#ffffff', weight:3, opacity:.95,
          fillColor:'#ffffff', fillOpacity:1 }).addTo(gNode);
        L.circleMarker(ll, { radius:7, color: isin ? '#1b5e20' : '#0d47a1', weight:3,
          fillColor: isin ? '#66bb6a' : '#ffffff', fillOpacity:1 }).addTo(gNode)
          .bindTooltip('node ' + nid + (isin ? '<br>폴리곤 내부' : '<br>폴리곤 외부'),
            { className:'lt' });
        LB.node(ll, 13);
      });
    }
  });



  let fb = poly.getBounds();
  gProp.eachLayer(x => { if(x.getBounds) fb = fb.extend(x.getBounds()); });
  map.fitBounds(fb.pad(0.08), { maxZoom:18 });
  LB.start();
  const w = el.closest('.mapwrap');
  w.querySelector('.ckP').onchange = e => e.target.checked ? map.addLayer(gPoly) : map.removeLayer(gPoly);
  const ckR = w.querySelector('.ckR');
  if(ckR) ckR.onchange = e => e.target.checked ? map.addLayer(gProp) : map.removeLayer(gProp);
  w.querySelector('.ckN').onchange = e => e.target.checked ? map.addLayer(gNode) : map.removeLayer(gNode);
  w.querySelector('.ckL').onchange = e => {
    if(e.target.checked){ map.addLayer(gLab); map.addLayer(gLead); LB.relayout(); }
    else { map.removeLayer(gLab); map.removeLayer(gLead); }
  };
}

const io = new IntersectionObserver((es, ob) => {
  es.forEach(e => { if(!e.isIntersecting) return;
    ob.unobserve(e.target);
    (e.target.classList.contains('pmap') ? buildPoly : buildMap)(e.target, e.target.dataset.z); });
}, { rootMargin:'300px' });
document.querySelectorAll('.map, .pmap').forEach(el => io.observe(el));
document.querySelectorAll('#sum tbody tr').forEach(tr => tr.addEventListener('click', () =>
  document.getElementById(tr.dataset.z).scrollIntoView({ behavior:'smooth', block:'start' })));
</script>
</body></html>'''

for k,v in (('@@NAV@@',navs),('@@PNAV@@',pnav),('@@ROWS@@',''.join(rows)),
            ('@@SECS@@',''.join(secs)),('@@PSECS@@',''.join(psecs)),('@@DATA@@',data)):
    HTML=HTML.replace(k,v)
io.open(DOCS+'zone-link-map.html','w',encoding='utf-8').write(HTML)
print('written %d bytes, zones=%d'%(len(HTML.encode('utf-8')),len(Z)))
for z in Z:
    print('  %-10s %s'%(z['road_id'],' | '.join(t for _,t in z['cause'])))
