#!/usr/bin/env bash
# roadnet DB 테이블·데이터 export
#
# 산출물 (기본: roadnet/export/YYYYMMDD_HHMMSS/)
#   manifest.json
#   init.sql              — PostGIS·스키마
#   schema/<schema.table>.sql
#   data/<schema.table>.dump  — pg_dump custom (data-only)
#
# 사용:
#   bash roadnet/scripts/export_dump.sh
#   bash roadnet/scripts/export_dump.sh /path/to/export_dir
#   PGHOST=127.0.0.1 PGUSER=mytobcom PGPASSWORD=xxx bash roadnet/scripts/export_dump.sh
#
# 환경변수: PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE(roadnet)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-mytobcom}"
PGDATABASE="${PGDATABASE:-roadnet}"
export PGHOST PGPORT PGUSER PGDATABASE
[ -n "${PGPASSWORD:-}" ] && export PGPASSWORD

STAMP="$(date +%Y%m%d_%H%M%S)"
EXPORT_DIR="${1:-$ROOT/roadnet/export/${STAMP}}"

# 제외 (roadnet.sh import 로 별도 구축):
#   network.moct_node, network.moct_link — 용량 큼
#   network.multilink, network.turn_info   — PRIM 지도 표시 미사용
TABLES=(
    roadnet.prim_rawgps
    roadnet.prim_link_info
)

psql_cmd() {
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -v ON_ERROR_STOP=1 "$@"
}

pg_dump_cmd() {
    pg_dump -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" "$@"
}

mkdir -p "$EXPORT_DIR/schema" "$EXPORT_DIR/data"

echo "[export] target=$EXPORT_DIR db=$PGDATABASE@$PGHOST"

# init: extension + schemas
cat > "$EXPORT_DIR/init.sql" <<'EOF'
-- roadnet import 초기화 (export_dump.sh 생성)
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE SCHEMA IF NOT EXISTS network;
CREATE SCHEMA IF NOT EXISTS roadnet;
COMMENT ON SCHEMA network IS '표준 노드·링크 (moct/multilink/turn_info 는 export 제외 — roadnet.sh 로 구축)';
COMMENT ON SCHEMA roadnet IS 'RUC 위치검증·맵매칭 표시 (PRIM_*)';
EOF

MANIFEST="$EXPORT_DIR/manifest.json"
echo "{" > "$MANIFEST"
echo "  \"exported_at\": \"$(date -Iseconds)\"," >> "$MANIFEST"
echo "  \"database\": \"$PGDATABASE\"," >> "$MANIFEST"
echo "  \"host\": \"$PGHOST\"," >> "$MANIFEST"
echo "  \"tables\": [" >> "$MANIFEST"

first=1
for qualified in "${TABLES[@]}"; do
    schema="${qualified%%.*}"
    table="${qualified#*.}"
    safe="${schema}.${table}"
    schema_file="$EXPORT_DIR/schema/${safe}.sql"
    data_file="$EXPORT_DIR/data/${safe}.dump"

    echo "[export] $qualified ..."
    row_count="$(psql_cmd -tAc "SELECT COUNT(*) FROM ${qualified};" | tr -d ' ')"

    pg_dump_cmd -t "$qualified" --schema-only --no-owner --no-privileges \
        -f "$schema_file"

    pg_dump_cmd -t "$qualified" --data-only --format=custom --no-owner --no-privileges \
        -f "$data_file"

    if [ "$first" -eq 0 ]; then
        echo "," >> "$MANIFEST"
    fi
    first=0
    printf '    {"name": "%s", "rows": %s, "schema": "schema/%s.sql", "data": "data/%s.dump"}' \
        "$qualified" "$row_count" "$safe" "$safe" >> "$MANIFEST"
done

echo "" >> "$MANIFEST"
echo "  ]" >> "$MANIFEST"
echo "}" >> "$MANIFEST"

# 편의: 최신 export 심볼릭 링크
ln -sfn "$EXPORT_DIR" "$ROOT/roadnet/export/latest"

echo "[export] done → $EXPORT_DIR"
echo "[export] latest → roadnet/export/latest"
