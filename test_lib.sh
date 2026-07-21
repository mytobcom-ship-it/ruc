#!/bin/bash
# RUC 테스트 엔진 공통 제어 (MapMatchSvr / RawGpsSimSvr / 웹 뷰어)
# test_svr.sh / test_kill.sh / test_ps.sh / bin/run_svr.sh 에서 source

TEST_LIB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MM_BIN="$TEST_LIB_ROOT/MapMatchSvr/bin/MapMatchSvr"
SIM_BIN="$TEST_LIB_ROOT/Simulator/bin/RawGpsSimSvr"
SIM_CONFIG="$TEST_LIB_ROOT/Simulator/bin/config.ini"
WEB_DIR="$TEST_LIB_ROOT/web"
WEB_RUN="$WEB_DIR/run.sh"
WEB_PIDFILE="$WEB_DIR/web_viewer.pid"
WEB_LOG="$WEB_DIR/web_launcher.log"
WEB_PORT=8088
MM_STOP_WAIT=35   # CFG_DEF_SHUTDOWN_WAIT(단위: ms, 30000) + 여유 (2026-07-11 최정우 수정)
SIM_STOP_WAIT=10
WEB_STOP_WAIT=5
MM_START_WAIT=30  # link.psf 로딩 대기
SIM_START_WAIT=8
WEB_START_WAIT=10

engine_name() {
	basename "$1"
}

engine_dir() {
	dirname "$1"
}

engine_pidfile() {
	echo "$(engine_dir "$1")/$(engine_name "$1").pid"
}

engine_launcher_log() {
	echo "$(engine_dir "$1")/$(engine_name "$1")_launcher.log"
}

# PID가 대상 바이너리인지 검증 (/proc/exe, 실패 시 cmdline+cwd)
engine_pid_valid() {
	local bin="$1"
	local pid="$2"
	local exe cwd
	[ -n "$pid" ] || return 1
	kill -0 "$pid" 2>/dev/null || return 1
	exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
	if [ "$exe" = "$bin" ]; then
		return 0
	fi
	# exe 링크가 순간적으로 비는 경우 보조 검사
	if grep -aqF "$(basename "$bin")" "/proc/$pid/cmdline" 2>/dev/null; then
		cwd="$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)"
		if [ "$cwd" = "$(engine_dir "$bin")" ]; then
			return 0
		fi
	fi
	return 1
}

# 실행 중인 PID 전부 수집 (pid 파일 + pgrep, 중복 포함)
engine_pids() {
	local bin="$1"
	local name pid pidfile found="" p
	name="$(engine_name "$bin")"
	pidfile="$(engine_pidfile "$bin")"

	if [ -f "$pidfile" ]; then
		pid="$(tr -d ' \n' < "$pidfile")"
		if engine_pid_valid "$bin" "$pid"; then
			found="$pid"
		else
			rm -f "$pidfile"
		fi
	fi

	for p in $(pgrep -x "$name" 2>/dev/null); do
		if engine_pid_valid "$bin" "$p"; then
			case " $found " in
				*" $p "*) ;;
				*) found="${found:+$found }$p" ;;
			esac
		fi
	done

	echo "$found"
}

engine_running() {
	[ -n "$(engine_pids "$1")" ]
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

engine_wait_running() {
	local bin="$1"
	local max_wait="$2"
	local i
	for ((i = 1; i <= max_wait; i++)); do
		if engine_running "$bin"; then
			return 0
		fi
		sleep 1
	done
	return 1
}

engine_show_launcher_tail() {
	local bin="$1"
	local log
	log="$(engine_launcher_log "$bin")"
	if [ -f "$log" ]; then
		echo "--- $(basename "$log") (last 8 lines) ---"
		tail -n 8 "$log" 2>/dev/null || true
	fi
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
	for pid in $pids; do
		kill -TERM "$pid" 2>/dev/null || true
	done

	local i
	for ((i = 1; i <= wait_sec; i++)); do
		pids="$(engine_pids "$bin")"
		[ -z "$pids" ] && break
		sleep 1
	done

	pids="$(engine_pids "$bin")"
	if [ -n "$pids" ]; then
		echo "$name graceful stop timeout — SIGKILL (pid: $pids)"
		for pid in $pids; do
			kill -KILL "$pid" 2>/dev/null || true
		done
		sleep 1
	fi

	rm -f "$pidfile"
	pids="$(engine_pids "$bin")"
	if [ -n "$pids" ]; then
		echo "오류: $name 종료 실패 (잔존 pid: $pids)"
		echo "  → 포그라운드 실행 중이면 해당 터미널에서 Ctrl+C 후 다시 시도"
		return 1
	fi
	echo "$name stopped."
	return 0
}

engine_start() {
	local bin="$1"
	local dir pidfile name log start_wait
	dir="$(engine_dir "$bin")"
	name="$(engine_name "$bin")"
	pidfile="$(engine_pidfile "$bin")"
	log="$(engine_launcher_log "$bin")"

	if [ "$name" = "MapMatchSvr" ]; then
		start_wait="$MM_START_WAIT"
	else
		start_wait="$SIM_START_WAIT"
	fi

	if engine_running "$bin"; then
		echo "기존 $name 종료 중..."
		if [ "$name" = "MapMatchSvr" ]; then
			engine_stop "$bin" "$MM_STOP_WAIT" || return 1
		else
			engine_stop "$bin" "$SIM_STOP_WAIT" || return 1
		fi
	fi

	cd "$dir" || return 1
	echo "==== $(date '+%F %T') start $name ====" >>"$log"
	# FD 상속 방지: 독립 세션에서 기동 (잠금 fd 등이 엔진에 물리지 않도록)
	nohup setsid "$bin" >>"$log" 2>&1 </dev/null &
	echo "$!" > "$pidfile"

	if ! engine_wait_running "$bin" "$start_wait"; then
		rm -f "$pidfile"
		echo "오류: $name 기동 실패 (${start_wait}s 내 프로세스 없음)"
		engine_show_launcher_tail "$bin"
		return 1
	fi

	echo "$name started (pid=$(engine_pids "$bin"))"
	return 0
}

web_pid_valid() {
	local pid="$1"
	local cwd
	[ -n "$pid" ] || return 1
	kill -0 "$pid" 2>/dev/null || return 1
	cwd="$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)"
	[ "$cwd" = "$WEB_DIR" ] || return 1
	grep -aqF "server.py" "/proc/$pid/cmdline" 2>/dev/null
}

web_pids() {
	local pid found="" p
	if [ -f "$WEB_PIDFILE" ]; then
		pid="$(tr -d ' \n' < "$WEB_PIDFILE")"
		if web_pid_valid "$pid"; then
			found="$pid"
		else
			rm -f "$WEB_PIDFILE"
		fi
	fi
	for p in $(pgrep -f "python.*server\.py" 2>/dev/null); do
		if web_pid_valid "$p"; then
			case " $found " in
				*" $p "*) ;;
				*) found="${found:+$found }$p" ;;
			esac
		fi
	done
	echo "$found"
}

web_running() {
	[ -n "$(web_pids)" ]
}

web_ps_line() {
	local pids pid
	pids="$(web_pids)"
	if [ -z "$pids" ]; then
		echo "web_viewer: not running"
		return 1
	fi
	for pid in $pids; do
		ps -fp "$pid" 2>/dev/null || ps -ef | awk -v p="$pid" '$2==p'
	done
}

web_wait_running() {
	local i
	for ((i = 1; i <= WEB_START_WAIT; i++)); do
		if web_running; then
			if curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$WEB_PORT/" 2>/dev/null | grep -q 200; then
				return 0
			fi
		fi
		sleep 1
	done
	return 1
}

web_show_launcher_tail() {
	if [ -f "$WEB_LOG" ]; then
		echo "--- web_launcher.log (last 8 lines) ---"
		tail -n 8 "$WEB_LOG" 2>/dev/null || true
	fi
}

web_stop() {
	local wait_sec="${1:-$WEB_STOP_WAIT}"
	local pids pid
	pids="$(web_pids)"
	if [ -z "$pids" ]; then
		echo "web_viewer not running."
		rm -f "$WEB_PIDFILE"
		return 0
	fi
	echo "Stopping web_viewer (pid: $pids)..."
	for pid in $pids; do
		kill -TERM "$pid" 2>/dev/null || true
	done
	local i
	for ((i = 1; i <= wait_sec; i++)); do
		pids="$(web_pids)"
		[ -z "$pids" ] && break
		sleep 1
	done
	pids="$(web_pids)"
	if [ -n "$pids" ]; then
		echo "web_viewer graceful stop timeout — SIGKILL (pid: $pids)"
		for pid in $pids; do
			kill -KILL "$pid" 2>/dev/null || true
		done
		sleep 1
	fi
	rm -f "$WEB_PIDFILE"
	pids="$(web_pids)"
	if [ -n "$pids" ]; then
		echo "오류: web_viewer 종료 실패 (잔존 pid: $pids)"
		return 1
	fi
	echo "web_viewer stopped."
	return 0
}

web_start() {
	if web_running; then
		echo "기존 web_viewer 종료 중..."
		web_stop "$WEB_STOP_WAIT" || return 1
	fi
	[ -x "$WEB_RUN" ] || { echo "오류: $WEB_RUN 없음"; return 1; }
	cd "$WEB_DIR" || return 1
	echo "==== $(date '+%F %T') start web_viewer ====" >>"$WEB_LOG"
	nohup setsid "$WEB_RUN" >>"$WEB_LOG" 2>&1 </dev/null &
	echo "$!" > "$WEB_PIDFILE"
	if ! web_wait_running; then
		rm -f "$WEB_PIDFILE"
		echo "오류: web_viewer 기동 실패 (${WEB_START_WAIT}s 내 HTTP 응답 없음)"
		web_show_launcher_tail
		return 1
	fi
	echo "web_viewer started (pid=$(web_pids), http://127.0.0.1:$WEB_PORT/)"
	return 0
}

# 웹 페이지 "전체기동" 버튼 전용 — 기동 후 1초 뒤 프로세스 존재만 확인(완전 초기화 완료 보장은
#   아님), 미기동이면 동일 방법으로 최대 3회까지 재시도. engine_wait_running(수십 초 대기)보다
#   훨씬 빠른 판정이라, "즉시 크래시했는지"만 잡아내는 용도 — link.psf 로딩처럼 정상적으로 오래
#   걸리는 초기화 중엔 존재 자체는 1초 내 확인되므로 오탐 위험은 낮다 (2026-07-21 최정우 추가)
engine_start_with_retry() {
	local bin="$1"
	local max_attempts=3
	local name
	name="$(engine_name "$bin")"
	local attempt
	for ((attempt = 1; attempt <= max_attempts; attempt++)); do
		echo "--- $name 기동 시도 $attempt/$max_attempts ---"
		if engine_running "$bin"; then
			echo "기존 $name 종료 중..."
			if [ "$name" = "MapMatchSvr" ]; then
				engine_stop "$bin" "$MM_STOP_WAIT" || true
			else
				engine_stop "$bin" "$SIM_STOP_WAIT" || true
			fi
		fi

		local dir pidfile log
		dir="$(engine_dir "$bin")"
		pidfile="$(engine_pidfile "$bin")"
		log="$(engine_launcher_log "$bin")"
		cd "$dir" || return 1
		echo "==== $(date '+%F %T') start $name (attempt $attempt/$max_attempts) ====" >>"$log"
		nohup setsid "$bin" >>"$log" 2>&1 </dev/null &
		echo "$!" > "$pidfile"

		sleep 1
		if engine_running "$bin"; then
			echo "$name 기동 확인됨 (pid=$(engine_pids "$bin"))"
			return 0
		fi

		echo "$name 1초 후 미기동 (시도 $attempt/$max_attempts)"
		rm -f "$pidfile"
	done

	echo "오류: $name ${max_attempts}회 재시도 후에도 기동 실패"
	engine_show_launcher_tail "$bin"
	return 1
}

# 웹 "전체기동" 버튼 전용 — MapMatchSvr → Simulator 순서로 1초 확인+3회 재시도 기동.
#   웹 자신은 이미 이 요청을 처리 중이므로 재기동 대상에서 제외 (2026-07-21 최정우 추가)
test_start_mm_sim_retry() {
	test_require_bins || return 1
	echo "=== MapMatchSvr 기동 (1초 확인, 최대 3회 재시도) ==="
	if ! engine_start_with_retry "$MM_BIN"; then
		echo "FAILED_STAGE=MapMatchSvr"
		return 1
	fi
	echo "=== RawGpsSimSvr 기동 (1초 확인, 최대 3회 재시도) ==="
	if ! engine_start_with_retry "$SIM_BIN"; then
		echo "FAILED_STAGE=RawGpsSimSvr"
		return 1
	fi
	echo "=== 완료 ==="
	test_ps_all
	return 0
}

# MapMatchSvr 단독 재시작 — config.ini 값 변경을 반영. Simulator/web_viewer 는 건드리지
#   않는다(engine_start 가 이미 떠있으면 내부적으로 stop→start 처리) (2026-07-21 최정우 추가)
test_restart_mm() {
	[ -x "$MM_BIN" ] || { echo "오류: $MM_BIN 없음 — MapMatchSvr/src 에서 make install"; return 1; }
	[ -f "$TEST_LIB_ROOT/MapMatchSvr/bin/config.ini" ] || { echo "오류: MapMatchSvr/bin/config.ini 없음"; return 1; }
	echo "=== MapMatchSvr 재시작 (설정 반영) ==="
	engine_start "$MM_BIN"
}

test_require_bins() {
	[ -x "$MM_BIN" ] || { echo "오류: $MM_BIN 없음 — MapMatchSvr/src 에서 make"; return 1; }
	[ -x "$SIM_BIN" ] || { echo "오류: $SIM_BIN 없음 — Simulator/src 에서 make install"; return 1; }
	[ -f "$TEST_LIB_ROOT/MapMatchSvr/bin/config.ini" ] || { echo "오류: MapMatchSvr/bin/config.ini 없음"; return 1; }
	[ -f "$TEST_LIB_ROOT/Simulator/bin/config.ini" ] || { echo "오류: Simulator/bin/config.ini 없음"; return 1; }
	[ -f "$TEST_LIB_ROOT/MapMatchSvr/bin/link.psf" ] || echo "경고: link.psf 없음 — 맵매칭 실패 가능"
	[ -x "$WEB_RUN" ] || { echo "오류: $WEB_RUN 없음 — web/venv 확인"; return 1; }
	[ -f "$WEB_DIR/config.ini" ] || { echo "오류: web/config.ini 없음"; return 1; }
	return 0
}

# PRIM_RAWGPS 기존 데이터 삭제 — 매 테스트 시작 시 이전 실행 데이터와 섞이지 않도록 (2026-07-20 최정우 추가)
#   Simulator/bin/config.ini [database] 접속정보 사용 (MapMatchSvr/web 과 동일 DB)
clear_rawgps_data() {
	local host port dbname user pass
	host="$(sed -n '/^\[database\]/,/^\[/{/^host=/s/^host=//p}' "$SIM_CONFIG")"
	port="$(sed -n '/^\[database\]/,/^\[/{/^port=/s/^port=//p}' "$SIM_CONFIG")"
	dbname="$(sed -n '/^\[database\]/,/^\[/{/^name=/s/^name=//p}' "$SIM_CONFIG")"
	user="$(sed -n '/^\[database\]/,/^\[/{/^userid=/s/^userid=//p}' "$SIM_CONFIG")"
	pass="$(sed -n '/^\[database\]/,/^\[/{/^password=/s/^password=//p}' "$SIM_CONFIG")"

	if [ -z "$host" ] || [ -z "$dbname" ] || [ -z "$user" ]; then
		echo "경고: $SIM_CONFIG 에서 DB 접속정보를 읽지 못해 PRIM_RAWGPS 삭제를 건너뜁니다."
		return 1
	fi

	echo "PRIM_RAWGPS 기존 데이터 삭제 중..."
	if PGPASSWORD="$pass" psql -h "$host" -p "${port:-5432}" -U "$user" -d "$dbname" \
			-c "TRUNCATE TABLE roadnet.prim_rawgps;" >/dev/null 2>&1; then
		echo "PRIM_RAWGPS 삭제 완료."
		return 0
	fi
	echo "경고: PRIM_RAWGPS 삭제 실패 — DB 접속 확인 필요."
	return 1
}

test_ps_all() {
	echo "--- MapMatchSvr ---"
	engine_ps_line "$MM_BIN" || true
	echo "--- RawGpsSimSvr ---"
	engine_ps_line "$SIM_BIN" || true
	echo "--- web_viewer ---"
	web_ps_line || true
}

test_verify_all() {
	local ok=0
	engine_running "$MM_BIN" || ok=1
	engine_running "$SIM_BIN" || ok=1
	web_running || ok=1
	return "$ok"
}

test_stop_all() {
	local rc=0
	echo "=== RUC 테스트 종료 ==="
	echo "[1/3] web_viewer"
	web_stop "$WEB_STOP_WAIT" || rc=1
	echo "[2/3] RawGpsSimSvr"
	engine_stop "$SIM_BIN" "$SIM_STOP_WAIT" || rc=1
	echo "[3/3] MapMatchSvr"
	engine_stop "$MM_BIN" "$MM_STOP_WAIT" || rc=1
	echo "=== 종료 완료 ==="
	test_ps_all
	return "$rc"
}

test_start_all() {
	local rc=0
	test_require_bins || return 1
	echo "=== RUC 테스트 시작 ==="
	# clear_rawgps_data || true	# 기존 데이터 유지 위해 비활성화 (2026-07-21 최정우 수정)
	echo "[1/3] MapMatchSvr"
	engine_start "$MM_BIN" || rc=1
	if [ "$rc" -eq 0 ]; then
		echo "[2/3] RawGpsSimSvr"
		engine_start "$SIM_BIN" || rc=1
	fi
	if [ "$rc" -eq 0 ]; then
		echo "[3/3] web_viewer"
		web_start || rc=1
	fi
	if [ "$rc" -ne 0 ]; then
		echo "=== 기동 실패 — 부분 기동 정리 중 ==="
		test_stop_all || true
		return 1
	fi
	echo "=== 기동 완료 ==="
	test_ps_all
	return 0
}

test_restart_all() {
	local rc=0
	test_stop_all || true
	sleep 2
	test_start_all || rc=1
	return "$rc"
}
