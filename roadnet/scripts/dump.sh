#!/usr/bin/env bash
# roadnet DB export / import 래퍼
#
#   bash roadnet/scripts/dump.sh export [출력디렉터리]
#   bash roadnet/scripts/dump.sh import [import디렉터리]
#
# 예:
#   PGUSER=mytobcom PGPASSWORD=xxx bash roadnet/scripts/dump.sh export
#   sudo -u postgres bash roadnet/scripts/dump.sh import roadnet/export/latest

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CMD="${1:-}"
shift || true

case "$CMD" in
    export)
        exec bash "$SCRIPT_DIR/export_dump.sh" "$@"
        ;;
    import)
        exec bash "$SCRIPT_DIR/import_dump.sh" "$@"
        ;;
    *)
        echo "Usage: $0 export [dir]" >&2
        echo "       $0 import [dir]" >&2
        exit 1
        ;;
esac
