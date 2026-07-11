#!/bin/bash
# RUC 테스트 통합 기동/종료 — MapMatchSvr + RawGpsSimSvr + 웹 뷰어
#   ./test_svr.sh start | ps | stop | restart

ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=test_lib.sh
source "$ROOT/test_lib.sh"

usage() {
	cat <<EOF
RUC 맵매칭 테스트 통합 제어

  $0 start     맵매칭 → 시뮬 → 웹 뷰어 기동
  $0 ps        맵매칭 / 시뮬 / 웹 뷰어 상태 확인
  $0 stop      웹 뷰어 → 시뮬 → 맵매칭 종료
  $0 restart   종료 후 재기동

별칭: status (= ps)

지도: http://127.0.0.1/ (nginx) 또는 http://127.0.0.1:8088/
주의: 터미널에서 ./MapMatchSvr 직접 실행하지 말고 본 스크립트만 사용하세요.
실패 시: MapMatchSvr/bin/MapMatchSvr_launcher.log, web/web_launcher.log 확인
EOF
}

main() {
	local rc=0

	case "${1:-}" in
		start)   test_start_all || rc=1 ;;
		stop)    test_stop_all || rc=1 ;;
		restart) test_restart_all || rc=1 ;;
		ps|status) test_ps_all ;;
		-h|--help|help) usage ;;
		*)
			usage
			rc=1
			;;
	esac
	exit "$rc"
}

main "$@"
