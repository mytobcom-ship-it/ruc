/**
 * @file Vehicle.h
 * @brief 차량 1대의 주행 시뮬레이션 (경로 생성 + 1초 tick + 도로 이탈 노이즈)
*/
#ifndef __VEHICLE_H__
#define __VEHICLE_H__

#include <string>
#include <vector>
#include <random>
#include "SimTypes.h"
#include "Config.h"
#include "RouteProvider.h"

using namespace std;

/**
 * @class CVehicle
 * @brief 차량 상태 및 매 tick GPS 표본 생성
*/
class CVehicle
{
public:
	CVehicle();
	~CVehicle();

	void Initialize(const string& strDeviceKey, CRouteProvider *pcRoute,
		const SIM_CONFIG& stConfig, unsigned int nSeed);

	// 1초 경과 처리. 생성된 표본을 vtOut 에 추가 (경로 준비 실패 시 미생성)
	void Tick(const char *pszGpsDt, vector<GPS_SAMPLE>& vtOut);

	const string& GetDeviceKey() const { return m_strDeviceKey; }

private:
	bool BuildRoute();
	void AppendLink(const LINK_GEOM& stLink, bool bFirst);
	bool Interpolate(double dfPos, GEO_POINT& stPt, double& dfBearing, int& nSegSpd);

private:
	string					m_strDeviceKey;
	CRouteProvider			*m_pcRoute;
	SIM_CONFIG				m_stConfig;
	mt19937					m_rng;

	vector<GEO_POINT>		m_vtRoute;		// 누적 경로 점열
	vector<double>			m_vtCum;		// 누적 거리 (m), size == 점 수
	vector<int>				m_vtSegSpd;		// 구간별 제한속도, size == 점 수 - 1

	double					m_dfPos;		// 경로상 진행 거리 (m)
	double					m_dfSpeedMps;	// 현재 속도 (m/s)
	double					m_dfLastHeading;// 직전 방위각
	double					m_dfAltitude;	// 고도 기준값
	double					m_dfBattery;	// 배터리 (%, 실수 누적)
	bool					m_bTripActive;	// 운행 중 여부
	bool					m_bStartPending;// START 이벤트 대기
};

#endif // __VEHICLE_H__
