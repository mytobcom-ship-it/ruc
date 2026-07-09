/**
 * @file SimTypes.h
 * @brief 시뮬레이터 공용 자료형
*/
#ifndef __SIM_TYPES_H__
#define __SIM_TYPES_H__

#include <string>
#include <vector>
#include "TypeDefine.h"

using namespace std;

/**
 * @struct sGeoPoint
 * @brief WGS-84 경위도 좌표 한 점
*/
typedef struct sGeoPoint
{
	double		lon;
	double		lat;

	sGeoPoint() : lon(0.0), lat(0.0) {}
	sGeoPoint(double l, double t) : lon(l), lat(t) {}
} GEO_POINT;

/**
 * @struct sLinkGeom
 * @brief 링크 한 개의 형상 정보
*/
typedef struct sLinkGeom
{
	string				strLinkID;		// 링크 ID
	string				strToNode;		// 종료 노드 ID (다음 링크 탐색 키)
	int					nMaxSpd;		// 제한속도 (km/h, 0=미지정)
	vector<GEO_POINT>	vtPoints;		// 형상 점열 (f_node → t_node, WGS-84)

	sLinkGeom() : nMaxSpd(0) {}
} LINK_GEOM;

#define SIM_TRIP_EVENT_START			0
#define SIM_TRIP_EVENT_NONE				1
#define SIM_TRIP_EVENT_END				2

#define SIM_DRIVE_STATUS_ON_ROAD		0
#define SIM_DRIVE_STATUS_IDLE			1
#define SIM_DRIVE_STATUS_PARKED			2
#define SIM_DRIVE_STATUS_TUNNELING		3

#define SIM_MATCH_STATUS_PENDING		0

/**
 * @brief RAW_GPS_LOG 에 INSERT 할 GPS 표본 1건
*/
typedef struct sGpsSample
{
	string		strDeviceKey;		// 차량 인증키
	sint16		nTripEvent;			// 0:START, 1:NONE, 2:END
	sint16		nDriveStatus;		// 0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING
	double		dfLat;				// 위도 (노이즈 포함)
	double		dfLon;				// 경도 (노이즈 포함)
	double		dfSpeedKmh;			// 속도
	double		dfHeading;			// 방향각
	double		dfAltitude;			// 고도
	double		dfAccuracy;			// 수평 오차
	int			nBattery;			// 배터리
	char		szGpsDt[14 + 1];	// GPS 측정 시각 (YYYYMMDDHH24MISS)

	sGpsSample() :
		nTripEvent(SIM_TRIP_EVENT_NONE),
		nDriveStatus(SIM_DRIVE_STATUS_ON_ROAD),
		dfLat(0.0), dfLon(0.0), dfSpeedKmh(0.0), dfHeading(0.0),
		dfAltitude(0.0), dfAccuracy(0.0), nBattery(0)
	{
		szGpsDt[0] = 0x00;
	}
} GPS_SAMPLE;

#endif // __SIM_TYPES_H__
