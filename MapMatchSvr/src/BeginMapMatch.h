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

	bool StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput, 
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry, PMATCH_TRACE_CTX pstTraceCtx = nullptr);
	// 반경 무시 기하 최근접 1건 (진단반경 초과·그리드 후보 있음 → SKIP 참고용) (2026-07-10 최정우 수정)
	bool FindGeomNearest(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry);

private:
	bool GridSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset, 
		uint32 dwEndSgmtOffset, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry);
	bool GridSgmtGeomNearest(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset,
		uint32 dwEndSgmtOffset, MATCH_ENTRY& stBest, double& dfBestDist, bool& bFound);

private:
	CGISUtil						m_cGISUtil;
	CDataLoader						*m_pcDataLoader;
};

#endif //__BEGINMAPMATCH_H__
