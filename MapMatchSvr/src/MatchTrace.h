/**
 * @file MatchTrace.h
 * @brief 맵매칭 후보·비용 디버그 로그 (LOGFMTD — config level=1 DEBUG 이하)
*/
#ifndef __MATCH_TRACE_H__
#define __MATCH_TRACE_H__

#include <list>
#include "DataDefine.h"
#include "GISUtil.h"

#define MATCH_TRACE_TOP_N												5

/**
 * @struct sMatchTraceCtx
 * @brief GPS 1건 맵매칭 trace 컨텍스트 (ProcessManager → MapMatch)
*/
typedef struct sMatchTraceCtx
{
	int								nThreadId;
	char							szDeviceKey[36+1];
	char							szTripId[60+1];
	uint32							dwSeqNo;
	double							dfGpsLat;
	double							dfGpsLon;
	sint16							nRadius;
	sint16							nSpeed;
	sint16							nHeading;
	sint16							nAltitudeM;
	sint16							nPrevAltitudeM;
	uint8							nPrevRoadType;
	bool							bContinue;
	uint64							qwPrevLinkId;
	sint16							nMatchedStep;
} MATCH_TRACE_CTX, *PMATCH_TRACE_CTX;

/**
 * @class CMatchTrace
 * @brief 맵매칭 후보·비용 디버그 로그 (static 유틸, 인스턴스 불필요)
*/
class CMatchTrace
{
public:
	/**
	 * @brief 맵매칭 trace·input·후보·당선 winner 디버그 로그 출력
	 * @param[in] pstCtx GPS 1건 trace 컨텍스트
	 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력
	 * @param[in] listCandidates 후보 목록
	 * @param[in] stWinner 최종 선택 링크
	 * @return void
	*/
	static void LogResult(const MATCH_TRACE_CTX *pstCtx, const SGMT_MATCH_INPUT& stSgmtMatchInput,
		const std::list<MATCH_ENTRY>& listCandidates, const MATCH_ENTRY& stWinner);
};

#endif //__MATCH_TRACE_H__
