#!/usr/bin/env bash
# roadnet 통합 스크립트
#   setup  — create.sql 실행 (DB + 스키마 + 테이블 + 권한)
#   import — Shapefile/DBF → PostgreSQL import
#   all    — setup + import (기본)
#
# 사용법:
#   bash roadnet/scripts/roadnet.sh
#   bash roadnet/scripts/roadnet.sh setup
#   bash roadnet/scripts/roadnet.sh import

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="${DATA_DIR:-$ROOT/CreateData/data}"
TURNINFO_DBF="${TURNINFO_DBF:-$DATA_DIR/TURNINFO.dbf}"
PGUSER="${PGUSER:-postgres}"
PGHOST="${PGHOST:-}"
PGPORT="${PGPORT:-5432}"
PGDATABASE="${PGDATABASE:-roadnet}"

PYTHON="${ROOT}/.venv-roadnet/bin/python3"
[ -x "$PYTHON" ] || PYTHON=python3

run_psql() {
    if [ -z "$PGHOST" ]; then
        sudo -u postgres psql "$@"
    else
        psql -U "$PGUSER" -h "$PGHOST" -p "$PGPORT" "$@"
    fi
}

do_setup() {
    echo "[setup] Creating database, schema, tables, grants ..."
    run_psql -f "$ROOT/roadnet/sql/create.sql"
    echo "[setup] Done."
}

do_import() {
    echo "[import] Loading MOCT data ..."
    PY_ARGS=(--dbname "$PGDATABASE" --user "$PGUSER" --data-dir "$DATA_DIR" --turninfo "$TURNINFO_DBF")
    [ -n "$PGHOST" ] && PY_ARGS+=(--host "$PGHOST" --port "$PGPORT")

    if command -v ogr2ogr >/dev/null 2>&1 && [ -z "${FORCE_PYTHON:-}" ]; then
        echo "[import] Using ogr2ogr for shapefiles ..."
        OGR_CONN="PG:host=${PGHOST:-localhost} port=$PGPORT dbname=$PGDATABASE user=$PGUSER"
        ogr2ogr -f PostgreSQL "$OGR_CONN" "$DATA_DIR/MOCT_NODE.shp" \
            -nln network.moct_node -lco GEOMETRY_NAME=geom -lco SPATIAL_INDEX=GIST -lco FID= \
            -nlt PROMOTE_TO_MULTI -s_srs EPSG:5186 -t_srs EPSG:5186 -overwrite
        ogr2ogr -f PostgreSQL "$OGR_CONN" "$DATA_DIR/MOCT_LINK.shp" \
            -nln network.moct_link -lco GEOMETRY_NAME=geom -lco SPATIAL_INDEX=GIST -lco FID= \
            -nlt PROMOTE_TO_MULTI -s_srs EPSG:5186 -t_srs EPSG:5186 -overwrite
        "$PYTHON" "$SCRIPT_DIR/import.py" --dbf-only "${PY_ARGS[@]}"
    else
        echo "[import] Using Python importer ..."
        "$PYTHON" "$SCRIPT_DIR/import.py" "${PY_ARGS[@]}"
    fi
    echo "[import] Done."
}

CMD="${1:-all}"
case "$CMD" in
    setup)  do_setup ;;
    import) do_import ;;
    all)    do_setup; do_import ;;
    *)
        echo "Usage: $0 [setup|import|all]" >&2
        exit 1
        ;;
esac
