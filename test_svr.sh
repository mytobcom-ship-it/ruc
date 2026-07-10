#!/bin/bash
# RUC 테스트 통합 기동/종료 — MapMatchSvr + RawGpsSimSvr
# 사용:
#   ./test_svr.sh start|stop|restart|status
#   ./test_kill.sh   ./test_ps.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=test_lib.sh
source "$ROOT/test_lib.sh"

usage() {
	cat <<EOF
RUC 맵매칭 테스트 통합 제어

  $0 start     맵매칭 엔진 + 시뮬레이터 기동 (기존 프로세스 자동 재시작)
  $0 stop      시뮬레이터 + 맵매칭 엔진 종료
  $0 restart   종료 후 재기동
  $0 status    프로세스 상태 확인

개별 제어: test_kill.sh (종료), test_ps.sh (상태)
EOF
}

cmd_restart() {
	test_stop_all || true
	sleep 2
	test_start_all
}

case "${1:-}" in
	start)   test_start_all ;;
	stop)    test_stop_all ;;
	restart) cmd_restart ;;
	status)  test_ps_all ;;
	-h|--help|help) usage ;;
	*)
		usage
		exit 1
		;;
esac
