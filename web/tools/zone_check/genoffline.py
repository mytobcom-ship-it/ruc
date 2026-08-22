# -*- coding: utf-8 -*-
import os as _os
# 산출물 경로 — 환경변수 ZONE_CHECK_WORK 로 바꿀 수 있고, 없으면 이 스크립트 옆의 work/
WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)
# web/docs 는 이 스크립트(web/tools/zone_check/) 기준 상대경로로 찾는다
DOCS = _os.path.normpath(_os.path.join(
    _os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'docs')) + '/'

"""온라인 문서(zone-link-map.html)를 인터넷 없이 열리는 단독 파일로 변환한다.
   Leaflet CSS/JS·플러그인·OSM 타일을 전부 파일 안에 심는다."""
import base64, io, os, re, glob

SC=WORK
SRC=DOCS+'zone-link-map.html'
DST=DOCS+'zone-link-map-offline.html'
s=io.open(SRC,encoding='utf-8').read()

def rep(a,b,n=1):
    global s
    assert s.count(a)==n,(s.count(a),a[:70]); s=s.replace(a,b)

css=io.open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)),'vendor')+'/leaflet.css',encoding='utf-8').read()
js =io.open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)),'vendor')+'/leaflet.js',encoding='utf-8').read()
dec=io.open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)),'vendor')+'/decorator.js',encoding='utf-8').read()

# 1) Leaflet CSS 인라인 — 아이콘 이미지 참조는 쓰지 않으므로 그대로 둬도 무방
rep('<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />',
    '<style>\n/* leaflet 1.9.4 */\n'+css+'\n</style>')

# 2) Leaflet JS · 플러그인 인라인
rep('<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>\n'
    '<script src="https://unpkg.com/leaflet-polylinedecorator@1.6.0/dist/leaflet.polylineDecorator.js"></script>',
    '<script>\n/* leaflet 1.9.4 */\n'+js+'\n</script>\n'
    '<script>\n/* leaflet-polylinedecorator 1.6.0 */\n'+dec+'\n</script>')

# 3) OSM 타일을 base64 로 심는다
tiles={}
_have = sorted(glob.glob(SC+'tiles/*.png'))
if not _have:
    raise SystemExit('타일 캐시(work/tiles/)가 비어 있어 중단합니다 — 배포본을 타일 없이 '
                     '덮어쓰지 않도록 막았습니다. ./build.sh --tiles 로 먼저 수집하세요.')
for fn in _have:
    z,x,y=os.path.basename(fn)[:-4].split('_')
    tiles['%s/%s/%s'%(z,x,y)]=base64.b64encode(open(fn,'rb').read()).decode()
raw=sum(os.path.getsize(f) for f in glob.glob(SC+'tiles/*.png'))
tjs='const TILES = {'+','.join('"%s":"%s"'%(k,v) for k,v in sorted(tiles.items()))+'};'
rep('const DATA = ', tjs+'''
// 내장 타일만 사용하는 레이어. 없는 좌표는 빈 이미지로 처리해 인터넷 요청을 만들지 않는다.
const OfflineTiles = L.TileLayer.extend({
  getTileUrl: function(c){
    const t = TILES[c.z + '/' + c.x + '/' + c.y];
    return t ? 'data:image/png;base64,' + t : L.Util.emptyImageUrl;
  }
});
const DATA = ''')

# 4) 타일 레이어 교체 (선형 지도 · POLY 지도 2곳)
rep("""L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
    { maxZoom:20, maxNativeZoom:19, attribution:'&copy; OpenStreetMap' }).addTo(map);""",
"""new OfflineTiles('', { maxZoom:20, minNativeZoom:15, maxNativeZoom:17,
    attribution:'&copy; OpenStreetMap contributors' }).addTo(map);""", 2)

# 5) 오프라인 안내 배지
rep('<h1>과금구역 링크·좌표 점검</h1>',
    '<h1>과금구역 링크·좌표 점검 <span class="offbadge">오프라인판</span></h1>')
rep('  .wrap { overflow-x:auto; }',
    '  .wrap { overflow-x:auto; }\n'
    '  .offbadge { font-size:13px; font-weight:700; color:#fff; background:var(--ok);\n'
    '      border-radius:20px; padding:3px 12px; vertical-align:middle; margin-left:10px; }')
rep('<p class="subtitle">ruc.base_roadlink',
    '<p class="subtitle" style="border-left:4px solid var(--ok);padding-left:12px">'
    '<b>이 파일은 인터넷 없이 단독으로 열립니다.</b> 지도 라이브러리와 배경 지도 타일이 파일 안에 들어 있어 '
    '외부 서버에 아무것도 요청하지 않습니다. 배경 지도는 확대 17단계까지 실제 타일이고 그 이상은 확대 표시됩니다.'
    '</p>\n<p class="subtitle">ruc.base_roadlink')

io.open(DST,'w',encoding='utf-8').write(s)
print('타일 %d개 (원본 %.1fMB → base64 %.1fMB)'%(len(tiles),raw/1048576,len(tjs)/1048576))
print('%s  %.1fMB'%(DST,os.path.getsize(DST)/1048576))
