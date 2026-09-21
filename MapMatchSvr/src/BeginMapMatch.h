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

	// [2026-09-21 최정우 정정] 아래 두 줄 주석이 엉뚱한 선언 위에 붙어 있었다 —
	//   "qwBiasLinkID ..." 는 StartMapMatch() 의 인자 설명이고 "왕복분리 짝 링크 heading 교정" 은
	//   FixOppositePairByHeading() 설명인데, 둘 다 GetLinkAzimuth() 바로 위에 있어 마치 이 함수의
	//   설명처럼 읽혔다. 각자 제 선언 위로 옮긴다.
	// 링크의 진행 방위각(시작노드 → 종료노드, 0=북 시계방향). 호출 전 pstLinkInfo nullptr 검사 필수
	sint16 GetLinkAzimuth(PLINK_INFO pstLinkInfo);
	// 왕복분리 짝 링크 heading 교정 (2026-08-22 최정우 추가)
	void FixOppositePairByHeading(const SGMT_MATCH_INPUT& stSgmtMatchInput,
			list<MATCH_ENTRY>& listMatchEntryList);
	// 위 교정의 일반화 — 짝 링크(qwOppositeLinkID) 등록 여부와 무관하게, 후보 목록 안에서
	//   링크 진행방향(GetLinkAzimuth)이 heading 과 맞는 최선 후보를 앞으로 당긴다.
	//   상세 근거는 구현부 주석 참고 (2026-09-05 최정우 추가, 사용자 지시)
	void FixReverseLinkByAzimuth(const SGMT_MATCH_INPUT& stSgmtMatchInput,
			list<MATCH_ENTRY>& listMatchEntryList);
	// 짝 링크(qwOppositeLinkID)가 있는 링크가 heading 과 거의 정반대로 채택됐는지 판정 —
	//   Begin 이 Continue 결과를 병행폴백으로 대체하기 직전 거부권 용도 (2026-08-24 최정우 추가)
	bool IsAntiHeadingOpposite(uint64 qwLinkID, sint16 nHeading, sint16 nSpeed);
	// qwBiasLinkID : 연속실패 후 Begin 재검색 시 직전 성공 링크(연결성 편향, 0=미적용) (2026-07-15 최정우 추가)
	// ※ stSgmtMatchInput 은 내부에서 좌표가 ×360000 으로 **덮어써진다** — 구현부 주의 주석 참고
	bool StartMapMatch(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput, 
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry, PMATCH_TRACE_CTX pstTraceCtx = nullptr,
		uint64 qwBiasLinkID = 0);
	// 반경 무시 기하 최근접 1건 (진단반경 초과·그리드 후보 있음 → SKIP 참고용) (2026-07-10 최정우 수정)
	bool FindGeomNearest(CDataLoader *pcDataLoader, SGMT_MATCH_INPUT& stSgmtMatchInput,
		uint16 *pwErrorCode, PMATCH_ENTRY pstMatchEntry);

private:
	// 셀 안에서 비용 오름차순 상위 MM_BEGIN_CELL_TOPN 건을 listOutEntryList 에 append 한다 —
	//   기존엔 1등 하나만 반환해 같은 셀의 정답 링크가 목록에 오르지 못했다(상수 주석 참고)
	//   (2026-09-05 최정우 수정, 사용자 지시)
	bool GridSgmtMapMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, uint32 dwStartSgmtOffset, 
		uint32 dwEndSgmtOffset, uint16 *pwErrorCode, list<MATCH_ENTRY>& listOutEntryList,
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
