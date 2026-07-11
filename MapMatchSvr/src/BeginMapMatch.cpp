/**
 * @file BeginMapMatch.cpp
 * @brief 초기 맵매칭 클래스 소스 파일
*/
#include "BeginMapMatch.h"

/**
 * @brief 생성자
*/
CBeginMapMatch::CBeginMapMatch() : 
	m_pcDataLoader(nullptr)
{
}

/**
 * @brief 소멸자
*/
CBeginMapMatch::~CBeginMapMatch()
{
}

/**
 * @brief 초기 맵매칭 시작
 * @param[in] pcDataLoader 데이터 로딩 클래스
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[out] pwErrorCode 에러 코드
 * @param[out] pstMatchEntry 검색 정보
 * @return true(성공), false(실패)
*/
bool CBeginMapMatch::StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput, 
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry, PMATCH_TRACE_CTX pstTraceCtx)
{
	m_pcDataLoader = pcDataLoader;

	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	// GRID 구하기
	uint32 dwGridID = m_cGISUtil.GetGridID(stSgmtMatchInput.stPoint.dfX, stSgmtMatchInput.stPoint.dfY);

	// GRID 별 세그먼트 범위
	PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(dwGridID);
	if (!pstGridInfo)
	{
		*pwErrorCode = NOT_FOUND_GRIDINFO;
		return false;
	}

	list<MATCH_ENTRY> listMatchEntryList;
	vector<uint32> vtNearGridIDList;
	
	// 현재 소속된 GRID ID
	vtNearGridIDList.push_back(dwGridID);

	// 인접 GRID 목록 검색
	m_cGISUtil.GetNearGridID(dwGridID, stSgmtMatchInput, vtNearGridIDList);

	stSgmtMatchInput.stPoint.dfX *= 360000.0;
	stSgmtMatchInput.stPoint.dfY *= 360000.0;

	uint32 dwStartSgmtOffset = pstGridInfo->dwSgmtOffset;
	uint32 dwEndSgmtOffset = dwStartSgmtOffset + pstGridInfo->wSgmtCount;

	if (!vtNearGridIDList.empty())
	{
		vector<uint32>::iterator it = vtNearGridIDList.begin();
		for (; it != vtNearGridIDList.end(); ++it)
		{
			// GRID 별 세그먼트 범위
			pstGridInfo = m_pcDataLoader->GetGridInfo(*it);
			if (!pstGridInfo)
			{
				*pwErrorCode = NOT_FOUND_GRIDINFO;
				continue;
			}

			dwStartSgmtOffset = pstGridInfo->dwSgmtOffset;
			dwEndSgmtOffset = dwStartSgmtOffset + pstGridInfo->wSgmtCount;

			MATCH_ENTRY stMatchEntry;
			// GRID 내 세그먼트 범위별 맵매칭 수행 (2026-07-08 최정우 주석 추가)
			if (GridSgmtMapMatch(stSgmtMatchInput, dwStartSgmtOffset, dwEndSgmtOffset, pwErrorCode, &stMatchEntry))
				listMatchEntryList.push_back(stMatchEntry);
		}
	}

	if (listMatchEntryList.empty())
	{
		*pwErrorCode = MAP_MATCH_FAIL;
		return false;
	}

	*pwErrorCode = NO_ERROR;
	listMatchEntryList.sort();
	if (pstTraceCtx != nullptr)
	{
		pstTraceCtx->nMatchedStep = 0;
		CMatchTrace::LogResult(pstTraceCtx, stSgmtMatchInput, listMatchEntryList, listMatchEntryList.front());
	}
	*pstMatchEntry = *listMatchEntryList.begin();
	return true;
}

/**
 * @brief GRID 별 세그먼트 맵매칭
 * @param[in] stSgmtMatchInput 세그먼트 입력 정보
 * @param[in] dwStartSgmtOffset 세그먼트 시작
 * @param[in] dwEndSgmtOffset 세그먼트 종료
 * @param[out] pwErrorCode 에러 코드
 * @param[out] pstMatchEntry 검색 정보
 * @return true(성공), false(실패)
*/
bool CBeginMapMatch::GridSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset, 
		uint32 dwEndSgmtOffset, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	list<MATCH_ENTRY> listMatchEntryList;
	listMatchEntryList.clear();

	for (uint32 i=dwStartSgmtOffset; i<dwEndSgmtOffset; ++i)
	{
		SGMT_MATCH_RES stSgmtMatchRes;

		// GRID 세그먼트 정보
		PGRID_SGMT_INFO pstGridSgmtInfo = m_pcDataLoader->GetGridSgmtInfo(i);
		if (!pstGridSgmtInfo)
			continue;

		// 세그먼트 정보 구조체
		SGMT_INFO stSgmtInfo;

		// 세그먼트 X,Y 좌표
		stSgmtInfo.stPoint.dfX = static_cast<double>(pstGridSgmtInfo->dwX);
		stSgmtInfo.stPoint.dfY = static_cast<double>(pstGridSgmtInfo->dwY);

		// 세그먼트 진행 방향(방위각)
		stSgmtInfo.nDirAng = static_cast<sint16>(pstGridSgmtInfo->wDirAng);

		// 세그먼트 길이 형변환
		stSgmtInfo.dfLen = static_cast<double>(pstGridSgmtInfo->wLenSgmt);

		// 링크 ID
		stSgmtInfo.qwLinkID = pstGridSgmtInfo->qwLinkID;

		// INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·방위 비용 매칭 (2026-07-08 최정우 주석 추가)
		if (!m_cGISUtil.SgmtMatch(stSgmtMatchInput, stSgmtInfo, &stSgmtMatchRes))
			continue;

		// 매칭 성공이면 링크 정보
		PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(stSgmtInfo.qwLinkID);
		if (!pstLinkInfo) continue;

		MATCH_ENTRY stMatchEntry;

		stMatchEntry.dfMatchX = stSgmtMatchRes.stMatchPoint.dfX;
		stMatchEntry.dfMatchY = stSgmtMatchRes.stMatchPoint.dfY;
		stMatchEntry.dfSgmtMatchLen = stSgmtMatchRes.dfSgmtMatchLen;
		stMatchEntry.dfIntersectLenSgmt = stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost;		// 소프트 비용(INTERSECT_LEN+방위각) → sort 선택 기준 (2026-07-08 최정우 추가)
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = 0.0;
		stMatchEntry.nDirAngleDiff = stSgmtMatchRes.nDirAngleDiff;
		stMatchEntry.qwLinkID = stSgmtMatchRes.qwLinkID;
		stMatchEntry.wLenFromLink = pstGridSgmtInfo->wLenFromLink;
		stMatchEntry.nMaxSpeed = pstLinkInfo->nMaxSpeed;
		stMatchEntry.dfLen = pstLinkInfo->dfLen;
		stMatchEntry.nRoadRank = pstLinkInfo->nRoadRank;
		stMatchEntry.nConnect = pstLinkInfo->nConnect;
		stMatchEntry.nRoadType = pstLinkInfo->nRoadType;
		stMatchEntry.nLanes = pstLinkInfo->nLanes;
		memcpy(stMatchEntry.szRoadName, pstLinkInfo->szRoadName, sizeof(stMatchEntry.szRoadName));
		stMatchEntry.qwStNodeID = pstLinkInfo->qwStNodeID;
		stMatchEntry.dfStNodeX = static_cast<double>(pstLinkInfo->dwStNodeX);
		stMatchEntry.dfStNodeY = static_cast<double>(pstLinkInfo->dwStNodeY);
		stMatchEntry.nStNodeType = pstLinkInfo->nStNodeType;
		stMatchEntry.qwEdNodeID = pstLinkInfo->qwEdNodeID;
		stMatchEntry.dfEdNodeX = static_cast<double>(pstLinkInfo->dwEdNodeX);
		stMatchEntry.dfEdNodeY = static_cast<double>(pstLinkInfo->dwEdNodeY);
		stMatchEntry.nEdNodeType = pstLinkInfo->nEdNodeType;

		listMatchEntryList.push_back(stMatchEntry);
	}

	if (listMatchEntryList.empty())								// 매칭된 결과가 없으면
		*pwErrorCode = MAP_MATCH_FAIL;
	else														// 매칭된 결과가 있으면
	{
		// 매칭 거리순 정렬
		listMatchEntryList.sort();

		*pwErrorCode = NO_ERROR;
		*pstMatchEntry = *listMatchEntryList.begin();
	}

	return (*pwErrorCode == NO_ERROR) ? true : false;
}

/**
 * @brief GRID 내 세그먼트 기하 최근접(반경 무시) — 진단반경 초과 SKIP 참고용 (2026-07-10 최정우 수정)
*/
bool CBeginMapMatch::GridSgmtGeomNearest(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset,
		uint32 dwEndSgmtOffset, MATCH_ENTRY& stBest, double& dfBestDist, bool& bFound)
{
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
		return false;

	for (uint32 i = dwStartSgmtOffset; i < dwEndSgmtOffset; ++i)
	{
		PGRID_SGMT_INFO pstGridSgmtInfo = m_pcDataLoader->GetGridSgmtInfo(i);
		if (!pstGridSgmtInfo)
			continue;

		SGMT_INFO stSgmtInfo;
		stSgmtInfo.stPoint.dfX = static_cast<double>(pstGridSgmtInfo->dwX);
		stSgmtInfo.stPoint.dfY = static_cast<double>(pstGridSgmtInfo->dwY);
		stSgmtInfo.nDirAng = static_cast<sint16>(pstGridSgmtInfo->wDirAng);
		stSgmtInfo.dfLen = static_cast<double>(pstGridSgmtInfo->wLenSgmt);
		stSgmtInfo.qwLinkID = pstGridSgmtInfo->qwLinkID;

		SGMT_MATCH_RES stSgmtMatchRes;
		// 반경(nRadius) 초과여도 INTERSECT_LEN·스냅 좌표만 계산 (정식 매칭 아님) (2026-07-10 최정우 수정)
		if (!m_cGISUtil.SgmtMatch(stSgmtMatchInput, stSgmtInfo, &stSgmtMatchRes, true))
			continue;

		PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(stSgmtInfo.qwLinkID);
		if (!pstLinkInfo)
			continue;

		if (bFound && (stSgmtMatchRes.dfIntersectLenSgmt >= dfBestDist))
			continue;

		MATCH_ENTRY stMatchEntry;
		stMatchEntry.dfMatchX = stSgmtMatchRes.stMatchPoint.dfX;
		stMatchEntry.dfMatchY = stSgmtMatchRes.stMatchPoint.dfY;
		stMatchEntry.dfSgmtMatchLen = stSgmtMatchRes.dfSgmtMatchLen;
		stMatchEntry.dfIntersectLenSgmt = stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost;
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = 0.0;
		stMatchEntry.nDirAngleDiff = stSgmtMatchRes.nDirAngleDiff;
		stMatchEntry.qwLinkID = stSgmtMatchRes.qwLinkID;
		stMatchEntry.wLenFromLink = pstGridSgmtInfo->wLenFromLink;
		stMatchEntry.nMaxSpeed = pstLinkInfo->nMaxSpeed;
		stMatchEntry.dfLen = pstLinkInfo->dfLen;
		stMatchEntry.nRoadRank = pstLinkInfo->nRoadRank;
		stMatchEntry.nConnect = pstLinkInfo->nConnect;
		stMatchEntry.nRoadType = pstLinkInfo->nRoadType;
		stMatchEntry.nLanes = pstLinkInfo->nLanes;
		memcpy(stMatchEntry.szRoadName, pstLinkInfo->szRoadName, sizeof(stMatchEntry.szRoadName));
		stMatchEntry.qwStNodeID = pstLinkInfo->qwStNodeID;
		stMatchEntry.dfStNodeX = static_cast<double>(pstLinkInfo->dwStNodeX);
		stMatchEntry.dfStNodeY = static_cast<double>(pstLinkInfo->dwStNodeY);
		stMatchEntry.nStNodeType = pstLinkInfo->nStNodeType;
		stMatchEntry.qwEdNodeID = pstLinkInfo->qwEdNodeID;
		stMatchEntry.dfEdNodeX = static_cast<double>(pstLinkInfo->dwEdNodeX);
		stMatchEntry.dfEdNodeY = static_cast<double>(pstLinkInfo->dwEdNodeY);
		stMatchEntry.nEdNodeType = pstLinkInfo->nEdNodeType;

		stBest = stMatchEntry;
		dfBestDist = stSgmtMatchRes.dfIntersectLenSgmt;
		bFound = true;
	}

	return bFound;
}

/**
 * @brief 소속·인접 GRID 에서 반경 무시 기하 최근접 세그먼트 1건 (2026-07-10 최정우 수정)
 * @remark 정식 매칭 실패·진단반경(MM_DIAG_RADIUS_M) 내 후보도 없을 때 호출.
 *         그리드에 링크가 있으나 거리만 먼 경우 SKIP용 MATCH_LAT/LON·INTERSECT_LEN 확보.
*/
bool CBeginMapMatch::FindGeomNearest(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry)
{
	m_pcDataLoader = pcDataLoader;

	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	uint32 dwGridID = m_cGISUtil.GetGridID(stSgmtMatchInput.stPoint.dfX, stSgmtMatchInput.stPoint.dfY);
	vector<uint32> vtNearGridIDList;
	vtNearGridIDList.push_back(dwGridID);
	// 인접 GRID 탐색 반경은 MM_DIAG_RADIUS_M 과 동일(그리드 선정용) (2026-07-10 최정우 수정)
	m_cGISUtil.GetNearGridID(dwGridID, stSgmtMatchInput, vtNearGridIDList);

	stSgmtMatchInput.stPoint.dfX *= 360000.0;
	stSgmtMatchInput.stPoint.dfY *= 360000.0;

	bool bFound = false;
	double dfBestDist = -1.0;
	MATCH_ENTRY stBest;

	for (size_t i = 0; i < vtNearGridIDList.size(); ++i)
	{
		PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(vtNearGridIDList[i]);
		if (!pstGridInfo)
			continue;

		uint32 dwStartSgmtOffset = pstGridInfo->dwSgmtOffset;
		uint32 dwEndSgmtOffset = dwStartSgmtOffset + pstGridInfo->wSgmtCount;
		GridSgmtGeomNearest(stSgmtMatchInput, dwStartSgmtOffset, dwEndSgmtOffset,
			stBest, dfBestDist, bFound);
	}

	if (!bFound)
	{
		*pwErrorCode = MAP_MATCH_FAIL;
		return false;
	}

	*pwErrorCode = NO_ERROR;
	*pstMatchEntry = stBest;
	return true;
}
