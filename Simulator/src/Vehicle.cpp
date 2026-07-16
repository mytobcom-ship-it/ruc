/**
 * @file Vehicle.cpp
 * @brief 차량 주행 시뮬레이션 구현
*/
#include "Vehicle.h"
#include "GeoUtil.h"
#include "log4z.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace zsummer::log4z;

static const double TICK_SEC = 1.0;		// tick 간격 (초)
static const double ACCEL_MPS = 2.0;	// 가속 한계 (m/s per tick)
static const double DECEL_MPS = 3.5;	// 감속 한계 (m/s per tick)

/**
 * @brief 보조 GPS 필드(속도·방위각·고도·수평오차·배터리) 간헐적 누락 시뮬레이션
 * @remark 좌표(GPS_LAT/LON)는 유지. MapMatchSvr 은 NULL → NO_SPEED/NO_ANGLE/NO_ACCURACY 등으로 처리
*/
static void ApplySensorFieldOmit(GPS_SAMPLE& stSample, mt19937& rng, const SIM_CONFIG& stConfig)
{
	uniform_real_distribution<double> dist01(0.0, 1.0);
	if (dist01(rng) < stConfig.dfOmitAllProb)
	{
		stSample.bHasSpeed = false;
		stSample.bHasHeading = false;
		stSample.bHasAltitude = false;
		stSample.bHasAccuracy = false;
		stSample.bHasBattery = false;
		return;
	}

	if (dist01(rng) >= stConfig.dfOmitPartialProb)
		return;

	uniform_int_distribution<int> distCnt(1, 2);
	const int nOmit = distCnt(rng);
	array<int, 5> aIdx = { {0, 1, 2, 3, 4} };
	shuffle(aIdx.begin(), aIdx.end(), rng);
	for (int i = 0; i < nOmit; ++i)
	{
		switch (aIdx[static_cast<size_t>(i)])
		{
		case 0: stSample.bHasSpeed = false; break;
		case 1: stSample.bHasHeading = false; break;
		case 2: stSample.bHasAltitude = false; break;
		case 3: stSample.bHasAccuracy = false; break;
		case 4: stSample.bHasBattery = false; break;
		default: break;
		}
	}
}

CVehicle::CVehicle()
	: m_pcRoute(nullptr), m_dfPos(0.0), m_dfSpeedMps(0.0),
	m_dfLastHeading(0.0), m_dfAltitude(40.0), m_dfBattery(100.0),
	m_bTripActive(false), m_bStartPending(false), m_qwGpsSeq(0)
{
}

CVehicle::~CVehicle()
{
}

void CVehicle::Initialize(const string& strDeviceKey, CRouteProvider *pcRoute,
	const SIM_CONFIG& stConfig, unsigned int nSeed)
{
	m_strDeviceKey = strDeviceKey;
	m_pcRoute = pcRoute;
	m_stConfig = stConfig;
	m_rng.seed(nSeed);

	uniform_real_distribution<double> distAlt(20.0, 80.0);
	m_dfAltitude = distAlt(m_rng);
	m_dfBattery = 100.0;
}

/**
 * @brief 링크 형상을 누적 경로에 이어붙임
 *        bFirst=false 이면 직전 끝점과 중복되는 첫 점은 건너뜀
*/
void CVehicle::AppendLink(const LINK_GEOM& stLink, bool bFirst)
{
	size_t nStart = 0;
	if (!bFirst && !m_vtRoute.empty() && !stLink.vtPoints.empty())
	{
		// 직전 끝점과 신규 링크 시작점 거리 비교 (2026-07-08 최정우 주석 추가)
		double dfGap = CGeoUtil::DistanceM(m_vtRoute.back(), stLink.vtPoints.front());
		if (dfGap < 1.0) nStart = 1;
	}

	int nSpd = stLink.nMaxSpd;
	for (size_t i = nStart; i < stLink.vtPoints.size(); ++i)
	{
		if (!m_vtRoute.empty())
		{
			// 인접 버텍스 간 Haversine 거리 계산 (2026-07-08 최정우 주석 추가)
			double dfSeg = CGeoUtil::DistanceM(m_vtRoute.back(), stLink.vtPoints[i]);
			if (dfSeg < 0.01) continue;		// 동일점 스킵
			m_vtSegSpd.push_back(nSpd);
		}
		m_vtRoute.push_back(stLink.vtPoints[i]);
	}
}

/**
 * @brief 시작 링크 + 연결 링크로 경로 1개 구성
*/
bool CVehicle::BuildRoute()
{
	m_vtRoute.clear();
	m_vtCum.clear();
	m_vtSegSpd.clear();

	LINK_GEOM stLink;
	// bbox 내 임의 시작 링크 조회 (2026-07-08 최정우 주석 추가)
	if (!m_pcRoute->SeedLink(stLink)) return false;

	// 시작 링크 형상을 경로에 이어붙임 (2026-07-08 최정우 주석 추가)
	AppendLink(stLink, true);
	string strNode = stLink.strToNode;
	string strLast = stLink.strLinkID;

	double dfLen = 0.0;
	for (size_t i = 1; i < m_vtRoute.size(); ++i)
		// 경로 누적 거리 합산 (2026-07-08 최정우 주석 추가)
		dfLen += CGeoUtil::DistanceM(m_vtRoute[i - 1], m_vtRoute[i]);

	int nLinks = 1;
	while (dfLen < m_stConfig.nRouteMinM && nLinks < m_stConfig.nRouteMaxLinks)
	{
		LINK_GEOM stNext;
		// 종료 노드에서 다음 연결 링크 조회 (2026-07-08 최정우 주석 추가)
		if (!m_pcRoute->NextLink(strNode, strLast, stNext)) break;
		if (stNext.vtPoints.size() < 2) break;

		size_t nBefore = m_vtRoute.size();
		// 다음 링크 형상을 경로에 이어붙임 (2026-07-08 최정우 주석 추가)
		AppendLink(stNext, false);
		if (m_vtRoute.size() <= nBefore) break;		// 추가 실패

		for (size_t i = nBefore; i < m_vtRoute.size(); ++i)
			// 신규 구간 누적 거리 합산 (2026-07-08 최정우 주석 추가)
			dfLen += CGeoUtil::DistanceM(m_vtRoute[i - 1], m_vtRoute[i]);

		strNode = stNext.strToNode;
		strLast = stNext.strLinkID;
		++nLinks;
	}

	if (m_vtRoute.size() < 2) return false;

	// 누적 거리 계산
	m_vtCum.resize(m_vtRoute.size());
	m_vtCum[0] = 0.0;
	for (size_t i = 1; i < m_vtRoute.size(); ++i)
		// 누적 거리 배열 계산 (2026-07-08 최정우 주석 추가)
		m_vtCum[i] = m_vtCum[i - 1] + CGeoUtil::DistanceM(m_vtRoute[i - 1], m_vtRoute[i]);

	if (m_vtCum.back() < 1.0) return false;
	return true;
}

/**
 * @brief 경로상 거리 dfPos 위치의 좌표/방위/구간속도
*/
bool CVehicle::Interpolate(double dfPos, GEO_POINT& stPt, double& dfBearing, int& nSegSpd)
{
	if (m_vtRoute.size() < 2) return false;
	double dfTotal = m_vtCum.back();
	if (dfPos <= 0.0)
	{
		stPt = m_vtRoute.front();
		// 경로 시작 구간 방위각 계산 (2026-07-08 최정우 주석 추가)
		dfBearing = CGeoUtil::BearingDeg(m_vtRoute[0], m_vtRoute[1]);
		nSegSpd = m_vtSegSpd.empty() ? 0 : m_vtSegSpd.front();
		return true;
	}
	if (dfPos >= dfTotal)
	{
		size_t n = m_vtRoute.size();
		stPt = m_vtRoute.back();
		// 경로 종료 구간 방위각 계산 (2026-07-08 최정우 주석 추가)
		dfBearing = CGeoUtil::BearingDeg(m_vtRoute[n - 2], m_vtRoute[n - 1]);
		nSegSpd = m_vtSegSpd.empty() ? 0 : m_vtSegSpd.back();
		return true;
	}

	for (size_t i = 1; i < m_vtCum.size(); ++i)
	{
		if (dfPos <= m_vtCum[i])
		{
			double dfSeg = m_vtCum[i] - m_vtCum[i - 1];
			double t = (dfSeg > 1e-9) ? (dfPos - m_vtCum[i - 1]) / dfSeg : 0.0;
			// 구간 내 선형 보간 좌표 계산 (2026-07-08 최정우 주석 추가)
			stPt = CGeoUtil::Lerp(m_vtRoute[i - 1], m_vtRoute[i], t);
			// 구간 방위각 계산 (2026-07-08 최정우 주석 추가)
			dfBearing = CGeoUtil::BearingDeg(m_vtRoute[i - 1], m_vtRoute[i]);
			nSegSpd = m_vtSegSpd[i - 1];
			return true;
		}
	}
	return false;
}

void CVehicle::Tick(const char *pszGpsDt, vector<GPS_SAMPLE>& vtOut)
{
	// 경로 준비
	if (!m_bTripActive)
	{
		// 시작 링크+연결 링크로 주행 경로 구성 (2026-07-08 최정우 주석 추가)
		if (!BuildRoute())
		{
			LOGFMTW("device=[%s] route build fail (skip tick)", m_strDeviceKey.c_str());
			return;
		}
		m_dfPos = 0.0;
		m_dfSpeedMps = 0.0;
		m_bStartPending = true;
		m_bTripActive = true;
	}

	// 현재 위치의 제한속도 파악
	GEO_POINT stTrue;
	double dfBearing = m_dfLastHeading;
	int nSegSpd = 0;
	// 경로상 현재 위치 좌표·방위·구간속도 보간 (2026-07-08 최정우 주석 추가)
	Interpolate(m_dfPos, stTrue, dfBearing, nSegSpd);

	// 속도 결정
	uniform_real_distribution<double> dist01(0.0, 1.0);
	double dfTargetMps;
	bool bIdle = (dist01(m_rng) < m_stConfig.dfIdleProb);
	if (bIdle)
	{
		dfTargetMps = 0.0;
	}
	else
	{
		double dfLimit = (nSegSpd > 0) ? (double)nSegSpd : m_stConfig.dfDefaultMaxSpd;
		uniform_real_distribution<double> distF(m_stConfig.dfSpeedFactorMin, m_stConfig.dfSpeedFactorMax);
		dfTargetMps = dfLimit * distF(m_rng) / 3.6;
	}

	double dfDiff = dfTargetMps - m_dfSpeedMps;
	if (dfDiff > ACCEL_MPS) dfDiff = ACCEL_MPS;
	if (dfDiff < -DECEL_MPS) dfDiff = -DECEL_MPS;
	m_dfSpeedMps += dfDiff;
	if (m_dfSpeedMps < 0.0) m_dfSpeedMps = 0.0;

	// 진행
	m_dfPos += m_dfSpeedMps * TICK_SEC;
	double dfTotal = m_vtCum.back();
	bool bEnd = false;
	if (m_dfPos >= dfTotal)
	{
		m_dfPos = dfTotal;
		bEnd = true;
	}

	// 이벤트 (SMALLINT: 0=START, 1=NONE, 2=END)
	sint16 nEvent = SIM_TRIP_EVENT_NONE;
	if (m_bStartPending) { nEvent = SIM_TRIP_EVENT_START; m_bStartPending = false; }
	else if (bEnd) nEvent = SIM_TRIP_EVENT_END;

	// 운행 시작 시 trip_id 발급: {DEVICE_KEY}_{YYYYMMDDHH24MISS(운행시작)} — END 까지 동일 유지 (2026-07-10 최정우 추가)
	if (nEvent == SIM_TRIP_EVENT_START)
		m_strTripId = m_strDeviceKey + "_" + string(pszGpsDt);

	// GPS_SEQ: 운행마다 초기화, START(첫 표본)=1, 이후 표본마다 +1 (2026-07-10 최정우 추가)
	if (nEvent == SIM_TRIP_EVENT_START)
		m_qwGpsSeq = 1;
	else
		++m_qwGpsSeq;

	// 현재 위치 재계산 (진행 반영)
	// 진행 반영 후 위치·방위 재보간 (2026-07-08 최정우 주석 추가)
	Interpolate(m_dfPos, stTrue, dfBearing, nSegSpd);
	if (m_dfSpeedMps >= 0.5) m_dfLastHeading = dfBearing;

	// 도로 이탈 노이즈 (정규분포 거리 + 임의 방향) — 현실적 GPS 수준(대부분 ≤10m) (2026-07-16 최정우 수정)
	normal_distribution<double> distNoise(0.0, m_stConfig.dfNoiseSigmaM);
	double dfOffset = fabs(distNoise(m_rng));
	if (dfOffset > m_stConfig.dfNoiseMaxM) dfOffset = m_stConfig.dfNoiseMaxM;

	// 예외: 낮은 확률로 큰 튀는 좌표(멀티패스·도심협곡·순간이상) 주입 → 예외처리(SKIP 등) 검증 (2026-07-16 최정우 추가)
	if (dist01(m_rng) < m_stConfig.dfOutlierProb)
	{
		uniform_real_distribution<double> distOutlier(m_stConfig.dfOutlierMinM, m_stConfig.dfOutlierMaxM);
		dfOffset = distOutlier(m_rng);
	}
	if (dfOffset < 1.0) dfOffset = 1.0;

	uniform_real_distribution<double> distDir(0.0, 360.0);
	// 도로 이탈 노이즈 좌표 오프셋 적용 (2026-07-08 최정우 주석 추가)
	GEO_POINT stNoisy = CGeoUtil::OffsetMeters(stTrue, dfOffset, distDir(m_rng));

	// 수평오차(ACCURACY_M): 실제 오프셋과 상관 (정수 적재). 튀는 좌표는 큰 오차로 반영 (2026-07-16 최정우 수정)
	normal_distribution<double> distAcc(0.0, 2.0);
	double dfAcc = dfOffset + fabs(distAcc(m_rng));
	if (dfAcc < 1.0) dfAcc = 1.0;
	if (dfAcc > 100.0) dfAcc = 100.0;

	// 배터리 완만한 감소
	m_dfBattery -= 0.01;
	if (m_dfBattery < 1.0) m_dfBattery = 100.0;

	// 고도 미세 변동
	normal_distribution<double> distAlt(0.0, 1.5);

	GPS_SAMPLE stSample;
	stSample.strDeviceKey = m_strDeviceKey;
	stSample.strTripId = m_strTripId;				// (2026-07-10 최정우 추가) 운행 trip_id 부여
	stSample.uqGpsSeq = m_qwGpsSeq;					// (2026-07-10 최정우 추가) 운행 내 GPS 순번
	stSample.nTripEvent = nEvent;
	stSample.nDriveStatus = (m_dfSpeedMps < 0.5) ? SIM_DRIVE_STATUS_IDLE : SIM_DRIVE_STATUS_ON_ROAD;
	stSample.dfLat = stNoisy.lat;
	stSample.dfLon = stNoisy.lon;
	stSample.dfSpeedKmh = m_dfSpeedMps * 3.6;
	stSample.dfHeading = m_dfLastHeading;
	stSample.dfAltitude = m_dfAltitude + distAlt(m_rng);
	stSample.dfAccuracy = dfAcc;
	stSample.nBattery = (int)(m_dfBattery + 0.5);
	// 시뮬은 도로 기준 유효 좌표를 생성 → RAW_VLD=TRUE(유효). 무효 케이스는 미생성 (2026-07-10 최정우 추가)
	stSample.bRawValid = true;
	memset(stSample.szGpsDt, 0, sizeof(stSample.szGpsDt));
	snprintf(stSample.szGpsDt, sizeof(stSample.szGpsDt), "%s", pszGpsDt);

	// 실제 단말처럼 속도·방위각·고도·수평오차·배터리가 가끔 NULL 로 수집되는 경우 (2026-07-10 최정우 추가)
	ApplySensorFieldOmit(stSample, m_rng, m_stConfig);

	vtOut.push_back(stSample);

	if (bEnd) m_bTripActive = false;	// 다음 tick 에서 새 경로(새 운행)
}
