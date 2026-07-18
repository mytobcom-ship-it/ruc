#!/usr/bin/env bash
# roadnet DB import — 테이블 없으면 DDL 생성 후 데이터, 있으면 데이터만(TRUNCATE 후)
#
# manifest 기준 적재 (moct/multilink/turn_info 는 export 제외 — roadnet.sh import 로 사전 구축)
#
# 사용:
#   bash roadnet/scripts/import_dump.sh roadnet/export/latest
#   bash roadnet/scripts/import_dump.sh /path/to/export_dir
#   TRUNCATE=0 bash roadnet/scripts/import_dump.sh ...   # TRUNCATE 생략(추가 적재)
#
# 환경변수:
#   PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE(roadnet)
#   TRUNCATE=1 (기본) — 기존 테이블 데이터 삭제 후 import
#   RUN_GRANT=1       — import 후 grant_mytobcom.sql 실행 (postgres 필요)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-roadnet}"
TRUNCATE="${TRUNCATE:-1}"
RUN_GRANT="${RUN_GRANT:-0}"
export PGHOST PGPORT PGUSER PGDATABASE
[ -n "${PGPASSWORD:-}" ] && export PGPASSWORD

IMPORT_DIR="${1:-$ROOT/roadnet/export/latest}"
if [ ! -d "$IMPORT_DIR" ]; then
    echo "import dir not found: $IMPORT_DIR" >&2
    exit 1
fi
if [ ! -f "$IMPORT_DIR/manifest.json" ]; then
    echo "manifest.json missing in $IMPORT_DIR" >&2
    exit 1
fi

psql_cmd() {
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -v ON_ERROR_STOP=1 "$@"
}

pg_restore_cmd() {
    pg_restore -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" "$@"
}

table_exists() {
    local qualified="$1"
    psql_cmd -tAc "SELECT to_regclass('${qualified}') IS NOT NULL;" | tr -d ' ' | grep -q '^t'
}

# manifest tables 순서 파싱 (python)
mapfile -t TABLE_LIST < <(python3 - "$IMPORT_DIR/manifest.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    m = json.load(f)
for t in m["tables"]:
    print(t["name"])
PY
)

echo "[import] source=$IMPORT_DIR db=$PGDATABASE@$PGHOST user=$PGUSER"

# DB 없으면 생성 (postgres 슈퍼유저일 때)
if ! psql_cmd -tAc "SELECT 1 FROM pg_database WHERE datname='${PGDATABASE}';" | grep -q 1; then
    echo "[import] creating database $PGDATABASE ..."
    psql_cmd -d postgres -c "CREATE DATABASE ${PGDATABASE};"
fi

echo "[import] init.sql (postgis, schemas)"
psql_cmd -f "$IMPORT_DIR/init.sql"

for qualified in "${TABLE_LIST[@]}"; do
    safe="${qualified}"
    schema_file="$IMPORT_DIR/schema/${safe}.sql"
    data_file="$IMPORT_DIR/data/${safe}.dump"

    if [ ! -f "$schema_file" ] || [ ! -f "$data_file" ]; then
        echo "[import] skip missing files: $qualified" >&2
        continue
    fi

    if table_exists "$qualified"; then
        echo "[import] $qualified — exists, data-only"
        if [ "$TRUNCATE" = "1" ]; then
            psql_cmd -c "TRUNCATE TABLE ${qualified} CASCADE;"
        fi
        pg_restore_cmd --data-only --disable-triggers --no-owner --no-privileges \
            -t "$qualified" "$data_file"
    else
        echo "[import] $qualified — create + data"
        psql_cmd -f "$schema_file"
        pg_restore_cmd --data-only --disable-triggers --no-owner --no-privileges \
            -t "$qualified" "$data_file"
    fi

    cnt="$(psql_cmd -tAc "SELECT COUNT(*) FROM ${qualified};" | tr -d ' ')"
    echo "[import] $qualified rows=$cnt"
done

if [ "$RUN_GRANT" = "1" ] && [ -f "$ROOT/roadnet/sql/grant_mytobcom.sql" ]; then
    echo "[import] grant_mytobcom.sql"
    psql_cmd -f "$ROOT/roadnet/sql/grant_mytobcom.sql"
fi

echo "[import] done."
