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

private:
	bool GridSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset, 
		uint32 dwEndSgmtOffset, uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry);

private:
	CGISUtil						m_cGISUtil;
	CDataLoader						*m_pcDataLoader;
};

#endif //__BEGINMAPMATCH_H__
