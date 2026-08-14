#!/bin/bash
# MapMatchSvr 단독 상태 확인 — 독립 실행(test_lib.sh 등 외부 의존 없음)
BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
MM_BIN="$BIN_DIR/MapMatchSvr"
PIDFILE="$BIN_DIR/MapMatchSvr.pid"

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
	echo "MapMatchSvr: not running"
	exit 1
fi
ps -fp "$pid"
