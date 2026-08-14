#!/bin/bash
# MapMatchSvr 단독 종료 — 독립 실행(test_lib.sh 등 외부 의존 없음)
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
MM_BIN="$BIN_DIR/MapMatchSvr"
PIDFILE="$BIN_DIR/MapMatchSvr.pid"
STOP_WAIT=35

find_pid() {
	local pid found=""
	if [ -f "$PIDFILE" ]; then
		pid="$(tr -d ' \n' < "$PIDFILE")"
		if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null \
				&& [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = "$MM_BIN" ]; then
			found="$pid"
		fi
		[ -z "$found" ] && rm -f "$PIDFILE"
	fi
	if [ -z "$found" ]; then
		for pid in $(pgrep -x MapMatchSvr 2>/dev/null); do
			if [ "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" = "$MM_BIN" ]; then
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
