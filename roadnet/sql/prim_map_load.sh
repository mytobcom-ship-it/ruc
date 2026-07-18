#!/usr/bin/env bash
# PRIM_LINK_INFO 테이블 생성 + link.psf 적재
# 사용 (프로젝트 루트):
#   bash roadnet/sql/prim_map_load.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PSF="${PSF:-$ROOT/MapMatchSvr/bin/link.psf}"
CONFIG="${CONFIG:-$ROOT/MapMatchSvr/bin/config.ini}"

echo "[1/2] DDL: prim_map_create.sql (postgres)"
sudo -u postgres psql -d roadnet -f "$ROOT/roadnet/sql/prim_map_create.sql"

echo "[2/2] ETL: link.psf -> PRIM_LINK_INFO"
if [[ ! -d "$ROOT/roadnet/scripts/.pydeps" ]]; then
  pip install psycopg2-binary --target "$ROOT/roadnet/scripts/.pydeps" -q
fi
PYTHONPATH="$ROOT/roadnet/scripts/.pydeps" \
  python3 "$ROOT/roadnet/scripts/psf_load_prim_link_info.py" \
    --psf "$PSF" --config "$CONFIG"

echo "done."
