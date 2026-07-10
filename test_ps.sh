#!/bin/bash
# RUC 테스트 엔진 상태 확인
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=test_lib.sh
source "$ROOT/test_lib.sh"
test_ps_all
