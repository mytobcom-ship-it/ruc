/**
 * @file GISUtil.h
 * @brief 맵매칭 유틸리티 클래스 헤더 파일 (WGS84 경위도 용)
*/
#ifndef __GISUTIL_H__
#define __GISUTIL_H__

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <vector>
#include <set>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "DataDefine.h"
#include "DataFormat.h"
#include "CoordConvert.h"
#include "MessageType.h"

using namespace std;

#define GRID_CELL_SIZE				0.003								// GRID 크기 (300m)
#define X_GRID_COUNT				2667								// 경도 GRID 컬럼 개수
#define Y_GRID_COUNT				2334								// 위도 GRID 컬럼 개수

#define DEG(x)						((x) * 180 / M_PI)					// 라디안을 각도로 변환
#define RAD(x)						((x) * M_PI / 180)					// 각도를 라디안으로 변환

/**
 * @struct sSgmtMatchInput
 * @brief 세그먼트 매칭 입력 정보
*/
typedef struct sSgmtMatchInput
{
	POINT							stPoint;							// X, Y 좌표
	sint16							nRadius;							// 맵매칭 유효거리
	sint16							nDirAng;							// 진행 방향 (방위각)
	uint8							nRoadRank;							// (미사용, 0 유지) 구 도로등급 힌트 필드
	sint16							nSpeed;								// 순간속도(km/h, -1:없음) — 방위각 가중치 적응용 (2026-07-08 최정우 추가)
	// 연속 맵매칭 고도(ALTITUDE_M) — Begin 미사용, bUseAltScore=true 일 때만 CalcAltRoadPenalty 적용
	sint16							nAltitudeM;							// 현재 GPS 고도(m). NO_ALTITUDE=없음
	sint16							nPrevAltitudeM;						// 직전 매칭 GPS 고도(m). NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE
	sint16							nDriveStatus;						// DRIVE_STATUS (터널 시 고도 무시)
	double							dfHorizMoveM;						// 직전 매칭점→현재 GPS 수평거리(m)
	bool							bUseAltScore;						// 연속 맵매칭 고도 보조 점수 적용 여부

	sSgmtMatchInput() :
		nRadius(0), 
		nDirAng(NO_ANGLE),  
		nRoadRank(0), 
		nSpeed(NO_SPEED),
		nAltitudeM(NO_ALTITUDE),
		nPrevAltitudeM(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		nDriveStatus(0),
		dfHorizMoveM(0.0),
		bUseAltScore(false)
	{}
} SGMT_MATCH_INPUT, *PSGMT_MATCH_INPUT;

/**
 * @struct sGridBorderDist
 * @brief GRID 경계·모서리까지의 거리(m)
*/
typedef struct sGridBorderDist
{
	double							dfLeftDist;							// GRID 왼쪽 경계 거리
	double							dfRightDist;							// GRID 오른쪽 경계 거리
	double							dfTopDist;							// GRID 위 경계 거리
	double							dfBottomDist;						// GRID 아래 경계 거리
	double							dfLeftTopDist;						// GRID 왼쪽 위 모서리 경계 거리
	double							dfLeftBottomDist;					// GRID 왼쪽 아래 모서리 경계 거리
	double							dfRightTopDist;						// GRID 오른쪽 위 모서리 경계 거리
	double							dfRightBottomDist;					// GRID 오른쪽 아래 모서리 경계 거리

	sGridBorderDist() :
		dfLeftDist(0), 
		dfRightDist(0), 
		dfTopDist(0), 
		dfBottomDist(0),
		dfLeftTopDist(0), 
		dfLeftBottomDist(0), 
		dfRightTopDist(0), 
		dfRightBottomDist(0)
	{}
} GRID_BORDER_DIST, *PGRID_BORDER_DIST;

/**
 * @struct sSgmtInfo
 * @brief 세그먼트 정보
*/
typedef struct sSgmtInfo
{
	POINT							stPoint;							// 세그먼트 X, Y 좌표
	sint16							nDirAng;							// 세그먼트 진행 방향 (방위각)
	double							dfLen;								// 세그먼트 길이
	uint64							qwLinkID;							// 링크 ID

	sSgmtInfo() :
		nDirAng(0), 
		dfLen(0), 
		qwLinkID(0)
	{}
} SGMT_INFO, *PSGMT_INFO;

/**
 * @struct sSgmtMatchRes
 * @brief 세그먼트 맵매칭 결과
*/
typedef struct sSgmtMatchRes
{
	POINT							stMatchPoint;						// 매칭 X,Y 좌표
	double							dfSgmtMatchLen;						// 세그먼트 시작점부터 교차점까지의 거리
	double							dfIntersectLenSgmt;					// 요청 좌표와 세그먼트 교차점까지의 거리
	double							dfCost;								// 소프트 비용 = 수직거리(m) + w_a·|방위각차| (2026-07-08 최정우 추가)
	sint16							nDirAngleDiff;						// 주행방향 각도 차이
	uint64							qwLinkID;							// 링크 ID

	sSgmtMatchRes() :
		dfSgmtMatchLen(0.0), 
		dfIntersectLenSgmt(0.0), 
		dfCost(0.0), 
		nDirAngleDiff(0), 
		qwLinkID(0)
	{}
} SGMT_MATCH_RES, *PSGMT_MATCH_RES;

/**
 * @class CGISUtil
 * @brief GIS 유틸리티 클래스
*/
class CGISUtil
{
public:
	CGISUtil();
	virtual ~CGISUtil();
	const uint32 GetGridID(double& dfX, double& dfY);
	inline const uint32 GetMaxGridCount() { return X_GRID_COUNT * Y_GRID_COUNT; }
	inline const uint32 GetGridID(uint32 dwColNo, uint32 dwRowNo) { return (dwRowNo * X_GRID_COUNT) + dwColNo; }
	const sint32 GetGridColNo(double& dfX);
	const sint32 GetGridRowNo(double& dfY);
	bool IsCrossSgmt2Grid(POINT& stPoint1, POINT& stPoint2, 
		uint32& dwGridColNo, uint32& dwGridRowNo);
	bool IsCrossSgmt2Sgmt(POINT& stPoint1, POINT& stPoint2, 
		double& dfXMin, double& dfYMin, double& dfXMax, double& dfYMax);
	const uint16 GetSgmtLength(const POINT& stPoint1, const POINT& stPoint2);
	const double GridBorderDistance(const uint32& dwGridID, const double& dfX, 
		const double& dfY, GRID_BORDER_DIST& stGridBorderDist);
	const uint8 GridSplitIndex(const uint32& dwGridID, const double& dfX, const double& dfY);
	void GetNearGridID(const uint32& dwGridID, const SGMT_MATCH_INPUT& stSgmtMatchInput, 
		vector<uint32>& vtNearGridIDList);
	bool SgmtMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, SGMT_INFO& stSgmtInfo, SGMT_MATCH_RES *pstSgmtMatchRes);
	double CalcAltRoadPenalty(const SGMT_MATCH_INPUT& stSgmtMatchInput, uint8 nCandRoadType,
		const ALTITUDE_SCORE_CONFIG& stAltConfig) const;
	sint16 GetAngleDiff(sint16& nAngle1, sint16& nAngle2);
	bool GetDirAngle(POINT& stSgmtPoint, POINT& stPoint, sint16 *pnDirAngle);
	const sint16 GetDirAngleDegree(POINT& stPoint1, POINT& stPoint2);
	const double GetDistanceGEO1(POINT& stPoint, POINT& stIntersect);
	const double GetDistanceGEO2(POINT& stPoint, POINT& stIntersect);
};

#endif //__GISUTIL_H__
