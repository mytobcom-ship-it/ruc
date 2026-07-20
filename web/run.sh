#!/bin/bash
# RUC 맵매칭 GPS 웹 뷰어 (2026-07-10 최정우)
# 포트 80 은 nginx 가 프록시 — Flask 는 config.ini [web] port (기본 8088)
cd "$(dirname "$0")"
# venv(python3.11) 깨짐 → python3.12 기반 .venv312 사용 (2026-07-15 최정우 수정)
PY="$(pwd)/.venv312/bin/python"
[ -x "$PY" ] || PY="$(pwd)/venv/bin/python"
# config.ini [web] port 조회 (미지정 시 8088)
WEB_PORT="$(sed -n '/^\[web\]/,/^\[/{/^port=/s/^port=//p}' config.ini | head -1)"
WEB_PORT="${WEB_PORT:-8088}"
# 1024 미만 포트만 root 권한 필요 → 그 외에는 sudo 없이 기동 (root 프로세스는
# 일반 계정 test_lib.sh 의 /proc/pid 조회 권한이 없어 running 상태 오판 원인이 됨) (2026-07-20 최정우 수정)
if [ "$WEB_PORT" -lt 1024 ] && [ "$(id -u)" -ne 0 ]; then
    exec sudo "$PY" server.py
fi
exec "$PY" server.py
