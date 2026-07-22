/**
 * @file SimServer.cpp
 * @brief 시뮬레이터 본체 구현
*/
#include "SimServer.h"
#include "GeoUtil.h"
#include "log4z.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <unistd.h>
#include <libpq-fe.h>

using namespace zsummer::log4z;

CSimServer::CSimServer()
	: m_pcPool(nullptr), m_pcSQL(nullptr), m_pcRoute(nullptr),
	m_ullTotalInsert(0), m_ullTotalFail(0), m_ullTotalGenerated(0)
{
}

CSimServer::~CSimServer()
{
	Uninitialize();
}

bool CSimServer::Initialize(const SIM_CONFIG& stConfig)
{
	m_stConfig = stConfig;

	// DB 커넥션 풀
	m_pcPool = new (std::nothrow) CPostgrePool();
	if (!m_pcPool) { LOGFMTE("postgre pool alloc fail"); return false; }

	int nMax = m_stConfig.nVehicles + 4;
	if (nMax < 5) nMax = 5;
	// PostgreSQL 커넥션 풀 초기화 (2026-07-08 최정우 주석 추가)
	if (!m_pcPool->InitializePool(m_stConfig.strDBUserID, m_stConfig.strDBPasswd,
		m_stConfig.strDBName, m_stConfig.strDBHost, m_stConfig.nDBPort, 3, nMax, 60))
	{
		LOGFMTE("db connection pool init fail! host=[%s] db=[%s] user=[%s]",
			m_stConfig.strDBHost.c_str(), m_stConfig.strDBName.c_str(), m_stConfig.strDBUserID.c_str());
		return false;
	}

	// SQL 파일
	m_pcSQL = new (std::nothrow) CSQLAccessor();
	// SQL 파일 로드·파싱 (2026-07-08 최정우 주석 추가)
	if (!m_pcSQL || !m_pcSQL->Initialize(m_stConfig.strSQLFile))
	{
		LOGFMTE("sql accessor init fail! file=[%s]", m_stConfig.strSQLFile.c_str());
		return false;
	}
	m_strInsertSQL = m_pcSQL->GetSQL("raw_gps_insert");
	if (m_strInsertSQL.empty())
	{
		LOGFMTE("raw_gps_insert sql not found in [%s]", m_stConfig.strSQLFile.c_str());
		return false;
	}

	// 도로망 경로 제공자
	m_pcRoute = new (std::nothrow) CRouteProvider();
	// 도로망 경로 제공자 초기화 (2026-07-08 최정우 주석 추가)
	if (!m_pcRoute || !m_pcRoute->Initialize(m_pcPool, m_pcSQL, m_stConfig))
	{
		LOGFMTE("route provider init fail");
		return false;
	}

	// 차량 생성 (겹치지 않는 임시 인증키). 차량마다 [tick_sec_min,tick_sec_max] 범위에서
	//   자기만의 GPS 생성 주기를 뽑아 고정 사용하고, 첫 tick 까지 남은 시간도 그 범위 내에서
	//   랜덤 산포시켜 차량마다 최초 표본 시점이 조금씩 어긋나게 한다 (2026-07-22 최정우 추가)
	unsigned int nBase = (unsigned int)(time(nullptr) ^ ((unsigned int)getpid() << 16));
	mt19937 rngTick(nBase ^ 0x9E3779B9u);
	uniform_real_distribution<double> distTickSec(m_stConfig.dfTickSecMin, m_stConfig.dfTickSecMax);
	for (int i = 0; i < m_stConfig.nVehicles; ++i)
	{
		char szKey[37];
		snprintf(szKey, sizeof(szKey), "SIM%08X%04d", nBase, i);

		double dfThisTickSec = distTickSec(rngTick);
		SIM_CONFIG stVehicleConfig = m_stConfig;
		stVehicleConfig.dfTickSec = dfThisTickSec;		// CVehicle 은 이 필드만 봄(범위는 모름)

		CVehicle *pcVeh = new (std::nothrow) CVehicle();
		if (!pcVeh) { LOGFMTE("vehicle alloc fail"); return false; }
		// 차량 시뮬레이터 초기화 (2026-07-08 최정우 주석 추가)
		pcVeh->Initialize(szKey, m_pcRoute, stVehicleConfig, nBase + (unsigned int)i * 2654435761u);
		m_vtVehicles.push_back(pcVeh);
		m_vtVehicleTickSec.push_back(dfThisTickSec);
		// 첫 tick 시점 산포 — [0, dfThisTickSec) 랜덤 (2026-07-22 최정우 추가)
		uniform_real_distribution<double> distFirst(0.0, dfThisTickSec);
		m_vtVehicleCountdown.push_back(distFirst(rngTick));
	}

	LOGFMTI("simulator initialized. vehicles=[%d] tick_sec=[%.2f~%.2f] flush=[%ds] area=[%.5f,%.5f ~ %.5f,%.5f]",
		m_stConfig.nVehicles, m_stConfig.dfTickSecMin, m_stConfig.dfTickSecMax, m_stConfig.nFlushSec,
		m_stConfig.dfMinLon, m_stConfig.dfMinLat, m_stConfig.dfMaxLon, m_stConfig.dfMaxLat);
	return true;
}

void CSimServer::Uninitialize()
{
	for (size_t i = 0; i < m_vtVehicles.size(); ++i)
		// 차량 객체 메모리 해제 (2026-07-08 최정우 주석 추가)
		delete m_vtVehicles[i];
	m_vtVehicles.clear();
	m_vtVehicleTickSec.clear();
	m_vtVehicleCountdown.clear();

	if (m_pcRoute) { delete m_pcRoute; m_pcRoute = nullptr; }
	if (m_pcSQL) { delete m_pcSQL; m_pcSQL = nullptr; }
	if (m_pcPool) { delete m_pcPool; m_pcPool = nullptr; }
}

/**
 * @brief 현재 시각(KST)을 YYYYMMDDHH24MISS 로 포맷
*/
void CSimServer::MakeNowKst(char *pszOut, size_t nSize)
{
	time_t now = time(nullptr);
	struct tm stTm;
	// 현재 시각을 로컬(KST) 시간으로 변환 (2026-07-08 최정우 주석 추가)
	localtime_r(&now, &stTm);
	strftime(pszOut, nSize, "%Y%m%d%H%M%S", &stTm);
}

namespace {
	// 메인루프 기본 해상도(초) — GPS_DT 문자열 자체가 초 단위라 이보다 잘게 쪼개도 의미가
	//   없다. 차량별 실제 tick 주기는 이 값보다 커지도록(AppMain.cpp 하한 0.1은 세밀 조정용
	//   여지) m_vtVehicleCountdown 으로 개별 스케줄링한다 (2026-07-22 최정우 추가)
	const double BASE_LOOP_SEC = 1.0;
}

void CSimServer::Run()
{
	LOGFMTI("simulator run start. tick_sec range=[%.2f~%.2f]",
		m_stConfig.dfTickSecMin, m_stConfig.dfTickSecMax);
	time_t dtLastReport = time(nullptr);
	// flush 는 차량 tick 과 무관하게 실제 경과시간 기준 — flush_sec(초) 값이 그대로 실제 시간
	//   간격으로 유지되게 함 (2026-07-22 최정우 수정)
	time_t dtLastFlush = time(nullptr);

	while (!g_nSimStop)
	{
		struct timespec stStart;
		clock_gettime(CLOCK_MONOTONIC, &stStart);

		char szDt[15];
		// 현재 KST 시각을 GPS 일시 문자열로 생성 (2026-07-08 최정우 주석 추가)
		MakeNowKst(szDt, sizeof(szDt));

		size_t nBufBefore = m_vtBuffer.size();
		for (size_t i = 0; i < m_vtVehicles.size(); ++i)
		{
			// 차량마다 자기 tick_sec 만큼 카운트다운 — 0 이하가 될 때만 그 차량을 실제로
			//   tick 한다(=차량별로 다른 주기에 GPS 표본 생성) (2026-07-22 최정우 추가)
			m_vtVehicleCountdown[i] -= BASE_LOOP_SEC;
			if (m_vtVehicleCountdown[i] > 0.0)
				continue;
			m_vtVehicleCountdown[i] += m_vtVehicleTickSec[i];
			m_vtVehicles[i]->Tick(szDt, m_vtBuffer);
		}
		m_ullTotalGenerated += (m_vtBuffer.size() - nBufBefore);

		// max_samples 도달 시 이번 tick까지만 반영하고 종료 (2026-07-20 최정우 추가)
		if (m_stConfig.nMaxSamples > 0 && m_ullTotalGenerated >= (unsigned long long)m_stConfig.nMaxSamples)
		{
			LOGFMTI("max_samples[%d] reached (generated=[%llu]). stopping.",
				m_stConfig.nMaxSamples, m_ullTotalGenerated);
			g_nSimStop = 1;
		}

		time_t dtNow = time(nullptr);
		if (m_stConfig.nFlushSec > 0 && (dtNow - dtLastFlush) >= m_stConfig.nFlushSec)
		{
			// 버퍼 GPS 샘플 DB 일괄 INSERT (2026-07-08 최정우 주석 추가)
			Flush();
			dtLastFlush = dtNow;
		}

		// 주기 통계 로그
		if (m_stConfig.nReportSec > 0 && (dtNow - dtLastReport) >= m_stConfig.nReportSec)
		{
			LOGFMTI("[stat] inserted=[%llu] fail=[%llu] buffer=[%zu] vehicles=[%zu]",
				m_ullTotalInsert, m_ullTotalFail, m_vtBuffer.size(), m_vtVehicles.size());
			dtLastReport = dtNow;
		}

		// 메인루프 기본 해상도(BASE_LOOP_SEC) 맞추기 (처리 시간 보정)
		struct timespec stEnd;
		clock_gettime(CLOCK_MONOTONIC, &stEnd);
		double dfElapsed = (stEnd.tv_sec - stStart.tv_sec) +
			(stEnd.tv_nsec - stStart.tv_nsec) / 1e9;
		double dfSleep = BASE_LOOP_SEC - dfElapsed;
		if (dfSleep > 0.0 && !g_nSimStop)
		{
			struct timespec stReq;
			stReq.tv_sec = (time_t)dfSleep;
			stReq.tv_nsec = (long)((dfSleep - stReq.tv_sec) * 1e9);
			// 기본 루프 해상도 맞추기 위해 대기 (2026-07-08 최정우 주석 추가, 2026-07-22 최정우 수정)
			nanosleep(&stReq, nullptr);
		}
	}

	LOGFMTI("simulator stopping... flush remaining [%zu] samples.", m_vtBuffer.size());
	// 종료 전 잔여 GPS 샘플 DB 일괄 INSERT (2026-07-08 최정우 주석 추가)
	Flush();
	LOGFMTI("simulator run end. total inserted=[%llu] fail=[%llu]", m_ullTotalInsert, m_ullTotalFail);
}

/**
 * @brief 버퍼의 표본을 트랜잭션으로 일괄 INSERT
*/
void CSimServer::Flush()
{
	if (m_vtBuffer.empty()) return;

	// DB 커넥션 풀에서 연결 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = m_pcPool->getConnection();
	if (!pcConn)
	{
		LOGFMTE("flush getConnection fail. drop [%zu] samples.", m_vtBuffer.size());
		m_ullTotalFail += m_vtBuffer.size();
		m_vtBuffer.clear();
		return;
	}

	// INSERT 트랜잭션 시작 (2026-07-08 최정우 주석 추가)
	PGresult *res = PQexec(pcConn, "BEGIN");
	if (res) PQclear(res);

	bool bOk = true;
	size_t nDone = 0;
	for (size_t i = 0; i < m_vtBuffer.size(); ++i)
	{
		const GPS_SAMPLE& s = m_vtBuffer[i];

		char szLat[32], szLon[32], szSpd[16], szHead[16], szAlt[16], szAcc[16], szBat[8];
		char szTripEvent[8], szDriveStatus[8], szGpsSeq[24];
		const char *pszSpd = nullptr;
		const char *pszHead = nullptr;
		const char *pszAlt = nullptr;
		const char *pszAcc = nullptr;
		const char *pszBat = nullptr;
		snprintf(szLat, sizeof(szLat), "%.6f", s.dfLat);
		snprintf(szLon, sizeof(szLon), "%.6f", s.dfLon);
		// 순간속도/방위각/고도/수평오차: 누락 시 libpq NULL → DB NULL (2026-07-10 최정우 추가)
		if (s.bHasSpeed)
		{
			int nSpd = (s.dfSpeedKmh > 0.0) ? (int)(s.dfSpeedKmh + 0.5) : 0;
			snprintf(szSpd, sizeof(szSpd), "%d", nSpd);
			pszSpd = szSpd;
		}
		if (s.bHasHeading)
		{
			int nHead = ((int)(s.dfHeading + 0.5)) % 360; if (nHead < 0) nHead += 360;
			snprintf(szHead, sizeof(szHead), "%d", nHead);
			pszHead = szHead;
		}
		if (s.bHasAltitude)
		{
			int nAlt = (int)(s.dfAltitude + (s.dfAltitude >= 0.0 ? 0.5 : -0.5));
			snprintf(szAlt, sizeof(szAlt), "%d", nAlt);
			pszAlt = szAlt;
		}
		if (s.bHasAccuracy)
		{
			int nAcc = (s.dfAccuracy > 0.0) ? (int)(s.dfAccuracy + 0.5) : 1;
			snprintf(szAcc, sizeof(szAcc), "%d", nAcc);
			pszAcc = szAcc;
		}
		if (s.bHasBattery)
		{
			snprintf(szBat, sizeof(szBat), "%d", s.nBattery);
			pszBat = szBat;
		}
		snprintf(szTripEvent, sizeof(szTripEvent), "%d", static_cast<int>(s.nTripEvent));
		snprintf(szDriveStatus, sizeof(szDriveStatus), "%d", static_cast<int>(s.nDriveStatus));
		// GPS_SEQ: 운행마다 1~N (BIGINT) (2026-07-10 최정우 추가)
		snprintf(szGpsSeq, sizeof(szGpsSeq), "%llu", (unsigned long long)s.uqGpsSeq);
		// RAW_VLD: 좌표 유효 여부 → boolean 't'/'f' (2026-07-10 최정우 추가)
		const char *pszRawVld = s.bRawValid ? "t" : "f";

		const char *aszParams[14] = {
			s.strDeviceKey.c_str(),
			s.szGpsDt,
			szTripEvent,
			szDriveStatus,
			szLat, szLon, pszSpd, pszHead, pszAlt, pszAcc, pszBat,
			s.strTripId.c_str(),		// $12 trip_id (2026-07-10 최정우 수정)
			szGpsSeq,					// $13 gps_seq (2026-07-10 최정우 추가)
			pszRawVld					// $14 raw_vld (2026-07-10 최정우 추가)
		};

		// GPS 샘플 1건 INSERT SQL 실행 (2026-07-08 최정우 주석 추가)
		PGresult *r = PQexecParams(pcConn, m_strInsertSQL.c_str(), 14, nullptr,
			aszParams, nullptr, nullptr, 0);
		ExecStatusType st = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
		if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
		{
			LOGFMTE("insert fail!error=[%s]", PQerrorMessage(pcConn));
			if (r) PQclear(r);
			bOk = false;
			break;
		}
		if (r) PQclear(r);
		++nDone;
	}

	if (bOk)
	{
		// INSERT 트랜잭션 커밋 (2026-07-08 최정우 주석 추가)
		res = PQexec(pcConn, "COMMIT");
		if (res) PQclear(res);
		m_ullTotalInsert += nDone;
	}
	else
	{
		// INSERT 트랜잭션 롤백 (2026-07-08 최정우 주석 추가)
		res = PQexec(pcConn, "ROLLBACK");
		if (res) PQclear(res);
		m_ullTotalFail += m_vtBuffer.size();
	}

	// DB 커넥션 풀에 연결 반환 (2026-07-08 최정우 주석 추가)
	m_pcPool->releaseConnection(pcConn);
	m_vtBuffer.clear();
}
