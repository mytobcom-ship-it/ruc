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
	string				strFromNode;	// 시작 노드 ID (역방향 링크 제외용) (2026-07-22 최정우 추가)
	string				strToNode;		// 종료 노드 ID (다음 링크 탐색 키)
	int					nMaxSpd;		// 제한속도 (km/h, 0=미지정)
	int					nRoadType;		// MOCT_LINK.ROAD_TYPE (0:일반 1:교량 2:터널 3:고가 4:지하) (2026-07-20 최정우 추가)
	vector<GEO_POINT>	vtPoints;		// 형상 점열 (f_node → t_node, WGS-84)

	sLinkGeom() : nMaxSpd(0), nRoadType(0) {}
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
 * @brief PRIM_RAWGPS 에 INSERT 할 GPS 표본 1건
*/
typedef struct sGpsSample
{
	string		strDeviceKey;		// 차량 인증키
	string		strTripId;			// 운행 ID {DEVICE_KEY}_{YYYYMMDDHH24MISS(운행시작)} (2026-07-10 최정우 추가)
	uint64		uqGpsSeq;			// 운행 내 GPS 순번 (운행마다 1~N 초기화) (2026-07-10 최정우 추가)
	sint16		nTripEvent;			// 0:START, 1:NONE, 2:END
	sint16		nDriveStatus;		// 0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING
	double		dfLat;				// 위도 (노이즈 포함)
	double		dfLon;				// 경도 (노이즈 포함)
	double		dfSpeedKmh;			// 속도
	double		dfHeading;			// 방향각
	double		dfAltitude;			// 고도
	double		dfAccuracy;			// 수평 오차
	int			nBattery;			// 배터리
	bool		bRawValid;			// 원시 GPS 좌표 유효 여부 → RAW_VLD (2026-07-10 최정우 추가)
	bool		bHasSpeed;			// 순간속도(SPEED_KMH) 적재 여부. false → DB NULL (2026-07-10 최정우 추가)
	bool		bHasHeading;		// 방위각(HEADING) 적재 여부. false → DB NULL (2026-07-10 최정우 추가)
	bool		bHasAltitude;		// 고도(ALTITUDE_M) 적재 여부. false → DB NULL (2026-07-10 최정우 추가)
	bool		bHasAccuracy;		// 수평오차(ACCURACY_M) 적재 여부. false → DB NULL (2026-07-10 최정우 추가)
	bool		bHasBattery;		// 배터리(BATTERY) 적재 여부. false → DB NULL (2026-07-10 최정우 추가)
	char		szGpsDt[14 + 1];	// GPS 측정 시각 (YYYYMMDDHH24MISS)

	sGpsSample() :
		uqGpsSeq(0),
		nTripEvent(SIM_TRIP_EVENT_NONE),
		nDriveStatus(SIM_DRIVE_STATUS_ON_ROAD),
		dfLat(0.0), dfLon(0.0), dfSpeedKmh(0.0), dfHeading(0.0),
		dfAltitude(0.0), dfAccuracy(0.0), nBattery(0),
		bRawValid(true),
		bHasSpeed(true), bHasHeading(true), bHasAltitude(true),
		bHasAccuracy(true), bHasBattery(true)
	{
		szGpsDt[0] = 0x00;
	}
} GPS_SAMPLE;

#endif // __SIM_TYPES_H__
