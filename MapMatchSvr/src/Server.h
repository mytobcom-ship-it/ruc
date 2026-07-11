/**
 * @file Server.h
 * @brief 서버 클래스 헤더 파일
*/
#ifndef __SERVER_H__
#define __SERVER_H__

#include <stdio.h>
#include <string>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <memory>
#include "TypeDefine.h"
#include "Config.h"
#include "ConfigDefaults.h"
#include "log4z.h"
#include "Util.h"
#include "Thread.h"
#include "ThreadPool.h"
#include "Singleton.h"
#include "SingleThread.h"
#include "PostgrePool.h"
#include "SQLAccessor.h"
#include "LoggerManager.h"
#include "DataLoader.h"
#include "ProcessManager.h"
#include "RawLogFetcher.h"
#include "RawLogWorker.h"
#include "Mutex.h"
#include "Condition.h"

using namespace zsummer::log4z;
using namespace std;

// 단위: ms (2026-07-11 최정우 주석 추가)
#define SERVER_RUN_WAIT					CFG_DEF_RUN_WAIT
// 단위: sec (2026-07-11 최정우 주석 추가)
#define SERVER_MONITOR_INTERVAL			CFG_DEF_MONITOR
#define RECOVER_RETRY_MAX				CFG_DEF_RECOVER_MAX
// 단위: ms (2026-07-11 최정우 주석 추가)
#define RECOVER_RETRY_INTERVAL			CFG_DEF_RECOVER_WAIT

/**
 * @class CServer
 * @brief 서버 클래스
*/
class CServer : public CSingleThread
{
	DECLARE_SINGLETON(CServer)
public:
	CServer();
	virtual ~CServer();

	bool Initialize(const CONFIG& stConfig);
	void Uninitialize();
	void ProcessPeriodSec(time_t dtNow);
	void RequestShutdown();
	bool DrainPendingBatchesAndRelease();

	inline const bool IsRun() { return m_bRun; }

private:
	virtual void run();
	void LogMonitorStatus(time_t dtNow);
	void WaitForNextCycle();

private:
	CPostgrePool					*m_pcPostgrePool;					// PostgreSQL DB 커넥션 풀
	CSQLAccessor					*m_pcSQLAccessor;					// SQL 파일 읽기
	CLoggerManager					*m_pcLoggerManager;					// 로그 관리 클래스
	CDataLoader						*m_pcDataLoader;					// 기반 데이터 클래스
	CThreadPool						*m_pcThreadPool;					// 스레드 풀
	CProcessManager					*m_pcProcessManager;				// GPS 정보 맵 매칭 처리 클래스
	CRawLogFetcher					*m_pcRawLogFetcher;					// 원시 GPS DB 폴링 클래스
	CRawLogWorker					*m_pcRawLogWorker;					// 원시 GPS batch 처리 워커

private:
	bool							m_bRun;								// 서버 실행 여부
	bool							m_bUninitialized;					// Uninitialize 중복 실행 가드 (2026-07-10 최정우 추가)
	string							m_strLogPath;						// 로그 경로
	int								m_nLogLevel;						// 로그 레벨
	int								m_nLogKeepRunTime;					// 로그 삭제 시간 설정
	int								m_nLogKeepDay;						// 로그 보관일
	int								m_nWorkerThread;					// 스레드 풀 개수
	pthread_t						m_hTimerThread;						// 로그 관리 Thread 핸들
	string							m_strSQLFile;						// SQL 파일
	string							m_strDBHost;						// DB 연결 Host
	int								m_nDBPort;							// DB 연결 Port
	string							m_strDBName;						// DB 이름
	string							m_strDBUserID;						// DB 아이디
	string							m_strDBPasswd;						// DB 비밀번호
	int								m_nDBMinConnect;					// DB pool 최소 연결 수
	int								m_nDBMaxConnect;					// DB pool 최대 연결 수
	string							m_strRawLogRecoverSQL;				// PROCESSING 복구 SQL
	string							m_strRawLogSelectSQL;				// 조회·예약 SQL (UPDATE RETURNING)
	string							m_strRawLogUpdateSQL;				// 결과 갱신 SQL
	string							m_strChargeInsertSQL;				// #10 보류: 로드만, Worker INSERT 미연동
	int								m_nFetchLimit;						// 1회 조회·예약 최대 건수 (건)
	int								m_nFetchInterval;					// 큐 여유 시 DB 조회 간격 (ms)
	int								m_nQueuePauseCount;					// 큐 batch 수, 이상이면 DB 조회 중단 (건)
	int								m_nQueueMaxCount;					// 큐 더 차면 대기 최대 구간 (건)
	int								m_nQueueBusyMin;					// 큐 혼잡 시 조회 대기 최소 (ms)
	int								m_nQueueBusyMax;					// 큐 혼잡 시 조회 대기 최대 (ms)
	int								m_nTtlSec;							// trip_id 세션 유지 시간 (초)
	int								m_nShutdownWait;					// 종료 시 워커 처리 완료 대기 (ms)
	int								m_nRetryMax;						// release 재시도 상한 (0=무제한)
	int								m_nThreads;							// 스레드 풀 개수
	string							m_strDataFile;						// 데이터 바이너리 파일명 및 경로
	uint8							m_nCoordinateType;					// GPS 좌표 측지계
	sint16							m_nRadius;							// 맵 매칭 유효 거리
	uint16							m_nMaxStep;							// 연속 맵매칭시 연결 링크 확인 최대 개수
	uint32							m_dwMaxDistance;					// 연속 맵 매칭시 좌표간 최대 유효 거리
	int								m_nMatchTimeout;					// 1 GPS 맵매칭 처리 임계 (ms, 0=비활성, #16)
	double							m_dfRadiusScale;					// config radius_scale — 검색반경 = scale × ACCURACY_M (2026-07-08 최정우)
	int								m_nRadiusMin;						// config radius_min — 적응형 검색 반경 하한 (m) (2026-07-08 최정우)
	int								m_nRadiusMax;						// config radius_max — 적응형 검색 반경 상한 (m) (2026-07-08 최정우)
	int								m_nRadiusSkip;						// config radius_skip — ACCURACY_M 초과 시 SKIP (m). 0=비활성 (2026-07-08 최정우)
	int								m_nAltitudeGap;						// config altitude_gap — 직전 매칭 고도와 허용 차이(m)
	int								m_nAltitudeBonus;					// config altitude_bonus — 차이 안·같은 ROAD_TYPE 비용 감산(m)
	int								m_nAltitudePenalty;					// config altitude_penalty — 차이 안·ROAD_TYPE 불일치 추가 비용(m)
	double							m_dfAltitudeWeight;					// config altitude_weight — 차이 초과 시 고도차 가중. 0=비활성
	double							m_dfAltitudeSlope;					// altitude_slope

	CMutex							m_cRunMutex;
	CCondition						m_cRunCondition;					// 종료 시그널
	time_t							m_dtLastMonitorLog;					// 마지막 모니터링 시각
	int								m_nLastQueueCount;					// 이전 큐 적재량
	bool							m_bQueueWarnActive;					// 큐 backlog WARN 상태
	bool							m_bAllBusyWarnActive;				// 워커 전원 사용 중 WARN 상태
};

#endif //__SERVER_H__
