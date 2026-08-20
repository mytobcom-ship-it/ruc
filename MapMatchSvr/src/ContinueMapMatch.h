/**
 * @file ContinueMapMatch.h
 * @brief 연속 맵매칭 클래스 헤더 파일
*/
#ifndef __CONTINUEMAPMATCH_H__
#define __CONTINUEMAPMATCH_H__

#include <stdio.h>
#include <string.h>
#include <list>
#include <set>
#include <vector>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "DataFormat.h"
#include "log4z.h"
#include "GISUtil.h"
#include "DataLoader.h"
#include "MatchTrace.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @struct sLinkDepthSgmtData
 * @brief 링크 ID 회전 정보 및 세그먼트 정보
*/
typedef struct sDepthLinkInfoData
{
	uint64							qwLinkID;							// 링크 ID
	uint32							dwStartTurnOffset;					// 연결 링크 회전 정보 시작 Offset
	uint32							dwEndTurnOffset;					// 연결 링크 회전 정보 종료 Offset
	uint32							dwStartSgmtOffset;					// 시작 세그먼트 Offset
	uint32							dwEndSgmtOffset;					// 종료 세그먼트 Offset

	sDepthLinkInfoData() :
		qwLinkID(0), 
		dwStartTurnOffset(0), 
		dwEndTurnOffset(0), 
		dwStartSgmtOffset(0), 
		dwEndSgmtOffset(0)
	{}
} DEPTH_LINK_INFO_DATA, *PDEPTH_LINK_INFO_DATA;

#define DEPTH_LINK_INFO_DATA_SIZE										sizeof(DEPTH_LINK_INFO_DATA)
typedef list<DEPTH_LINK_INFO_DATA>										listDepthLinkInfo;

/**
 * @class CContinueMapMatch
 * @brief 연속 맵매칭 클래스
*/
class CContinueMapMatch
{
public:
	CContinueMapMatch();
	virtual ~CContinueMapMatch();

	void SetAltitudeConfig(const ALTITUDE_SCORE_CONFIG& stAltConfig);
	// pvtPathLinkIDs(선택) — 직전 확정 링크(qwLinkID) 다음부터 이번 확정 링크까지 실제 경유한
	//   링크 ID 목록(경유 링크가 없으면 최종 링크 1개만) — 게이트/구역 판정이 "이번 확정 링크
	//   1개"만이 아니라 그 사이 모든 경유 링크를 확인할 수 있도록 제공한다. GPS 수신 주기가
	//   늘어나 포인트 간 이동거리가 커지면, 짧은 게이트 링크가 두 GPS 포인트 사이에 통째로
	//   끼어 어느 쪽에도 직접 매칭 안 되는 경우가 있어 이걸로 보완한다 (2026-08-20 최정우 추가)
	bool StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint64& qwLinkID, sint16& nSearchStep, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx = nullptr, vector<uint64> *pvtPathLinkIDs = nullptr);

private:
	bool LinkSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput,
		DEPTH_LINK_INFO_DATA& stDepthLinkInfoData, list<MATCH_ENTRY> *plistMatchEntryList,
		bool bAllowOppositeCheck = true);
	// 역행 의심 후보의 반대방향(왕복분리) 짝 링크를 추가 후보로 평가 (2026-08-19 최정우 추가)
	void TryOppositeLinkCandidate(SGMT_MATCH_INPUT& stSgmtMatchInput,
		const uint64& qwOppositeLinkID, list<MATCH_ENTRY> *plistMatchEntryList);
	// 링크 ID 는 uint64(전국 10자리 코드 기준 지역코드 43 이상은 2^32 초과) — set<uint32> 로 받으면
	//   삽입/조회 시 조용히 하위 32비트로 잘려 서로 다른 링크가 충돌할 수 있었음(2026-08-14 최정우
	//   수정 — "소스상 문제" 검토 중 발견, BeginMapMatch 의 동일 목적 집합은 원래부터 set<uint64>)
	// pmapParentLink(선택) — 새로 발견된(qwOutLinkID) 링크가 어느 링크(qwLinkID, 확장 시작 링크)를
	//   거쳐 도달했는지 기록 — 경로 역추적용 (2026-08-20 최정우 추가)
	// nSpeed(선택, 기본 -1=무시) — 0 이면 GetLinkDepthInfo 가 지리적 브릿지(BridgeNearbyLinkStarts)를
	//   건너뜀. 정차 중엔 depth 확장이 매 tick 같은 링크에서 반복되는데, 그때마다 브릿지가 새 경쟁
	//   후보를 끼워넣어 원래 안정적이던 매칭까지 흔드는 회귀가 실측으로 확인됨(2026-08-20 최정우 추가)
	bool GetLinkDepthInfo(set<uint64> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList,
		unordered_map<uint64, uint64> *pmapParentLink = nullptr, sint16 nSpeed = -1);
	// 막다른 링크(TURNINFO 로 다음 링크 없음) 끝점 근처(MM_NODE_BRIDGE_MAX_M 이내)에서 다른 링크의
	//   시작점을 찾아 depth 후보로 추가 — 지도 데이터 자체는 안 건드리고 그래프 탐색만 런타임에
	//   보강하는 우회 (2026-08-20 최정우 추가, 상세 근거는 MM_NODE_BRIDGE_MAX_M 주석 참고)
	void BridgeNearbyLinkStarts(uint64 qwFromLinkID, double dfEndRawX, double dfEndRawY,
		set<uint64> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList,
		unordered_map<uint64, uint64> *pmapParentLink);
	void GetMatchEntry(list<MATCH_ENTRY> *plistMatchEntryList, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx = nullptr, const SGMT_MATCH_INPUT& stSgmtMatchInput = SGMT_MATCH_INPUT());
	// mapParentLink 를 이용해 qwFinalLinkID 부터 시작 링크까지 역추적 — 시작 링크는 제외하고
	//   경유 링크(있다면)+최종 링크만 순서대로(시작→끝) pvtOut 에 채운다 (2026-08-20 최정우 추가)
	void ReconstructPath(uint64 qwFinalLinkID, const unordered_map<uint64, uint64>& mapParentLink,
		vector<uint64> *pvtOut);
	// 최적 후보가 링크 경계(시작/끝)에 스냅(클램프)됐는지 — 클램프면 다음 depth 확장해 연결 링크와 비교 (2026-07-15 최정우 추가)
	bool IsBoundaryClamped(const MATCH_ENTRY& stMatchEntry);
	// 최적 후보의 방위각이 심하게 안 맞는지(방위각 비용이 상한 MM_DIR_MAX_PENALTY 도달) — 그래도
	//   depth 확장해 연결 링크와 비교(회전·교차로에서 직전 링크에 계속 고정되는 것 방지) (2026-07-18 최정우 추가)
	bool IsPoorAngleFit(const MATCH_ENTRY& stMatchEntry);

private:
	CGISUtil							m_cGISUtil;
	CDataLoader							*m_pcDataLoader;
	ALTITUDE_SCORE_CONFIG				m_stAltitudeConfig;					// config altitude_* — 연속 맵매칭 고도 보조 점수
};

#endif //__CONTINUEMAPMATCH_H__
