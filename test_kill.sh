#!/bin/bash
# RUC 테스트 엔진 종료 (시뮬 → 맵매칭)
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=test_lib.sh
source "$ROOT/test_lib.sh"
test_stop_all
