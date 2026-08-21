#!/bin/bash
# RUC 테스트 통합 기동/종료 — 기본은 웹 뷰어만. 맵매칭(MapMatchSvr)·시뮬레이터(RawGpsSimSvr)는
#   start-mm-sim-retry 로 별도 기동 (2026-08-19 최정우 수정)
#   ./test_svr.sh start | ps | stop | restart

ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=test_lib.sh
source "$ROOT/test_lib.sh"

usage() {
	cat <<EOF
RUC 맵매칭 테스트 통합 제어

  $0 start     웹 뷰어만 기동 (맵매칭·시뮬레이터는 기동하지 않음)
  $0 ps        맵매칭 / 시뮬 / 웹 뷰어 상태 확인
  $0 stop      웹 뷰어 → 시뮬 → 맵매칭 종료 (떠있는 것만)
  $0 restart   종료 후 재기동 (웹 뷰어만)
  $0 mm-restart  맵매칭(MapMatchSvr)만 재시작 — config.ini 값 변경 반영 (시뮬·웹 뷰어는 유지)
  $0 start-mm-sim-retry  맵매칭→시뮬 순서로 1초 확인+최대 3회 재시도 기동 — 로컬 엔진으로
                         직접 GPS 매칭이 필요할 때 사용 (웹은 건드리지 않음, 웹 "신규테스트" 버튼 전용이기도 함)

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
		mm-restart) test_restart_mm || rc=1 ;;
		start-mm-sim-retry) test_start_mm_sim_retry || rc=1 ;;
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
