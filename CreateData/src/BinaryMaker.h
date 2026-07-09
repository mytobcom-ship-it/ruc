/**
 * @file BinaryMaker.h
 * @brief 형상 정보(표준노드링크) → 맵매칭 바이너리(link.psf) 생성 클래스 헤더 파일
 * @remark
 *   기존 CDataManager::CreateData() 의 "바이너리 생성" 부분만 분리한 독립 실행용 클래스.
 *   - 스레드/스케줄러(run, CREATE_RUN_TIME) 제거
 *   - DataLoader(메모리 적재) 의존성 제거 (생성만 수행)
 *   - 동작/산출물 포맷은 CDataManager 와 동일
*/
#ifndef __BINARYMAKER_H__
#define __BINARYMAKER_H__

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <set>
#include <algorithm>
#include <math.h>
#include "TypeDefine.h"
#include "DataFormat.h"
#include "log4z.h"
#include "Clock.h"
#include "GISUtil.h"
#include "ShapeLoader.h"
#include "TurnInfoLoader.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @class CBinaryMaker
 * @brief 맵매칭 바이너리 데이터 생성 클래스 (독립 실행용)
*/
class CBinaryMaker
{
public:
	CBinaryMaker();
	virtual ~CBinaryMaker();

	bool Initialize(CShapeFileLoader *pcShapeLoader, 
		const string& strGeometryPath, const string& strGeometryFile,
		CTurnInfoLoader *pcTurnInfoLoader);

	void Uninitialize();

	bool Create();

private:
	uint16 InferTurnTypeFromAngle(sint16 nTurnAng) const;
	bool SetCreateInitial();
	void SetCreateUninitial();
	bool SetCreateBinary(FILE *fp);
	bool SetGridMapData();

	bool GetTurnInfo(const uint64& qwInLinkID, 
		const vector<POINT>& vtVertexs, LINK_INFO_DATA& stLinkInfoData);
	bool GetLinkSgmtInfo(const uint64& qwLinkID, 
		const vector<POINT>& vtVertexs, LINK_INFO_DATA& stLinkInfoData);
	void GetLinkInfoData(LINK_INFO_DATA& stLinkInfoData);
	bool GetGridSgmtInfo(const uint64& qwLinkID, const vector<POINT>& vtVertexs);
	void SetGridSgmtInfo(uint32& dwGridID, const uint64& qwLinkID, 
		uint16& wLenFromLink, POINT& stPoint1, POINT& stPoint2);

private:
	CShapeFileLoader				*m_pcShapeLoader;
	CTurnInfoLoader					*m_pcTurnInfoLoader;
	CGISUtil						m_cGISUtil;					// GIS 유틸리티 클래스
	uint32							m_dwTurnRestrictedSkipCount;	// 회전 제한으로 제외된 건수

private:
	string							m_strGeometryPath;			// 형상 정보 바이너리 파일 경로
	string							m_strGeometryFile;			// 형상 정보 바이너리 파일명 및 경로
	mapShapeLinkInfo				*m_mapShapeLinkInfoList;	// LINK 형상 정보
	mapShapeNodeInfo				*m_mapShapeNodeInfoList;	// NODE 형상 정보
	mapGridSgmtInfo					*m_mapGridSgmtInfoList;		// GRID 별 세그먼트 저장
	vector<GRID_INFO>				*m_vtGridInfoList;			// 그리드별 세그먼트 범위 로딩 메모리
	vector<GRID_SGMT_INFO>			*m_vtGridSgmtInfoList;		// 그리드별 세그먼트 정보 로딩 메모리
	vector<LINK_SGMT_INFO>			*m_vtLinkSgmtInfoList;		// 링크별 세그먼트 정보 로딩 메모리
	vector<LINK_INFO_DATA>			*m_vtLinkInfoDataList;		// 세그먼트별 링크 정보 데이터
	vector<TURN_INFO>				*m_vtTurnInfoList;			// 시작 링크 기준으로 연결된 링크 회전 정보 로딩 메모리
	uint32							m_dwTurnOffset;				// 회전 정보 Offset
	uint32							m_dwLinkSgmtOffset;			// LINK 세그먼트 Offset
	uint32							m_dwGridSgmtOffset;			// GRID 세그먼트 Offset
	uint32							m_dwGridOffset;				// GRID Offset
};

#endif //__BINARYMAKER_H__
