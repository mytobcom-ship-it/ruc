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
 * @return true(성공), false(실패)
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
				return true;
			}
		}

		// 현재 검색 단계가 최대 설정 검색 단계이면 다음 depth 링크 목록 정보를 가져오지 않음
		if (i == nSearchStep) break;

		// 현재 depth 링크 목록이 맵 매칭에 실패시 다음 depth 링크 목록 정보
		if (!GetLinkDepthInfo(&setSearchHistoryLinkList, &listDepthLinkInfoList))
		{
			// 더 확장할 연결 링크가 없으면, 지금까지 누적 후보가 있으면 그걸로 확정 (2026-07-15 최정우 수정)
			if (!listAllEntryList.empty())
			{
				*pwErrorCode = NO_ERROR;
				if (pstTraceCtx != nullptr)
					pstTraceCtx->nMatchedStep = nBestStep;
				GetMatchEntry(&listAllEntryList, pstMatchEntry, pstTraceCtx, stSgmtMatchInput);
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
 * @return true(성공), false(실패)
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
		// ── 연속 맵매칭 고도 보조 비용 가산 (Begin 미적용) ──
		//   dfCost = INTERSECT_LEN + 방위각비용 + CalcAltRoadPenalty(Δalt, ROAD_TYPE)
		// 값이 작을수록 우선 — 보너스(음수)면 동일 거리·방향 후보보다 유리
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
		}

		plistMatchEntryList->push_back(stMatchEntry);
	}

	return (!plistMatchEntryList->empty()) ? true : false;
}

/**
 * @brief 연결 링크 정보
 * @param[in,out] psetSearchHistoryLinkList 검색된 링크 UID 목록 (중복 검사용)
 * @param[out] plistDepthLinkInfoList 연결 링크 UID 정보
 * @return true(성공), false(실패)
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
