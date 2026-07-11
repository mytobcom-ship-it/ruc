/**
 * @file ShapeLoader.cpp
 * @brief Shapefile(링크/노드) → 형상 정보 맵 생성 클래스 소스 파일
*/
#include "ShapeLoader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <iconv.h>
#include "log4z.h"
#include "Clock.h"
#include "GISUtil.h"

using namespace zsummer::log4z;

namespace {

bool IsAsciiOnly(const string& strValue)
{
	for (size_t i=0; i<strValue.size(); ++i)
	{
		if (static_cast<unsigned char>(strValue[i]) > 0x7F)
			return false;
	}
	return true;
}

bool IsValidUtf8(const string& strValue)
{
	const unsigned char *pBytes = reinterpret_cast<const unsigned char *>(strValue.c_str());
	size_t nSize = strValue.size();
	size_t i = 0;

	while (i < nSize)
	{
		unsigned char cByte = pBytes[i];
		size_t nCharLen = 0;

		if (cByte <= 0x7F) nCharLen = 1;
		else if ((cByte & 0xE0) == 0xC0) nCharLen = 2;
		else if ((cByte & 0xF0) == 0xE0) nCharLen = 3;
		else if ((cByte & 0xF8) == 0xF0) nCharLen = 4;
		else return false;

		if (i + nCharLen > nSize)
			return false;

		for (size_t j=1; j<nCharLen; ++j)
		{
			if ((pBytes[i + j] & 0xC0) != 0x80)
				return false;
		}
		i += nCharLen;
	}
	return true;
}

bool IconvConvert(const char *pszToEnc, const char *pszFromEnc,
		const string& strInput, string& strOutput)
{
	iconv_t hIconv = iconv_open(pszToEnc, pszFromEnc);
	if (hIconv == (iconv_t)-1)
		return false;

	string strInBuf = strInput;
	size_t nInLeft = strInBuf.size();
	size_t nOutCapacity = strInput.size() * 4 + 16;
	string strOutBuf(nOutCapacity, '\0');
	char *pszIn = &strInBuf[0];
	char *pszOut = &strOutBuf[0];
	size_t nOutLeft = nOutCapacity;

	if (iconv(hIconv, &pszIn, &nInLeft, &pszOut, &nOutLeft) == (size_t)-1)
	{
		iconv_close(hIconv);
		return false;
	}

	iconv_close(hIconv);
	strOutput.assign(strOutBuf.data(), nOutCapacity - nOutLeft);
	return true;
}

bool ToUtf8RoadName(const string& strRaw, string& strUtf8)
{
	strUtf8.clear();

	string strTrimmed = strRaw;
	while (!strTrimmed.empty() && (strTrimmed.back() == ' ' || strTrimmed.back() == '\t'))
		strTrimmed.pop_back();

	size_t nStart = 0;
	while (nStart < strTrimmed.size() && (strTrimmed[nStart] == ' ' || strTrimmed[nStart] == '\t'))
		++nStart;
	strTrimmed = strTrimmed.substr(nStart);

	if (strTrimmed.empty())
		return true;

	if (IsAsciiOnly(strTrimmed) || IsValidUtf8(strTrimmed))
	{
		strUtf8 = strTrimmed;
		return true;
	}

	if (IconvConvert("UTF-8", "CP949", strTrimmed, strUtf8))
		return true;

	if (IconvConvert("UTF-8", "EUC-KR", strTrimmed, strUtf8))
		return true;

	return false;
}

void TruncateRoadNameDelimiter(string& strRoadName)
{
	size_t nPos = strRoadName.find_first_of("- ");
	if (nPos != string::npos)
		strRoadName.erase(nPos);
}

void CopyUtf8RoadName(const string& strUtf8, char *pszDst, size_t nDstSize)
{
	if (pszDst == nullptr || nDstSize == 0)
		return;

	memset(pszDst, 0, nDstSize);
	size_t nMaxBytes = nDstSize - 1;
	size_t i = 0;

	while (i < strUtf8.size())
	{
		unsigned char cByte = static_cast<unsigned char>(strUtf8[i]);
		size_t nCharLen = 1;

		if (cByte <= 0x7F) nCharLen = 1;
		else if ((cByte & 0xE0) == 0xC0) nCharLen = 2;
		else if ((cByte & 0xF0) == 0xE0) nCharLen = 3;
		else if ((cByte & 0xF8) == 0xF0) nCharLen = 4;
		else break;

		if (i + nCharLen > nMaxBytes)
			break;

		i += nCharLen;
	}

	if (i > 0)
		memcpy(pszDst, strUtf8.c_str(), i);
	pszDst[i] = '\0';
}

} // namespace

/**
 * @brief 생성자
*/
CShapeFileLoader::CShapeFileLoader() : 
	m_nSourceCoord(SRC_WGS84GEO)
{
}

/**
 * @brief 소멸자
*/
CShapeFileLoader::~CShapeFileLoader()
{
}

/**
 * @brief 초기화
 * @param[in] strLinkShp 링크 PolyLine shapefile 경로
 * @param[in] strNodeShp 노드 Point shapefile 경로 (빈 문자열이면 링크 끝점으로 대체)
 * @param[in] nSourceCoord 입력 좌표계 (eSourceCoord)
 * @param[in] stFieldMap DBF 필드 매핑
 * @return true(성공), false(실패)
*/
bool CShapeFileLoader::Initialize(const string& strLinkShp, const string& strNodeShp, 
		int nSourceCoord, const SHAPE_FIELD_MAP& stFieldMap)
{
	m_strLinkShp = strLinkShp;
	m_strNodeShp = strNodeShp;
	m_nSourceCoord = nSourceCoord;
	m_stFieldMap = stFieldMap;

	if (m_strLinkShp.empty())
	{
		LOGFMTE("link shapefile path is empty!");
		return false;
	}

	if ((m_nSourceCoord < SRC_WGS84GEO) || (m_nSourceCoord > SRC_GRS80TM2010))
	{
		LOGFMTE("source coordinate type is invalid![%d]", m_nSourceCoord);
		return false;
	}

	return true;
}

/**
 * @brief 입력 좌표(X=동향/경도, Y=북향/위도) → WGS84 경위도 변환
 * @param[in] dfX 입력 X 좌표 (경도 또는 Easting)
 * @param[in] dfY 입력 Y 좌표 (위도 또는 Northing)
 * @param[out] dfLon WGS84 경도
 * @param[out] dfLat WGS84 위도
 * @return void
 * @remark CCoordConvert 변환 함수는 (inLat=Y, inLon=X) 순서로 호출 (투영계는 Northing/Easting)
*/
void CShapeFileLoader::ConvertToWGS84(double dfX, double dfY, double& dfLon, double& dfLat)
{
	double dfOutLat = 0.0;
	double dfOutLon = 0.0;

	switch (m_nSourceCoord)
	{
	case SRC_WGS84GEO:
		dfLon = dfX;
		dfLat = dfY;
		return;
	case SRC_GRS80GEO:
		// GRS80 경위도 → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.GRS80GEOToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_GRS80TM:
		// GRS80 TM → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.GRS80TMToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_GRS80UTMK:
		// GRS80 UTM-K → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.GRS80UTMKToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_KATECH:
		// KATECH → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.KATECHToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_BESSELGEO:
		// Bessel 경위도 → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.BESSELGEOToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_BESSELTM:
		// Bessel TM → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.BESSELTMToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_EPSG3857:
		// EPSG3857 → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.EPSG3857ToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	case SRC_GRS80TM2010:
		// GRS80 TM2010 → WGS84 경위도 변환 (2026-07-08 최정우 주석 추가)
		m_cCoordConvert.GRS80TM2010ToWGS84GEO(dfY, dfX, &dfOutLat, &dfOutLon);
		break;
	default:
		dfLon = dfX;
		dfLat = dfY;
		return;
	}

	dfLon = dfOutLon;
	dfLat = dfOutLat;
}

/**
 * @brief 노드 shapefile 로딩 (노드 ID → 좌표/속성)
 * @param[out] mapNode 노드 ID → NODE_DATA
 * @return true(성공), false(실패)
*/
bool CShapeFileLoader::LoadNode(unordered_map<uint64, NODE_DATA>& mapNode)
{
	CShapeFile cShapeFile;
	// 노드 shapefile 열기 (2026-07-08 최정우 주석 추가)
	if (!cShapeFile.Open(m_strNodeShp))
	{
		LOGFMTE("node shapefile open failed!file=[%s]", m_strNodeShp.c_str());
		return false;
	}

	uint32 dwCount = cShapeFile.GetRecordCount();
	for (uint32 i=0; i<dwCount; ++i)
	{
		POINT stPoint;
		// i번째 Point 형상 좌표 읽기 (2026-07-08 최정우 주석 추가)
		if (!cShapeFile.GetPoint(i, stPoint))
			continue;

		// 노드 ID DBF 필드값 읽기 (2026-07-08 최정우 주석 추가)
		uint64 qwNodeID = static_cast<uint64>(cShapeFile.GetLong(i, m_stFieldMap.strNodeID));
		if (qwNodeID == 0)
			continue;

		NODE_DATA stNodeData;
		// 노드 좌표를 WGS84 경위도로 변환 (2026-07-08 최정우 주석 추가)
		ConvertToWGS84(stPoint.dfX, stPoint.dfY, stNodeData.dfLon, stNodeData.dfLat);
		// 노드 유형 DBF 필드값 읽기 (2026-07-08 최정우 주석 추가)
		stNodeData.nNodeType = static_cast<uint8>(cShapeFile.GetLong(i, m_stFieldMap.strNodeType));

		mapNode[qwNodeID] = stNodeData;
	}

	LOGFMTI("node load complete!count=[%zu]", mapNode.size());
	return true;
}

/**
 * @brief 형상 정보 로딩 (CSQLManager::GetShapeInfoLoad 대체)
 * @param[out] pmapShapeLinkInfoList 링크 형상 정보 (키:링크 ID)
 * @param[out] pmapShapeNodeInfoList 노드 형상 정보 (키:링크 시작 노드 ID)
 * @return true(성공), false(실패)
*/
bool CShapeFileLoader::Load(mapShapeLinkInfo *pmapShapeLinkInfoList, 
		mapShapeNodeInfo *pmapShapeNodeInfoList)
{
	if ((pmapShapeLinkInfoList == nullptr) || (pmapShapeNodeInfoList == nullptr))
	{
		LOGFMTE("shape info map is null!");
		return false;
	}

	LOGFMTI("shapefile geometry loading start!");

	CClock cClock;
	// 형상 로딩 소요시간 측정 시작 (2026-07-08 최정우 주석 추가)
	cClock.Start();

	CGISUtil cGISUtil;

	// 노드 좌표/속성 lookup (노드 shp 가 지정된 경우)
	unordered_map<uint64, NODE_DATA> mapNode;
	bool bUseNode = !m_strNodeShp.empty();
	if (bUseNode)
	{
		// 노드 shapefile 로딩 (2026-07-08 최정우 주석 추가)
		if (!LoadNode(mapNode))
		{
			// 형상 로딩 소요시간 측정 종료 (2026-07-08 최정우 주석 추가)
			cClock.Stop();
			return false;
		}
	}
	else
		LOGFMTW("node shapefile not set! node coord/type derived from link endpoints.");

	// 링크 shapefile 로딩
	CShapeFile cLinkShape;
	// 링크 shapefile 열기 (2026-07-08 최정우 주석 추가)
	if (!cLinkShape.Open(m_strLinkShp))
	{
		// 형상 로딩 소요시간 측정 종료 (2026-07-08 최정우 주석 추가)
		cClock.Stop();
		LOGFMTE("link shapefile open failed!file=[%s]", m_strLinkShp.c_str());
		return false;
	}

	// 링크 길이 DBF 필드 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	bool bHasLength = cLinkShape.HasField(m_stFieldMap.strLength);

	uint32 dwCount = cLinkShape.GetRecordCount();
	for (uint32 i=0; i<dwCount; ++i)
	{
		// 형상(버텍스) 읽기
		vector<POINT> vtRawVertexs;
		// i번째 PolyLine 형상(버텍스) 읽기 (2026-07-08 최정우 주석 추가)
		if (!cLinkShape.GetPolyLine(i, vtRawVertexs))
			continue;
		if (vtRawVertexs.size() < 2)
			continue;

		// 링크 ID DBF 필드값 읽기 (2026-07-08 최정우 주석 추가)
		uint64 qwLinkID = static_cast<uint64>(cLinkShape.GetLong(i, m_stFieldMap.strLinkID));
		if (qwLinkID == 0)
			continue;

		PSHAPE_LINK_INFO pstShapeLinkInfo = new (std::nothrow)SHAPE_LINK_INFO;
		if (pstShapeLinkInfo == nullptr)
		{
			LOGFMTE("link shape info memory allocate failed!");
			return false;
		}

		PSHAPE_NODE_INFO pstShapeNodeInfo = new (std::nothrow)SHAPE_NODE_INFO;
		if (pstShapeNodeInfo == nullptr)
		{
			delete pstShapeLinkInfo;
			LOGFMTE("node shape info memory allocate failed!");
			return false;
		}

		// 버텍스 좌표를 WGS84 경위도로 변환하여 저장
		for (size_t v=0; v<vtRawVertexs.size(); ++v)
		{
			POINT stPoint;
			// 버텍스 좌표를 WGS84 경위도로 변환 (2026-07-08 최정우 주석 추가)
			ConvertToWGS84(vtRawVertexs[v].dfX, vtRawVertexs[v].dfY, stPoint.dfX, stPoint.dfY);
			pstShapeLinkInfo->vtVertexs.push_back(stPoint);
			pstShapeNodeInfo->vtVertexs.push_back(stPoint);
		}

		// 링크 속성
		pstShapeLinkInfo->qwLinkID = qwLinkID;
		// 최고속도 DBF 필드값 읽기 (2026-07-08 최정우 주석 추가)
		pstShapeLinkInfo->nMaxSpeed = static_cast<uint8>(round(cLinkShape.GetDouble(i, m_stFieldMap.strMaxSpeed)));

		// 링크 길이 : 속성에 있으면 사용, 없으면 형상에서 계산 (m)
		if (bHasLength)
			// 링크 길이 DBF 필드값 읽기 (2026-07-08 최정우 주석 추가)
			pstShapeLinkInfo->dfLen = cLinkShape.GetDouble(i, m_stFieldMap.strLength);
		else
		{
			double dfLen = 0.0;
			for (size_t v=1; v<pstShapeLinkInfo->vtVertexs.size(); ++v)
				// 인접 버텍스 간 경위도 거리 합산 (2026-07-08 최정우 주석 추가)
				dfLen += cGISUtil.GetDistanceGEO2(pstShapeLinkInfo->vtVertexs[v-1], pstShapeLinkInfo->vtVertexs[v]);
			pstShapeLinkInfo->dfLen = dfLen;
		}

		// 도로 종별/연결로/유형 (없으면 0, 도로종별은 기타 108)
		long lRoadRank = cLinkShape.GetLong(i, m_stFieldMap.strRoadRank);
		pstShapeLinkInfo->nRoadRank = (lRoadRank > 0) ? static_cast<uint8>(lRoadRank) : 108;
		pstShapeLinkInfo->nConnect = static_cast<uint8>(cLinkShape.GetLong(i, m_stFieldMap.strConnect));
		pstShapeLinkInfo->nRoadType = static_cast<uint8>(cLinkShape.GetLong(i, m_stFieldMap.strRoadType));
		pstShapeLinkInfo->nLanes = static_cast<uint8>(cLinkShape.GetLong(i, m_stFieldMap.strLanes));

		// 도로명: MOCT DBF(CP949/ASCII) → UTF-8 변환 후 저장 (2026-07-08 최정우)
		string strRoadNameRaw;
		// 도로명 DBF 필드 문자열 읽기 (2026-07-08 최정우 주석 추가)
		cLinkShape.GetString(i, m_stFieldMap.strRoadName, strRoadNameRaw);

		string strRoadNameUtf8;
		if (!ToUtf8RoadName(strRoadNameRaw, strRoadNameUtf8))
		{
			LOGFMTW("road name utf-8 convert failed!link=[%llu] errno=[%d]",
				static_cast<unsigned long long>(qwLinkID), errno);
			strRoadNameUtf8.clear();
		}

		// UTF-8 문자열 기준 '-' 또는 공백에서 절단 (2026-07-08 최정우)
		TruncateRoadNameDelimiter(strRoadNameUtf8);
		CopyUtf8RoadName(strRoadNameUtf8, pstShapeLinkInfo->szRoadName, sizeof(pstShapeLinkInfo->szRoadName));

		// 시작/종료 노드 ID
		uint64 qwStNodeID = static_cast<uint64>(cLinkShape.GetLong(i, m_stFieldMap.strFNode));
		uint64 qwEdNodeID = static_cast<uint64>(cLinkShape.GetLong(i, m_stFieldMap.strTNode));
		pstShapeLinkInfo->qwStNodeID = qwStNodeID;
		pstShapeLinkInfo->qwEdNodeID = qwEdNodeID;

		// 시작/종료 노드 좌표/속성 (노드 shp 우선, 없으면 링크 끝점으로 대체)
		double dfStLon = pstShapeLinkInfo->vtVertexs.front().dfX;
		double dfStLat = pstShapeLinkInfo->vtVertexs.front().dfY;
		double dfEdLon = pstShapeLinkInfo->vtVertexs.back().dfX;
		double dfEdLat = pstShapeLinkInfo->vtVertexs.back().dfY;
		uint8 nStNodeType = 0;
		uint8 nEdNodeType = 0;

		if (bUseNode)
		{
			unordered_map<uint64, NODE_DATA>::iterator itSt = mapNode.find(qwStNodeID);
			if (itSt != mapNode.end())
			{
				dfStLon = itSt->second.dfLon;
				dfStLat = itSt->second.dfLat;
				nStNodeType = itSt->second.nNodeType;
			}
			unordered_map<uint64, NODE_DATA>::iterator itEd = mapNode.find(qwEdNodeID);
			if (itEd != mapNode.end())
			{
				dfEdLon = itEd->second.dfLon;
				dfEdLat = itEd->second.dfLat;
				nEdNodeType = itEd->second.nNodeType;
			}
		}

		pstShapeLinkInfo->dwStNodeX = static_cast<uint32>(dfStLon * 360000);
		pstShapeLinkInfo->dwStNodeY = static_cast<uint32>(dfStLat * 360000);
		pstShapeLinkInfo->nStNodeType = nStNodeType;
		pstShapeLinkInfo->dwEdNodeX = static_cast<uint32>(dfEdLon * 360000);
		pstShapeLinkInfo->dwEdNodeY = static_cast<uint32>(dfEdLat * 360000);
		pstShapeLinkInfo->nEdNodeType = nEdNodeType;

		// 노드 형상 정보 (회전정보 그래프용: 키 는 링크 시작 노드 ID)
		pstShapeNodeInfo->qwLinkID = qwLinkID;

		// 맵에 등록 (중복 링크 ID 는 무시)
		if (pmapShapeLinkInfoList->find(qwLinkID) != pmapShapeLinkInfoList->end())
		{
			delete pstShapeLinkInfo;
			delete pstShapeNodeInfo;
			continue;
		}

		pmapShapeLinkInfoList->insert(pair<uint64, PSHAPE_LINK_INFO>(qwLinkID, pstShapeLinkInfo));
		pmapShapeNodeInfoList->insert(pair<uint64, PSHAPE_NODE_INFO>(qwStNodeID, pstShapeNodeInfo));
	}

	// 형상 로딩 소요시간 측정 종료 (2026-07-08 최정우 주석 추가)
	cClock.Stop();
	LOGFMTI("shapefile geometry loading end!link count=[%zu], node(start) count=[%zu], elapsed=[%.06lf]", 
		pmapShapeLinkInfoList->size(), pmapShapeNodeInfoList->size(), cClock.GetElapsedTime());

	return true;
}
