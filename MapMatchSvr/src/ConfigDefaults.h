/**
 * @file ConfigDefaults.h
 * @brief config.ini 파라미터 기본값 — AppMain / Server / RawLogFetcher 공용
 *
 * 상수명 = CFG_DEF_<키약어>  (단위는 상수명이 아닌 주석에 표기) (2026-07-11 최정우 수정)
 */
#ifndef __CONFIG_DEFAULTS_H__
#define __CONFIG_DEFAULTS_H__

// ── [log] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_PATH				"./log"								// [log] path (단위: 경로) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_LEVEL				2									// [log] level (단위: 레벨) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RUNTIME				(-1)								// [log] runtime (단위: 초) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_KEEPDAY				7									// [log] keepday (단위: 일) (2026-07-11 최정우 주석 추가)
// ── [database] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_PORT				5432								// [database] port (단위: 포트) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_MINCONNECT			3									// [database] minconnect (단위: 최소 연결) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_MAXCONNECT			0									// [database] maxconnect (0=자동) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_CONN_RETRY_MAX		3									// [database] retrymax (단위: 최대 재시도) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_CONN_RETRY_WAIT		100									// [database] retrywait (단위: ms) (2026-07-11 최정우 주석 추가)
// ── [feeder] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_LIMIT				500									// [feeder]	limit (단위: 건수) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_FETCH_INTVL			500									// [feeder]	fetch_interval (단위: ms) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_PAUSE_CNT			400									// [feeder]	queue_pause (단위: 건수) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_MAX_CNT			800									// [feeder]	queue_max (단위: 건수) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_BUSY_MIN			2000								// [feeder]	queue_busymin (단위: ms) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_BUSY_MAX			10000								// [feeder]	queue_busymax (단위: ms) (2026-07-11 최정우 주석 추가)
// ── [worker] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_TTL					3600								// [worker]	ttl_sec (단위: sec) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_SHUTDOWN_WAIT		30000								// [worker]	shutdown_wait (단위: ms) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RETRY_MAX			5									// [worker]	retry_max (2026-07-11 최정우 주석 추가)
// ── [threads] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_COUNT				10									// [threads]	count (2026-07-11 최정우 주석 추가)
// ── [mapmatch] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_GEODETIC			1									// [mapmatch] geodetic (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RADIUS				50									// [mapmatch] radius (단위: m) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RADIUS_SCALE		2.5									// [mapmatch] radius_scale (2026-07-11 최정우 수정)
#define CFG_DEF_RADIUS_MIN			20									// [mapmatch] radius_min (단위: m) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RADIUS_SKIP			0									// [mapmatch] radius_skip (단위: m) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_ALT_GAP				8									// [mapmatch] alt_gap (단위: m) (2026-07-21 최정우 수정 — altitude_gap 이름 변경)
#define CFG_DEF_ALT_PENALTY			10									// [mapmatch] alt_penalty (양수=페널티·음수=보너스) (2026-07-21 최정우 수정 — altitude_bonus/altitude_penalty 통합)
#define CFG_DEF_ALT_WEIGHT			0.5									// [mapmatch] alt_weight (2026-07-21 최정우 수정 — altitude_weight 이름 변경)
#define CFG_DEF_ALT_SLOPE			0.12								// [mapmatch] alt_slope (2026-07-21 최정우 수정 — altitude_slope 이름 변경)
#define CFG_DEF_REVERSE_CONFIRM	3									// [mapmatch] reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
#define CFG_DEF_SPEED_FACTOR	2.0									// [mapmatch] speed_factor — 이동거리 환산속도/SPEED_KMH 배율 상한 (2026-07-20 최정우 추가)
#define CFG_DEF_SPEED_MARGIN	25									// [mapmatch] speed_margin (단위: km/h) — 노이즈 허용 여유분 (2026-07-20 최정우 추가)
#define CFG_DEF_DISTANCE			500									// [mapmatch] distance (단위: m) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_TIMEOUT				200									// [mapmatch] timeout (단위: ms) (2026-07-11 최정우 주석 추가)
// ── [server] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_RUN_WAIT			1000								// [server]	run_wait (단위: ms) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_MONITOR				30									// [server]	monitor (단위: sec) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RECOVER_MAX			3									// [server]	recover_retry_max (2026-07-11 최정우 주석 추가)
#define CFG_DEF_RECOVER_WAIT		2000								// [server]	recover_retry_wait (단위: ms) (2026-07-11 최정우 주석 추가)
// ── [charge] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_GATE_RELOAD			0									// [charge]	gate_reload (단위: sec, 0=재조회 없음) (2026-08-12 최정우 추가)
#define CFG_DEF_PARK_BUF			15									// [charge]	park_buf (단위: m) — 구역판정 버퍼 상한(ACCURACY_M 캡) (2026-08-13 최정우 추가)
#define CFG_DEF_PARK_ACCMAX			50									// [charge]	park_accmax (단위: m) — 주정차 판정 좌표 정확도 상한, 0=비활성 (2026-08-23 최정우 추가)
#define CFG_DEF_IGNORE_RAWVLD		0									// [mapmatch] ignore_rawvld — 1이면 RAW_VLD 무시하고 전량 맵매칭 시도(검증용) (2026-08-23 최정우 추가)
#define CFG_DEF_PARK_EXITCNT		3									// [charge]	park_exitcnt — 구역 이탈 확정 연속 GPS 건수(디바운스) (2026-08-13 최정우 추가)
#define CFG_DEF_PARK_SPEEDMAX		0									// [charge]	park_speedmax (단위: km/h) — 주정차 판정 속도 상한, 0=비활성 (2026-08-22 최정우 추가)
#define CFG_DEF_PARK_ENTRYCNT		3									// [charge]	park_entrycnt — 주정차 세션 개시 연속 GPS 건수 (2026-08-22 최정우 추가)
#define CFG_DEF_PARK_REGRACE		60									// [charge]	park_regrace (단위: sec) — 재진입 유예시간 (2026-08-14 최정우 추가)
#define CFG_DEF_PARK_TTL			600									// [charge]	park_ttl (단위: sec) — 마지막 신뢰(RAW_VLD=true) 확인 후 좌표 없이 강제 마감까지의 시간 (2026-08-19 최정우 추가, 2026-08-21 최정우 수정 — 실측 raw_vld=false 최장 269초 대비 여유 확보)
#define CFG_DEF_EXEMPT_REGRACE		60									// [charge]	exempt_regrace (단위: sec) — 재진입 유예시간 (2026-08-14 최정우 추가)

#define CFG_DEF_SERVER_ID			"location"							// [server]	id — PROC_SERVERSTATUS.SERVER_ID (2026-08-20 최정우 추가)
#define CFG_DEF_STATUS_INTVL		600									// [server]	status_interval (단위: sec, 0=비활성) — CPU/메모리 하트비트 주기 (2026-08-20 최정우 추가)

#endif //__CONFIG_DEFAULTS_H__

