/**
 * @file BeginMapMatch.h
 * @brief 초기 맵매칭 클래스 헤더 파일
*/
#ifndef __BEGINMAPMATCH_H__
#define __BEGINMAPMATCH_H__

#include <stdio.h>
#include <string.h>
#include <string>
#include <list>
#include <vector>
#include <set>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "log4z.h"
#include "GISUtil.h"
#include "DataLoader.h"
#include "MatchTrace.h"

using namespace zsummer::log4z;
using namespace std;

/**
 @class CBeginMapMatch
 @brief 초기 맵매칭 클래스
*/
class CBeginMapMatch
{
public:
	CBeginMapMatch();
	virtual ~CBeginMapMatch();

	// qwBiasLinkID : 연속실패 후 Begin 재검색 시 직전 성공 링크(연결성 편향, 0=미적용) (2026-07-15 최정우 추가)
	bool StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput, 
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry, PMATCH_TRACE_CTX pstTraceCtx = nullptr,
		uint64 qwBiasLinkID = 0);
	// 반경 무시 기하 최근접 1건 (진단반경 초과·그리드 후보 있음 → SKIP 참고용) (2026-07-10 최정우 수정)
	bool FindGeomNearest(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry);

private:
	bool GridSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset, 
		uint32 dwEndSgmtOffset, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry,
		const std::set<uint64>* psetConnected = nullptr);
	bool GridSgmtGeomNearest(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset,
		uint32 dwEndSgmtOffset, MATCH_ENTRY& stBest, double& dfBestDist, bool& bFound);
	// 직전 성공 링크 기준 "회전 가능(연결)" 링크 집합 구성 (bias link + 진출 링크) (2026-07-15 최정우 추가)
	void BuildConnectedSet(uint64 qwBiasLinkID, std::set<uint64>& setConnected);

private:
	CGISUtil						m_cGISUtil;
	CDataLoader						*m_pcDataLoader;
};

#endif //__BEGINMAPMATCH_H__
