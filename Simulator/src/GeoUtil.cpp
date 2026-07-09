/**
 * @file GeoUtil.cpp
 * @brief 좌표 계산 유틸 구현
*/
#include "GeoUtil.h"
#include <cmath>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double EARTH_R = 6378137.0;		// WGS-84 지구 반경 (m)
static const double DEG2RAD = M_PI / 180.0;
static const double METER_PER_DEG_LAT = 111320.0;

/**
 * @brief WKT 문자열에서 모든 좌표쌍을 순차 파싱한다.
 *        LINESTRING / MULTILINESTRING 모두 (lon lat) 쌍을 순서대로 추출.
 *        (2D 전용 - Z 좌표가 있으면 사용하지 않음)
*/
bool CGeoUtil::ParseLineString(const string& strWkt, vector<GEO_POINT>& vtOut)
{
	vtOut.clear();

	// 첫 '(' 이전의 타입명(LINESTRING 등) 건너뛰기
	size_t nPos = strWkt.find('(');
	if (nPos == string::npos) return false;

	const char *p = strWkt.c_str() + nPos;
	vector<double> vtNums;
	char *pEnd = nullptr;

	while (*p)
	{
		if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.')
		{
			// WKT 숫자 토큰 파싱 (2026-07-08 최정우 주석 추가)
			double dfVal = strtod(p, &pEnd);
			if (pEnd == p) { ++p; continue; }
			vtNums.push_back(dfVal);
			p = pEnd;
		}
		else
		{
			++p;
		}
	}

	for (size_t i = 0; i + 1 < vtNums.size(); i += 2)
		vtOut.push_back(GEO_POINT(vtNums[i], vtNums[i + 1]));

	return vtOut.size() >= 2;
}

/**
 * @brief Haversine 거리 (m)
*/
double CGeoUtil::DistanceM(const GEO_POINT& a, const GEO_POINT& b)
{
	double dfLat1 = a.lat * DEG2RAD;
	double dfLat2 = b.lat * DEG2RAD;
	double dfDLat = (b.lat - a.lat) * DEG2RAD;
	double dfDLon = (b.lon - a.lon) * DEG2RAD;

	double h = sin(dfDLat / 2) * sin(dfDLat / 2) +
		cos(dfLat1) * cos(dfLat2) * sin(dfDLon / 2) * sin(dfDLon / 2);
	double c = 2 * atan2(sqrt(h), sqrt(1 - h));
	return EARTH_R * c;
}

/**
 * @brief a→b 초기 방위각 (0~360도, 북=0, 시계방향)
*/
double CGeoUtil::BearingDeg(const GEO_POINT& a, const GEO_POINT& b)
{
	double dfLat1 = a.lat * DEG2RAD;
	double dfLat2 = b.lat * DEG2RAD;
	double dfDLon = (b.lon - a.lon) * DEG2RAD;

	double y = sin(dfDLon) * cos(dfLat2);
	double x = cos(dfLat1) * sin(dfLat2) - sin(dfLat1) * cos(dfLat2) * cos(dfDLon);
	double dfBrg = atan2(y, x) / DEG2RAD;
	if (dfBrg < 0) dfBrg += 360.0;
	return dfBrg;
}

/**
 * @brief 기준점에서 방위/거리만큼 이동한 좌표 (평면 근사)
*/
GEO_POINT CGeoUtil::OffsetMeters(const GEO_POINT& base, double dfDistM, double dfBearingDeg)
{
	double dfTheta = dfBearingDeg * DEG2RAD;
	double dfNorth = dfDistM * cos(dfTheta);		// 북쪽(+)
	double dfEast = dfDistM * sin(dfTheta);			// 동쪽(+)

	double dfCosLat = cos(base.lat * DEG2RAD);
	if (dfCosLat < 1e-6) dfCosLat = 1e-6;

	GEO_POINT out;
	out.lat = base.lat + (dfNorth / METER_PER_DEG_LAT);
	out.lon = base.lon + (dfEast / (METER_PER_DEG_LAT * dfCosLat));
	return out;
}

/**
 * @brief 선형 보간
*/
GEO_POINT CGeoUtil::Lerp(const GEO_POINT& a, const GEO_POINT& b, double t)
{
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	return GEO_POINT(a.lon + (b.lon - a.lon) * t, a.lat + (b.lat - a.lat) * t);
}
