/**
 * @file AppMain.cpp
 * @brief RUC 맵매칭 시뮬레이터 데몬 main
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <string>
#include "TypeDefine.h"
#include "Config.h"
#include "log4z.h"
#include "IniReader.h"
#include "SimServer.h"

using namespace zsummer::log4z;
using namespace std;

// 종료 신호 플래그 (SimServer.h 의 extern 정의)
volatile sig_atomic_t g_nSimStop = 0;

/**
 * @brief 종료 시그널 핸들러
*/
static void OnSignal(int /*nSig*/)
{
	g_nSimStop = 1;
}

/**
 * @brief config.ini 에서 double 값 읽기 (IniReader 는 정수/문자만 지원)
*/
static double GetProfileDouble(CIniReader& cIni, const string& strSec,
	const string& strKey, double dfDefault)
{
	char szDef[64];
	snprintf(szDef, sizeof(szDef), "%.8f", dfDefault);
	string strVal;
	// ini Section/Key 문자열 값 조회 (2026-07-08 최정우 주석 추가)
	cIni.GetProfileStr(strSec, strKey, szDef, strVal);
	if (strVal.empty()) return dfDefault;
	return atof(strVal.c_str());
}

/**
 * @brief 환경설정 로드
*/
static bool LoadConfig(const string& strFile, SIM_CONFIG& stConfig, int& nLogLevel)
{
	// config.ini 파일 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(strFile.c_str(), F_OK) != 0)
	{
		perror("config.ini not found!\n");
		return false;
	}

	CIniReader cIni(strFile.c_str());
	// ini 파일 열기·파싱 (2026-07-08 최정우 주석 추가)
	if (!cIni.Open())
	{
		perror("config.ini open fail!\n");
		return false;
	}

	// [log]
	cIni.GetProfileStr("log", "path", "./log", stConfig.strLogPath);
	cIni.GetProfileInt("log", "level", 2, stConfig.nLogLevel);
	switch (stConfig.nLogLevel)
	{
	case 0: nLogLevel = LOG_LEVEL_TRACE; break;
	case 1: nLogLevel = LOG_LEVEL_DEBUG; break;
	case 2: nLogLevel = LOG_LEVEL_INFO; break;
	case 3: nLogLevel = LOG_LEVEL_WARN; break;
	case 4: nLogLevel = LOG_LEVEL_ERROR; break;
	default: nLogLevel = LOG_LEVEL_INFO; break;
	}

	// [database]
	cIni.GetProfileStr("database", "host", "localhost", stConfig.strDBHost);
	cIni.GetProfileInt("database", "port", 5432, stConfig.nDBPort);
	cIni.GetProfileStr("database", "name", "roadnet", stConfig.strDBName);
	cIni.GetProfileStr("database", "userid", "mytobcom", stConfig.strDBUserID);
	cIni.GetProfileStr("database", "password", "", stConfig.strDBPasswd);
	if (stConfig.strDBPasswd.empty()) { perror("db password empty!\n"); return false; }

	// [query]
	cIni.GetProfileStr("query", "file", "./query.sql", stConfig.strSQLFile);

	// [sim]
	cIni.GetProfileInt("sim", "vehicles", 10, stConfig.nVehicles);
	cIni.GetProfileInt("sim", "flush_sec", 3, stConfig.nFlushSec);
	cIni.GetProfileInt("sim", "report_sec", 30, stConfig.nReportSec);
	stConfig.dfIdleProb = GetProfileDouble(cIni, "sim", "idle_prob", 0.05);
	stConfig.dfOmitAllProb = GetProfileDouble(cIni, "sim", "omit_all_prob", 0.005);
	stConfig.dfOmitPartialProb = GetProfileDouble(cIni, "sim", "omit_partial_prob", 0.08);
	if (stConfig.dfOmitAllProb < 0.0) stConfig.dfOmitAllProb = 0.0;
	if (stConfig.dfOmitAllProb > 1.0) stConfig.dfOmitAllProb = 1.0;
	if (stConfig.dfOmitPartialProb < 0.0) stConfig.dfOmitPartialProb = 0.0;
	if (stConfig.dfOmitPartialProb > 1.0) stConfig.dfOmitPartialProb = 1.0;
	if (stConfig.nVehicles <= 0) stConfig.nVehicles = 1;
	if (stConfig.nFlushSec <= 0) stConfig.nFlushSec = 3;

	// [area]
	stConfig.dfMinLon = GetProfileDouble(cIni, "area", "min_lon", 126.90);
	stConfig.dfMinLat = GetProfileDouble(cIni, "area", "min_lat", 37.48);
	stConfig.dfMaxLon = GetProfileDouble(cIni, "area", "max_lon", 127.10);
	stConfig.dfMaxLat = GetProfileDouble(cIni, "area", "max_lat", 37.62);

	// [경로]
	cIni.GetProfileInt("route", "min_m", 2000, stConfig.nRouteMinM);
	cIni.GetProfileInt("route", "max_links", 20, stConfig.nRouteMaxLinks);
	cIni.GetProfileInt("route", "seed_candidates", 20, stConfig.nSeedCandidates);

	// [noise] — 현실적 스마트폰 GPS 수준(대부분 ≤10m) + 예외 튀는 좌표 주입 (2026-07-16 최정우 수정)
	stConfig.dfNoiseSigmaM = GetProfileDouble(cIni, "noise", "sigma_m", 4.0);
	stConfig.dfNoiseMaxM = GetProfileDouble(cIni, "noise", "max_m", 20.0);
	stConfig.dfOutlierProb = GetProfileDouble(cIni, "noise", "outlier_prob", 0.03);
	stConfig.dfOutlierMinM = GetProfileDouble(cIni, "noise", "outlier_min_m", 25.0);
	stConfig.dfOutlierMaxM = GetProfileDouble(cIni, "noise", "outlier_max_m", 80.0);
	if (stConfig.dfOutlierProb < 0.0) stConfig.dfOutlierProb = 0.0;
	if (stConfig.dfOutlierProb > 1.0) stConfig.dfOutlierProb = 1.0;
	if (stConfig.dfOutlierMaxM < stConfig.dfOutlierMinM) stConfig.dfOutlierMaxM = stConfig.dfOutlierMinM;

	// [speed]
	stConfig.dfSpeedFactorMin = GetProfileDouble(cIni, "speed", "factor_min", 0.5);
	stConfig.dfSpeedFactorMax = GetProfileDouble(cIni, "speed", "factor_max", 1.0);
	stConfig.dfDefaultMaxSpd = GetProfileDouble(cIni, "speed", "default_max_kmh", 50.0);

	return true;
}

int main()
{
	SIM_CONFIG stConfig;
	int nLogLevel = LOG_LEVEL_INFO;
	string strConfigFile = "./config.ini";

	// config.ini 환경설정 로드 (2026-07-08 최정우 주석 추가)
	if (!LoadConfig(strConfigFile, stConfig, nLogLevel))
		exit(0);

	// 로그 초기화
	ILog4zManager::getRef().setLoggerPath(LOG4Z_MAIN_LOGGER_ID, stConfig.strLogPath.c_str());
	ILog4zManager::getRef().setLoggerLevel(LOG4Z_MAIN_LOGGER_ID, nLogLevel);
	ILog4zManager::getRef().setLoggerOutFile(LOG4Z_MAIN_LOGGER_ID, true);
	if (!ILog4zManager::getRef().start())
	{
		perror("log start fail!\n");
		exit(0);
	}

	// 시그널 핸들러
	// SIGINT/SIGTERM 종료 신호 등록 (2026-07-08 최정우 주석 추가)
	signal(SIGINT, OnSignal);
	// SIGTERM 종료 신호 등록 (2026-07-08 최정우 주석 추가)
	signal(SIGTERM, OnSignal);
	signal(SIGPIPE, SIG_IGN);

	CSimServer cServer;
	// 시뮬레이터 초기화 (2026-07-08 최정우 주석 추가)
	if (cServer.Initialize(stConfig))
		// 시뮬레이터 메인 루프 실행 (2026-07-08 최정우 주석 추가)
		cServer.Run();
	else
		LOGFMTE("simulator initialize fail.");

	// 시뮬레이터 리소스 해제 (2026-07-08 최정우 주석 추가)
	cServer.Uninitialize();
	ILog4zManager::getRef().stop();
	return 0;
}
