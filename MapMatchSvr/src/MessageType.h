/**
 * @file MessageType.h
 * @brief http 수신 메시지 정의 헤더 파일
*/
#ifndef __MESSAGETYPE_H__
#define __MESSAGETYPE_H__

#include <time.h>
#include <vector>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataDefine.h"

using namespace std;

#define TRIP_EVENT_START				0									// 출발
#define TRIP_EVENT_NONE					1									// 주행 중 (기본값)
#define TRIP_EVENT_END					2									// 도착

#define DRIVE_STATUS_ON_ROAD			0									// 주행 중 (기본값)
#define DRIVE_STATUS_IDLE				1									// 일시 정지
#define DRIVE_STATUS_PARKED				2									// 정차·주차
#define DRIVE_STATUS_TUNNELING			3									// GPS 음영 구간

#define MATCH_STATUS_PENDING			0									// 대기 (기본값)
#define MATCH_STATUS_MATCHED			1									// 맵매칭 완료
#define MATCH_STATUS_PROCESSING			2									// 처리 중
#define MATCH_STATUS_SKIP				3									// 제외
#define MATCH_STATUS_ERROR				4									// 오류

/**
 * @struct sRawLogInfo
 * @brief GPS 로그 정보
*/
typedef struct sRawLogInfo
{
	char							szDeviceID[56+1];					// 디바이스 ID (DEVICE_KEY_YYYYMMDD_운행순번4자리)
	char							szDeviceKey[47+1];					// 디바이스 키 (모바일 앱 인증 키)
	char							szTripID[60+1];						// 운행 ID (수집서버 적재: {DEVICE_KEY}_{YYYYMMDDHH24MISS})
	sint16							nTripEvent;							// TRIP_EVENT SMALLINT (0:START, 1:NONE, 2:END)
	sint16							nDriveStatus;						// DRIVE_STATUS SMALLINT (0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING)
	uint32							dwSeqNo;							// 순번
	bool							bGpsLatNull;						// GPS_LAT NULL 여부
	bool							bGpsLonNull;						// GPS_LON NULL 여부
	bool							bRawVldKnown;						// RAW_VLD 컬럼 조회 여부
	bool							bRawVld;							// RAW_VLD TRUE (bRawVldKnown 일 때만 유효)
	double							dfX;								// X 좌표
	double							dfY;								// Y 좌표
	float							fSpeed;								// 순간 속도 (km/h)
	time_t							dtGPS;								// GPS 수신 시간
	time_t							dtRecv;								// 수집서버 수신 시간 (서버 수신 시간)
	sint16							nAngle;								// 방위각 (0~359, -1:사용안함)
	sint16							nAccuracyM;							// 수평 오차(m). NO_ACCURACY=미적용
	sint16							nAltitudeM;							// 고도(m). NO_ALTITUDE=미적용
	uint8							nRoadRank;							// (미사용, 0 유지) 구 도로등급 힌트 필드
	uint8							nCoordinateType;					// 측지계 코드 (1:EPSG3857, 2:WGS84GEO, 3:KATECH, 4:BESSELGEO)
	
	sRawLogInfo() :
		nTripEvent(TRIP_EVENT_NONE),
		nDriveStatus(DRIVE_STATUS_ON_ROAD),
		dwSeqNo(0),
		bGpsLatNull(false),
		bGpsLonNull(false),
		bRawVldKnown(false),
		bRawVld(false),
		dfX(0.0), 
		dfY(0.0), 
		fSpeed(0.0f), 
		dtGPS(0), 
		dtRecv(0), 
		nAngle(-1), 
		nAccuracyM(NO_ACCURACY),
		nAltitudeM(NO_ALTITUDE),
		nRoadRank(0), 
		nCoordinateType(WGS84GEO)
	{
		memset(reinterpret_cast<void *>(szDeviceID), 0, sizeof(szDeviceID));
		memset(reinterpret_cast<void *>(szDeviceKey), 0, sizeof(szDeviceKey));
		memset(reinterpret_cast<void *>(szTripID), 0, sizeof(szTripID));
	}
} RAW_LOG_INFO, *PRAW_LOG_INFO;

#define RAW_LOG_INFO_SIZE												sizeof(RAW_LOG_INFO)

/**
 * @typedef RAW_LOG_BATCH
 * @brief 동일 운행(trip_id) 단위 원시 GPS 로그 묶음 (gps_dt·gps_seq 순)
*/
typedef vector<sRawLogInfo> RAW_LOG_BATCH;

/**
 * @struct sAltMatchCtx
 * @brief 연속 맵매칭 고도 보조 점수용 세션 컨텍스트 (trip_id 세션 → ProcessManager 전달)
 * @remark
 *   직전 매칭 성공 시 RawLogWorker가 GPS ALTITUDE_M·ROAD_TYPE을 기억.
 *   Δalt = 현재 nAltitudeM − nPrevAltitudeM, dfHorizMoveM로 경사(altitude_slope) 검사.
*/
typedef struct sAltMatchCtx
{
	sint16							nPrevAltitudeM;						// 직전 매칭 성공 시 GPS 고도(m). NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE
	bool							bHasPrevAlt;						// 직전 고도·도로유형 보유 여부
	double							dfHorizMoveM;						// 직전 매칭점→현재 GPS 수평거리(m)

	sAltMatchCtx() :
		nPrevAltitudeM(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		bHasPrevAlt(false),
		dfHorizMoveM(0.0)
	{}
} ALT_MATCH_CTX, *PALT_MATCH_CTX;

/**
 * @struct sMapMatchInput
 * @brief 맵 매칭 입력 정보 (CMapMatch Begin/Continue 호출용)
*/
typedef struct sMapMatchInput
{
	uint8							nCoordinateType;					// 측지계 코드 (1:EPSG3857, 2:WGS84GEO, 3:KATECH, 4:BESSELGEO)
	sint16							nRadius;							// 맵매칭 유효거리
	double 							dfX;								// X 좌표
	double 							dfY;								// Y 좌표
	sint16							nAngle;								// 방위각 (0~359, -1:사용안함)
	sint16							nSpeed;								// 순간속도 (km/h, -1:없음) — 방위각 가중치 적응용 (2026-07-08 최정우 추가)
	uint64							qwLinkID;							// 현재 주행중인 링크 ID (연속 측위)
	uint8							nRoadRank;							// (미사용, 0 유지) 구 도로등급 힌트 필드
	sint16							nSearchStep;						// 연속 측위시 탐색할 단계 (연속 측위, 기본:0, 0~최대검색단계)
	sint16							nAltitudeM;							// 현재 GPS 고도(m). NO_ALTITUDE=없음
	sint16							nPrevAltitudeM;						// 직전 매칭 GPS 고도(m). NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE
	sint16							nDriveStatus;						// DRIVE_STATUS (터널 시 고도 무시)
	double							dfHorizMoveM;						// 직전 매칭점→현재 GPS 수평거리(m)
	bool							bUseAltScore;						// 연속 맵매칭 고도 보조 점수 적용 여부

	sMapMatchInput() :
		nCoordinateType(WGS84GEO), 
		nRadius(NO_RADIUS), 
		dfX(0.0), 
		dfY(0.0), 
		nAngle(NO_ANGLE), 
		nSpeed(NO_SPEED), 
		qwLinkID(0), 
		nRoadRank(0), 
		nSearchStep(2),
		nAltitudeM(NO_ALTITUDE),
		nPrevAltitudeM(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		nDriveStatus(DRIVE_STATUS_ON_ROAD),
		dfHorizMoveM(0.0),
		bUseAltScore(false)
	{}
} MAP_MATCH_INPUT, *PMAP_MATCH_INPUT;

/**
 * @struct sMatchLinkInfo
 * @brief 매칭 링크 정보
 * @remark
 * 	- nConnect : 0:연결로 아님, 1:연결로 (MOCT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 * 	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sMatchLinkInfo
{
	uint16							wErrorCode;							// 에러 코드
	char							szErrorMsg[48];						// 에러 메시지
	double							dfMatchX;							// 매핑 X 좌표
	double							dfMatchY;							// 매핑 Y 좌표
	double							dfSgmtMatchLen;						// 세그먼트 시작 좌표에서 매핑 좌표까지의 거리(m)
	double                          dfIntersectLenSgmt;					// 요청 좌표(GPS)와 세그먼트 교차점까지의 거리
	sint16							nDirAngleDiff;						// 매핑 각도 차이
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점부터 세그먼트 시작점까지 거리 (m)
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별[3]
	uint8							nConnect;							// 연결 코드[3]
	uint8							nRoadType;							// 도로 유형[3]
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	double							dfStNodeX;							// 시작 노드 X
	double							dfStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성[3]
	uint64							qwEdNodeID;							// 종료 노드 ID
	double							dfEdNodeX;							// 종료 노드 X
	double							dfEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성[3]
} MATCH_LINK_INFO, *PMATCH_LINK_INFO;

#define MATCH_LINK_INFO_SIZE											sizeof(MATCH_LINK_INFO)

#endif //__MESSAGETYPE_H__
