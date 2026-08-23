/**
 * @file DataLoader.h
 * @brief 세그먼트 및 링크 정보 로딩 클래스 헤더 파일
*/
#ifndef __DATALOADER_H__
#define __DATALOADER_H__

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataFormat.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @class CDataLoader
 * @brief 세그먼트 및 링크 정보 로딩 클래스
*/
class CDataLoader
{
public:
	CDataLoader();
	virtual ~CDataLoader();

	void Initialize(string& strGeometryFile, const sint16 nMaxDepth);
	void Uninitialize();
	bool SetDataUpdate();
	void SetDataInfoDisplay();

	PGRID_INFO GetGridInfo(const uint32 dwGridID);
	PGRID_SGMT_INFO GetGridSgmtInfo(const uint32 dwOffset);
	PLINK_SGMT_INFO GetLinkSgmtInfo(const uint32 dwOffset);
	PLINK_INFO GetLinkInfo(const uint64 qwLinkID);
	PTURN_INFO GetTurnInfo(const uint32 dwOffset);

	inline const bool IsLoad() const { return m_bLoad; }
	inline const sint16 GetSearchStep() const { return m_nMaxStep; }
	// depth 1단계당 가산 비용(m) — config [mapmatch] hoppenalty. 0=비활성 (2026-08-22 최정우 추가)
	inline const double GetHopPenalty() const { return m_dfHopPenalty; }
	inline void SetHopPenalty(const double dfVal) { m_dfHopPenalty = dfVal; }
	// hop 벌점의 링크길이 비례 상한 — config [mapmatch] hoppenalty_lenratio. 0=비활성(현행)
	//   hoppenalty 는 우회 매칭을 막으려고 넣었는데(2026-08-22), 짧은 지선을 정상적으로
	//   지나가는 경우까지 같은 벌점을 물린다. 7m 링크에 8m 벌점이 붙으면 링크보다 벌점이 커서
	//   차가 그 위에 있어도 직전 링크가 이긴다 — 합성 실측에서 지선 진입 7건 중 3건(42.9%)이
	//   그렇게 누락됐다. 이 값을 주면 벌점을 min(hoppenalty, 링크길이 x ratio) 로 깎는다.
	//   긴 링크로 우회하는 건 종전대로 막으면서 짧은 조각은 통과시키는 것이 목적
	//   (2026-08-23 최정우 추가)
	inline const double GetHopLenRatio() const { return m_dfHopLenRatio; }
	inline void SetHopLenRatio(const double dfVal) { m_dfHopLenRatio = dfVal; }

private:
	void SetDataInit();

private:
	bool							m_bLoad;							// 데이터 로딩 여부 플래그
	string							m_strDataFile;						// 데이터 바이너리 파일명 및 경로
	sint16							m_nMaxStep;							// 연속 맵매칭 최대 검색 단계
	double							m_dfHopPenalty;						// depth 1단계당 가산 비용(m) (2026-08-22 최정우 추가)
	double							m_dfHopLenRatio;					// hop 벌점 링크길이 비례 상한 배율, 0=비활성 (2026-08-23 최정우 추가)
	uint32							m_dwGridInfoSize;					// 그리드별 세그먼트 범위 byte 크기
	uint32							m_dwGridSgmtInfoSize;				// 그리드별 세그먼트 정보 byte 크기
	uint32							m_dwLinkSgmtInfoSize;				// 링크별 세그먼트 정보 byte 크기
	uint32							m_dwLinkInfoSize;					// 세그먼트별 링크 정보 byte 크기
	uint32							m_dwTurnInfoSize;					// 시작링크 기준으로 연결된 링크 회전 정보 byte 크기
	uint32							m_dwGridInfoStartOffset;			// 그리드별 세그먼트 범위 정보 시작 Offset
	uint32							m_dwGridSgmtInfoStartOffset;		// 그리드별 세그먼트 정보 시작 Offset
	uint32							m_dwLinkSgmtInfoStartOffset;		// 링크별 세그먼트 정보 시작 Offset
	uint32							m_dwLinkInfoStartOffset;			// 세그먼트별 링크 정보 시작 Offset
	uint32							m_dwTurnInfoStartOffset;			// 시작링크 기준으로 연결된 링크 회전 정보 시작 Offset
	uint32							m_dwGridInfoCount;					// 그리드별 세그먼트 범위 개수
	uint32							m_dwGridSgmtInfoCount;				// 그리드별 세그먼트 정보 개수
	uint32							m_dwLinkSgmtInfoCount;				// 링크별 세그먼트 정보 개수
	uint32							m_dwLinkInfoCount;					// 세그먼트별 링크 정보 개수
	uint32							m_dwTurnInfoCount;					// 시작링크 기준으로 연결된 링크 회전 정보 개수
	PDATA_FILE_HEAD					m_pstDataFileHead;					// 형상 정보 바이너리 파일 헤더 정보
	PGRID_INFO						m_pstGridInfoList;					// 그리드별 세그먼트 범위 로딩 메모리
	PGRID_SGMT_INFO					m_pstGridSgmtInfoList;				// 그리드별 세그먼트 정보 로딩 메모리
	PLINK_SGMT_INFO					m_pstLinkSgmtInfoList;				// 링크별 세그먼트 정보 로딩 메모리
	mapLinkInfo						*m_mapLinkInfoList;					// 세그먼트별 링크 정보 (키:링크 ID)
	PTURN_INFO						m_pstTurnInfoList;					// 시작 링크 기준으로 연결된 링크 회전 정보 로딩 메모리
};

#endif //__DATALOADER_H__
