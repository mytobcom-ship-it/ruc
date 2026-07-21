/**
 * @file AppMain.cpp
 * @brief main 함수 소스 파일
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "Config.h"
#include "ConfigDefaults.h"
#include "log4z.h"
#include "IniReader.h"
#include "Util.h"
#include "Server.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @brief 초기화 함수
 * @param[in] config_file 환경설정 파일 경로
 * @param[in,out] pstConfig 프로세스 구동 환경 설정 값
 * @return true(성공), false(실패)
*/
bool Initialize(string config_file, PCONFIG pstConfig)
{
	if (access(config_file.c_str(), F_OK) != 0)
	{
		perror("config.ini file not found!\n");
		return false;
	}

	CIniReader cIniReader(config_file.c_str());
	if (!cIniReader.Open())
	{
		perror("config file is not found!\n");
		return false;
	}

	// [log] (2026-07-11 최정우 주석 추가)
	// [log] path (단위: 경로) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("log", "path", CFG_DEF_PATH, pstConfig->strLogPath);
	if (pstConfig->strLogPath.empty())
	{
		perror("log path is empty!\n");
		return false;
	}

	// [log] level (단위: 레벨) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "level", CFG_DEF_LEVEL, pstConfig->nLogLevel);
	switch (pstConfig->nLogLevel)
	{
	case 0: pstConfig->nLogLevel = LOG_LEVEL_TRACE; break;
	case 1: pstConfig->nLogLevel = LOG_LEVEL_DEBUG; break;
	case 2: pstConfig->nLogLevel = LOG_LEVEL_INFO; break;
	case 3: pstConfig->nLogLevel = LOG_LEVEL_WARN; break;
	case 4: pstConfig->nLogLevel = LOG_LEVEL_ERROR; break;
	default: pstConfig->nLogLevel = LOG_LEVEL_INFO; break;
	}

	// [log] runtime (단위: 시) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "runtime", CFG_DEF_RUNTIME, pstConfig->nLogKeepRunTime);
	if (pstConfig->nLogKeepRunTime > 23)
	{
		perror("log keep runtime is invalid!\n");
		return false;
	}
	if (pstConfig->nLogKeepRunTime < 0)
		pstConfig->nLogKeepRunTime = UNUSE_LOG_KEEP;

	// [log] keepday (단위: 일) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "keepday", CFG_DEF_KEEPDAY, pstConfig->nLogKeepDay);
	if (pstConfig->nLogKeepRunTime > UNUSE_LOG_KEEP && pstConfig->nLogKeepDay <= 0)
	{
		perror("log keep day is invalid!\n");
		return false;
	}

	// [database] (2026-07-11 최정우 주석 추가)
	// [database] host (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "host", "", pstConfig->strDBHost);
	if (pstConfig->strDBHost.empty())
	{
		perror("db host is empty!\n");
		return false;
	}

	// [database] port (단위: 포트) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "port", CFG_DEF_PORT, pstConfig->nDBPort);
	if ((pstConfig->nDBPort <= 0) || (pstConfig->nDBPort > 65535))
	{
		perror("db port is invalid!\n");
		return false;
	}

	// [database] name (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "name", "", pstConfig->strDBName);
	if (pstConfig->strDBName.empty())
	{
		perror("db name is empty!\n");
		return false;
	}

	// [database] userid (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "userid", "", pstConfig->strDBUserID);
	if (pstConfig->strDBUserID.empty())
	{
		perror("db user id is empty!\n");
		return false;
	}

	// [database] password (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "password", "", pstConfig->strDBPasswd);
	if (pstConfig->strDBPasswd.empty())
	{
		perror("db user password is empty!\n");
		return false;
	}

	// [database] minconnect (단위: 최소 연결) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "minconnect", CFG_DEF_MINCONNECT, pstConfig->nDBMinConnect);
	// [database] maxconnect (0=자동) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "maxconnect", CFG_DEF_MAXCONNECT, pstConfig->nDBMaxConnect);
	// [database] conn_retry_max (단위: 최대 재시도) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "conn_retry_max", CFG_DEF_CONN_RETRY_MAX, pstConfig->nConnRetryMax);
	// [database] conn_retry_wait (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "conn_retry_wait", CFG_DEF_CONN_RETRY_WAIT, pstConfig->nConnRetryWait);
	if (pstConfig->nConnRetryMax < 1)
		pstConfig->nConnRetryMax = CFG_DEF_CONN_RETRY_MAX;
	if (pstConfig->nConnRetryWait < 0)
		pstConfig->nConnRetryWait = 0;

	// [query] (2026-07-11 최정우 주석 추가)
	// [query] file (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("query", "file", "", pstConfig->strSQLFile);
	if (pstConfig->strSQLFile.empty())
	{
		perror("sql file is empty!\n");
		return false;
	}

	// [sql] (2026-07-11 최정우 주석 추가)
	// [sql] rawlog_recover (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_recover", "", pstConfig->strRawLogRecoverSession);
	if (pstConfig->strRawLogRecoverSession.empty())
	{
		perror("gps data recover sql session is empty!\n");
		return false;
	}

	// [sql] rawlog_select (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_select", "", pstConfig->strRawLogSelectSession);
	if (pstConfig->strRawLogSelectSession.empty())
	{
		perror("gps data select sql session is empty!\n");
		return false;
	}

	// [sql] rawlog_update (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_update", "", pstConfig->strRawLogUpdateSession);
	if (pstConfig->strRawLogUpdateSession.empty())
	{
		perror("gps data update sql session is empty!\n");
		return false;
	}

	// [sql] charge_insert (선택) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "charge_insert", "", pstConfig->strChargeInsertSession);

	// [feeder] (2026-07-11 최정우 주석 추가)
	// [feeder] limit (단위: 건수) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "limit", CFG_DEF_LIMIT, pstConfig->nFetchLimit);
	// [feeder] fetch_interval (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "fetch_interval", CFG_DEF_FETCH_INTVL, pstConfig->nFetchInterval);
	// [feeder] queue_pause_count (단위: 건수) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "queue_pause_count", CFG_DEF_Q_PAUSE_CNT, pstConfig->nQueuePauseCount);
	// [feeder] queue_max_count (단위: 건수) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "queue_max_count", CFG_DEF_Q_MAX_CNT, pstConfig->nQueueMaxCount);
	// [feeder] queue_busy_min (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "queue_busy_min", CFG_DEF_Q_BUSY_MIN, pstConfig->nQueueBusyMin);
	// [feeder] queue_busy_max (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "queue_busy_max", CFG_DEF_Q_BUSY_MAX, pstConfig->nQueueBusyMax);
	if (pstConfig->nFetchLimit <= 0)
		pstConfig->nFetchLimit = CFG_DEF_LIMIT;
	if (pstConfig->nFetchInterval < 0)
		pstConfig->nFetchInterval = CFG_DEF_FETCH_INTVL;
	if (pstConfig->nQueuePauseCount <= 0)
		pstConfig->nQueuePauseCount = CFG_DEF_Q_PAUSE_CNT;
	if (pstConfig->nQueueMaxCount < pstConfig->nQueuePauseCount)
		pstConfig->nQueueMaxCount = pstConfig->nQueuePauseCount;
	if (pstConfig->nQueueBusyMin < pstConfig->nFetchInterval)
		pstConfig->nQueueBusyMin = pstConfig->nFetchInterval;
	if (pstConfig->nQueueBusyMax < pstConfig->nQueueBusyMin)
		pstConfig->nQueueBusyMax = pstConfig->nQueueBusyMin;

	// [worker] (2026-07-11 최정우 주석 추가)
	// [worker] ttl_sec (단위: sec) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("worker", "ttl_sec", CFG_DEF_TTL, pstConfig->nTtlSec);
	// [worker] shutdown_wait (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("worker", "shutdown_wait", CFG_DEF_SHUTDOWN_WAIT, pstConfig->nShutdownWait);
	// [worker] retry_max (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("worker", "retry_max", CFG_DEF_RETRY_MAX, pstConfig->nRetryMax);
	if (pstConfig->nRetryMax < 0)
		pstConfig->nRetryMax = 0;
	if (pstConfig->nTtlSec < 0)
		pstConfig->nTtlSec = 0;
	if (pstConfig->nShutdownWait < 0)
		pstConfig->nShutdownWait = 0;

	// [threads] (2026-07-11 최정우 주석 추가)
	// [threads] count (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("threads", "count", CFG_DEF_COUNT, pstConfig->nThreads);
	if (pstConfig->nThreads <= 0)
	{
		perror("thread count is invalid!\n");
		return false;
	}

	// maxconnect 자동 보정 — threads.count 참조 (2026-07-11 최정우 주석 추가)
	if (pstConfig->nDBMaxConnect <= 0)
		pstConfig->nDBMaxConnect = pstConfig->nThreads + 2;
	if (pstConfig->nDBMaxConnect < (pstConfig->nThreads + 1))
		pstConfig->nDBMaxConnect = pstConfig->nThreads + 1;
	if (pstConfig->nDBMinConnect < 1)
		pstConfig->nDBMinConnect = 1;
	if (pstConfig->nDBMinConnect > pstConfig->nDBMaxConnect)
		pstConfig->nDBMinConnect = pstConfig->nDBMaxConnect;

	// [data] (2026-07-11 최정우 주석 추가)
	// [data] file (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("data", "file", "", pstConfig->strDataFile);
	if (pstConfig->strDataFile.empty())
	{
		perror("data binary file is empty!\n");
		return false;
	}

	// [mapmatch] (2026-07-11 최정우 주석 추가)
	// [mapmatch] geodetic (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "geodetic", CFG_DEF_GEODETIC, pstConfig->nGeodetic);
	if ((pstConfig->nGeodetic <= 0) || (pstConfig->nGeodetic > 4))
		pstConfig->nGeodetic = CFG_DEF_GEODETIC;

	// [mapmatch] radius (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius", CFG_DEF_RADIUS, pstConfig->nRadius);
	if ((pstConfig->nRadius < 0) || (pstConfig->nRadius > 250))
		pstConfig->nRadius = CFG_DEF_RADIUS;

	// [mapmatch] radius_scale (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileDouble("mapmatch", "radius_scale", CFG_DEF_RADIUS_SCALE, pstConfig->dfRadiusScale);
	if (pstConfig->dfRadiusScale <= 0.0)
		pstConfig->dfRadiusScale = CFG_DEF_RADIUS_SCALE;

	// [mapmatch] radius_min (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius_min", CFG_DEF_RADIUS_MIN, pstConfig->nRadiusMin);
	// [mapmatch] radius_max (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius_max", CFG_DEF_RADIUS, pstConfig->nRadiusMax);
	if (pstConfig->nRadiusMin <= 0)
		pstConfig->nRadiusMin = CFG_DEF_RADIUS_MIN;
	if (pstConfig->nRadiusMax < pstConfig->nRadiusMin)
		pstConfig->nRadiusMax = pstConfig->nRadius;

	// [mapmatch] radius_skip (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius_skip", CFG_DEF_RADIUS_SKIP, pstConfig->nRadiusSkip);
	if (pstConfig->nRadiusSkip < 0)
		pstConfig->nRadiusSkip = CFG_DEF_RADIUS_SKIP;

	// [mapmatch] alt_gap (단위: m) (2026-07-21 최정우 수정 — altitude_gap 이름 변경)
	cIniReader.GetProfileInt("mapmatch", "alt_gap", CFG_DEF_ALT_GAP, pstConfig->nAltGap);
	// [mapmatch] alt_penalty (양수=페널티·음수=보너스) (2026-07-21 최정우 수정 — altitude_bonus/altitude_penalty 통합)
	cIniReader.GetProfileInt("mapmatch", "alt_penalty", CFG_DEF_ALT_PENALTY, pstConfig->nAltPenalty);

	// [mapmatch] alt_weight (2026-07-21 최정우 수정 — altitude_weight 이름 변경)
	cIniReader.GetProfileDouble("mapmatch", "alt_weight", CFG_DEF_ALT_WEIGHT, pstConfig->dfAltWeight);
	if (pstConfig->dfAltWeight < 0.0)
		pstConfig->dfAltWeight = CFG_DEF_ALT_WEIGHT;

	// [mapmatch] alt_slope (2026-07-21 최정우 수정 — altitude_slope 이름 변경)
	cIniReader.GetProfileDouble("mapmatch", "alt_slope", CFG_DEF_ALT_SLOPE, pstConfig->dfAltSlope);
	if (pstConfig->dfAltSlope < 0.0)
		pstConfig->dfAltSlope = CFG_DEF_ALT_SLOPE;

	// [mapmatch] reverse_weight — 직전 매칭 위치보다 역행하는 후보 1m당 비용 가중 (2026-07-20 최정우 추가)
	cIniReader.GetProfileDouble("mapmatch", "reverse_weight", CFG_DEF_REVERSE_WEIGHT, pstConfig->dfReverseWeight);
	if (pstConfig->dfReverseWeight < 0.0)
		pstConfig->dfReverseWeight = CFG_DEF_REVERSE_WEIGHT;

	// [mapmatch] reverse_speed/reverse_margin — 저속 역행 데드존 (2026-07-20 최정우 추가)
	cIniReader.GetProfileDouble("mapmatch", "reverse_speed", CFG_DEF_REVERSE_SPEED, pstConfig->dfReverseSpeed);
	if (pstConfig->dfReverseSpeed < 0.0)
		pstConfig->dfReverseSpeed = CFG_DEF_REVERSE_SPEED;
	cIniReader.GetProfileDouble("mapmatch", "reverse_margin", CFG_DEF_REVERSE_MARGIN, pstConfig->dfReverseMargin);
	if (pstConfig->dfReverseMargin < 0.0)
		pstConfig->dfReverseMargin = CFG_DEF_REVERSE_MARGIN;
	// [mapmatch] reverse_confirm — 연속 역행 확정 포인트 수. 미만이면 노이즈로 보고 SKIP·앵커 고정 (2026-07-21 최정우 추가)
	cIniReader.GetProfileInt("mapmatch", "reverse_confirm", CFG_DEF_REVERSE_CONFIRM, pstConfig->nReverseConfirm);
	if (pstConfig->nReverseConfirm <= 0)
		pstConfig->nReverseConfirm = CFG_DEF_REVERSE_CONFIRM;

	// [mapmatch] speed_factor/margin — 이동거리 환산속도와 SPEED_KMH 정합성 SKIP 판정 (2026-07-20 최정우 추가)
	cIniReader.GetProfileDouble("mapmatch", "speed_factor", CFG_DEF_SPEED_FACTOR, pstConfig->dfSpeedFactor);
	if (pstConfig->dfSpeedFactor < 0.0)
		pstConfig->dfSpeedFactor = CFG_DEF_SPEED_FACTOR;
	cIniReader.GetProfileInt("mapmatch", "speed_margin", CFG_DEF_SPEED_MARGIN, pstConfig->nSpeedMargin);
	if (pstConfig->nSpeedMargin < 0)
		pstConfig->nSpeedMargin = CFG_DEF_SPEED_MARGIN;

	if (pstConfig->nAltGap < 0)
		pstConfig->nAltGap = 0;
	// nAltPenalty 는 부호 자체가 의미(양수=페널티·음수=보너스)이므로 하한 clamp 없음 (2026-07-21 최정우 수정)

	// [mapmatch] maxstep (필수 >0) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "maxstep", 0, pstConfig->nMaxStep);
	if (pstConfig->nMaxStep <= 0)
	{
		perror("map match is max step is invalid!\n");
		return false;
	}

	// [mapmatch] distance (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "distance", CFG_DEF_DISTANCE, pstConfig->nDistance);
	if (pstConfig->nDistance <= 0)
		pstConfig->nDistance = CFG_DEF_DISTANCE;

	// [mapmatch] timeout (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "timeout", CFG_DEF_TIMEOUT, pstConfig->nMatchTimeout);
	if (pstConfig->nMatchTimeout < 0)
		pstConfig->nMatchTimeout = 0;

	return true;
}

/**
 * @brief main 함수
 * @return -1, 0
*/
int main()
{
	CUtil cUtil;
	CONFIG stConfig;
	string config_file = "./config.ini";

	// 환경설정 파일 읽기
	if (!Initialize(config_file, &stConfig))
		exit(0);

	// log 경로
	if (stConfig.strLogPath.empty())
	{
		perror("log path is empty!\n");
		exit(0);
	}

	// 데이터 바이너리 절대 경로 (실행 디렉터리 기준)
	char szPath[MAX_PATH];
	memset(reinterpret_cast<void *>(szPath), 0, MAX_PATH);
	// 실행 디렉터리 절대 경로 획득 (2026-07-08 최정우 주석 추가)
	if (getcwd(szPath, MAX_PATH) == nullptr)
	{
		perror("directory is not found!\n");
		exit(0);
	}
	if (stConfig.strDataFile.empty() || stConfig.strDataFile[0] == '/')
		; // already absolute or empty handled above
	else
		stConfig.strDataFile = string(szPath) + "/" + stConfig.strDataFile;
	
	ILog4zManager::getRef().setLoggerPath(LOG4Z_MAIN_LOGGER_ID, stConfig.strLogPath.c_str());
	ILog4zManager::getRef().setLoggerLevel(LOG4Z_MAIN_LOGGER_ID, stConfig.nLogLevel);
	ILog4zManager::getRef().setLoggerOutFile(LOG4Z_MAIN_LOGGER_ID, true);
	// log4z 로거 기동 (2026-07-08 최정우 주석 추가)
	if (!ILog4zManager::getRef().start())
	{
		perror("log open fail!\n");
		exit(0);
	}

	CServer *pcServer = new (std::nothrow)CServer;
	if (pcServer == nullptr)
	{
		LOGFMTE("server is null");
		exit(0);
	}

	try
	{
		try
		{
			if (pcServer->Initialize(stConfig))
			{
				pcServer->start();
				pcServer->join();
			}
		}
		catch (exception& e)
		{
			pcServer->Uninitialize();
			LOGFMTE("error=[%s]", e.what());
		}
	}
	catch (IllegalThreadStateException& e)
	{
		pcServer->Uninitialize();
		LOGFMTE("error=[%s]", e.what());
	}

	pcServer->Uninitialize();
	delete pcServer;
	pcServer = nullptr;
	// log4z 로거 종료 (2026-07-08 최정우 주석 추가)
	ILog4zManager::getRef().stop();

	return 0;
}
