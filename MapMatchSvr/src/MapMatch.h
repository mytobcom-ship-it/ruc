/**
 * @file MapMatch.h
 * @brief 맵매칭 클래스 헤더 파일
*/
#ifndef __MAPMATCH_H__
#define __MAPMATCH_H__

#include <stdio.h>
#include <unistd.h>
#include <string>
#include "TypeDefine.h"
#include "Config.h"
#include "MessageType.h"
#include "log4z.h"
#include "DataDefine.h"
#include "Coordinate.h"
#include "CodeMap.h"
#include "GISUtil.h"
#include "DataLoader.h"
#include "BeginMapMatch.h"
#include "ContinueMapMatch.h"
#include "MatchTrace.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @class CMapMatch
 * @brief 맵매칭 클래스
*/
class CMapMatch
{
public:
	CMapMatch();
	virtual ~CMapMatch();

	bool Initialize(CDataLoader *pcDataLoader);
	void SetAltitudeConfig(const ALTITUDE_SCORE_CONFIG& stAltConfig);
	bool BeginMapMatch(MAP_MATCH_INPUT stMapMatchInput, 
		PMATCH_LINK_INFO pstMatchLinkInfo, PMATCH_TRACE_CTX pstTraceCtx = nullptr);
	// 반경 무시 기하 최근접 Begin (진단반경 초과 SKIP 참고용) (2026-07-10 최정우 수정)
	bool BeginGeomNearest(MAP_MATCH_INPUT stMapMatchInput, PMATCH_LINK_INFO pstMatchLinkInfo);
	bool ContinueMapMatch(MAP_MATCH_INPUT stMapMatchInput, 
		PMATCH_LINK_INFO pstMatchLinkInfo, PMATCH_TRACE_CTX pstTraceCtx = nullptr);

private:
	bool IsValidCommonRequestValue(enum eCoordinateType& eCoordType, 
		sint16& nRadius, double& dfX, double& dfY, sint16 nAngle, 
		PMATCH_LINK_INFO pstMatchLinkInfo);
	bool SetResponseValue(uint16 wErrorCode, MATCH_ENTRY stMatchEntry, 
		PMATCH_LINK_INFO pstMatchLinkInfo);
	bool IsValidCoordinateType(enum eCoordinateType& eCoordType);
	bool IsValidCoordinate(enum eCoordinateType& eCoordType, 
		double *dfX, double *dfY);
	bool IsValidSearchRadius(sint16& nRadius);
	bool IsValidAngle(sint16& nAngle);
	bool IsValidSearchStep(sint16& nSearchStep);

private:
	CCodeMap						m_cCodeMap;
	CCoordinate						m_cCoordinate;
	CBeginMapMatch					m_cBeginMapMatch;
	CContinueMapMatch				m_cContinueMapMatch;
	CDataLoader						*m_pcDataLoader;
};

#endif //__MAPMATCH_H__
