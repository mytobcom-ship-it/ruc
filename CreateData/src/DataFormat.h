/**
 * @file DataFormat.h
 * @brief 세그먼트 및 링크 정보 구조 헤더 파일
*/
#ifndef __DATAFOFORMAT_H__
#define __DATAFOFORMAT_H__

#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataDefine.h"

using namespace std;

#define INVALID_GRID_COL_NO			-1									// 유효하지 않은 GRID X 좌표 번호
#define INVALID_GRID_ROW_NO			-1									// 유효하지 않은 GRID Y 좌표 번호
#define INVALID_GRID_ID				-1									// 유효하지 않은 GRID ID

#pragma pack(push, 1)

/**
 * @struct sPoint
 * @brief 좌표 정보
*/
typedef struct sPoint
{
	double							dfX;								// X 좌표
	double							dfY;								// Y 좌표

	sPoint() : 
		dfX(0.0), 
		dfY(0.0)
	{}

	bool operator==(const struct sPoint& stPoint) const
	{
		if (dfX != stPoint.dfX) return false;
		return dfY == stPoint.dfY;
	}

	bool operator!=(const struct sPoint& stPoint) const
	{
		if (dfX != stPoint.dfX) return true;
		return dfY != stPoint.dfY;
	}
} POINT, *PPOINT;

#define POINT_SIZE														sizeof(POINT)

/**
 * @struct sShapeLinkInfo
 * @brief 링크, 노드 형상 속성 정보 [생성 전용]
 * @remark
 * 	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 * 	- nConnect : 0:연결로 아님, 1:연결로 (MOCT_LINK.CONNECT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 * 	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sShapeLinkInfo
{
	uint64							qwLinkID;							// 링크 ID
	uint32							dwSgmtOffset;						// 세그먼트 Offset (0~n)
	uint16							wSgmtCount;							// 링크에 해당하는 세그먼트 개수
	uint32							dwTurnOffset;						// 진출 링크 회전정보 Offset
	uint8							nTurnCount;							// 진출 링크의 개수
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별[3]
	uint8							nConnect;							// 연결로 코드[3]
	uint8							nRoadType;							// 도로 유형[3]
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	uint32							dwStNodeX;							// 시작 노드 X
	uint32							dwStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성[3]
	uint64							qwEdNodeID;							// 종료 노드 ID
	uint32							dwEdNodeX;							// 종료 노드 X
	uint32							dwEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성[3]
	vector<POINT>					vtVertexs;							// 링크 좌표 정보

	sShapeLinkInfo() :
		dwSgmtOffset(0),
		wSgmtCount(0),
		dwTurnOffset(0),
		nTurnCount(0)
	{
		memset(szRoadName, 0, sizeof(szRoadName));
	}
} SHAPE_LINK_INFO, *PSHAPE_LINK_INFO;

#define SHAPE_LINK_INFO_SIZE											sizeof(SHAPE_LINK_INFO)
typedef map<uint64, PSHAPE_LINK_INFO>									mapShapeLinkInfo;

/**
 * @struct sShapeNodeInfo
 * @brief 링크 좌표 정보 [생성 전용]
*/
typedef struct sShapeNodeInfo
{
	uint64							qwLinkID;							// 링크 ID
	vector<POINT>					vtVertexs;							// 링크 좌표 정보

	sShapeNodeInfo() :
		qwLinkID(0)
	{}
} SHAPE_NODE_INFO, *PSHAPE_NODE_INFO;

#define SHAPE_NODE_INFO_SIZE											sizeof(SHAPE_NODE_INFO)
typedef multimap<uint64, PSHAPE_NODE_INFO>								mapShapeNodeInfo;

/**
 * @struct sDataFileHead
 * @brief 데이터 파일 헤더
*/
typedef struct sDataFileHead
{
	uint32							dwGridInfoCount;					// 그리드별 세그먼트 범위 개수
	uint32							dwGridSgmtInfoCount;				// 그리드별 세그먼트 정보 개수
	uint32							dwLinkSgmtInfoCount;				// 링크별 세그먼트 정보 개수
	uint32							dwLinkInfoCount;					// 링크별 세그먼트 정보 개수
	uint32							dwTurnInfoCount;					// 시작 링크 기준으로 연결된 링크 회전 정보 개수
	uint32							dwGridInfoStartOffset;				// 그리드별 세그먼트 범위 시작 Offset
	uint32							dwGridInfoSize;						// 그리드별 세그먼트 범위 크기
	uint32							dwGridSgmtInfoStartOffset;			// 그리드별 세그먼트 정보 시작 Offset
	uint32							dwGridSgmtInfoSize;					// 그리드별 세그먼트 정보 크기
	uint32							dwLinkSgmtInfoStartOffset;			// 링크별 세그먼트 정보 시작 Offset
	uint32							dwLinkSgmtInfoSize;					// 링크별 세그먼트 정보 크기
	uint32							dwLinkInfoStartOffset;				// 링크 정보 시작 Offset
	uint32							dwLinkInfoSize;						// 링크 정보 크기
	uint32							dwTurnInfoStartOffset;				// 시작 링크 기준으로 연결된 링크 회전 정보 시작 Offset
	uint32							dwTurnInfoSize;						// 시작 링크 기준으로 연결된 링크 회전 정보 크기
} DATA_FILE_HEAD, *PDATA_FILE_HEAD;

#define DATA_FILE_HEAD_SIZE												sizeof(DATA_FILE_HEAD)

/**
 * @struct sGridInfo
 * @brief 그리드별 세그먼트 범위
*/
typedef struct sGridInfo
{
	uint32							dwGridID;							// 그리드 아이디
	uint32							dwSgmtOffset;						// 그리드별 세그먼트 시작 Offset
	uint16							wSgmtCount;							// 그리드별 세그먼트 개수

	sGridInfo() :
		dwGridID(0), 
		dwSgmtOffset(0), 
		wSgmtCount(0)
	{}
} GRID_INFO, *PGRID_INFO;

#define GRID_INFO_SIZE													sizeof(GRID_INFO)

/**
 * @struct sGridNoInfo
 * @brief 그리드 X,Y 좌표 번호 [생성 전용]
*/
typedef struct sGridNoInfo
{
	sint32							nColNo;								// 그리드 X 좌표 번호
	sint32							nRowNo;								// 그리드 Y 좌표 번호

	sGridNoInfo() :
		nColNo(INVALID_GRID_COL_NO),
		nRowNo(INVALID_GRID_ROW_NO)
	{}
} GRID_NO_INFO, *PGRID_NO_INFO;

#define GRID_NO_INFO_SIZE												sizeof(GRID_NO_INFO)

/**
 * @struct sGridNoMinMaxInfo
 * @brief 그리드 X,Y 좌표 번호 최소,최대 값 [생성 전용]
*/
typedef struct sGridNoMinMaxInfo
{
	uint32							dwColNoMin;							// 그리드 X 좌표 번호 최소 값
	uint32							dwColNoMax;							// 그리드 X 좌표 번호 최대 값
	uint32							dwRowNoMin;							// 그리드 X 좌표 번호 최소 값
	uint32							dwRowNoMax;							// 그리드 Y 좌표 번호 최대 값

	sGridNoMinMaxInfo() :
		dwColNoMin(0),
		dwColNoMax(0),
		dwRowNoMin(0),
		dwRowNoMax(0)
	{}
} GRID_NO_MIN_MAX_INFO, *PGRID_NO_MIN_MAX_INFO;

#define GRID_NO_MIN_MAX_INFO_SIZE										sizeof(GRID_NO_MIN_MAX_INFO)

/**
 * @struct sGridSgmtInfo
 * @brief GRID 별 세그먼트 정보
*/
typedef struct sGridSgmtInfo
{
	uint32							dwSgmtOffset;						// 세그먼트 Offset (0~n)
	uint32							dwX;								// 세그먼트 X 좌표 (Degree * 360000)
	uint32							dwY;								// 세그먼트 Y 좌표 (Degree * 360000)
	uint16							wDirAng;							// 세그먼트 진행 방향 (방위각, Degree)
	uint16							wLenSgmt;							// 세그먼트의 거리 (m)
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점부터 세그먼트 시작점까지 거리 (m)

	bool operator<(const struct sGridSgmtInfo& stGridSgmtInfo) const
	{
		return qwLinkID < stGridSgmtInfo.qwLinkID;
	}
} GRID_SGMT_INFO, *PGRID_SGMT_INFO;

#define GRID_SGMT_INFO_SIZE												sizeof(GRID_SGMT_INFO)
typedef multiset<GRID_SGMT_INFO>										setGridSgmtInfo;
typedef unordered_map<uint32, setGridSgmtInfo>							mapGridSgmtInfo;

/**
 * @struct sLinkSgmtInfo
 * @brief 링크별 세그먼트 정보
*/
typedef struct sLinkSgmtInfo
{
	uint32							dwSgmtOffset;						// 세그먼트 Offset (0~n)
	uint32							dwX;								// 세그먼트 X 좌표 (Degree * 360000)
	uint32							dwY;								// 세그먼트 Y 좌표 (Degree * 360000)
	uint16							wDirAng;							// 세그먼트 진행 방향 (방위각, Degree)
	uint16							wLenSgmt;							// 세그먼트 거리 (m)
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점부터 세그먼트 시작점까지 거리 (m)

	sLinkSgmtInfo() : 
		dwSgmtOffset(0), 
		dwX(0), 
		dwY(0), 
		wDirAng(0), 
		qwLinkID(0), 
		wLenFromLink(0)
	{}
} LINK_SGMT_INFO, *PLINK_SGMT_INFO;

#define LINK_SGMT_INFO_SIZE												sizeof(LINK_SGMT_INFO)

/**
 * @struct sLinkInfoData
 * @brief 세그먼트별 링크 정보 데이터
 * @remark
 * 	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 *	- nConnect : 0:연결로 아님, 1:연결로 (MOCT_LINK.CONNECT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 *	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sLinkInfoData
{
	uint64							qwLinkID;							// 링크 ID
	uint32                          dwSgmtOffset;						// 링크에 해당하는 세그먼트 시작 Offset
	uint16							wSgmtCount;							// 링크에 해당하는 세그먼트 개수
	uint32							dwTurnOffset;						// 진출 링크에 해당하는 회전 정보 Offset
	uint8							nTurnCount;							// 진출 링크의 개수
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별
	uint8							nConnect;							// 연결로 코드
	uint8							nRoadType;							// 도로 유형
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	uint32							dwStNodeX;							// 시작 노드 X
	uint32							dwStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성
	uint64							qwEdNodeID;							// 종료 노드 ID
	uint32							dwEdNodeX;							// 종료 노드 X
	uint32							dwEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성[3]
} LINK_INFO_DATA, *PLINK_INFO_DATA;

#define LINK_INFO_DATA_SIZE												sizeof(LINK_INFO_DATA)

/**
 * @struct sLinkInfo
 * @brief 세그먼트별 링크 정보
 * @remark
 * 	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 *	- nConnect : 0:연결로 아님, 1:연결로 (MOCT_LINK.CONNECT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 *	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sLinkInfo
{
	uint32                          dwSgmtOffset;						// 링크에 해당하는 세그먼트 시작 Offset
	uint16							wSgmtCount;							// 링크에 해당하는 세그먼트 개수
	uint32							dwTurnOffset;						// 진출 링크에 해당하는 회전 정보 Offset
	uint8							nTurnCount;							// 진출 링크의 개수
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별
	uint8							nConnect;							// 연결로 코드
	uint8							nRoadType;							// 도로 유형
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	uint32							dwStNodeX;							// 시작 노드 X
	uint32							dwStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성
	uint64							qwEdNodeID;							// 종료 노드 ID
	uint32							dwEdNodeX;							// 종료 노드 X
	uint32							dwEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성
} LINK_INFO, *PLINK_INFO;

#define LINK_INFO_SIZE													sizeof(LINK_INFO)
typedef unordered_map<uint64, LINK_INFO>								mapLinkInfo;

#define TURN_TYPE_UNKNOWN				0									// MOCT TURN_TYPE 미정의
#define TURN_OPER_ALLOW					0									// MOCT TURN_OPER 허용
#define TURN_OPER_RESTRICT				1									// MOCT TURN_OPER 제한(금지)

/**
 * @struct sTurnInfoDisk
 * @brief link.psf 디스크 저장용 회전 정보 (MapMatchSvr 호환 22바이트)
*/
typedef struct sTurnInfoDisk
{
	uint32							dwTurnOffset;						// 진출 링크 회전정보 Offset
	uint64							qwInLinkID;							// 진입 링크 ID
	uint64							qwOutLinkID;						// 진출 링크 ID
	sint16							nTurnAng;							// 진입 링크에서 진출 링크의 회전 각도 (Degree, -180~180)
} TURN_INFO_DISK, *PTURN_INFO_DISK;

#define TURN_INFO_DISK_SIZE												sizeof(TURN_INFO_DISK)

/**
 * @struct sTurnInfo
 * @brief 시작 링크 기준으로 연결된 링크 회전 정보 [생성 전용]
 * @remark
 * 	- nTurnType : MOCT TURNINFO.TURN_TYPE (011→11, 101, 102, 103 …)
 * 	- nTurnOper : MOCT TURNINFO.TURN_OPER (0:허용, 1:제한). 제한(1)은 바이너리에 미포함
*/
typedef struct sTurnInfo
{
	uint32							dwTurnOffset;						// 진출 링크 회전정보 Offset
	uint64							qwInLinkID;							// 진입 링크 ID
	uint64							qwOutLinkID;						// 진출 링크 ID
	sint16							nTurnAng;							// 진입 링크에서 진출 링크의 회전 각도 (Degree, -180~180)
	uint16							nTurnType;							// MOCT 회전 유형 코드
	uint8							nTurnOper;							// MOCT 회전 허용/제한 (0/1)

	bool operator<(const struct sTurnInfo& stTurnInfo) const
	{
		return nTurnAng < stTurnInfo.nTurnAng;
	}
} TURN_INFO, *PTURN_INFO;

#define TURN_INFO_SIZE													sizeof(TURN_INFO)

#pragma pack(pop)

#endif //__DATAFOFORMAT_H__
