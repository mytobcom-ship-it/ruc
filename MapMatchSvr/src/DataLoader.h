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

private:
	void SetDataInit();

private:
	bool							m_bLoad;							// 데이터 로딩 여부 플래그
	string							m_strDataFile;						// 데이터 바이너리 파일명 및 경로
	sint16							m_nMaxStep;							// 연속 맵매칭 최대 검색 단계
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
