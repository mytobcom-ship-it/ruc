#!/bin/bash
# MapMatchSvr 단독 종료 — 독립 실행(test_lib.sh 등 외부 의존 없음)
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
MM_BIN="$BIN_DIR/MapMatchSvr"
PIDFILE="$BIN_DIR/MapMatchSvr.pid"
STOP_WAIT=35

# /proc/<pid>/exe 의 실제 대상 경로 — make install 이 바이너리를 mv 로 교체하면 실행 중이던
#   프로세스의 이 링크는 "<원래경로> (deleted)" 가 되어, 경로를 그대로 비교하면 자기 프로세스를
#   못 찾는다. 그러면 kill_svr.sh 는 "not running" 으로 오판해 구 프로세스를 안 죽이고,
#   run_svr.sh 는 그 위에 새 프로세스를 또 띄워 엔진 여러 개가 같은 DB 를 동시에 처리한다
#   (2026-09-05 최정우 수정 — 실측: 빌드·재기동을 반복하다 MapMatchSvr 3개 동시 실행)
exe_path() {
	local raw
	raw="$(readlink -f "/proc/$1/exe" 2>/dev/null)"
	[ -n "$raw" ] || raw="$(readlink "/proc/$1/exe" 2>/dev/null)"
	printf '%s' "${raw% (deleted)}"
}

find_pid() {
	local pid found=""
	if [ -f "$PIDFILE" ]; then
		pid="$(tr -d ' \n' < "$PIDFILE")"
		if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null \
				&& [ "$(exe_path "$pid")" = "$MM_BIN" ]; then
			found="$pid"
		fi
		[ -z "$found" ] && rm -f "$PIDFILE"
	fi
	if [ -z "$found" ]; then
		for pid in $(pgrep -x MapMatchSvr 2>/dev/null); do
			if [ "$(exe_path "$pid")" = "$MM_BIN" ]; then
				found="$pid"
				break
			fi
		done
	fi
	echo "$found"
}

pid="$(find_pid)"
if [ -z "$pid" ]; then
	echo "MapMatchSvr not running."
	rm -f "$PIDFILE"
	exit 0
fi

echo "Stopping MapMatchSvr (pid: $pid)..."
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
	echo "오류: MapMatchSvr 종료 실패 (잔존 pid: $pid)"
	echo "  → 포그라운드 실행 중이면 해당 터미널에서 Ctrl+C 후 다시 시도"
	exit 1
fi
echo "MapMatchSvr stopped."
