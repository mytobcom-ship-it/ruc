/**
 * @file ChargeDataLoader.cpp
 * @brief 과금 게이트 정보 로딩 클래스 소스 파일
*/
#include "ChargeDataLoader.h"
#include "CoordConvert.h"
#include "GISUtil.h"

/**
 * @brief 생성자
*/
CChargeDataLoader::CChargeDataLoader() :
	m_pcPostgrePool(nullptr),
	m_bLoad(false)
{
}

/**
 * @brief 소멸자
*/
CChargeDataLoader::~CChargeDataLoader()
{
	Uninitialize();
}

/**
 * @brief 초기화 — 커넥션 풀·게이트/구역 조회 SQL 보관(2026-08-12 최정우 추가)
 * @param[in] pcPostgrePool DB 커넥션 풀(비소유 — Server 가 생성한 것을 그대로 공유)
 * @param[in] strGateSelectSQL base_tollgate 전량 조회 SQL(query.sql 세션명으로 로드된 실 SQL 문자열)
 * @param[in] strZoneSelectSQL base_roadlink 전량 조회 SQL(비어 있으면 구역 캐시만 비활성 — 게이트
 *            캐시는 그대로 사용 가능) (2026-08-12 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CChargeDataLoader::Initialize(CPostgrePool *pcPostgrePool, const string& strGateSelectSQL,
	const string& strZoneSelectSQL)
{
	if (pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool is null!");
		return false;
	}
	if (strGateSelectSQL.empty())
	{
		LOGFMTE("gate select sql is empty!");
		return false;
	}

	m_pcPostgrePool = pcPostgrePool;
	m_strGateSelectSQL = strGateSelectSQL;
	m_strZoneSelectSQL = strZoneSelectSQL;
	return true;
}

/**
 * @brief 종료 — 캐시 해제
 * @return void
*/
void CChargeDataLoader::Uninitialize()
{
	m_mapGateInfo.clear();
	m_mapZoneInfo.clear();
	m_bLoad = false;
}

/**
 * @brief base_tollgate 전량을 조회해 인메모리 캐시로 (재)로드 — 기동 시 1회, 또는 주기
 *        재조회 스레드에서 반복 호출 (2026-08-12 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CChargeDataLoader::LoadGates()
{
	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool not initialized!");
		return false;
	}

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("get connection failed!");
		return false;
	}

	PGresult *pcResult = PQexec(pcConn, m_strGateSelectSQL.c_str());
	if (PQresultStatus(pcResult) != PGRES_TUPLES_OK)
	{
		LOGFMTE("gate select failed!error=[%s]", PQerrorMessage(pcConn));
		PQclear(pcResult);
		m_pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	// 컬럼 순서: tollgate_id, road_id, gate_div, lon, lat, link_id
	//   (query.sql [gate_select] 세션과 반드시 일치해야 함) (2026-08-12 최정우 추가)
	CCoordConvert cCoordConvert;
	mapGateInfo mapNewGateInfo;
	int nRows = PQntuples(pcResult);
	for (int i = 0; i < nRows; i++)
	{
		const char *pszTollgateID = PQgetvalue(pcResult, i, 0);
		const char *pszRoadID = PQgetvalue(pcResult, i, 1);
		const char *pszGateDiv = PQgetvalue(pcResult, i, 2);
		const char *pszLon = PQgetvalue(pcResult, i, 3);
		const char *pszLat = PQgetvalue(pcResult, i, 4);
		bool bLinkIdNull = PQgetisnull(pcResult, i, 5);
		const char *pszLinkID = bLinkIdNull ? nullptr : PQgetvalue(pcResult, i, 5);

		// link_id 없는 게이트는 아직 폴백(좌표거리) 전용 조회 경로가 없어 캐시에서 제외
		//   — GetGateNearby 는 현재 "이미 로드된" 게이트 목록을 선형 탐색하는 구조라 무관하지만,
		//   link_id 기반 캐시 키(unordered_map) 특성상 link_id 없는 행은 별도 vector 보관이 필요함.
		//   지금은 스캐폴드 단계라 link_id 있는 행만 우선 적재 (2026-08-12 최정우 추가, TODO)
		if (bLinkIdNull) continue;

		GATE_INFO stGateInfo;
		strncpy(stGateInfo.szTollgateID, pszTollgateID, sizeof(stGateInfo.szTollgateID) - 1);
		strncpy(stGateInfo.szRoadID, pszRoadID, sizeof(stGateInfo.szRoadID) - 1);
		strncpy(stGateInfo.szGateDiv, pszGateDiv, sizeof(stGateInfo.szGateDiv) - 1);
		stGateInfo.dfLon = atof(pszLon);
		stGateInfo.dfLat = atof(pszLat);
		cCoordConvert.WGS84ToSearchCoord(stGateInfo.dfLat, stGateInfo.dfLon,
			&stGateInfo.dwSearchLat, &stGateInfo.dwSearchLon);
		stGateInfo.qwLinkID = strtoull(pszLinkID, nullptr, 10);

		// push_back — 폐쇄형/구간단속 입/출구가 같은 link_id 를 공유하는 실사례가 있어(TG00007/08,
		// TG00012/13) 대입(map[key]=value)하면 뒤 행이 앞 행을 덮어써 유실됨 (2026-08-12 최정우 수정)
		mapNewGateInfo[stGateInfo.qwLinkID].push_back(stGateInfo);
	}

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	m_mapGateInfo.swap(mapNewGateInfo);		// 짧은 스왑 — 조회 스레드와의 락 경합 최소화(doc/README.txt §F 패턴)
	m_bLoad = true;

	LOGFMTI("gate cache loaded!count=[%zu]", GetGateCount());
	return true;
}

/**
 * @brief link_id로 게이트 조회 — O(1), 게이트 판정 1순위 (2026-08-12 최정우 추가)
 * @remark
 *   같은 link_id를 공유하는 게이트가 있을 수 있어(TG00007/08, TG00012/13 확인됨) cGateDiv 로
 *   걸러야 정확함. cGateDiv=0(생략)이면 해당 link_id의 첫 게이트를 그대로 반환 — 개방형(M, 단독
 *   게이트)처럼 공유 걱정이 없는 호출부 전용, 폐쇄형·구간단속은 반드시 gate_div 지정할 것 (2026-08-12 최정우 수정)
 * @param[in] qwLinkID 매칭된 링크 ID
 * @param[in] cGateDiv 게이트 구분 필터(0=생략, 첫 건 반환)
 * @return 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateByLinkId(const uint64 qwLinkID, const char cGateDiv)
{
	mapGateInfo::iterator it = m_mapGateInfo.find(qwLinkID);
	if (it == m_mapGateInfo.end())
		return nullptr;

	if (cGateDiv == 0)
		return &(it->second.front());

	for (size_t i = 0; i < it->second.size(); ++i)
	{
		if (cGateDiv == it->second[i].szGateDiv[0])
			return &(it->second[i]);
	}
	return nullptr;
}

/**
 * @brief road_id+gate_div로 게이트 조회 — 폐쇄형/구간단속 진입~진출 짝짓기 전용 (2026-08-12 최정우 추가)
 * @remark
 *   별도 인덱스 없이 m_mapGateInfo(link_id 키)를 선형 탐색 — 게이트 총량이 작아(전국 기준도
 *   도로망 대비 극소수, doc/README.txt §F) O(n) 탐색 비용이 무시할 만한 수준이라 인덱스 추가는
 *   보류. 규모가 커지면 road_id 보조 인덱스로 재검토
 * @param[in] strRoadID 진입 시 저장해둔 세션의 road_id(base_roadlink.road_id)
 * @param[in] cGateDiv 찾을 게이트 구분('I' 또는 'O')
 * @return 일치하는 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateByRoadId(const string& strRoadID, const char cGateDiv)
{
	for (mapGateInfo::iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); ++i)
		{
			if ((strRoadID == it->second[i].szRoadID) && (cGateDiv == it->second[i].szGateDiv[0]))
				return &(it->second[i]);
		}
	}
	return nullptr;
}

/**
 * @brief 로드된 게이트 총 개수 — link_id 공유로 vector 에 여러 건 들어간 경우도 전부 합산 (2026-08-12 최정우 추가)
 * @return 게이트 총 개수
*/
size_t CChargeDataLoader::GetGateCount() const
{
	size_t nCount = 0;
	for (mapGateInfo::const_iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
		nCount += it->second.size();
	return nCount;
}

/**
 * @brief 좌표거리 폴백 조회 — link_id 매칭 실패 시에만 사용, 게이트 판정 2순위 (2026-08-12 최정우 추가)
 * @param[in] dfLat 현재 위치 위도(WGS84)
 * @param[in] dfLon 현재 위치 경도(WGS84)
 * @param[in] dfGateRadiusM 유효 반경(m) — config.ini [charge] gate_radius(아직 미배치, TODO)
 * @return 반경 내 가장 가까운 게이트 정보 포인터(없으면 nullptr)
*/
PGATE_INFO CChargeDataLoader::GetGateNearby(const double dfLat, const double dfLon, const double dfGateRadiusM)
{
	CCoordConvert cCoordConvert;
	CGISUtil cGISUtil;

	uint32 dwSearchLat = 0, dwSearchLon = 0;
	cCoordConvert.WGS84ToSearchCoord(dfLat, dfLon, &dwSearchLat, &dwSearchLon);
	POINT stCurPos;
	stCurPos.dfX = static_cast<double>(dwSearchLon);
	stCurPos.dfY = static_cast<double>(dwSearchLat);

	// 게이트 총량이 작아(전국 기준으로도 도로망 대비 극소수) 선형 탐색으로 충분 — 규모가 커지면
	// GRID 분할(CDataLoader 패턴) 재검토 (2026-08-12 최정우 추가)
	PGATE_INFO pstNearest = nullptr;
	double dfNearestDist = dfGateRadiusM;
	for (mapGateInfo::iterator it = m_mapGateInfo.begin(); it != m_mapGateInfo.end(); ++it)
	{
		for (size_t i = 0; i < it->second.size(); ++i)
		{
			GATE_INFO& stGate = it->second[i];
			POINT stGatePos;
			stGatePos.dfX = static_cast<double>(stGate.dwSearchLon);
			stGatePos.dfY = static_cast<double>(stGate.dwSearchLat);

			double dfDist = cGISUtil.GetDistanceGEO1(stCurPos, stGatePos);
			if (dfDist <= dfNearestDist)
			{
				dfNearestDist = dfDist;
				pstNearest = &stGate;
			}
		}
	}
	return pstNearest;
}

/**
 * @brief base_roadlink 전량을 조회해 인메모리 캐시로 (재)로드 — 기동 시 1회, 또는 주기
 *        재조회 스레드에서 반복 호출 (2026-08-12 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CChargeDataLoader::LoadZones()
{
	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("postgre pool not initialized!");
		return false;
	}
	if (m_strZoneSelectSQL.empty())
	{
		LOGFMTW("zone select sql is empty!skip zone load");
		return true;
	}

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("get connection failed!");
		return false;
	}

	PGresult *pcResult = PQexec(pcConn, m_strZoneSelectSQL.c_str());
	if (PQresultStatus(pcResult) != PGRES_TUPLES_OK)
	{
		LOGFMTE("zone select failed!error=[%s]", PQerrorMessage(pcConn));
		PQclear(pcResult);
		m_pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	// 컬럼 순서: road_id, road_kind, road_nm, geom_type, speed_limit_kmh, use_yn, link_ids, coords,
	//   length_m, first_lon, first_lat, last_lon, last_lat
	//   (query.sql [zone_select] 세션과 반드시 일치해야 함) (2026-08-12 최정우 수정 — first/last 좌표 추가)
	mapZoneInfo mapNewZoneInfo;
	int nRows = PQntuples(pcResult);
	for (int i = 0; i < nRows; i++)
	{
		const char *pszRoadID = PQgetvalue(pcResult, i, 0);
		const char *pszRoadKind = PQgetvalue(pcResult, i, 1);
		const char *pszRoadNm = PQgetvalue(pcResult, i, 2);
		const char *pszGeomType = PQgetvalue(pcResult, i, 3);
		bool bSpeedLimitNull = PQgetisnull(pcResult, i, 4);
		const char *pszSpeedLimit = bSpeedLimitNull ? nullptr : PQgetvalue(pcResult, i, 4);
		const char *pszUseYN = PQgetvalue(pcResult, i, 5);
		const char *pszLinkIds = PQgetvalue(pcResult, i, 6);
		const char *pszCoords = PQgetvalue(pcResult, i, 7);
		const char *pszLengthM = PQgetvalue(pcResult, i, 8);
		const char *pszFirstLon = PQgetvalue(pcResult, i, 9);
		const char *pszFirstLat = PQgetvalue(pcResult, i, 10);
		const char *pszLastLon = PQgetvalue(pcResult, i, 11);
		const char *pszLastLat = PQgetvalue(pcResult, i, 12);

		ZONE_INFO stZoneInfo;
		strncpy(stZoneInfo.szRoadID, pszRoadID, sizeof(stZoneInfo.szRoadID) - 1);
		strncpy(stZoneInfo.szRoadKind, pszRoadKind, sizeof(stZoneInfo.szRoadKind) - 1);
		strncpy(stZoneInfo.szRoadNm, pszRoadNm, sizeof(stZoneInfo.szRoadNm) - 1);
		strncpy(stZoneInfo.szGeomType, pszGeomType, sizeof(stZoneInfo.szGeomType) - 1);
		stZoneInfo.dfSpeedLimitKmh = bSpeedLimitNull ? 0.0 : atof(pszSpeedLimit);
		strncpy(stZoneInfo.szUseYN, pszUseYN, sizeof(stZoneInfo.szUseYN) - 1);
		stZoneInfo.strLinkIdsJson = pszLinkIds;
		stZoneInfo.strCoordsJson = pszCoords;
		stZoneInfo.dfLengthM = atof(pszLengthM);
		stZoneInfo.dfFirstLon = atof(pszFirstLon);
		stZoneInfo.dfFirstLat = atof(pszFirstLat);
		stZoneInfo.dfLastLon = atof(pszLastLon);
		stZoneInfo.dfLastLat = atof(pszLastLat);

		mapNewZoneInfo[stZoneInfo.szRoadID] = stZoneInfo;
	}

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	m_mapZoneInfo.swap(mapNewZoneInfo);		// 짧은 스왑 — 조회 스레드와의 락 경합 최소화(doc/README.txt §F 패턴)
	m_bLoad = true;

	LOGFMTI("zone cache loaded!count=[%zu]", GetZoneCount());
	return true;
}

/**
 * @brief road_id로 구역 조회 — O(1), 개방형 zone_name 조회 등에 사용 (2026-08-12 최정우 추가)
 * @param[in] strRoadID 조회할 구역 ID(base_roadlink.road_id)
 * @return 구역 정보 포인터(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetZoneByRoadId(const string& strRoadID)
{
	mapZoneInfo::iterator it = m_mapZoneInfo.find(strRoadID);
	if (it == m_mapZoneInfo.end())
		return nullptr;

	return &(it->second);
}
