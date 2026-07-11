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
	bool StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput, 
		uint64& qwLinkID, sint16& nSearchStep, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx = nullptr);

private:
	bool LinkSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, 
		DEPTH_LINK_INFO_DATA& stDepthLinkInfoData, list<MATCH_ENTRY> *plistMatchEntryList);
	bool GetLinkDepthInfo(set<uint32> *psetSearchHistoryLinkList, listDepthLinkInfo *plistDepthLinkInfoList);
	void GetMatchEntry(list<MATCH_ENTRY> *plistMatchEntryList, PMATCH_ENTRY pstMatchEntry,
		PMATCH_TRACE_CTX pstTraceCtx = nullptr, const SGMT_MATCH_INPUT& stSgmtMatchInput = SGMT_MATCH_INPUT());

private:
	CGISUtil							m_cGISUtil;
	CDataLoader							*m_pcDataLoader;
	ALTITUDE_SCORE_CONFIG				m_stAltitudeConfig;					// config altitude_* — 연속 맵매칭 고도 보조 점수
};

#endif //__CONTINUEMAPMATCH_H__
