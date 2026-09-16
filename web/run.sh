#!/bin/bash
# RUC 맵매칭 GPS 웹 뷰어 (2026-07-10 최정우)
# 포트 80 은 nginx 가 프록시 — Flask 는 config.ini [web] port (기본 8088)
#
# 사용법: ./run.sh            기존 프로세스 정리 후 백그라운드 기동 + 응답 확인
#         ./run.sh stop       종료
#         ./run.sh status     상태 확인
#         ./run.sh -f         포그라운드 실행(Ctrl+C 로 종료) — 2026-09-16 이전의 종전 동작
#
# 종전에는 exec 로 포그라운드 실행이라 터미널을 잡았고, 이미 떠 있는 상태에서 다시 실행하면
#   포트 충돌로 새 프로세스만 조용히 죽었다. MapMatchSvr/bin/run_svr.sh 와 같은 방식(pid 파일
#   기반 단일 인스턴스 + nohup 백그라운드 + 기동 확인)으로 통일 (2026-09-16 최정우 수정)
WEB_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$WEB_DIR" || exit 1
PIDFILE="$WEB_DIR/web.pid"
LOG="$WEB_DIR/web_run.log"
STOP_WAIT=10
START_WAIT=15

# venv(python3.11) 깨짐 → python3.12 기반 .venv312 사용 (2026-07-15 최정우 수정)
PY="$WEB_DIR/.venv312/bin/python"
[ -x "$PY" ] || PY="$WEB_DIR/venv/bin/python"
[ -x "$PY" ] || { echo "오류: python 실행파일 없음 ($WEB_DIR/.venv312/bin/python)"; exit 1; }

# config.ini [web] port 조회 (미지정 시 8088)
WEB_PORT="$(sed -n '/^\[web\]/,/^\[/{/^port=/s/^port=//p}' config.ini | head -1)"
WEB_PORT="${WEB_PORT:-8088}"

# 이 디렉터리의 server.py 를 돌리는 프로세스만 우리 것으로 인정 — 다른 경로의 동명
#   스크립트(server.py)를 오인해 죽이지 않도록 /proc/<pid>/cwd 까지 대조한다.
#   cwd 를 못 읽는 경우(1024 미만 포트라 sudo 로 띄운 root 프로세스)는 판정을 생략한다
is_ours() {
	local pid="$1" cwd
	[ -n "$pid" ] || return 1
	kill -0 "$pid" 2>/dev/null || return 1
	grep -qsa 'server\.py' "/proc/$pid/cmdline" || return 1
	cwd="$(readlink -f "/proc/$pid/cwd" 2>/dev/null)"
	[ -z "$cwd" ] || [ "$cwd" = "$WEB_DIR" ]
}

# pid 파일 우선, 없거나 죽었으면 프로세스 목록에서 탐색(pid 파일 없이 수동 기동한 경우 대비)
find_pid() {
	local pid found=""
	if [ -f "$PIDFILE" ]; then
		pid="$(tr -d ' \n' < "$PIDFILE")"
		is_ours "$pid" && found="$pid"
		[ -z "$found" ] && rm -f "$PIDFILE"
	fi
	if [ -z "$found" ]; then
		for pid in $(pgrep -f 'server\.py' 2>/dev/null); do
			[ "$pid" = "$$" ] && continue
			if is_ours "$pid"; then
				found="$pid"
				break
			fi
		done
	fi
	echo "$found"
}

stop_web() {
	local pid i
	pid="$(find_pid)"
	if [ -z "$pid" ]; then
		rm -f "$PIDFILE"
		return 1
	fi
	echo "기존 웹 뷰어 종료 중... (pid: $pid)"
	kill -TERM "$pid" 2>/dev/null
	for ((i = 1; i <= STOP_WAIT; i++)); do
		kill -0 "$pid" 2>/dev/null || break
		sleep 1
	done
	if kill -0 "$pid" 2>/dev/null; then
		echo "graceful stop timeout — SIGKILL (pid: $pid)"
		kill -KILL "$pid" 2>/dev/null
		sleep 1
	fi
	rm -f "$PIDFILE"
	if kill -0 "$pid" 2>/dev/null; then
		echo "오류: 웹 뷰어 종료 실패 (잔존 pid: $pid)"
		echo "  → sudo 로 기동된 프로세스면 sudo kill $pid 로 종료할 것"
		exit 1
	fi
	return 0
}

case "${1:-start}" in
	stop)
		stop_web && echo "웹 뷰어 종료 완료." || echo "웹 뷰어 not running."
		exit 0
		;;
	status)
		pid="$(find_pid)"
		if [ -n "$pid" ]; then
			echo "웹 뷰어 running (pid: $pid)  http://127.0.0.1:$WEB_PORT/"
			exit 0
		fi
		echo "웹 뷰어 not running."
		exit 1
		;;
	-f|--foreground)
		stop_web
		# 1024 미만 포트만 root 권한 필요 → 그 외에는 sudo 없이 기동 (root 프로세스는
		# 일반 계정 test_lib.sh 의 /proc/pid 조회 권한이 없어 running 상태 오판 원인이 됨) (2026-07-20 최정우 수정)
		if [ "$WEB_PORT" -lt 1024 ] && [ "$(id -u)" -ne 0 ]; then
			exec sudo "$PY" server.py
		fi
		exec "$PY" server.py
		;;
	start)
		;;
	*)
		echo "사용법: $0 [start|stop|status|-f]"
		exit 1
		;;
esac

# ── 백그라운드 기동 ────────────────────────────────────────────────────────────
# 1024 미만 포트는 sudo 가 필요해 비밀번호 입력을 요구할 수 있다 — 백그라운드로 돌리면
#   프롬프트가 로그로 새어 기동이 멈추므로 이 경우만 종전대로 포그라운드 실행한다
if [ "$WEB_PORT" -lt 1024 ] && [ "$(id -u)" -ne 0 ]; then
	echo "포트 $WEB_PORT 는 root 권한 필요 — 포그라운드(sudo)로 실행합니다. Ctrl+C 로 종료"
	stop_web
	exec sudo "$PY" server.py
fi

stop_web
echo "==== $(date '+%F %T') start web viewer ====" >>"$LOG"
nohup "$PY" server.py >>"$LOG" 2>&1 </dev/null &
echo "$!" > "$PIDFILE"

# Flask 기동까지 최대 START_WAIT 초 폴링 — 포트 충돌·import 오류는 여기서 드러난다
code=""
for ((i = 1; i <= START_WAIT; i++)); do
	pid="$(find_pid)"
	if [ -z "$pid" ]; then
		echo "오류: 웹 뷰어 기동 실패 — 로그 마지막 20줄:"
		tail -n 20 "$LOG"
		rm -f "$PIDFILE"
		exit 1
	fi
	code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$WEB_PORT/" 2>/dev/null)"
	[ "$code" = "200" ] && break
	sleep 1
done

if [ "$code" = "200" ]; then
	echo "웹 뷰어 기동 완료 (pid: $pid)  http://127.0.0.1:$WEB_PORT/"
	echo "  로그: $LOG    종료: $0 stop"
	exit 0
fi

echo "경고: ${START_WAIT}초 내 HTTP 200 미확인 (pid: $pid, 마지막 응답: ${code:-none}) — 로그 마지막 20줄:"
tail -n 20 "$LOG"
exit 1
