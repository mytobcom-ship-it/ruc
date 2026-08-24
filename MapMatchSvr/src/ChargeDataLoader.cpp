/**
 * @file ChargeDataLoader.cpp
 * @brief 과금 게이트 정보 로딩 클래스 소스 파일
*/
#include "ChargeDataLoader.h"
#include "CoordConvert.h"
#include "GISUtil.h"

// 재조회로 교체된 이전 세대 캐시를 몇 개까지 계속 살려둘지 — 댕글링 포인터 방지용(LoadGates/
//   LoadZones 참고). 조회 함수가 반환한 포인터는 락 해제 직후 짧게(같은 함수 내 몇 줄)만 쓰이므로
//   2세대(현재+직전 재조회분)면 충분한 여유 (2026-08-14 최정우 추가)
static const size_t RETIRED_CACHE_GENERATIONS = 2;

/**
 * @brief COORDS(jsonb) 파싱 — "[[lon,lat],[lon,lat],...]" 형태에서 정점 목록 추출 (2026-08-13 최정우 추가)
 * @param[in] strJson base_roadlink.coords 원본 텍스트(GEOM_TYPE='POLY')
 * @param[out] pvtOut 파싱된 정점 목록(dfX=경도, dfY=위도)
 * @remark 이 코드베이스에 JSON 파서가 없어 정식 파서 대신 이 컬럼의 고정 포맷([[lon,lat],...], 중첩
 *   1단계)만 가정하고 직접 스캔. '[' 뒤에서 숫자 두 개(strtod) 파싱을 시도해 실패하면(예: 바깥쪽
 *   '[[' 의 첫 '[') 그냥 건너뛰므로 중첩 구조에 안전함
*/
static void ParseCoordsJson(const string& strJson, vector<POINT> *pvtOut)
{
	pvtOut->clear();
	const char *p = strJson.c_str();
	while (*p != '\0')
	{
		if (*p == '[')
		{
			char *pAfterLon = nullptr;
			double dfLon = strtod(p + 1, &pAfterLon);
			if (pAfterLon != (p + 1))
			{
				const char *pLat = pAfterLon;
				while ((*pLat == ',') || (*pLat == ' ')) pLat++;
				char *pAfterLat = nullptr;
				double dfLat = strtod(pLat, &pAfterLat);
				if (pAfterLat != pLat)
				{
					POINT stPt;
					stPt.dfX = dfLon;
					stPt.dfY = dfLat;
					pvtOut->push_back(stPt);
					p = pAfterLat;
					continue;
				}
			}
		}
		p++;
	}
}

/**
 * @brief LINK_IDS(jsonb) 파싱 — ["2040425701", "2040424401"] 형태에서 link_id 목록 추출 (2026-08-13 최정우 추가)
 * @param[in] strJson base_roadlink.link_ids 원본 텍스트(문자열 배열)
 * @param[out] pvtOut 파싱된 link_id 목록(uint64)
 * @remark ParseCoordsJson 과 동일한 이유로 정식 JSON 파서 대신 직접 스캔 — 큰따옴표 안의 숫자만
 *   strtoull 로 추출. 큰따옴표가 아닌 곳(대괄호·쉼표·공백)은 건너뜀
*/
static void ParseLinkIdsJson(const string& strJson, vector<uint64> *pvtOut)
{
	pvtOut->clear();
	const char *p = strJson.c_str();
	while (*p != '\0')
	{
		if (*p == '"')
		{
			char *pAfter = nullptr;
			uint64 qwLinkID = strtoull(p + 1, &pAfter, 10);
			if (pAfter != (p + 1))
			{
				pvtOut->push_back(qwLinkID);
				p = pAfter;
				continue;
			}
		}
		p++;
	}
}

/**
 * @brief 점 P 가 폴리곤 내부인지 — 표준 ray-casting(even-odd) 판정 (2026-08-13 최정우 추가)
 * @param[in] dfLon 점 경도, dfLat 점 위도
 * @param[in] vtPoly 폴리곤 정점 목록(닫힌 도형 가정 — 첫/끝 정점 별도 처리 불필요)
 * @return true=내부(경계 포함 판정은 버퍼 거리 검사가 별도로 처리)
*/
bool CChargeDataLoader::IsPointInPolygon(double dfLon, double dfLat, const vector<POINT>& vtPoly)
{
	if (vtPoly.size() < 3) return false;

	bool bInside = false;
	size_t nCount = vtPoly.size();
	for (size_t i = 0, j = nCount - 1; i < nCount; j = i++)
	{
		double dfXi = vtPoly[i].dfX, dfYi = vtPoly[i].dfY;
		double dfXj = vtPoly[j].dfX, dfYj = vtPoly[j].dfY;
		if (((dfYi > dfLat) != (dfYj > dfLat)) &&
			(dfLon < (dfXj - dfXi) * (dfLat - dfYi) / (dfYj - dfYi) + dfXi))
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

/**
 * @brief 점 P와 폴리곤 경계(각 변) 사이 최단거리(m) — 적응형 버퍼 판정용 (2026-08-13 최정우 추가)
 * @remark 버퍼 거리가 수 m~십수 m 수준으로 짧아 P 주변 국소 평면(경위도→m 환산)으로 근사해도
 *   오차가 무시할만함 — 하버사인을 변마다 반복 호출할 필요 없음
*/
double CChargeDataLoader::DistanceToPolygonBoundaryMeters(double dfLon, double dfLat, const vector<POINT>& vtPoly)
{
	if (vtPoly.size() < 2) return 1e18;

	const double dfMPerLon = 111320.0 * cos(RAD(dfLat));
	const double dfMPerLat = 111320.0;

	double dfMinDist = 1e18;
	size_t nCount = vtPoly.size();
	for (size_t i = 0, j = nCount - 1; i < nCount; j = i++)
	{
		double dfAx = (vtPoly[j].dfX - dfLon) * dfMPerLon;
		double dfAy = (vtPoly[j].dfY - dfLat) * dfMPerLat;
		double dfBx = (vtPoly[i].dfX - dfLon) * dfMPerLon;
		double dfBy = (vtPoly[i].dfY - dfLat) * dfMPerLat;

		double dfDx = dfBx - dfAx, dfDy = dfBy - dfAy;
		double dfLen2 = dfDx * dfDx + dfDy * dfDy;
		double dfT = (dfLen2 > 0.0) ? (((-dfAx) * dfDx + (-dfAy) * dfDy) / dfLen2) : 0.0;
		if (dfT < 0.0) dfT = 0.0;
		if (dfT > 1.0) dfT = 1.0;

		double dfCx = dfAx + dfT * dfDx, dfCy = dfAy + dfT * dfDy;
		double dfDist = sqrt(dfCx * dfCx + dfCy * dfCy);
		if (dfDist < dfMinDist) dfMinDist = dfDist;
	}
	return dfMinDist;
}

/**
 * @brief 생성자
*/
CChargeDataLoader::CChargeDataLoader() :
	m_pcPostgrePool(nullptr),
	m_bLoad(false),
	m_nParkFineMinSec(0)
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
	const string& strZoneSelectSQL, const string& strParkFineSelectSQL)
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
	m_strParkFineSelectSQL = strParkFineSelectSQL;
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
	m_nParkFineMinSec = 0;
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
	// tollgate_id(PK) 중복 방어 — 정상 상황에선 DB PK 제약이 이미 막아주지만, 스키마 변경(제약 삭제/
	//   우회)으로 중복이 들어오는 만일의 경우를 대비. link_id 기준 캐시라 중복이 있어도 그 자체로는
	//   죽지 않지만, tollgate_id 문자열을 세션 추적 키로 쓰는 상위 로직(vtActiveGateIds 등)이 서로
	//   다른 물리적 게이트를 같은 게이트로 오판할 수 있어 방어 필요 (2026-08-14 최정우 추가)
	unordered_set<string> setSeenTollgateIds;
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

		// tollgate_id 중복(2번째부터) — 첫 행만 유지하고 이후는 스킵, 어떤 ID인지 에러로그로 남김
		//   (2026-08-14 최정우 추가 — 정상 시엔 PK 위반으로 절대 안 나오는 케이스)
		if (!setSeenTollgateIds.insert(pszTollgateID).second)
		{
			LOGFMTE("duplicate tollgate_id!skip 2nd+ row - gate id=[%s] road_id=[%s] link_id=[%s]",
				pszTollgateID, pszRoadID, bLinkIdNull ? "" : pszLinkID);
			continue;
		}

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

	{
		// 재조회 시점 교체 — m_cGateCacheMutex 로 조회 스레드(GetGateByLinkId 등)와 동기화(doc/README.txt
		// §F 패턴, 2026-08-14 최정우 수정 — 락 없이 swap만 하던 걸 수정, "소스상 문제" 검토 중 발견).
		// 단순 swap 만으로는 부족함 — 이전 세대를 즉시 파괴하지 않고 retired 목록에 보관해 댕글링
		// 포인터를 막음(ChargeDataLoader.h m_dqRetiredGateInfo 주석 참고, 2026-08-14 최정우 추가)
		lock_guard<CMutex> cLock(m_cGateCacheMutex);
		m_dqRetiredGateInfo.push_back(std::move(m_mapGateInfo));
		m_mapGateInfo = std::move(mapNewGateInfo);
		while (m_dqRetiredGateInfo.size() > RETIRED_CACHE_GENERATIONS)
			m_dqRetiredGateInfo.pop_front();
	}
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
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
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
 * @brief link_id+gate_div 로 일치하는 게이트 전부 조회 (2026-08-13 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[in] cGateDiv 게이트 구분('M'/'I'/'O'/'B')
 * @param[out] pvtOut 일치하는 게이트 포인터 목록(비우고 채움) — 없으면 빈 상태로 반환
 * @remark GetGateByLinkId 는 첫 매치 1개만 반환해 같은 link_id 에 같은 gate_div 게이트가
 *   2개 이상이면 나머지가 조회 자체가 안 됨(개방형 부과 누락 위험) — 이 함수는 전부 수집
*/
void CChargeDataLoader::GetGatesByLinkId(const uint64 qwLinkID, const char cGateDiv, vector<PGATE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	pvtOut->clear();

	lock_guard<CMutex> cLock(m_cGateCacheMutex);
	mapGateInfo::iterator it = m_mapGateInfo.find(qwLinkID);
	if (it == m_mapGateInfo.end())
		return;

	for (size_t i = 0; i < it->second.size(); ++i)
	{
		if (cGateDiv == it->second[i].szGateDiv[0])
			pvtOut->push_back(&(it->second[i]));
	}
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
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
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
	lock_guard<CMutex> cLock(m_cGateCacheMutex);
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

	lock_guard<CMutex> cLock(m_cGateCacheMutex);
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

		// road_id 중복(2번째부터) — 첫 행만 유지하고 이후는 스킵, 어떤 ID인지 에러로그로 남김.
		//   기존엔 mapNewZoneInfo[road_id]=값 대입이라 뒤 행이 앞 행을 조용히 덮어썼음(정상 시엔
		//   PK 위반으로 절대 안 나오는 케이스지만, 스키마 변경으로 중복이 생기면 그동안은 어느 쪽이
		//   남는지도 모른 채 유실됐음) (2026-08-14 최정우 추가)
		if (mapNewZoneInfo.find(pszRoadID) != mapNewZoneInfo.end())
		{
			LOGFMTE("duplicate road_id!skip 2nd+ row - zone id=[%s] road_kind=[%s]",
				pszRoadID, pszRoadKind);
			continue;
		}

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

		// POLY(주정차)만 PointInPolygon 판정용 정점 파싱 — LINE 은 LENGTH_M/FIRST/LAST 로 충분해
		//   불필요한 파싱·메모리를 아낌 (2026-08-13 최정우 추가)
		if (strcmp(stZoneInfo.szGeomType, "POLY") == 0)
			ParseCoordsJson(stZoneInfo.strCoordsJson, &stZoneInfo.vtCoords);

		// 일반도로(ROAD_KIND=0)·면제도로(ROAD_KIND=5)만 link_ids 파싱 — 게이트가 없어 매칭
		//   링크→구역 역인덱스가 유일한 진입/이탈 판정 수단(다른 LINE 유형은 게이트 기반이라 불필요)
		//   (2026-08-13 최정우 추가, 2026-08-14 두 차례 수정 — ROAD_KIND=5 기반 판정을 "모든 미등록
		//   링크" 방식으로 재설계했다가 폐기하고, charge_type=0 통합 출력 방식으로 다시 zone 기반 부활)
		if ((strcmp(stZoneInfo.szRoadKind, "0") == 0) || (strcmp(stZoneInfo.szRoadKind, "5") == 0))
			ParseLinkIdsJson(stZoneInfo.strLinkIdsJson, &stZoneInfo.vtLinkIds);

		mapNewZoneInfo[stZoneInfo.szRoadID] = stZoneInfo;
	}

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	// 일반도로(NODE_STEP) link_id → road_id 역인덱스 재구성 (2026-08-14 최정우 추가)
	unordered_map<uint64, vector<string> > mapNewNodeStepLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "0") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewNodeStepLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	// 면제도로 link_id → road_id 역인덱스 재구성 (2026-08-14 최정우 부활)
	unordered_map<uint64, vector<string> > mapNewExemptLinkToRoadId;
	for (mapZoneInfo::iterator it = mapNewZoneInfo.begin(); it != mapNewZoneInfo.end(); ++it)
	{
		if (strcmp(it->second.szRoadKind, "5") != 0) continue;
		for (size_t i = 0; i < it->second.vtLinkIds.size(); ++i)
			mapNewExemptLinkToRoadId[it->second.vtLinkIds[i]].push_back(it->second.szRoadID);
	}

	{
		// 재조회 시점 교체 — m_cZoneCacheMutex 로 조회 스레드(GetZoneByRoadId/GetParkingZoneContaining 등)와
		// 동기화(doc/README.txt §F 패턴, 2026-08-14 최정우 수정 — 락 없이 swap만 하던 걸 수정). 세 맵을
		// 같은 락 안에서 함께 교체해 서로 항상 일관된 스냅샷 유지.
		// m_mapZoneInfo 만 이전 세대를 즉시 파괴하지 않고 retired 목록에 보관 — GetZoneByRoadId 등이
		// 반환하는 PZONE_INFO 는 그 주소를 외부에 노출하지만, 두 역인덱스 맵(link_id→road_id 문자열)은
		// 내부에서 조회 즉시 GetZoneByRoadId 로 넘길 뿐 자기 주소를 외부에 노출한 적이 없어 단순 swap으로
		// 충분함(ChargeDataLoader.h m_dqRetiredZoneInfo 주석 참고, 2026-08-14 최정우 추가)
		lock_guard<CMutex> cLock(m_cZoneCacheMutex);
		m_dqRetiredZoneInfo.push_back(std::move(m_mapZoneInfo));
		m_mapZoneInfo = std::move(mapNewZoneInfo);
		while (m_dqRetiredZoneInfo.size() > RETIRED_CACHE_GENERATIONS)
			m_dqRetiredZoneInfo.pop_front();
		m_mapNodeStepLinkToRoadId.swap(mapNewNodeStepLinkToRoadId);
		m_mapExemptLinkToRoadId.swap(mapNewExemptLinkToRoadId);
	}
	m_bLoad = true;

	LOGFMTI("zone cache loaded!count=[%zu]", GetZoneCount());
	return true;
}

/**
 * @brief base_parking_fine 최소 from_min(분)을 초로 환산해 (재)로드 — 기동 시 1회, 또는 게이트·구역과
 *        같은 주기 재조회 스레드에서 반복 호출. 세션 미설정·SQL 없음은 실패가 아니라 "임계 비활성"
 *        (사용자 지시, 2026-08-24 최정우 추가)
 * @return true(조회 성공, 테이블이 비어 있어도 true — 그 경우 임계 0으로 비활성), false(조회 자체 실패)
*/
bool CChargeDataLoader::LoadParkingFine()
{
	if (m_strParkFineSelectSQL.empty())
		return true;								// 세션 미설정 — 애초에 비활성, 실패 아님

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

	PGresult *pcResult = PQexec(pcConn, m_strParkFineSelectSQL.c_str());
	if (PQresultStatus(pcResult) != PGRES_TUPLES_OK)
	{
		LOGFMTE("park fine select failed!error=[%s]", PQerrorMessage(pcConn));
		PQclear(pcResult);
		m_pcPostgrePool->releaseConnection(pcConn);
		return false;
	}

	// MIN(from_min) — 테이블이 비어 있으면 NULL(임계 비활성, 0) (2026-08-24 최정우 추가)
	int nMinSec = 0;
	if ((PQntuples(pcResult) > 0) && !PQgetisnull(pcResult, 0, 0))
		nMinSec = atoi(PQgetvalue(pcResult, 0, 0)) * 60;

	PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);

	{
		lock_guard<CMutex> cLock(m_cParkFineMutex);
		m_nParkFineMinSec = nMinSec;
	}

	LOGFMTI("park fine minimum loaded!min_sec=[%d]", nMinSec);
	return true;
}

/**
 * @brief road_id로 구역 조회 — O(1), 개방형 zone_name 조회 등에 사용 (2026-08-12 최정우 추가)
 * @param[in] strRoadID 조회할 구역 ID(base_roadlink.road_id)
 * @return 구역 정보 포인터(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetZoneByRoadId(const string& strRoadID)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	mapZoneInfo::iterator it = m_mapZoneInfo.find(strRoadID);
	if (it == m_mapZoneInfo.end())
		return nullptr;

	return &(it->second);
}

/**
 * @brief 주정차(POLY) 구역 중 (dfLon,dfLat) 포함 구역 조회 — 게이트가 없어 road_id 선판별 불가,
 *   POLY 전량 순회 (2026-08-13 최정우 추가)
 * @param[in] dfLon 판정 경도(raw GPS, 맵매칭 전)
 * @param[in] dfLat 판정 위도
 * @param[in] dfBufM 폴리곤 경계 버퍼(m). 0 이하면 버퍼 미적용(폴리곤 내부만 인정)
 * @return 포함 구역, 없으면 nullptr
*/
PZONE_INFO CChargeDataLoader::GetParkingZoneContaining(const double dfLon, const double dfLat, const double dfBufM)
{
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	for (mapZoneInfo::iterator it = m_mapZoneInfo.begin(); it != m_mapZoneInfo.end(); ++it)
	{
		ZONE_INFO& stZone = it->second;
		if (strcmp(stZone.szGeomType, "POLY") != 0) continue;
		if (stZone.vtCoords.size() < 3) continue;

		if (IsPointInPolygon(dfLon, dfLat, stZone.vtCoords))
			return &stZone;

		if ((dfBufM > 0.0) && (DistanceToPolygonBoundaryMeters(dfLon, dfLat, stZone.vtCoords) <= dfBufM))
			return &stZone;
	}
	return nullptr;
}

/**
 * @brief 주정차 구역 복수 조회 — 폴리곤이 겹쳐 설정될 수 있다(시간대별 규제가 다른 구역 등)
 *   (2026-08-23 최정우 추가)
 * @param[out] pvtOut 좌표를 포함하는 구역 전부(없으면 빈 목록)
*/
void CChargeDataLoader::GetParkingZonesContaining(const double dfLon, const double dfLat,
		const double dfBufM, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	for (mapZoneInfo::iterator it = m_mapZoneInfo.begin(); it != m_mapZoneInfo.end(); ++it)
	{
		ZONE_INFO& stZone = it->second;
		if (strcmp(stZone.szGeomType, "POLY") != 0) continue;
		if (stZone.vtCoords.size() < 3) continue;

		if (IsPointInPolygon(dfLon, dfLat, stZone.vtCoords)
			|| ((dfBufM > 0.0)
				&& (DistanceToPolygonBoundaryMeters(dfLon, dfLat, stZone.vtCoords) <= dfBufM)))
			pvtOut->push_back(&stZone);
	}
}

/**
 * @brief 일반도로(ROAD_KIND=0, NODE_STEP) 구역 조회 — 매칭 링크 ID로 역인덱스 O(1) 조회 (2026-08-14 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @return 그 링크가 속한 일반도로 구역(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetNodeStepZoneByLinkId(const uint64 qwLinkID)
{
	// m_cZoneCacheMutex 는 CMutex(재귀 락) — 아래 GetZoneByRoadId() 가 같은 락을 다시 잡아도
	// 같은 스레드면 데드락 없음 (2026-08-14 최정우 추가)
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapNodeStepLinkToRoadId.find(qwLinkID);
	if ((itLink == m_mapNodeStepLinkToRoadId.end()) || itLink->second.empty())
		return nullptr;

	return GetZoneByRoadId(itLink->second[0]);		// 호환용 — 첫 구역만. 복수는 아래 함수 사용
}

/**
 * @brief 일반도로 구역 복수 조회 — 한 링크가 여러 구역에 속할 수 있다 (2026-08-23 최정우 추가)
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[out] pvtOut 그 링크가 속한 구역 전부(없으면 빈 목록). 호출측이 비우지 않아도 되게 clear 하지 않는다
*/
void CChargeDataLoader::GetNodeStepZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapNodeStepLinkToRoadId.find(qwLinkID);
	if (itLink == m_mapNodeStepLinkToRoadId.end()) return;
	for (size_t i = 0; i < itLink->second.size(); ++i)
	{
		PZONE_INFO p = GetZoneByRoadId(itLink->second[i]);
		if (p != nullptr) pvtOut->push_back(p);
	}
}

/**
 * @brief 면제도로(ROAD_KIND=5) 구역 조회 — 매칭 링크 ID로 역인덱스 O(1) 조회 (2026-08-14 최정우 부활)
 * @param[in] qwLinkID 매칭 링크 ID
 * @return 그 링크가 속한 면제도로 구역(없으면 nullptr)
*/
PZONE_INFO CChargeDataLoader::GetExemptZoneByLinkId(const uint64 qwLinkID)
{
	// m_cZoneCacheMutex 는 CMutex(재귀 락) — 아래 GetZoneByRoadId() 가 같은 락을 다시 잡아도
	// 같은 스레드면 데드락 없음
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapExemptLinkToRoadId.find(qwLinkID);
	if ((itLink == m_mapExemptLinkToRoadId.end()) || itLink->second.empty())
		return nullptr;

	return GetZoneByRoadId(itLink->second[0]);		// 호환용 — 첫 구역만
}

/**
 * @brief 면제도로 구역 복수 조회 (2026-08-23 최정우 추가)
*/
void CChargeDataLoader::GetExemptZonesByLinkId(const uint64 qwLinkID, vector<PZONE_INFO> *pvtOut)
{
	if (pvtOut == nullptr) return;
	lock_guard<CMutex> cLock(m_cZoneCacheMutex);
	unordered_map<uint64, vector<string> >::iterator itLink = m_mapExemptLinkToRoadId.find(qwLinkID);
	if (itLink == m_mapExemptLinkToRoadId.end()) return;
	for (size_t i = 0; i < itLink->second.size(); ++i)
	{
		PZONE_INFO p = GetZoneByRoadId(itLink->second[i]);
		if (p != nullptr) pvtOut->push_back(p);
	}
}
