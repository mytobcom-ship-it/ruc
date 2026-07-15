/**
 * @file ProcessManager.h
 * @brief 작업용 클래스 헤더 파일
*/
#ifndef __PROCESS_MANAGER_H__
#define __PROCESS_MANAGER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "DataFormat.h"
#include "MessageType.h"
#include "DataLoader.h"
#include "Clock.h"
#include "log4z.h"
#include "GISUtil.h"
#include "MapMatch.h"

using namespace zsummer::log4z;
using namespace std;

#pragma pack(push, 1)

/**
 * @struct sMapMatchInfo
 * @brief 맵 매칭 GPS 요청 및 결과 정보
*/
typedef struct sMapMatchInfo
{
	char							szSTRE_DT[14+1];					// GPS 저장 일시
	uint32							dwDriveSeq;							// 주행 순서
	POINT							stPoint;							// GPS 좌표
	sint16							nAccuracyM;							// ACCURACY_M 수평오차(m). 기본 NO_ACCURACY (2026-07-08 최정우)
	MATCH_LINK_INFO					stMatchLinkInfo;					// 맵 매칭 정보
	bool							bMatch;								// 맵 매칭 성공 유.무

	sMapMatchInfo() :
		dwDriveSeq(0),
		nAccuracyM(static_cast<sint16>(NO_ACCURACY)),
		bMatch(false)
	{
		memset(szSTRE_DT, 0, sizeof(szSTRE_DT));
		memset(reinterpret_cast<void *>(&stPoint), 0, sizeof(stPoint));
		memset(reinterpret_cast<void *>(&stMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	}

	bool operator<(const struct sMapMatchInfo& stMapMatchInfo) const
	{
		if (stMatchLinkInfo.qwLinkID == stMapMatchInfo.stMatchLinkInfo.qwLinkID)
			return dwDriveSeq < stMapMatchInfo.dwDriveSeq;
		else
			return stMatchLinkInfo.qwLinkID < stMapMatchInfo.stMatchLinkInfo.qwLinkID;
	}
} MAP_MATCH_INFO, *PMAP_MATCH_INFO;

#define MAP_MATCH_INFO_SIZE												sizeof(MAP_MATCH_INFO)

/**
 * @struct sLinkSpeed
 * @brief 맵 매칭 결과 링크별 속도 정보
*/
typedef struct sLinkSpeed
{
	uint64							qwLinkID;							// 링크 ID
	char							szLinkID[10+1];						// 링크 ID 문자열
	time_t							dtInDT;								// 링크 진입 시간
	time_t							dtOutDT;							// 링크 진출 시간
	char							szInDT[14+1];						// 링크 진입 일시
	char							szOutDT[14+1];						// 링크 진출 일시
	char							szCreatDE[8+1];						// 가공 생성 년월일
	char							szCreatHM[4+1];						// 가공 생성 시분
	double							dfInLenFromLink;					// 진입시 링크 시작부터 매칭된 거리
	double							dfOutLenFromLink;					// 진출시 링크 시작부터 매칭된 거리
	uint32							dwElapseSec;						// 실측 통행 시간 (초)
	double							dfLen;								// 링크 길이 (m)
	char							szRoadRank[3+1];					// 도로 등급
	char							szDSTRCT_CODE[3+1];					// 권역 코드
	double							dfDistance;							// 이동 거리
	double							dfSpeed;							// 실측 주행 속도

	sLinkSpeed() : 
		qwLinkID(0), 
		dtInDT(0), 
		dtOutDT(0), 
		dfInLenFromLink(0.0), 
		dfOutLenFromLink(0.0), 
		dwElapseSec(0), 
		dfLen(0.0), 
		dfDistance(0.0), 
		dfSpeed(0.0)
	{
		memset(reinterpret_cast<void *>(szCreatDE), 0, sizeof(szCreatDE));
		memset(reinterpret_cast<void *>(szCreatHM), 0, sizeof(szCreatHM));
		memset(reinterpret_cast<void *>(szLinkID), 0, sizeof(szLinkID));
		memset(reinterpret_cast<void *>(szRoadRank), 0, sizeof(szRoadRank));
		memset(reinterpret_cast<void *>(szDSTRCT_CODE), 0, sizeof(szDSTRCT_CODE));
	}
} LINK_SPEED, *PLINK_SPEED;

#pragma pack(pop)

/**
 * @class CProcessManager
 * @brief 작업용 클래스
*/
class CProcessManager
{
public:
	CProcessManager();
	virtual ~CProcessManager();

	bool Initialize(const int nThreadId, CDataLoader *pcDataLoader, 
		const uint8& nCoordinateType, const sint16& nRadius, const uint32& dwMaxDistance,
		const double& dfRadiusScale, const sint16& nRadiusMin, const sint16& nRadiusMax,
		const ALTITUDE_SCORE_CONFIG& stAltitudeConfig);
	bool StartProcess(const char *pszStartDate, const char *pszDriveID, const char *pszOperID);
	bool ProcessRawLog(const sRawLogInfo& stRawLogInfo, uint64& qwInOutLinkID,
		MATCH_LINK_INFO *pstMatchLinkInfo, const ALT_MATCH_CTX *pstAltCtx = nullptr);
	// 반경 밖 최근접 세그먼트 탐색(진단용) — INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 확보
	bool FindNearestSegment(const sRawLogInfo& stRawLogInfo, MATCH_LINK_INFO *pstMatchLinkInfo);
	// 진단반경(MM_DIAG_RADIUS) 초과여도 기하 최근접 1건 (SKIP 참고용, 세션 미갱신) (2026-07-10 최정우 수정)
	bool FindGeomNearestSegment(const sRawLogInfo& stRawLogInfo, MATCH_LINK_INFO *pstMatchLinkInfo);

private:
	void BuildMapMatchInput(const sRawLogInfo& stRawLogInfo, MAP_MATCH_INPUT *pstMapMatchInput,
		uint64 qwLinkID, const ALT_MATCH_CTX *pstAltCtx) const;
	sint16 CalcAdaptiveRadius(sint16 nAccuracyM) const;
	// 지정 반경(stMapMatchInput.nRadius)으로 Continue→시작 1회 시도 (widen-on-miss 재시도용) (2026-07-10 최정우 추가)
	bool AttemptMatch(const sRawLogInfo& stRawLogInfo, MAP_MATCH_INPUT& stMapMatchInput,
		uint64& qwInOutLinkID, uint64 qwPrevLinkId, MATCH_LINK_INFO *pstMatchLinkInfo,
		const ALT_MATCH_CTX *pstAltCtx);
	time_t GetConvertTime(char *pszDate);
	const double GetDistance(POINT stPrePoint, 
		MATCH_LINK_INFO stMatchLinkInfo);
	bool GetDirAzimuth(POINT& stMatchPt, POINT& stPoint, sint16 *pnHeading);

private:
	CDataLoader						*m_pcDataLoader;					// 형상정보 데이터 클래스
	CMapMatch						*m_pcMapMatch;						// 맵매칭 (실시간 처리용)
	CGISUtil						m_cGISUtil;

private:
	int								m_nThreadId;						// 쓰레드 ID
	uint8							m_nCoordinateType;					// GPS 좌표 측지계
	sint16							m_nRadius;							// config radius — ACCURACY_M NULL 시 검색 반경 폴백 (m) (2026-07-08 최정우)
	double							m_dfRadiusScale;					// config radius_scale — 검색반경 = scale × ACCURACY_M (2026-07-08 최정우)
	sint16							m_nRadiusMin;						// config radius_min — 적응형 검색 반경 하한 (m) (2026-07-08 최정우)
	sint16							m_nRadiusMax;						// config radius_max — 적응형 검색 반경 상한 (m) (2026-07-08 최정우)
	ALTITUDE_SCORE_CONFIG			m_stAltitudeConfig;
	uint64							m_qwLinkID;
	uint32							m_dwMaxDistance;					// 연속 맵매칭시 Heading 유효거리
	char							m_szStartDate[14+1];				// 맵 매칭 요청 SEQ 1번 저장 일시
	char							m_szDriveID[20+1];					// 주행 ID
	char							m_szOperID[30+1];					// 운영자 ID
	vector<MAP_MATCH_INFO>			m_vtMapMatchInfoList;				// 맵 매칭 결과 정보
	vector<LINK_SPEED>				m_vtLinkSpeedList;					// 링크별 주행 속도 정보 목록
};

#endif //__PROCESS_MANAGER_H__
