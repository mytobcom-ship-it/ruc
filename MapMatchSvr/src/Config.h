/**
 * @file Config.h
 * @brief 환경설정 구조체 정의 헤더 파일
*/
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <string>

using namespace std;

/**
 * @struct sConfig
 * @brief 프로세스 구동 환경
*/
typedef struct sConfig
{
	string							strLogPath;							// 로그 경로
	int								nLogLevel;							// 로그 레벨
	int								nLogKeepRunTime;					// 로그 삭제 시간 설정
	int								nLogKeepDay;						// 로그 보관일

	// DB 접속 정보
	string							strDBHost;							// 데이터베이 연결 Host
	int								nDBPort;							// 데이터베이스 연결 Port
	string							strDBName;							// 데이터베이스 이름
	string							strDBUserID;						// 데이터베이스 아이디
	string							strDBPasswd;						// 데이터베이스 비밀번호
	int								nDBMinConnect;						// DB 커넥션 풀 최소 연결 수
	int								nDBMaxConnect;						// DB 커넥션 풀 최대 연결 수
	int								nConnRetryMax;						// conn_retry_max
	int								nConnRetryWait;						// conn_retry_wait

	// SQL 파일명
	string							strSQLFile;							// SQL 파일명

	// SQL 문 (쿼리.sql 세션 키)
	string							strRawLogRecoverSession;			// GPS 좀비 PROCESSING 복구 SQL
	string							strRawLogSelectSession;				// GPS 로그 조회·예약 SQL
	string							strRawLogUpdateSession;				// GPS 로그 갱신 SQL
	string							strChargeInsertSession;				// 과금 INSERT SQL (#10 보류, 비어 있으면 비활성)

	// 피더 (DB poll)
	int								nFetchLimit;						// 1회 조회·예약 최대 건수 (건)
	int								nFetchInterval;						// 큐 여유 시 DB 조회 간격 (ms)
	int								nQueuePauseCount;					// 큐 batch 수, 이상이면 DB 조회 중단 (건)
	int								nQueueMaxCount;						// 큐 더 차면 대기 최대 구간 (건)
	int								nQueueBusyMin;						// 큐 혼잡 시 조회 대기 최소 (ms)
	int								nQueueBusyMax;						// 큐 혼잡 시 조회 대기 최대 (ms)

	// 워커 (세션·종료)
	int								nTtlSec;							// trip_id 세션 유지 시간 (초)
	int								nShutdownWait;						// 종료 시 진행 중(활성) batch 완료 대기 (ms)
	int								nRetryMax;							// release 재시도 상한 (0=무제한)

	// 스레드 풀 개수
	int								nThreads;							// 스레드 풀 개수

	string							strDataFile;						// 데이터 바이너리 파일명 및 경로

	// 연속 맵매칭 정보
	int								nGeodetic;							// GPS 좌표 측지계
	int								nRadius;							// radius
	double							dfRadiusScale;					// radius_scale
	int								nRadiusMin;							// radius_min
	int								nRadiusMax;							// radius_max
	int								nRadiusSkip;						// radius_skip
	int								nMaxStep;							// maxstep
	int								nDistance;							// distance
	int								nMatchTimeout;						// timeout

	int								nAltGap;							// alt_gap
	int								nAltPenalty;						// alt_penalty (양수=페널티·음수=보너스)
	double							dfAltWeight;						// alt_weight
	double							dfAltSlope;							// alt_slope
	int								nReverseConfirm;					// reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
	double							dfSpeedFactor;					// speed_factor (2026-07-20 최정우 추가)
	int								nSpeedMargin;					// speed_margin (km/h) (2026-07-20 최정우 추가)
} CONFIG, *PCONFIG;

#define CONFIG_SIZE												sizeof(CONFIG)

#endif //__CONFIG_H__
