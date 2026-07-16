#!/bin/bash
# link.psf 생성 파이프라인 (MapMatchSvr 배포 복사는 제외) (2026-07-16 최정우)
#   1) 원본 MOCT shp(EPSG:5186) → EPSG:4326 재투영 (pyproj = PostGIS와 동일 proj, 웹 좌표와 일치)
#      · geometry(.shp/.shx)만 재투영, 속성(.dbf/.cpg)은 원본 그대로 복사(인덱스 정렬 유지)
#   2) MakeBinary 로 link.psf 생성 (config.ini 의 coordtype=1(WGS84GEO) 사용 → 재변환 없음)
#   ※ 생성된 link.psf 를 MapMatchSvr/bin 으로 옮기는 배포 복사는 포함하지 않음(수동 처리).
set -e

cd "$(dirname "$0")"

PY="/home/mytobcom/ruc/web/.venv312/bin/python"
REPROJ="/home/mytobcom/ruc/CreateData/scripts/reproject_5186_to_4326.py"
DATA="/home/mytobcom/ruc/CreateData/data"

echo "===================================================="
echo " link.psf 생성 시작 $(date '+%Y-%m-%d %H:%M:%S')"
echo "===================================================="

# ── 1) 재투영 5186 → 4326 (geometry) ────────────────────────────────
echo "[1/2] shp 재투영 (EPSG:5186 → EPSG:4326)"
"$PY" "$REPROJ" "$DATA/MOCT_LINK.shp" "$DATA/MOCT_LINK_4326.shp"
"$PY" "$REPROJ" "$DATA/MOCT_NODE.shp" "$DATA/MOCT_NODE_4326.shp"

# 속성/코드페이지는 원본 그대로 복사 (레코드 순서 = 원본과 동일)
cp -f "$DATA/MOCT_LINK.dbf" "$DATA/MOCT_LINK_4326.dbf"
cp -f "$DATA/MOCT_LINK.cpg" "$DATA/MOCT_LINK_4326.cpg"
cp -f "$DATA/MOCT_NODE.dbf" "$DATA/MOCT_NODE_4326.dbf"
cp -f "$DATA/MOCT_NODE.cpg" "$DATA/MOCT_NODE_4326.cpg"

# ── 2) link.psf 생성 ────────────────────────────────────────────────
echo "[2/2] link.psf 생성 (MakeBinary, coordtype=1)"
./MakeBinary

echo "----------------------------------------------------"
if [ -f ./link.psf ]; then
    echo "완료: link.psf = $(stat -c%s ./link.psf) bytes"
    echo "※ 운영 반영: ./link.psf 를 MapMatchSvr/bin 으로 복사 후 엔진 재기동하세요."
else
    echo "실패: link.psf 가 생성되지 않았습니다."
    exit 1
fi
