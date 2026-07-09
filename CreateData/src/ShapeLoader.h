/**
 * @file ShapeLoader.h
 * @brief Shapefile(링크/노드) → 형상 정보 맵 생성 클래스 헤더 파일
 * @remark
 *   기존 CSQLManager::GetShapeInfoLoad() (Oracle) 를 대체.
 *   - 링크 PolyLine shapefile + 노드 Point shapefile 을 읽어
 *     CBinaryMaker 가 사용하는 두 맵(mapShapeLinkInfo / mapShapeNodeInfo) 을 생성한다.
 *   - 원본 좌표계를 WGS84 경위도(degree) 로 변환(CCoordConvert)하여 저장.
 *   - DBF 필드명 / 좌표계는 외부에서 설정(SHAPE_FIELD_MAP, eSourceCoord).
*/
#ifndef __SHAPELOADER_H__
#define __SHAPELOADER_H__

#include <string>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataFormat.h"
#include "CoordConvert.h"
#include "ShapeFile.h"

using namespace std;

/**
 * @enum eSourceCoord
 * @brief 입력 shapefile 의 좌표계 (WGS84 경위도로 변환하기 위함)
*/
enum eSourceCoord
{
	SRC_WGS84GEO = 1,
	SRC_GRS80GEO,
	SRC_GRS80TM,
	SRC_GRS80UTMK,
	SRC_KATECH,
	SRC_BESSELGEO,
	SRC_BESSELTM,
	SRC_EPSG3857,
	SRC_GRS80TM2010		// EPSG:5186 (Korea 2000 / Central Belt 2010, FN=600000) — MOCT 표준노드링크 실제 좌표계
};

/**
 * @struct sShapeFieldMap
 * @brief DBF 필드명 매핑 (링크/노드)
*/
typedef struct sShapeFieldMap
{
	string		strLinkID;
	string		strMaxSpeed;
	string		strLength;
	string		strRoadRank;
	string		strConnect;
	string		strRoadType;
	string		strLanes;
	string		strRoadName;
	string		strFNode;
	string		strTNode;

	string		strNodeID;
	string		strNodeType;

	sShapeFieldMap() : 
		strLinkID("LINK_ID"), strMaxSpeed("MAX_SPD"), strLength("LENGTH"), 
		strRoadRank("ROAD_RANK"), strConnect("CONNECT"), strRoadType("ROAD_TYPE"), 
		strLanes("LANES"), strRoadName("ROAD_NAME"), strFNode("F_NODE"), strTNode("T_NODE"), 
		strNodeID("NODE_ID"), strNodeType("NODE_TYPE")
	{}
} SHAPE_FIELD_MAP, *PSHAPE_FIELD_MAP;

/**
 * @class CShapeFileLoader
 * @brief Shapefile → 형상 맵 생성
*/
class CShapeFileLoader
{
public:
	CShapeFileLoader();
	virtual ~CShapeFileLoader();

	bool Initialize(const string& strLinkShp, const string& strNodeShp, 
		int nSourceCoord, const SHAPE_FIELD_MAP& stFieldMap);

	bool Load(mapShapeLinkInfo *pmapShapeLinkInfoList, mapShapeNodeInfo *pmapShapeNodeInfoList);

private:
	typedef struct sNodeData
	{
		double	dfLon;
		double	dfLat;
		uint8	nNodeType;
		sNodeData() : dfLon(0.0), dfLat(0.0), nNodeType(0) {}
	} NODE_DATA;

	bool LoadNode(unordered_map<uint64, NODE_DATA>& mapNode);
	void ConvertToWGS84(double dfX, double dfY, double& dfLon, double& dfLat);

private:
	CCoordConvert			m_cCoordConvert;
	string					m_strLinkShp;
	string					m_strNodeShp;
	int						m_nSourceCoord;
	SHAPE_FIELD_MAP			m_stFieldMap;
};

#endif //__SHAPELOADER_H__
