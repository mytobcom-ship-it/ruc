#!/bin/bash
# 과금구역 링크·좌표 점검 문서 생성 파이프라인 (2026-08-22 최정우)
#
#   ruc DB 를 읽어 web/docs/zone-link-map.html 을 만들고, 이어서 인터넷 없이 열리는
#   단독 파일 web/docs/zone-link-map-offline.html 을 만든다.
#
#   사용:  ./build.sh          문서만 재생성(타일 캐시 재사용)
#          ./build.sh --tiles  OSM 타일까지 새로 내려받음(첫 실행 또는 구역 변경 시)
#
#   DB 접속은 각 스크립트 상단의 psycopg2.connect() 값을 따른다.
#   산출 중간파일은 work/ 아래에 쌓인다(ZONE_CHECK_WORK 로 변경 가능).
set -e
cd "$(dirname "$0")"
PY=${PYTHON:-python3}

echo "[1/4] 선형 구역 체인·좌표 검증"
$PY zonechk2.py
echo "[2/4] 면(POLY) 구역 폴리곤 검증"
$PY poly.py
if [ "$1" = "--tiles" ]; then
  echo "[3/4] OSM 타일 내려받기"
  $PY gettiles.py
else
  echo "[3/4] 타일 내려받기 생략 (--tiles 로 강제)"
fi
echo "[4/4] HTML 생성"
$PY genhtml2.py
$PY genoffline.py
echo "완료 — ../../docs/zone-link-map.html, zone-link-map-offline.html"
