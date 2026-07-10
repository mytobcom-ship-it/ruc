#!/bin/bash
# RUC 테스트 엔진 공통 제어 (MapMatchSvr / RawGpsSimSvr)
# test_svr.sh, test_kill.sh, test_ps.sh 및 bin/run_svr.sh 에서 source

TEST_LIB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MM_BIN="$TEST_LIB_ROOT/MapMatchSvr/bin/MapMatchSvr"
SIM_BIN="$TEST_LIB_ROOT/Simulator/bin/RawGpsSimSvr"
MM_STOP_WAIT=35   # config.ini shutdown_wait=30000ms + 여유
SIM_STOP_WAIT=10

engine_name() {
	basename "$1"
}

engine_pidfile() {
	local dir
	dir="$(dirname "$1")"
	echo "$dir/$(engine_name "$1").pid"
}

# 실행 중인 PID 목록 (pid 파일 + /proc/exe 검증)
engine_pids() {
	local bin="$1"
	local name pid pidfile exe
	name="$(engine_name "$bin")"
	pidfile="$(engine_pidfile "$bin")"

	if [ -f "$pidfile" ]; then
		pid="$(tr -d ' \n' < "$pidfile")"
		if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
			exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
			if [ "$exe" = "$bin" ]; then
				echo "$pid"
				return 0
			fi
		fi
		rm -f "$pidfile"
	fi

	for pid in $(pgrep -x "$name" 2>/dev/null); do
		exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
		if [ "$exe" = "$bin" ]; then
			echo "$pid"
		fi
	done
}

engine_running() {
	local pids
	pids="$(engine_pids "$1")"
	[ -n "$pids" ]
}

engine_ps_line() {
	local bin="$1"
	local pids pid
	pids="$(engine_pids "$bin")"
	if [ -z "$pids" ]; then
		echo "$(engine_name "$bin"): not running"
		return 1
	fi
	for pid in $pids; do
		ps -fp "$pid" 2>/dev/null || ps -ef | awk -v p="$pid" '$2==p'
	done
}

engine_stop() {
	local bin="$1"
	local wait_sec="${2:-10}"
	local name pids pid pidfile
	name="$(engine_name "$bin")"
	pidfile="$(engine_pidfile "$bin")"
	pids="$(engine_pids "$bin")"

	if [ -z "$pids" ]; then
		echo "$name not running."
		rm -f "$pidfile"
		return 0
	fi

	echo "Stopping $name (pid: $pids)..."
	kill -TERM $pids 2>/dev/null || true

	local i
	for ((i = 1; i <= wait_sec; i++)); do
		pids="$(engine_pids "$bin")"
		[ -z "$pids" ] && break
		sleep 1
	done

	pids="$(engine_pids "$bin")"
	if [ -n "$pids" ]; then
		echo "$name graceful stop timeout — SIGKILL"
		kill -KILL $pids 2>/dev/null || true
		sleep 1
	fi

	rm -f "$pidfile"
	pids="$(engine_pids "$bin")"
	if [ -n "$pids" ]; then
		echo "오류: $name 종료 실패 (pid: $pids)"
		return 1
	fi
	echo "$name stopped."
	return 0
}

engine_start() {
	local bin="$1"
	local dir pidfile name
	dir="$(dirname "$bin")"
	name="$(engine_name "$bin")"
	pidfile="$(engine_pidfile "$bin")"

	if engine_running "$bin"; then
		echo "기존 $name 종료 중..."
		if [ "$name" = "MapMatchSvr" ]; then
			engine_stop "$bin" "$MM_STOP_WAIT"
		else
			engine_stop "$bin" "$SIM_STOP_WAIT"
		fi
	fi

	cd "$dir" || return 1
	nohup "$bin" >/dev/null 2>&1 &
	echo "$!" > "$pidfile"
	sleep 1

	if ! engine_running "$bin"; then
		echo "오류: $name 기동 실패"
		rm -f "$pidfile"
		return 1
	fi
	echo "$name started (pid=$(engine_pids "$bin"))"
	return 0
}

test_require_bins() {
	[ -x "$MM_BIN" ] || { echo "오류: $MM_BIN 없음 — MapMatchSvr/src 에서 make"; return 1; }
	[ -x "$SIM_BIN" ] || { echo "오류: $SIM_BIN 없음 — Simulator/src 에서 make install"; return 1; }
	[ -f "$TEST_LIB_ROOT/MapMatchSvr/bin/link.psf" ] || echo "경고: link.psf 없음 — 맵매칭 실패 가능"
	return 0
}

test_ps_all() {
	echo "--- MapMatchSvr ---"
	engine_ps_line "$MM_BIN" || true
	echo "--- RawGpsSimSvr ---"
	engine_ps_line "$SIM_BIN" || true
}

test_stop_all() {
	echo "=== RUC 테스트 종료 ==="
	echo "[1/2] RawGpsSimSvr"
	engine_stop "$SIM_BIN" "$SIM_STOP_WAIT" || true
	echo "[2/2] MapMatchSvr"
	engine_stop "$MM_BIN" "$MM_STOP_WAIT" || true
	echo "=== 종료 완료 ==="
	test_ps_all
}

test_start_all() {
	test_require_bins || return 1
	echo "=== RUC 테스트 시작 ==="
	echo "[1/2] MapMatchSvr"
	engine_start "$MM_BIN" || return 1
	echo "[2/2] RawGpsSimSvr"
	engine_start "$SIM_BIN" || return 1
	echo "=== 기동 완료 ==="
	test_ps_all
}
