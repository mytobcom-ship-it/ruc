/**
 * @file SimServer.h
 * @brief 시뮬레이터 본체 - 차량 생성, 1초 tick, 3초 배치 INSERT
*/
#ifndef __SIM_SERVER_H__
#define __SIM_SERVER_H__

#include <signal.h>
#include <string>
#include <vector>
#include "Config.h"
#include "SimTypes.h"
#include "PostgrePool.h"
#include "SQLAccessor.h"
#include "RouteProvider.h"
#include "Vehicle.h"

using namespace std;

// 종료 신호 플래그 (AppMain 의 시그널 핸들러가 설정)
extern volatile sig_atomic_t g_nSimStop;

/**
 * @class CSimServer
 * @brief 시뮬레이터 구동 클래스
*/
class CSimServer
{
public:
	CSimServer();
	~CSimServer();

	bool Initialize(const SIM_CONFIG& stConfig);
	void Run();
	void Uninitialize();

private:
	void Flush();
	void MakeNowKst(char *pszOut, size_t nSize);

private:
	SIM_CONFIG				m_stConfig;
	CPostgrePool			*m_pcPool;
	CSQLAccessor			*m_pcSQL;
	CRouteProvider			*m_pcRoute;
	vector<CVehicle *>		m_vtVehicles;

	string					m_strInsertSQL;
	vector<GPS_SAMPLE>		m_vtBuffer;

	unsigned long long		m_ullTotalInsert;	// 누적 INSERT 건수
	unsigned long long		m_ullTotalFail;		// 누적 실패 건수
	unsigned long long		m_ullTotalGenerated;	// 누적 생성(tick) 건수 — max_samples 도달 판정용
};

#endif // __SIM_SERVER_H__
