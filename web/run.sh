#!/bin/bash
# RUC 맵매칭 GPS 웹 뷰어 (2026-07-10 최정우)
# 포트 80 은 nginx 가 프록시 — Flask 는 config.ini [web] port (기본 8088)
cd "$(dirname "$0")"
# venv(python3.11) 깨짐 → python3.12 기반 .venv312 사용 (2026-07-15 최정우 수정)
PY="$(pwd)/.venv312/bin/python"
[ -x "$PY" ] || PY="$(pwd)/venv/bin/python"
# 포트 80(1024 이하)은 root 권한 필요 → 비root면 sudo 로 재실행 (2026-07-16 최정우 수정)
if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$PY" server.py
fi
exec "$PY" server.py
