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
 * @param[out] psetSearchHistoryLinkID (선택) depth 탐색(maxstep 이내)으로 실제 방문한 전체
 *   링크 UID 집합 — CMapMatch::ContinueMapMatch()가 Begin 병행폴백 후보의 "진짜 갈림길" 여부
 *   판별에 사용 (2026-08-21 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CContinueMapMatch::StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint64& qwLinkID, sint16& nSearchStep, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx, vector<uint64> *pvtPathLinkIDs,
		set<uint64> *psetSearchHistoryLinkID)
{
	// 경로 역추적용 — 새로 발견된 링크가 어느 링크를 거쳐 도달했는지 기록 (2026-08-20 최정우 추가)
	unordered_map<uint64, uint64> mapParentLink;
	m_pcDataLoader = pcDataLoader;

	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((m_pcDataLoader == nullptr) || (!m_pcDataLoader->IsLoad()))
	{
		LOGFMTE("data loading fail!");
		return false;
	}

	// 검색된 목록 저장 (중복 검사 처리용) — 링크 ID는 uint64(2026-08-14 최정우 수정, set<uint32>였던
	//   걸 원복 — "소스상 문제" 검토 중 발견, BeginMapMatch 의 동일 목적 집합은 원래부터 set<uint64>)
	set<uint64> setSearchHistoryLinkList;
	// psetSearchHistoryLinkID 로 밖에 노출하는 목록 — setSearchHistoryLinkList 와 달리 BridgeNearbyLinkStarts
	//   로 찾은(TURNINFO 아닌 좌표 근접) 링크는 제외한다. Begin 폴백 화이트리스트 목적(진짜 갈림길
	//   형제 판별)에는 좌표만 근접한 링크까지 "탐색해봤다"고 인정하면 안 되기 때문 — 실측 확인
	//   (trip 000376_20260819094414 seq54, 2040425201 이 브릿지로 setSearchHistoryLinkList 에는
	//   들어가 화이트리스트를 통과, Begin 폴백에서 다시 채택되던 문제) (2026-08-21 최정우 추가)
	set<uint64> setGenuineSearchHistoryLinkList;

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
	// 같은 링크 노이즈 보정 기준점도 동일 내부 스케일로 변환 (2026-07-22 최정우 추가)
	stSgmtMatchInput.dfPrevMatchX *= 360000.0;
	stSgmtMatchInput.dfPrevMatchY *= 360000.0;
	// 역행 페널티 판정용 직전 링크 ID — 이 링크 위 후보만 dfPrevLinkPos 와 비교 (2026-07-20 최정우 추가)
	stSgmtMatchInput.qwPrevLinkID = qwLinkID;
	// 링크가 바뀌는 후보의 진입/진출 노드 판정용 — 직전 링크의 종료 노드 (2026-07-21 최정우 추가)
	stSgmtMatchInput.qwPrevEdNodeID = pstLinkInfo->qwEdNodeID;
	// 진입링크 후보의 확장 위치(역행 거리) 계산용 — 직전 링크 길이 (2026-07-22 최정우 추가)
	stSgmtMatchInput.dfPrevLinkLen = pstLinkInfo->dfLen;

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
	list<MATCH_ENTRY> listMatchEntryList;			// 현재 depth 후보
	// 누적 후보 목록 — 최적 후보가 링크 경계 클램프면 다음 depth 를 확장해 함께 비교 (2026-07-15 최정우 추가)
	list<MATCH_ENTRY> listAllEntryList;
	sint16 nBestStep = 0;

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
			// 화이트리스트 노출용 — 좌표 근접 브릿지 링크는 제외 (2026-08-21 최정우 추가)
			if (!it->bGeometricBridge)
				setGenuineSearchHistoryLinkList.insert(it->qwLinkID);
		}

		// 이번 depth 후보를 누적 목록에 병합 (2026-07-15 최정우 수정)
		if (!listMatchEntryList.empty())
		{
			listAllEntryList.insert(listAllEntryList.end(),
				listMatchEntryList.begin(), listMatchEntryList.end());
			nBestStep = static_cast<sint16>(i);
		}

		// 누적 후보가 있으면 최적 후보 판정
		if (!listAllEntryList.empty())
		{
			listAllEntryList.sort();
			// 최적 후보가 링크 경계 클램프가 아니고(정상 내부 수선발) 방위각도 잘 맞으면, 더 깊이 갈 수 없으면 확정.
			//   경계 클램프면 차량이 링크 끝을 지난 것 → 연결 다음 링크에 더 나은 후보가 있을 수 있어 depth 확장.
			//   방위각 부적합(비용이 상한 도달)이면 직전 링크 위 내부 수선발이어도 회전·교차로일 수 있어
			//   depth 확장해 연결 링크와 비교(2026-07-18 최정우 추가) — 직전 링크에 계속 고정되는 것 방지.
			//   (두 경우 모두, 확장해도 다음 depth 후보가 더 나쁘면 sort 후 그대로 이 후보가 선택되므로 안전) (2026-07-15 최정우 추가)
			if ((!IsBoundaryClamped(listAllEntryList.front())
					&& !IsPoorAngleFit(listAllEntryList.front())) || (i == nSearchStep))
			{
				*pwErrorCode = NO_ERROR;
				if (pstTraceCtx != nullptr)
					pstTraceCtx->nMatchedStep = nBestStep;
				GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
				if (pvtPathLinkIDs != nullptr)
					ReconstructPath(pstMatchEntry->qwLinkID, mapParentLink, pvtPathLinkIDs);
				if (psetSearchHistoryLinkID != nullptr)
					*psetSearchHistoryLinkID = setGenuineSearchHistoryLinkList;
				return true;
			}
		}

		// 현재 검색 단계가 최대 설정 검색 단계이면 다음 depth 링크 목록 정보를 가져오지 않음
		if (i == nSearchStep) break;

		// 현재 depth 링크 목록이 맵 매칭에 실패시 다음 depth 링크 목록 정보
		if (!GetLinkDepthInfo(&setSearchHistoryLinkList, &listDepthLinkInfoList, &mapParentLink, stSgmtMatchInput.nSpeed))
		{
			// 더 확장할 연결 링크가 없으면, 지금까지 누적 후보가 있으면 그걸로 확정 (2026-07-15 최정우 수정)
			if (!listAllEntryList.empty())
			{
				*pwErrorCode = NO_ERROR;
				if (pstTraceCtx != nullptr)
					pstTraceCtx->nMatchedStep = nBestStep;
				GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
				if (pvtPathLinkIDs != nullptr)
					ReconstructPath(pstMatchEntry->qwLinkID, mapParentLink, pvtPathLinkIDs);
				if (psetSearchHistoryLinkID != nullptr)
					*psetSearchHistoryLinkID = setGenuineSearchHistoryLinkList;
				return true;
			}
			*pwErrorCode = MAP_MATCH_FAIL;
			return false;
		}
	}

	// 루프 종료 후 누적 후보가 있으면 확정 (경계 클램프만 있었던 경우)
	if (!listAllEntryList.empty())
	{
		*pwErrorCode = NO_ERROR;
		if (pstTraceCtx != nullptr)
			pstTraceCtx->nMatchedStep = nBestStep;
		GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
		if (pvtPathLinkIDs != nullptr)
			ReconstructPath(pstMatchEntry->qwLinkID, mapParentLink, pvtPathLinkIDs);
		if (psetSearchHistoryLinkID != nullptr)
			*psetSearchHistoryLinkID = setGenuineSearchHistoryLinkList;
		return true;
	}

	*pwErrorCode = MAP_MATCH_FAIL;
	return false;
}

/**
 * @brief 최적 후보가 링크 경계(시작/끝)에 스냅(클램프)됐는지 판정 (2026-07-15 최정우 추가)
 * @param[in] stMatchEntry 판정 대상 후보
 * @return true(경계 클램프 — 링크 끝/시작에 수선발 스냅), false(내부 수선발)
 * @remark
 *   링크 시작점→수선발 거리(m) = wLenFromLink(링크시작→세그먼트시작) + dfSgmtMatchLen(세그먼트내 거리).
 *   이 값이 0 근처(시작) 또는 링크길이(dfLen) 근처(끝)이면 세그먼트 끝점에 스냅된 경계 클램프.
 *   차량이 링크 끝을 지나 다음 링크로 넘어간 상황에서 발생 → 연결 링크 확장 판단에 사용.
*/
bool CContinueMapMatch::IsBoundaryClamped(const MATCH_ENTRY& stMatchEntry)
{
	if (stMatchEntry.dfLen <= 0.0)
		return false;

	double dfFootFromStart = static_cast<double>(stMatchEntry.wLenFromLink) + stMatchEntry.dfSgmtMatchLen;
	return (dfFootFromStart <= MM_CLAMP_EPS) ||
	       (dfFootFromStart >= (stMatchEntry.dfLen - MM_CLAMP_EPS));
}

/**
 * @brief 최적 후보의 방위각이 심하게 안 맞는지 판정 (2026-07-18 최정우 추가)
 * @param[in] stMatchEntry 판정 대상 후보
 * @return true(방위각 비용이 상한(MM_DIR_MAX_PENALTY)에 도달 — 방향이 거의 안 맞음), false(정상)
 * @remark
 *   dfAngleCost = dfCost - dfIntersectLenSgmt (방위각 비용만 분리, GISUtil::SgmtMatch 참고).
 *   상한 도달 = 방향이 심하게 어긋남에도 직전 링크 위에 내부 수선발이 잡혀 depth 확장이
 *   안 되던 경우(회전·교차로 구간에서 직전 링크에 계속 고정되는 현상) 방지용.
*/
bool CContinueMapMatch::IsPoorAngleFit(const MATCH_ENTRY& stMatchEntry)
{
	// dfAngleCost = (dfIntersectLenSgmt+cap) - dfIntersectLenSgmt 형태로 역산되어 부동소수점
	//   반올림 오차로 정확히 cap 값이 아닐 수 있어 허용오차(0.01m) 적용 (2026-07-18 최정우 수정)
	return stMatchEntry.dfAngleCost >= (MM_DIR_MAX_PENALTY - 0.01);
}

/**
 * @brief 링크별 세그컨트 맵매칭
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] stDepthLinkInfoData 링크 회전 정보 및 세그먼트 정보
 * @param[out] plistMatchEntryList 검색 정보 목록
 * @param[in] bAllowOppositeCheck 역행 의심 시 opposite link(qwOppositeLinkID) 후보 재귀 평가 허용 여부 —
 *   opposite link 자체를 평가하는 재귀 호출에서는 false 로 넘겨 상호 재귀를 막는다 (2026-08-19 최정우 추가)
 * @return true(성공), false(실패)
*/
bool CContinueMapMatch::LinkSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput,
		DEPTH_LINK_INFO_DATA& stDepthLinkInfoData, list<MATCH_ENTRY> *plistMatchEntryList,
		bool bAllowOppositeCheck)
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

		// INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·방위 기본 매칭 (2026-07-08 최정우 주석 추가)
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
		stMatchEntry.bSgmtClamped = stSgmtMatchRes.bSgmtClamped;			// 세그먼트 끝점 스냅 여부 (2026-07-21 최정우 추가)
		stMatchEntry.bHasHeading = stSgmtMatchRes.bHasHeading;				// heading 값 존재 여부 (2026-07-22 최정우 추가)
		stMatchEntry.bClampTrustedByHeading = stSgmtMatchRes.bClampTrustedByHeading;	// 클램프+heading 신뢰 구제 신호 (2026-08-20 최정우 추가)
		// ── 연속 맵매칭 고도 보조 비용 가산 (Begin 미적용) ──
		//   dfCost = INTERSECT_LEN + 방위각비용 + CalcAltRoadPenalty(Δalt, ROAD_TYPE)
		// 값이 작을수록 우선 — 보너스(음수)면 동일 거리·방향 후보보다 유리
		//   예) 기본 25 + 고도−3 = 22 → 같은 고가·Δ6m 후보가 일반도로(+10)보다 선택
		//   (2026-07-08 최정우 추가)
		stMatchEntry.dfAngleCost = stSgmtMatchRes.dfCost - stSgmtMatchRes.dfIntersectLenSgmt;
		stMatchEntry.dfAltAdj = m_cGISUtil.CalcAltRoadPenalty(stSgmtMatchInput, pstLinkInfo->nRoadType, m_stAltitudeConfig);
		stMatchEntry.dfCost = stSgmtMatchRes.dfCost + stMatchEntry.dfAltAdj;
		// 좌표 근접(BridgeNearbyLinkStarts)만으로 발견된 후보는 위상 우선순위 페널티 — 같은 경합에
		//   진짜 TURNINFO 연결 후보가 있으면 근소한 차이로는 못 이기게 한다 (2026-08-21 최정우 추가)
		if (stDepthLinkInfoData.bGeometricBridge)
			stMatchEntry.dfCost += MM_GEOM_BRIDGE_PENALTY;
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

		// 직전 매칭 위치보다 뒤로 가는 후보 감지 — 같은 링크 위 GPS 노이즈로 인한 역행 스냅(오락가락)
		//   구분용. 링크가 바뀌면(정상 전진) 비교 대상 아님 (2026-07-20 최정우 추가)
		if (stSgmtMatchInput.bHasPrevLinkPos && (stMatchEntry.qwLinkID == stSgmtMatchInput.qwPrevLinkID))
		{
			double dfCurPos = static_cast<double>(stMatchEntry.wLenFromLink) + stMatchEntry.dfSgmtMatchLen;
			if (dfCurPos < stSgmtMatchInput.dfPrevLinkPos)
			{
				double dfBackward = stSgmtMatchInput.dfPrevLinkPos - dfCurPos;

				// 역행 의심(bReverseSuspect) — 위치가 조금이라도 뒤로 갔고 heading 도 세그먼트
				//   역방향에 더 가까울 때만 표시. RawLogWorker 의 연속역행(reverse_confirm)
				//   판정이 GPS 노이즈에 흔들리지 않고 heading 이 뒷받침되는 경우만 스트릭에 반영하게 함
				//   (2026-07-21 최정우 추가)
				if ((dfBackward > MM_REVERSE_SUSPECT_EPS) && stSgmtMatchRes.bReverseFit)
				{
					stMatchEntry.bReverseSuspect = true;

					// 반대방향(왕복분리) 짝 링크가 있으면 추가 후보로 평가 — 그래프(TURNINFO)상
					//   연결 안 된 물리적 짝 링크를 후보에 끌어와, 비용이 더 낮으면(정방향 적합)
					//   listAllEntryList 정렬(operator<)로 자연스럽게 우선 선택되게 한다 (2026-08-19 최정우 추가)
					if (bAllowOppositeCheck)
						TryOppositeLinkCandidate(stSgmtMatchInput, pstLinkInfo->qwOppositeLinkID, plistMatchEntryList);
				}
				else if (dfBackward > MM_REVERSE_SUSPECT_EPS)
				{
					// heading 이 역방향 쪽이 아님(bReverseFit=false) — 이 뒤로 밀린 위치가
					//   "확실한 노이즈"인지 "판단 불가"인지 구분한다.
					//   확실한 노이즈: heading 이 있고, 방위각이 정방향과 잘 맞고(각도비용이 상한 미도달),
					//     GPS 오차(INTERSECT_LEN)도 작음 → 매칭 좌표를 직전 위치보다 살짝 앞으로 보정해
					//     지도에 역주행처럼 튀어 보이지 않게 하고, MATCH_STATUS 는 그대로 둔다(MATCHED 유지).
					//   판단 불가: heading 이 없거나(NO_ANGLE) 각도가 애매/큼 → 노이즈라 단정 못 하므로
					//     좌표는 계산된 값 그대로 두고 SKIP 표시만 얹어 다운스트림이 신뢰하지 않게 한다.
					//   (2026-07-22 최정우 추가)
					bool bPoorAngle = IsPoorAngleFit(stMatchEntry);
					bool bDefiniteNoise = stSgmtMatchRes.bHasHeading && !bPoorAngle
						&& (stMatchEntry.dfIntersectLenSgmt <= MM_CLAMP_SKIP_LEN);

					if (bDefiniteNoise)
					{
						double dfNewPos = stSgmtMatchInput.dfPrevLinkPos + MM_NOISE_FORWARD_NUDGE_M;
						if (dfNewPos >= stMatchEntry.dfLen)
						{
							// 링크 끝(END 노드)을 넘으면 END 노드 좌표로 클램프
							stMatchEntry.dfMatchX = stMatchEntry.dfEdNodeX;
							stMatchEntry.dfMatchY = stMatchEntry.dfEdNodeY;
							stMatchEntry.dfSgmtMatchLen = stMatchEntry.dfLen
								- static_cast<double>(stMatchEntry.wLenFromLink);
						}
						else
						{
							// 이번 후보(현재 GPS) 자신의 계산값이 아니라, 마지막으로 신뢰했던
							//   실제 매칭 좌표(dfPrevMatchX/Y)를 기준점으로 삼아 세그먼트 방향으로
							//   정확히 MM_NOISE_FORWARD_NUDGE_M(1m)만 전진시킨다 — 이번 후보의
							//   좌표 자체가 노이즈라 못 믿는 상황이므로, 노이즈가 섞인 값을 기준으로
							//   재보정하지 않고 마지막 확실한 지점에서 다시 계산한다 (2026-07-22 최정우 수정)
							stMatchEntry.dfMatchX = stSgmtMatchInput.dfPrevMatchX
								+ MM_NOISE_FORWARD_NUDGE_M * sin(RAD(static_cast<double>(stSgmtInfo.nDirAng)));
							stMatchEntry.dfMatchY = stSgmtMatchInput.dfPrevMatchY
								+ MM_NOISE_FORWARD_NUDGE_M * cos(RAD(static_cast<double>(stSgmtInfo.nDirAng)));
							stMatchEntry.dfSgmtMatchLen = dfNewPos - static_cast<double>(stMatchEntry.wLenFromLink);
						}
					}
					else
					{
						stMatchEntry.bAmbiguousReverse = true;
					}
				}
			}
		}
		// 링크가 바뀌는 후보 — 진입/진출 노드 판정 (2026-07-21 최정우 추가 — 진입링크 역행 감지)
		//   위 역행 페널티/의심 판정은 "같은 링크 위에서 뒤로 감"만 잡아서, 후보 링크 자체가
		//   바뀌면(예: 링크 경계 클램프로 depth 확장돼 인접 링크가 선택되는 경우) 전혀 걸리지 않았다.
		//   그 결과 reverse_confirm(연속역행 확정) 스트릭 판정 대상에서 완전히 빠져, 저속+GPS 노이즈만
		//   있어도 진입 링크(직전 링크와 같은 노드로 "들어가는" 링크, 즉 진행 방향상 나올 수 없는 링크)로
		//   단 1건만에 확정 매칭되는 문제가 있었다.
		//   정상 전진(진출)이라면 후보의 시작 노드(qwStNodeID)가 직전 링크의 종료 노드와 같아야 한다.
		//   대신 후보의 "종료" 노드가 직전 링크의 종료 노드와 같다면, 그 후보는 같은 노드로 들어가는
		//   방향(진입)이라는 뜻 — 위상적으로 확실한 역행 신호이므로 heading/거리와 무관하게 표시한다.
		//   (같은 링크 위 판정과 마찬가지로 여기서도 SKIP 이 아니라 "의심" 신호만 세팅 — reverse_confirm
		//   만큼 연속되어야 실제 역행/유턴으로 확정되고, 1건짜리 노이즈는 이 신호만으로 SKIP·앵커 고정된다)
		else if (stSgmtMatchInput.qwPrevEdNodeID != 0 && (stMatchEntry.qwLinkID != stSgmtMatchInput.qwPrevLinkID)
			&& (stMatchEntry.qwStNodeID != stSgmtMatchInput.qwPrevEdNodeID)
			&& (stMatchEntry.qwEdNodeID == stSgmtMatchInput.qwPrevEdNodeID))
		{
			// 위상적으로 확실한 역행이므로 의심 신호는 무조건 세팅(거리·margin 무관) — 후보의
			//   "종료" 노드가 직전 링크의 종료 노드와 같다면, 그 후보는 같은 노드로 들어가는
			//   방향(진입)이라는 뜻 — heading/거리와 무관하게 표시한다 (2026-07-22 최정우 추가)
			stMatchEntry.bReverseSuspect = true;

			// 반대방향(왕복분리) 짝 링크가 있으면 추가 후보로 평가 (2026-08-19 최정우 추가)
			if (bAllowOppositeCheck)
				TryOppositeLinkCandidate(stSgmtMatchInput, pstLinkInfo->qwOppositeLinkID, plistMatchEntryList);
		}

		plistMatchEntryList->push_back(stMatchEntry);
	}

	return (!plistMatchEntryList->empty()) ? true : false;
}

/**
 * @brief 역행 의심 후보의 반대방향(왕복분리) 짝 링크를 추가 후보로 평가
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 정보
 * @param[in] qwOppositeLinkID 짝 링크 ID (LINK_INFO.qwOppositeLinkID, 0=없음)
 * @param[out] plistMatchEntryList 검색 정보 목록 — 짝 링크의 매칭 결과를 추가로 append
 * @return void
 * @remark
 *   그래프(TURNINFO) 기반 depth 확장으로는 도달 불가능한, 물리적으로만 가까운 반대방향 짝
 *   링크(CreateData::CBinaryMaker::ComputeOppositeLinkPairs 가 사전 계산)를 강제로 후보에
 *   포함시킨다. LinkSgmtMapMatch 를 bAllowOppositeCheck=false 로 재귀 호출해, 짝 링크 평가
 *   결과가 다시 자기 자신을 짝으로 끌어오는 상호 재귀를 막는다 (2026-08-19 최정우 추가)
*/
void CContinueMapMatch::TryOppositeLinkCandidate(SGMT_MATCH_INPUT& stSgmtMatchInput,
		const uint64& qwOppositeLinkID, list<MATCH_ENTRY> *plistMatchEntryList)
{
	if (qwOppositeLinkID == 0)
		return;

	PLINK_INFO pstOppLinkInfo = m_pcDataLoader->GetLinkInfo(qwOppositeLinkID);
	if (pstOppLinkInfo == nullptr)
		return;

	DEPTH_LINK_INFO_DATA stOppDepthInfo;
	stOppDepthInfo.qwLinkID = qwOppositeLinkID;
	stOppDepthInfo.dwStartTurnOffset = pstOppLinkInfo->dwTurnOffset;
	stOppDepthInfo.dwEndTurnOffset = stOppDepthInfo.dwStartTurnOffset + pstOppLinkInfo->nTurnCount;
	stOppDepthInfo.dwStartSgmtOffset = pstOppLinkInfo->dwSgmtOffset;
	stOppDepthInfo.dwEndSgmtOffset = stOppDepthInfo.dwStartSgmtOffset + pstOppLinkInfo->wSgmtCount;

	LinkSgmtMapMatch(stSgmtMatchInput, stOppDepthInfo, plistMatchEntryList, false);
}

/**
 * @brief 연결 링크 정보
 * @param[in,out] psetSearchHistoryLinkList 검색된 링크 UID 목록 (중복 검사용)
 * @param[out] plistDepthLinkInfoList 연결 링크 UID 정보
 * @param[out] pmapParentLink 새로 발견된 링크가 거쳐온 직전 링크 기록 (경로 역추적용, 2026-08-20 최정우 추가)
 * @return true(성공), false(실패)
 * @remark (2026-08-21 최정우) 브릿지로 먼저 발견된 링크를 TURNINFO 로 재발견 시 "화이트리스트만
 *   승격"하는 시도를 했으나, 중복 DEPTH_LINK_INFO_DATA 항목이 각자 같은 회전정보를 다시 확장하며
 *   depth 마다 배가돼 지수적으로 폭증(실측: 처리 행 하나에 메모리 수 GB·응답 정지)하는 것 확인 —
 *   되돌림. 이 판정은 psetSearchHistoryLinkList(브릿지 포함 전체) 그대로 사용해 중복 확장을 막는다.
*/
bool CContinueMapMatch::GetLinkDepthInfo(set<uint64> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList,
		unordered_map<uint64, uint64> *pmapParentLink, sint16 nSpeed)
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
		// erase(it++) 이후엔 it 가 다음 원소를 가리켜 더 이상 이 링크 ID 를 안 가리킴 — 지리적
		//   브릿지 탐색에서 "확장 중이던 링크"로 쓰기 위해 지우기 전에 따로 보관 (2026-08-20 최정우 추가)
		uint64 qwErasedLinkID = it->qwLinkID;

		// 이전 depth 링크 ID 삭제
		plistDepthLinkInfoList->erase(it++);

		for (uint32 i=dwStartTurnOffset; i<dwEndTurnOffset; ++i)
		{
			DEPTH_LINK_INFO_DATA stDepthLinkInfoData;

			// 분기(턴) 정보로 후속 링크 ID 조회 (2026-07-08 최정우 주석 추가)
			PTURN_INFO pstTurnInfo = m_pcDataLoader->GetTurnInfo(i);
			if (!pstTurnInfo) continue;

			// 이미 검사한 링크 ID 이면 제외
			set<uint64>::iterator history_it = psetSearchHistoryLinkList->find(pstTurnInfo->qwOutLinkID);
			if (history_it != psetSearchHistoryLinkList->end())
				continue;

			// 출력 링크 메타(세그먼트·턴 오프셋) 조회 (2026-07-08 최정우 주석 추가)
			PLINK_INFO pstLinkInfo = m_pcDataLoader->GetLinkInfo(pstTurnInfo->qwOutLinkID);
			if (!pstLinkInfo) continue;

			// 회전 정보가 없는(막다른) 링크도 반드시 이번 depth 후보 목록에 넣어야 함 — 아래
			//   push_back 안 하고 여기서 continue 해버리면 주차장 진입로·막힌 골목 같은 막다른
			//   링크는 실제로 그 위에 있어도 LinkSgmtMapMatch 스코어링 대상에서 영원히 빠져
			//   맵매칭이 안 됨(2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견). dwTurnOffset/
			//   nTurnCount 가 0이면 아래에서 dwStartTurnOffset==dwEndTurnOffset 이 되어 다음 depth
			//   확장은 자연히 안 일어나므로(line 414의 for 루프가 빈 범위) 별도 분기 불필요
			stDepthLinkInfoData.qwLinkID = pstTurnInfo->qwOutLinkID;
			stDepthLinkInfoData.dwStartTurnOffset = pstLinkInfo->dwTurnOffset;
			stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstLinkInfo->nTurnCount;
			stDepthLinkInfoData.dwStartSgmtOffset = pstLinkInfo->dwSgmtOffset;
			stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstLinkInfo->wSgmtCount;

			// 이 링크(qwOutLinkID)는 it->qwLinkID(확장 중인 직전 링크)를 거쳐 도달함 — 경로
			//   역추적용 기록. 링크당 최초 1회만 발견되므로(위 history 체크) 덮어쓸 일 없음 (2026-08-20 최정우 추가)
			if (pmapParentLink != nullptr)
				(*pmapParentLink)[pstTurnInfo->qwOutLinkID] = it->qwLinkID;

			plistDepthLinkInfoList->push_back(stDepthLinkInfoData);
		}

		// 지리적 브릿지 시도 — "턴 정보 아예 없을 때만"(1차 시도)으로는 다른 방향 턴은 있고 하필
		//   필요한 방향만 없는 실측 케이스가 전혀 안 잡혀서(094414/140532 교차로 케이스) 턴 유무와
		//   무관하게 시도하도록 넓혔다가, 정차 중(속도 0, 매 tick 같은 링크를 반복 확장)인 지점까지
		//   매번 새 경쟁 후보가 끼어들어 원래 안정적이던 매칭이 흔들리는 회귀가 실측 확인됨(093337
		//   트립 90→86). "속도 0(정차 확실)일 때만 제외"로 절충 — 정차 아닐 때는 계속 시도, 정차
		//   중엔 안 건드림(2026-08-20 최정우 재수정 — "정확히 0"만 제외로는 회귀가 그대로 재현돼,
		//   저속 전체(MM_SPEED_LOW_KMH 이하 — 방위각 자체를 안 믿는 기존 기준과 동일 임계 재사용)로
		//   범위를 넓힘). qwErasedLinkID 는 위에서 erase(it++) 전에 미리 보관해둔 값(끝점 좌표는
		//   GetLinkInfo() 로 다시 조회).
		if ((nSpeed < 0) || (nSpeed > MM_SPEED_LOW_KMH))
		{
			PLINK_INFO pstFromLinkInfo = m_pcDataLoader->GetLinkInfo(qwErasedLinkID);
			if (pstFromLinkInfo != nullptr)
			{
				double dfEndRawX = static_cast<double>(pstFromLinkInfo->dwEdNodeX) / 360000.0;
				double dfEndRawY = static_cast<double>(pstFromLinkInfo->dwEdNodeY) / 360000.0;
				BridgeNearbyLinkStarts(qwErasedLinkID, dfEndRawX, dfEndRawY,
					psetSearchHistoryLinkList, plistDepthLinkInfoList, pmapParentLink);
			}
		}
	}

	return (!plistDepthLinkInfoList->empty()) ? true : false;
}

/**
 * @brief 막다른 링크 끝점 근처에서 다른 링크의 시작점을 지리적으로 찾아 depth 후보에 추가
 * @param[in] qwFromLinkID 확장 중이던(막다른) 링크 ID — 자기 자신 제외, 경로 역추적 부모로 기록
 * @param[in] dfEndRawX/dfEndRawY qwFromLinkID 의 끝 노드 좌표(WGS84, 도 단위 — 미변환 원본)
 * @param[in,out] psetSearchHistoryLinkList 이미 발견된 링크 목록(중복 방지) — 새로 찾은 링크 추가
 * @param[out] plistDepthLinkInfoList 새로 찾은 링크를 다음 depth 후보로 추가
 * @param[out] pmapParentLink 경로 역추적용 — qwFromLinkID 를 거쳐 도달한 것으로 기록
 * @return void
 * @remark GRID_SGMT_INFO.wLenFromLink==0 인 세그먼트는 그 링크의 "첫 세그먼트" — 좌표(dwX,dwY)가
 *   곧 그 링크의 시작 노드 좌표와 같다. BeginMapMatch 의 그리드 반경 탐색과 동일한 GetGridID/
 *   GetNearGridID 조합을 재사용하되, 전체 매칭 비용 계산 없이 "링크 시작점이 근처에 있는가"만
 *   가볍게 확인 — 그래프가 막힌 경우에만 호출되므로 평소 매칭 경로엔 비용이 안 붙는다.
*/
void CContinueMapMatch::BridgeNearbyLinkStarts(uint64 qwFromLinkID, double dfEndRawX, double dfEndRawY,
		set<uint64> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList,
		unordered_map<uint64, uint64> *pmapParentLink)
{
	SGMT_MATCH_INPUT stGridInput;
	stGridInput.stPoint.dfX = dfEndRawX;
	stGridInput.stPoint.dfY = dfEndRawY;
	stGridInput.nRadius = static_cast<sint16>(MM_NODE_BRIDGE_MAX_M);

	uint32 dwGridID = m_cGISUtil.GetGridID(stGridInput.stPoint.dfX, stGridInput.stPoint.dfY);
	if (dwGridID == static_cast<uint32>(INVALID_GRID_ID))
		return;

	vector<uint32> vtNearGridIDList;
	vtNearGridIDList.push_back(dwGridID);
	m_cGISUtil.GetNearGridID(dwGridID, stGridInput, vtNearGridIDList);

	POINT stEndPoint;
	stEndPoint.dfX = dfEndRawX * 360000.0;
	stEndPoint.dfY = dfEndRawY * 360000.0;

	set<uint64> setBridgedThisCall;			// 여러 그리드에 걸친 같은 링크 중복 추가 방지

	for (size_t g = 0; g < vtNearGridIDList.size(); ++g)
	{
		PGRID_INFO pstGridInfo = m_pcDataLoader->GetGridInfo(vtNearGridIDList[g]);
		if (!pstGridInfo)
			continue;

		uint32 dwStart = pstGridInfo->dwSgmtOffset;
		uint32 dwEnd = dwStart + pstGridInfo->wSgmtCount;

		for (uint32 s = dwStart; s < dwEnd; ++s)
		{
			PGRID_SGMT_INFO pstSgmt = m_pcDataLoader->GetGridSgmtInfo(s);
			if ((!pstSgmt) || (pstSgmt->wLenFromLink != 0))
				continue;						// 링크의 첫 세그먼트(=시작 노드)만 대상

			if (pstSgmt->qwLinkID == qwFromLinkID)
				continue;						// 자기 자신 제외

			if (psetSearchHistoryLinkList->find(pstSgmt->qwLinkID) != psetSearchHistoryLinkList->end())
				continue;						// 이미 발견된 링크
			if (setBridgedThisCall.find(pstSgmt->qwLinkID) != setBridgedThisCall.end())
				continue;

			POINT stCandStart;
			stCandStart.dfX = static_cast<double>(pstSgmt->dwX);
			stCandStart.dfY = static_cast<double>(pstSgmt->dwY);
			double dfDistM = m_cGISUtil.GetDistanceGEO1(stEndPoint, stCandStart);
			if (dfDistM > MM_NODE_BRIDGE_MAX_M)
				continue;

			PLINK_INFO pstBridgeLinkInfo = m_pcDataLoader->GetLinkInfo(pstSgmt->qwLinkID);
			if (!pstBridgeLinkInfo)
				continue;

			DEPTH_LINK_INFO_DATA stDepthLinkInfoData;
			stDepthLinkInfoData.qwLinkID = pstSgmt->qwLinkID;
			stDepthLinkInfoData.dwStartTurnOffset = pstBridgeLinkInfo->dwTurnOffset;
			stDepthLinkInfoData.dwEndTurnOffset = stDepthLinkInfoData.dwStartTurnOffset + pstBridgeLinkInfo->nTurnCount;
			stDepthLinkInfoData.dwStartSgmtOffset = pstBridgeLinkInfo->dwSgmtOffset;
			stDepthLinkInfoData.dwEndSgmtOffset = stDepthLinkInfoData.dwStartSgmtOffset + pstBridgeLinkInfo->wSgmtCount;
			// TURNINFO 아닌 좌표 근접으로 찾은 후보 — 위상 우선순위 페널티 대상 표시 (2026-08-21 최정우 추가)
			stDepthLinkInfoData.bGeometricBridge = true;

			if (pmapParentLink != nullptr)
				(*pmapParentLink)[pstSgmt->qwLinkID] = qwFromLinkID;

			plistDepthLinkInfoList->push_back(stDepthLinkInfoData);
			setBridgedThisCall.insert(pstSgmt->qwLinkID);
		}
	}
}

/**
 * @brief 경로 역추적 — mapParentLink 를 최종 확정 링크에서부터 거슬러 올라가며 경유 링크 목록을 만든다
 * @param[in] qwFinalLinkID 이번에 최종 확정된 링크 ID
 * @param[in] mapParentLink GetLinkDepthInfo() 가 채운 "링크→그 직전 링크" 맵
 * @param[out] pvtOut 확장 없이 확정된 경우(최종=시작) 그 링크 1개만, 확장이 있었으면 시작 링크는
 *   제외하고 경유 링크+최종 링크를 순서대로(시작→끝) 채운다 — 어느 경우든 최소 1개는 담긴다
 * @return void
 * @remark 최대 depth 가 config maxstep(작은 값, 보통 2~3)로 제한돼 있어 경로 길이도 그만큼 짧다 (2026-08-20 최정우 추가)
*/
void CContinueMapMatch::ReconstructPath(uint64 qwFinalLinkID, const unordered_map<uint64, uint64>& mapParentLink,
		vector<uint64> *pvtOut)
{
	if (pvtOut == nullptr)
		return;
	pvtOut->clear();

	vector<uint64> vtReversed;
	uint64 qwCur = qwFinalLinkID;
	// mapParentLink 에 없는 링크를 만나면 그게 시작 링크(확장 시작점) — 역추적 종료.
	//   링크 수만큼(=depth 만큼, 매우 작음) 도는 루프라 무한루프 걱정 없음
	while (true)
	{
		vtReversed.push_back(qwCur);
		unordered_map<uint64, uint64>::const_iterator it = mapParentLink.find(qwCur);
		if (it == mapParentLink.end())
			break;
		qwCur = it->second;
	}

	if (vtReversed.size() == 1)
	{
		// 확정 링크가 곧 시작 링크(확장 없이 그대로 확정) — 기존 단일 링크 체크와 동일하게
		//   그 자체 1개만 담는다
		pvtOut->push_back(vtReversed[0]);
	}
	else
	{
		// vtReversed 는 [최종, ..., 시작] 순 — 시작 링크(마지막 원소)만 제외하고 뒤집어서 채움
		for (sint32 i = static_cast<sint32>(vtReversed.size()) - 2; i >= 0; --i)
			pvtOut->push_back(vtReversed[static_cast<size_t>(i)]);
	}
}

/**
 * @brief 검색 정보 목록 중 비용 최소 후보 추출 (dfCost 오름차순, 동률 시 INTERSECT_LEN)
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
