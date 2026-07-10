/**
 * @file AppMain.cpp
 * @brief main 함수 소스 파일
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "TypeDefine.h"
#include "Config.h"
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
 * @return true, false
*/
bool Initialize(string config_file, PCONFIG pstConfig)
{
	// config.ini 파일 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(config_file.c_str(), F_OK) != 0)
	{
		perror("config.ini file not found!\n");
		return false;
	}

	CIniReader cIniReader(config_file.c_str());
	// ini 파일 파싱·섹션/키 맵 로드 (2026-07-08 최정우 주석 추가)
	if (!cIniReader.Open())
	{
		perror("config file is not found!\n");
		return false;
	}

	// log
	// [log] path 문자열 설정값 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("log", "path", "./log", pstConfig->strLogPath);
	if (pstConfig->strLogPath.empty())
	{
		perror("log path is empty!\n");
		return false;
	}

	// Log level setting
	// [log] level 정수 설정값 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "level", 2, pstConfig->nLogLevel);
	switch (pstConfig->nLogLevel)
	{
	case 0:					// TRACE
		pstConfig->nLogLevel = LOG_LEVEL_TRACE;
		break;
	case 1:					// DEBUG
		pstConfig->nLogLevel = LOG_LEVEL_DEBUG;
		break;
	case 2:					// INNFORMATION
		pstConfig->nLogLevel = LOG_LEVEL_INFO;
		break;
	case 3:					// WARNNING
		pstConfig->nLogLevel = LOG_LEVEL_WARN;
		break;
	case 4:					// ERROR
		pstConfig->nLogLevel = LOG_LEVEL_ERROR;
		break;
	default:
		pstConfig->nLogLevel = LOG_LEVEL_INFO;
		break;
	}

	// log keep check time setting
	// [log] runtime(삭제 실행 시) 정수 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "runtime", -1, pstConfig->nLogKeepRunTime);
	if (pstConfig->nLogKeepRunTime > 23)
	{
		perror("log keep runtime is invalid!\n");
		return false;
	}
	if (pstConfig->nLogKeepRunTime < 0) pstConfig->nLogKeepRunTime = UNUSE_LOG_KEEP;

	// log keep day setting
	// [log] keepday(보관 일수) 정수 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "keepday", 7, pstConfig->nLogKeepDay);
	if (pstConfig->nLogKeepRunTime > UNUSE_LOG_KEEP)
	{
		if (pstConfig->nLogKeepDay <= 0)
		{
			perror("log keep day is invalid!\n");
			return false;
		}
	}

	// database
	// [database] host 문자열 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "host", "", pstConfig->strDBHost);
	if (pstConfig->strDBHost.empty())
	{
		perror("db host is empty!\n");
		return false;
	}

	// [database] port 정수 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "port", 5432, pstConfig->nDBPort);	
	if ((pstConfig->nDBPort <= 0) || (pstConfig->nDBPort > 65535))
	{
		perror("db port is invalid!\n");
		return false;
	}

	// [database] name(DB명) 문자열 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "name", "", pstConfig->strDBName);
	if (pstConfig->strDBName.empty())
	{
		perror("db name is empty!\n");
		return false;
	}

	// 데이터베이스 아이디
	// [database] userid 문자열 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "userid", "", pstConfig->strDBUserID);
	if (pstConfig->strDBUserID.empty())
	{
		perror("db user id is empty!\n");
		return false;
	}

	// 데이터베이스 비밀번호
	// [database] password 문자열 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "password", "", pstConfig->strDBPasswd);
	if (pstConfig->strDBPasswd.empty())
	{
		perror("db user password is empty!\n");
		return false;
	}

	// query file
	// [query] file(SQL 파일 경로) 문자열 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("query", "file", "", pstConfig->strSQLFile);
	if (pstConfig->strSQLFile.empty())
	{
		perror("sql file is empty!\n");
		return false;
	}

	// 원시 GPS 로그 복구 SQL (기동 시 1회)
	// [sql] rawlog_recover 세션명 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_recover", "", pstConfig->strRawLogRecoverSession);
	if (pstConfig->strRawLogRecoverSession.empty())
	{
		perror("gps data recover sql session is empty!\n");
		return false;
	}

	// 원시 GPS 로그 조회·예약 SQL
	// [sql] rawlog_select 세션명 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_select", "", pstConfig->strRawLogSelectSession);
	if (pstConfig->strRawLogSelectSession.empty())
	{
		perror("gps data select sql session is empty!\n");
		return false;
	}

	// 원시 GPS 로그 갱신 SQL
	// [sql] rawlog_update 세션명 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_update", "", pstConfig->strRawLogUpdateSession);
	if (pstConfig->strRawLogUpdateSession.empty())
	{
		perror("gps data update sql session is empty!\n");
		return false;
	}

	// 과금 대상 적재 SQL (#10 보류: 테이블 재설계 후 INSERT 배선)
	// [sql] charge_insert 세션명 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "charge_insert", "", pstConfig->strChargeInsertSession);
	if (pstConfig->strChargeInsertSession.empty())
	{
		perror("charge data insert sql session is empty!\n");
		return false;
	}

	// 피더 설정
	// [feeder] limit(1회 fetch 건수) 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "limit", 500, pstConfig->nFetchLimit);
	if (!cIniReader.GetProfileInt("feeder", "fetch_interval", 500, pstConfig->nFetchInterval))
		cIniReader.GetProfileInt("feeder", "poll_interval_ms", 500, pstConfig->nFetchInterval);
	if (!cIniReader.GetProfileInt("feeder", "queue_pause_count", 400, pstConfig->nQueuePauseCount))
		cIniReader.GetProfileInt("feeder", "queue_pause_threshold", 400, pstConfig->nQueuePauseCount);
	if (!cIniReader.GetProfileInt("feeder", "queue_max_count", 800, pstConfig->nQueueMaxCount))
		cIniReader.GetProfileInt("feeder", "queue_max_threshold", 800, pstConfig->nQueueMaxCount);
	if (!cIniReader.GetProfileInt("feeder", "queue_busy_min", 2000, pstConfig->nQueueBusyMin))
		cIniReader.GetProfileInt("feeder", "queue_backpressure_ms", 2000, pstConfig->nQueueBusyMin);
	if (!cIniReader.GetProfileInt("feeder", "queue_busy_max", 10000, pstConfig->nQueueBusyMax))
		cIniReader.GetProfileInt("feeder", "poll_max_interval_ms", 10000, pstConfig->nQueueBusyMax);
	if (pstConfig->nFetchLimit <= 0)
		pstConfig->nFetchLimit = 500;
	if (pstConfig->nFetchInterval < 0)
		pstConfig->nFetchInterval = 500;
	if (pstConfig->nQueuePauseCount <= 0)
		pstConfig->nQueuePauseCount = 400;
	if (pstConfig->nQueueMaxCount < pstConfig->nQueuePauseCount)
		pstConfig->nQueueMaxCount = pstConfig->nQueuePauseCount;
	if (pstConfig->nQueueBusyMin < pstConfig->nFetchInterval)
		pstConfig->nQueueBusyMin = pstConfig->nFetchInterval;
	if (pstConfig->nQueueBusyMax < pstConfig->nQueueBusyMin)
		pstConfig->nQueueBusyMax = pstConfig->nQueueBusyMin;

	// 워커 세션·종료
	if (!cIniReader.GetProfileInt("worker", "ttl_sec", 3600, pstConfig->nTtlSec))
		cIniReader.GetProfileInt("worker", "session_ttl_sec", 3600, pstConfig->nTtlSec);
	if (!cIniReader.GetProfileInt("worker", "shutdown_wait", 30000, pstConfig->nShutdownWait))
		cIniReader.GetProfileInt("worker", "shutdown_drain_ms", 30000, pstConfig->nShutdownWait);
	// release→PENDING 재시도 상한. 초과 시 ERROR(4) 고정. 0=무제한 (#16)
	cIniReader.GetProfileInt("worker", "retry_max", 5, pstConfig->nRetryMax);
	if (pstConfig->nRetryMax < 0)
		pstConfig->nRetryMax = 0;
	if (pstConfig->nTtlSec < 0)
		pstConfig->nTtlSec = 0;
	if (pstConfig->nShutdownWait < 0)
		pstConfig->nShutdownWait = 0;

	// ThreadPool
	// [threads] count(워커 스레드 수) 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("threads", "count", 10, pstConfig->nThreads);
	if (pstConfig->nThreads <= 0)
	{
		perror("thread count is invalid!\n");
		return false;
	}

	// DB connection pool (max 기본: worker + fetcher + 1)
	// [database] minconnect 풀 최소 연결 수 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "minconnect", 3, pstConfig->nDBMinConnect);
	// [database] maxconnect 풀 최대 연결 수 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "maxconnect", 0, pstConfig->nDBMaxConnect);
	if (pstConfig->nDBMaxConnect <= 0)
		pstConfig->nDBMaxConnect = pstConfig->nThreads + 2;

	// worker 전원 DB 사용 + fetcher 1 을 수용 (P2)
	if (pstConfig->nDBMaxConnect < (pstConfig->nThreads + 1))
		pstConfig->nDBMaxConnect = pstConfig->nThreads + 1;

	if (pstConfig->nDBMinConnect < 1)
		pstConfig->nDBMinConnect = 1;

	if (pstConfig->nDBMinConnect > pstConfig->nDBMaxConnect)
		pstConfig->nDBMinConnect = pstConfig->nDBMaxConnect;

	// [database] conn_retry_max — 풀 연결 핸들 확보 재시도 최대 횟수 (회, 2026-07-10 최정우 추가)
	cIniReader.GetProfileInt("database", "conn_retry_max", 3, pstConfig->nConnRetryMax);
	// [database] conn_retry_wait — 재시도 사이 대기 (ms, 2026-07-10 최정우 추가)
	cIniReader.GetProfileInt("database", "conn_retry_wait", 100, pstConfig->nConnRetryWait);
	if (pstConfig->nConnRetryMax < 1)
		pstConfig->nConnRetryMax = 1;
	if (pstConfig->nConnRetryWait < 0)
		pstConfig->nConnRetryWait = 0;

	// 데이터 바이너리 파일명 및 경로
	// [data] file(link.psf 등) 경로 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileStr("data", "file", "", pstConfig->strDataFile);
	if (pstConfig->strDataFile.empty())
	{
		perror("data binary file is empty!\n");
		return false;
	}

	// GPS 좌표 측지계
	// [mapmatch] geodetic 측지계 코드 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "geodetic", 1, pstConfig->nGeodetic);
	if ((pstConfig->nGeodetic <= 0) || (pstConfig->nGeodetic > 4))
		pstConfig->nGeodetic = 1;

	// 맵 매칭 반경 (m) — ACCURACY_M NULL 시 폴백
	// [mapmatch] radius 기본 검색 반경(m) 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius", 50, pstConfig->nRadius);
	if ((pstConfig->nRadius < 0) || (pstConfig->nRadius > 250))
		pstConfig->nRadius = 50;

	// ACCURACY_M 적응형 검색 반경 — clamp(scale×ACCURACY_M, radius_min, radius_max) (2026-07-08 최정우)
	// {
	// 	string strAccuracyK;
	// 	cIniReader.GetProfileStr("mapmatch", "accuracy_k", "2.5", strAccuracyK);
	// 	pstConfig->dfAccuracyK = atof(strAccuracyK.c_str());
	// 	if (pstConfig->dfAccuracyK <= 0.0)
	// 		pstConfig->dfAccuracyK = 2.5;
	// }
	// cIniReader.GetProfileInt("mapmatch", "accuracy_skip", 0, pstConfig->nAccuracySkip);
	// if (pstConfig->nAccuracySkip < 0)
	// 	pstConfig->nAccuracySkip = 0;
	{
		string strRadiusScale;
		if (!cIniReader.GetProfileStr("mapmatch", "radius_scale", "2.5", strRadiusScale))
			cIniReader.GetProfileStr("mapmatch", "accuracy_k", "2.5", strRadiusScale);
		pstConfig->dfRadiusScale = atof(strRadiusScale.c_str());
		if (pstConfig->dfRadiusScale <= 0.0)
			pstConfig->dfRadiusScale = 2.5;
	}
	cIniReader.GetProfileInt("mapmatch", "radius_min", 20, pstConfig->nRadiusMin);
	cIniReader.GetProfileInt("mapmatch", "radius_max", pstConfig->nRadius, pstConfig->nRadiusMax);
	if (pstConfig->nRadiusMin <= 0)
		pstConfig->nRadiusMin = 20;
	if (pstConfig->nRadiusMax < pstConfig->nRadiusMin)
		pstConfig->nRadiusMax = pstConfig->nRadius;
	// if (!cIniReader.GetProfileInt("mapmatch", "radius_skip_m", 0, pstConfig->nRadiusSkipM))
	// 	cIniReader.GetProfileInt("mapmatch", "accuracy_skip", 0, pstConfig->nRadiusSkipM);
	// if (pstConfig->nRadiusSkipM < 0)
	// 	pstConfig->nRadiusSkipM = 0;
	if (!cIniReader.GetProfileInt("mapmatch", "radius_skip", 0, pstConfig->nRadiusSkip))
	{
		if (!cIniReader.GetProfileInt("mapmatch", "radius_skip_m", 0, pstConfig->nRadiusSkip))
			cIniReader.GetProfileInt("mapmatch", "accuracy_skip", 0, pstConfig->nRadiusSkip);
	}
	if (pstConfig->nRadiusSkip < 0)
		pstConfig->nRadiusSkip = 0;

	// ── 연속 맵매칭 고도(ALTITUDE_M) 보조 점수 — config [mapmatch] altitude_* ──
	//   Begin 미적용. 직전·현재 GPS 고도로 Δalt 계산 후 dfCost 소프트 가산.
	//   altitude_weight=0 → 비활성. 상세 공식·예제는 GISUtil::CalcAltRoadPenalty 주석 참고.
	cIniReader.GetProfileInt("mapmatch", "altitude_gap", 8, pstConfig->nAltitudeGap);
	cIniReader.GetProfileInt("mapmatch", "altitude_bonus", 3, pstConfig->nAltitudeBonus);
	cIniReader.GetProfileInt("mapmatch", "altitude_penalty", 10, pstConfig->nAltitudePenalty);
	{
		string strAltitudeWeight;
		cIniReader.GetProfileStr("mapmatch", "altitude_weight", "0.5", strAltitudeWeight);
		pstConfig->dfAltitudeWeight = atof(strAltitudeWeight.c_str());
		if (pstConfig->dfAltitudeWeight < 0.0)
			pstConfig->dfAltitudeWeight = 0.0;
	}
	{
		string strAltitudeSlope;
		cIniReader.GetProfileStr("mapmatch", "altitude_slope", "0.12", strAltitudeSlope);
		pstConfig->dfAltitudeSlope = atof(strAltitudeSlope.c_str());
		if (pstConfig->dfAltitudeSlope < 0.0)
			pstConfig->dfAltitudeSlope = 0.0;
	}
	if (pstConfig->nAltitudeGap < 0)
		pstConfig->nAltitudeGap = 0;
	if (pstConfig->nAltitudeBonus < 0)
		pstConfig->nAltitudeBonus = 0;
	if (pstConfig->nAltitudePenalty < 0)
		pstConfig->nAltitudePenalty = 0;

	// 연속 맵 매칭시 연결 링크 확인 최대 개수
	// [mapmatch] maxstep 연속 링크 탐색 최대 스텝 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "maxstep", 0, pstConfig->nMaxStep);
	if (pstConfig->nMaxStep <= 0)
	{
		perror("map match is max step is invalid!\n");
		return false;
	}

	// (레거시) StartProcess 배치 경로 전용 — 실시간 RawLogWorker 미사용 (#M-4)
	cIniReader.GetProfileInt("mapmatch", "distance", 500, pstConfig->nDistance);
	if (pstConfig->nDistance <= 0)
		pstConfig->nDistance = 500;

	// 1 GPS 맵매칭 처리 임계 (ms). 초과 시 ERROR(4) 격리. 0=비활성
	// [mapmatch] timeout 맵매칭 처리 시간 임계(ms) 조회 (2026-07-08 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "timeout", 200, pstConfig->nMatchTimeout);
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
