#!/bin/bash
# Simulator/bin 에서 RUC 통합 테스트 제어 (상위 test_svr.sh 호출)
exec "$(cd "$(dirname "$0")/../.." && pwd)/test_svr.sh" "$@"
