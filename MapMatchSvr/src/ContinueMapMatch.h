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
	// config reverse_penalty_weight/reverse_speed_gate_kmh/reverse_dead_zone_m (2026-07-20 최정우 추가)
	void SetReversePenaltyWeight(double dfWeight, double dfSpeedGateKmh = 0.0, double dfDeadZoneM = 0.0);
	bool StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint64& qwLinkID, sint16& nSearchStep, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx = nullptr);

private:
	bool LinkSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, 
		DEPTH_LINK_INFO_DATA& stDepthLinkInfoData, list<MATCH_ENTRY> *plistMatchEntryList);
	bool GetLinkDepthInfo(set<uint32> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList);
	void GetMatchEntry(list<MATCH_ENTRY> *plistMatchEntryList, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx = nullptr, const SGMT_MATCH_INPUT& stSgmtMatchInput = SGMT_MATCH_INPUT());
	// 최적 후보가 링크 경계(시작/끝)에 스냅(클램프)됐는지 — 클램프면 다음 depth 확장해 연결 링크와 비교 (2026-07-15 최정우 추가)
	bool IsBoundaryClamped(const MATCH_ENTRY& stMatchEntry);
	// 최적 후보의 방위각이 심하게 안 맞는지(방위각 비용이 상한 MM_DIR_MAX_PENALTY_M 도달) — 그래도
	//   depth 확장해 연결 링크와 비교(회전·교차로에서 직전 링크에 계속 고정되는 것 방지) (2026-07-18 최정우 추가)
	bool IsPoorAngleFit(const MATCH_ENTRY& stMatchEntry);

private:
	CGISUtil							m_cGISUtil;
	CDataLoader							*m_pcDataLoader;
	ALTITUDE_SCORE_CONFIG				m_stAltitudeConfig;					// config altitude_* — 연속 맵매칭 고도 보조 점수
	double								m_dfReversePenaltyWeight;				// config reverse_penalty_weight — 역행 1m당 비용 가중 (2026-07-20 최정우 추가)
	double								m_dfReverseSpeedGateKmh;				// config reverse_speed_gate_kmh — 이 속도 미만일 때만 데드존 적용 (2026-07-20 최정우 추가)
	double								m_dfReverseDeadZoneM;					// config reverse_dead_zone_m — 저속 시 페널티 없이 허용하는 역행 거리(m) (2026-07-20 최정우 추가)
};

#endif //__CONTINUEMAPMATCH_H__
