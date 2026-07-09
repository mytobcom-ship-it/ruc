/**
 * @file GeoUtil.h
 * @brief 좌표 계산 유틸 (WKT 파싱, 거리/방위, 미터 오프셋)
*/
#ifndef __GEO_UTIL_H__
#define __GEO_UTIL_H__

#include <string>
#include <vector>
#include "SimTypes.h"

using namespace std;

/**
 * @class CGeoUtil
 * @brief WGS-84 경위도 기반 좌표 계산 모음 (정적 메서드)
*/
class CGeoUtil
{
public:
	// "LINESTRING(lon lat, ...)" / "MULTILINESTRING(((...)))" 파싱 → 점열
	static bool ParseLineString(const string& strWkt, vector<GEO_POINT>& vtOut);

	// 두 점 사이 거리 (m, Haversine)
	static double DistanceM(const GEO_POINT& a, const GEO_POINT& b);

	// a→b 진행 방위각 (0~360도)
	static double BearingDeg(const GEO_POINT& a, const GEO_POINT& b);

	// 기준점에서 dfBearing 방향으로 dfDistM 미터 이동한 좌표
	static GEO_POINT OffsetMeters(const GEO_POINT& base, double dfDistM, double dfBearingDeg);

	// 선형 보간 (t: 0~1)
	static GEO_POINT Lerp(const GEO_POINT& a, const GEO_POINT& b, double t);
};

#endif // __GEO_UTIL_H__
