#!/bin/bash
# RUC 맵매칭 GPS 웹 뷰어 (2026-07-10 최정우)
# 포트 80 은 nginx 가 프록시 — Flask 는 config.ini [web] port (기본 8088)
cd "$(dirname "$0")"
exec ./venv/bin/python server.py
