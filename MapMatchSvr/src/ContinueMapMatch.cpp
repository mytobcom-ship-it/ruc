/**
 * @file ContinueMapMatch.cpp
 * @brief 연속 맵매칭 클래스 소스 파일
*/
#include "ContinueMapMatch.h"

/**
 * @brief 생성자
*/
CContinueMapMatch::CContinueMapMatch() : 
	m_pcDataLoader(nullptr)
{
}

/**
 * @brief 소멸자
*/
CContinueMapMatch::~CContinueMapMatch()
{
}

/**
 * @brief 연속 맵매칭 고도 보조 점수 설정 (config altitude_*)
 * @param[in] stAltConfig 고도 점수 설정 — Server→ProcessManager→MapMatch→ContinueMapMatch 전달
 * @return void
*/
void CContinueMapMatch::SetAltitudeConfig(const ALTITUDE_SCORE_CONFIG& stAltConfig)
{
	m_stAltitudeConfig = stAltConfig;
}

/**
 * @brief 연속 맵매칭 시작
 * @param[in] pcDataLoader 데이터 로딩 클래스
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] qwLinkID 링크 ID
 * @param[in] nSearchStep 탐색할 단계 (0~5)
 * @param[out] pwErrorCode 에러 코드
 * @param[out] pstMatchEntry 검색 정보
 * @return true, false
*/
bool CContinueMapMatch::StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput, 
		uint64& qwLinkID, sint16& nSearchStep, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx)
{
	m_pcDataLoader = pcDataLoader;

	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	// 검색된 목록 저장 (중복 검사 처리용)
	set<uint32> setSearchHistoryLinkList;

	// 연결 링크 목록
	listDepthLinkInfo listDepthLinkInfoList;

	// 직전 링크 ID로 링크 메타 정보 조회 (2026-07-08 최정우 주석 추가)
	PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(qwLinkID);
	if (!pstLinkInfo)
	{
		*pwErrorCode = NOT_FOUND_LINKID;
		return false;
	}

	stSgmtMatchInput.stPoint.dfX *= 360000.0;
	stSgmtMatchInput.stPoint.dfY *= 360000.0;

	// 0 depth 세팅 
	// 링크 세그먼트 정보
	DEPTH_LINK_INFO_DATA stDepthLinkInfoData;

	stDepthLinkInfoData.qwLinkID = qwLinkID;
	stDepthLinkInfoData.dwStartTurnOffset = pstLinkInfo->dwTurnOffset;
	stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstLinkInfo->nTurnCount;
	stDepthLinkInfoData.dwStartSgmtOffset = pstLinkInfo->dwSgmtOffset;
	stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstLinkInfo->wSgmtCount;

	listDepthLinkInfoList.push_back(stDepthLinkInfoData);

	// depth 이내의 맵 매칭 유효 거리 이내 링크 목록
	list<MATCH_ENTRY> listMatchEntryList;

	// 요청 depth 이내에서 검색
	for (uint16 i=0; i<=nSearchStep; ++i)
	{
		// 같은 depth 내 맵 매칭 유효 거리 이내 링크 목록 초기화
		listMatchEntryList.clear();

		// 같은 depth 링크 UID 목록 맵 매칭
		listDepthLinkInfo::iterator it = listDepthLinkInfoList.begin();
		for (; it != listDepthLinkInfoList.end(); ++it)
		{
			// 링크별 세그컨트 맵매칭
			LinkSgmtMapMatch(stSgmtMatchInput, *it, &listMatchEntryList);

			// 검색된 목록 저장 (중복 검사 처리용)
			setSearchHistoryLinkList.insert(it->qwLinkID);
		}

		// 같은 depth 의 링크를 모두 검색하고 매칭된 링크 목록이 있으면
		if (!listMatchEntryList.empty())
		{
			*pwErrorCode = NO_ERROR;
			// 동일 depth 후보 중 비용 최소 매칭 결과 선택 (2026-07-08 최정우 주석 추가)
			if (pstTraceCtx != nullptr)
				pstTraceCtx->nMatchedStep = static_cast<sint16>(i);
			GetMatchEntry(&listMatchEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
			return true;
		}

		// 현재 검색 단계가 최대 설정 검색 단계이면 다음 depth 링크 목록 정보를 가져오지 않음
		if (i == nSearchStep) break;

		// 현재 depth 링크 목록이 맵 매칭에 실패시 다음 depth 링크 목록 정보
		if (!GetLinkDepthInfo(&setSearchHistoryLinkList, &listDepthLinkInfoList))
		{
			*pwErrorCode = MAP_MATCH_FAIL;
			return false;
		}
	}

	*pwErrorCode = MAP_MATCH_FAIL;
	return false;
}

/**
 * @brief 링크별 세그컨트 맵매칭
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] stDepthLinkInfoData 링크 회전 정보 및 세그먼트 정보
 * @param[out] plistMatchEntryList 검색 정보 목록
 * @return true, false
*/
bool CContinueMapMatch::LinkSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, 
		DEPTH_LINK_INFO_DATA& stDepthLinkInfoData, list<MATCH_ENTRY> *plistMatchEntryList)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	for (uint32 i=stDepthLinkInfoData.dwStartSgmtOffset; i<stDepthLinkInfoData.dwEndSgmtOffset; ++i)
	{
		SGMT_MATCH_RES stSgmtMatchRes;

		// 링크 세그먼트 정보
		PLINK_SGMT_INFO pstLinkSgmtInfo = m_pcDataLoader->GetLinkSgmtInfo(i);
		if (!pstLinkSgmtInfo)
			continue;

		// 세그먼트 정보 구조체
		SGMT_INFO stSgmtInfo;

		// 세그먼트 X,Y 좌표
		stSgmtInfo.stPoint.dfX = static_cast<double>(pstLinkSgmtInfo->dwX);
		stSgmtInfo.stPoint.dfY = static_cast<double>(pstLinkSgmtInfo->dwY);

		// 세그먼트 진행 방향(방위각)
		stSgmtInfo.nDirAng = static_cast<sint16>(pstLinkSgmtInfo->wDirAng);

		// 세그먼트 길이 형변환
		stSgmtInfo.dfLen = static_cast<double>(pstLinkSgmtInfo->wLenSgmt);

		// 링크 ID
		stSgmtInfo.qwLinkID = pstLinkSgmtInfo->qwLinkID;

		// GPS-세그먼트 거리·방위 기본 매칭 (2026-07-08 최정우 주석 추가)
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
		// ── 연속 맵매칭 고도 보조 비용 가산 (Begin 미적용) ──
		//   dfCost = 수직거리 + 방위각비용 + CalcAltRoadPenalty(Δalt, ROAD_TYPE)
		//   값이 작을수록 우선 — bonus(음수)면 동일 거리·방향 후보보다 유리
		//   예) 기본 25 + 고도−3 = 22 → 같은 고가·Δ6m 후보가 일반도로(+10)보다 선택
		//   (2026-07-08 최정우 추가)
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = m_cGISUtil.CalcAltRoadPenalty(stSgmtMatchInput, pstLinkInfo->nRoadType, m_stAltitudeConfig);
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost + stMatchEntry.dfAltAdj;
		stMatchEntry.nDirAngleDiff = stSgmtMatchRes.nDirAngleDiff;
		stMatchEntry.qwLinkID = stSgmtMatchRes.qwLinkID;
		stMatchEntry.wLenFromLink = pstLinkSgmtInfo->wLenFromLink;
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

		plistMatchEntryList->push_back(stMatchEntry);
	}

	return (!plistMatchEntryList->empty()) ? true : false;
}

/**
 * @brief 연결 링크 정보
 * @param[in,out] psetSearchHistoryLinkList 검색된 링크 UID 목록 (중복 검사용)
 * @param[out] plistDepthLinkInfoList 연결 링크 UID 정보
 * @return true, false
*/
bool CContinueMapMatch::GetLinkDepthInfo(set<uint32> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	// 이전 depth 링크 UID 정보 목록
	uint32 dwDepthCount = static_cast<uint32>(plistDepthLinkInfoList->size());
	listDepthLinkInfo::iterator it = plistDepthLinkInfoList->begin();
	for (uint32 dwIndex=0; it != plistDepthLinkInfoList->end() && dwIndex < dwDepthCount; ++dwIndex)
	{
		uint32 dwStartTurnOffset = it->dwStartTurnOffset;
		uint32 dwEndTurnOffset = it->dwEndTurnOffset;

		// 이전 depth 링크 ID 삭제
		plistDepthLinkInfoList->erase(it++);

		for (uint32 i=dwStartTurnOffset; i<dwEndTurnOffset; ++i)
		{
			DEPTH_LINK_INFO_DATA stDepthLinkInfoData;

			// 분기(턴) 정보로 후속 링크 ID 조회 (2026-07-08 최정우 주석 추가)
			PTURN_INFO pstTurnInfo = m_pcDataLoader->GetTurnInfo(i);
			if (!pstTurnInfo) continue;

			// 이미 검사한 링크 ID 이면 제외
			set<uint32>::iterator history_it = psetSearchHistoryLinkList->find(pstTurnInfo->qwOutLinkID);
			if (history_it != psetSearchHistoryLinkList->end())
				continue;

			// 출력 링크 메타(세그먼트·턴 오프셋) 조회 (2026-07-08 최정우 주석 추가)
			PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(pstTurnInfo->qwOutLinkID);
			if (!pstLinkInfo) continue;

			// 회전 정보가 없으면
			if ((pstLinkInfo->dwTurnOffset == 0) && (pstLinkInfo->nTurnCount == 0))
			{
				psetSearchHistoryLinkList->insert(pstTurnInfo->qwOutLinkID);
				continue;
			}

			stDepthLinkInfoData.qwLinkID = pstTurnInfo->qwOutLinkID;
			stDepthLinkInfoData.dwStartTurnOffset = pstLinkInfo->dwTurnOffset;
			stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstLinkInfo->nTurnCount;
			stDepthLinkInfoData.dwStartSgmtOffset = pstLinkInfo->dwSgmtOffset;
			stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstLinkInfo->wSgmtCount;

			plistDepthLinkInfoList->push_back(stDepthLinkInfoData);
		}
	}

	return (!plistDepthLinkInfoList->empty()) ? true : false;
}

/**
 * @brief 검색 정보 목록 중 비용 최소 후보 추출 (dfCost 오름차순, 동률 시 수직거리)
 * @param[in] plistMatchEntryList 검색 정보 목록
 * @param[out] pstMatchEntry 검색 정보
 * @return void
*/
void CContinueMapMatch::GetMatchEntry(list<MATCH_ENTRY> *plistMatchEntryList, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx, const SGMT_MATCH_INPUT& stSgmtMatchInput)
{
	plistMatchEntryList->sort();
	if (pstTraceCtx != nullptr)
		CMatchTrace::LogResult(pstTraceCtx, stSgmtMatchInput, *plistMatchEntryList, plistMatchEntryList->front());
	*pstMatchEntry = *plistMatchEntryList->begin();
}
