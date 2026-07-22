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
#define CFG_DEF_CONN_RETRY_MAX		3									// [database] conn_retry_max (단위: 최대 재시도) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_CONN_RETRY_WAIT		100									// [database] conn_retry_wait (단위: ms) (2026-07-11 최정우 주석 추가)
// ── [feeder] ─────────────────────────────────────────────────────────────────────
#define CFG_DEF_LIMIT				500									// [feeder]	limit (단위: 건수) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_FETCH_INTVL			500									// [feeder]	fetch_interval (단위: ms) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_PAUSE_CNT			400									// [feeder]	queue_pause_count (단위: 건수) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_MAX_CNT			800									// [feeder]	queue_max_count (단위: 건수) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_BUSY_MIN			2000								// [feeder]	queue_busy_min (단위: ms) (2026-07-11 최정우 주석 추가)
#define CFG_DEF_Q_BUSY_MAX			10000								// [feeder]	queue_busy_max (단위: ms) (2026-07-11 최정우 주석 추가)
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

#endif //__CONFIG_DEFAULTS_H__

