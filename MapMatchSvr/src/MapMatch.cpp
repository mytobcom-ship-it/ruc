/**
 * @file MapMatch.cpp
 * @brief 맵매칭 클래스 소스 파일
*/

#include "MapMatch.h"

// 에러 코드별 메시지
CODE_ENTRY ErrorCodeTable[] = 
{
	{NO_ERROR,					"오류 없음"}, 
	{INVALID_COORDTYPE,			"측지계 코드 오류"}, 
	{INVALID_COORDINATE,		"좌표범위 초과"}, 
	{INVALID_ANGLE,				"방위각 오류"}, 
	{INVALID_ROADTYPE,			"도로등급 오류"}, 
	{INVALID_SEARCHRADIUS,		"검색 반경 오류"}, 
	{INVALID_LINKID,			"LinkID 오류"}, 
	{INVALID_RESULTCOUNT,		"응답 개수 오류"}, 
	{INVALID_SEARCHSTEP,		"탐색 단계 오류"}, 
	{NOT_FOUND_GRIDINFO,		"GRID 검색 실패"}, 
	{NOT_FOUND_LINKID,			"LinkID 검색 실패"}, 
	{MAP_MATCH_FAIL,			"맵매칭 실패"}
};

/**
 * @brief 생성자
*/
CMapMatch::CMapMatch() : 
	m_pcDataLoader(nullptr)
{
}

/**
 * @brief 소멸자
*/
CMapMatch::~CMapMatch()
{
}

/**
 * @brief 데이터 초기화
 * @param[in]
 * @return true, false
*/
bool CMapMatch::Initialize(CDataLoader *pcDataLoader)
{
	// 형상 데이터 로더 유효성·로드 상태 확인 (2026-07-08 최정우 주석 추가)
	if ((pcDataLoader == nullptr) || (!pcDataLoader->IsLoad()))
		return false;

	m_pcDataLoader = pcDataLoader;
	return true;
}

/**
 * @brief 연속 맵매칭 고도 보조 점수 설정 (config altitude_*)
 * @param[in] stAltConfig 고도 점수 설정
 * @return void
*/
void CMapMatch::SetAltitudeConfig(const ALTITUDE_SCORE_CONFIG& stAltConfig)
{
	// 연속 맵매칭 모듈에 고도 보조 점수 설정 전달 (2026-07-08 최정우 주석 추가)
	m_cContinueMapMatch.SetAltitudeConfig(stAltConfig);
}

/**
 * @brief 초기 맵매칭
 * @param[in] stMapMatchInput 초기 맵매칭 입력 정보
 * @param[out] pstMatchLinkInfo 초기 맵매칭 응답 정보
 * @return true, false
*/
bool CMapMatch::BeginMapMatch(MAP_MATCH_INPUT stMapMatchInput, 
		PMATCH_LINK_INFO pstMatchLinkInfo, PMATCH_TRACE_CTX pstTraceCtx)
{
	uint16 wErrorCode = NO_ERROR;
	enum eCoordinateType eCoordType = static_cast<enum eCoordinateType>(stMapMatchInput.nCoordinateType);
	sint16 nRadius = stMapMatchInput.nRadius;
	double dfX = stMapMatchInput.dfX;
	double dfY = stMapMatchInput.dfY;
	sint16 nAngle = stMapMatchInput.nAngle;

	// 공통 맵 매칭 요청 입력 정보 유효성 검사
	pstMatchLinkInfo->wErrorCode = wErrorCode;
	if (!IsValidCommonRequestValue(eCoordType, nRadius, dfX, dfY, nAngle, pstMatchLinkInfo))
		return false;

	// 초기 맵매칭 시작
	SGMT_MATCH_INPUT stSgmtMatchInput;
	MATCH_ENTRY stMatchEntry;
	
	stSgmtMatchInput.stPoint.dfX = dfX;
	stSgmtMatchInput.stPoint.dfY = dfY;
	stSgmtMatchInput.nRadius = nRadius;
	stSgmtMatchInput.nDirAng = nAngle;
	stSgmtMatchInput.nSpeed = stMapMatchInput.nSpeed;		// 방위각 가중치 적응용(속도) (2026-07-08 최정우 추가)

	// 초기(GRID 기반) 맵매칭 엔진 호출 (2026-07-08 최정우 주석 추가)
	if (!m_cBeginMapMatch.StartMapMatch(m_pcDataLoader, stSgmtMatchInput, &wErrorCode, &stMatchEntry, pstTraceCtx))
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		if (stMatchEntry.dfIntersectLenSgmt >= 0.0)
			pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		return false;
	}

	// 매칭 결과를 응답 구조체로 변환·좌표 역스케일 (2026-07-08 최정우 주석 추가)
	return SetResponseValue(wErrorCode, stMatchEntry, pstMatchLinkInfo);
}

/**
 * @brief 연속 맵매칭
 * @param[in] stMapMatchInput 연속 맵매칭 입력 정보
 * @param[out] pstMatchLinkInfo 연속 맵매칭 응답 정보
 * @return true, false
*/
bool CMapMatch::ContinueMapMatch(MAP_MATCH_INPUT stMapMatchInput, 
		PMATCH_LINK_INFO pstMatchLinkInfo, PMATCH_TRACE_CTX pstTraceCtx)
{
	uint16 wErrorCode = NO_ERROR;
	enum eCoordinateType eCoordType = static_cast<enum eCoordinateType>(stMapMatchInput.nCoordinateType);
	sint16 nRadius = stMapMatchInput.nRadius;
	double dfX = stMapMatchInput.dfX;
	double dfY = stMapMatchInput.dfY;
	sint16 nAngle = stMapMatchInput.nAngle;
	uint64 qwLinkID = stMapMatchInput.qwLinkID;
	// config에서 연속 탐색 depth(검색 단계) 조회 (2026-07-08 최정우 주석 추가)
	sint16 nSearchStep = m_pcDataLoader->GetSearchStep();

	// 공통 맵 매칭 요청 입력 정보 유효성 검사
	pstMatchLinkInfo->wErrorCode = wErrorCode;
	if (!IsValidCommonRequestValue(eCoordType, nRadius, dfX, dfY, nAngle, pstMatchLinkInfo))
		return false;
	
	// 연속 탐색 단계 유효성 검사 (2026-07-08 최정우 주석 추가)
	if (!IsValidSearchStep(nSearchStep))
		return false;

	// 연속 맵매칭 시작
	SGMT_MATCH_INPUT stSgmtMatchInput;
	MATCH_ENTRY stMatchEntry;

	stSgmtMatchInput.stPoint.dfX = dfX;
	stSgmtMatchInput.stPoint.dfY = dfY;
	stSgmtMatchInput.nRadius = nRadius;
	stSgmtMatchInput.nDirAng = nAngle;
	stSgmtMatchInput.nSpeed = stMapMatchInput.nSpeed;		// 방위각 가중치 적응용(속도) (2026-07-08 최정우 추가)
	// 연속 맵매칭 고도 컨텍스트 — 세션 앵커·현재 GPS 고도 (Begin 미전달)
	stSgmtMatchInput.nAltitudeM = stMapMatchInput.nAltitudeM;
	stSgmtMatchInput.nPrevAltitudeM = stMapMatchInput.nPrevAltitudeM;
	stSgmtMatchInput.nPrevRoadType = stMapMatchInput.nPrevRoadType;
	stSgmtMatchInput.nDriveStatus = stMapMatchInput.nDriveStatus;
	stSgmtMatchInput.dfHorizMoveM = stMapMatchInput.dfHorizMoveM;
	stSgmtMatchInput.bUseAltScore = stMapMatchInput.bUseAltScore;

	// 연속(링크 그래프) 맵매칭 엔진 호출 (2026-07-08 최정우 주석 추가)
	if (!m_cContinueMapMatch.StartMapMatch(m_pcDataLoader, stSgmtMatchInput, qwLinkID, nSearchStep, &wErrorCode, &stMatchEntry, pstTraceCtx))
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		if (stMatchEntry.dfIntersectLenSgmt >= 0.0)
			pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		return false;
	}

	// 매칭 결과를 응답 구조체로 변환·좌표 역스케일 (2026-07-08 최정우 주석 추가)
	return SetResponseValue(wErrorCode, stMatchEntry, pstMatchLinkInfo);
}

/**
 * @brief 공통 맵 매칭 요청 입력 정보 유효성 검사
 * @param[in] eCoordType 측지계 코드
 * @param[in] nRadius 맵매칭 유효거리
 * @param[in,out] dfX X 좌표
 * @param[in,out] dfY Y 좌표
 * @param[in] nAngle 방위각
 * @param pstMatchLinkInfo 에러 코드 및 에러 메시지
 * @return true, false
*/
bool CMapMatch::IsValidCommonRequestValue(enum eCoordinateType& eCoordType, 
		sint16& nRadius, double& dfX, double& dfY, sint16 nAngle, 
		PMATCH_LINK_INFO pstMatchLinkInfo)
{
	// 초기화
	memset(pstMatchLinkInfo, 0, MATCH_LINK_INFO_SIZE);

	// 측지계 코드 유효성 검사
	if (!IsValidCoordinateType(eCoordType))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_COORDTYPE;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	// 좌표 변환 및 유효성 검사
	if (!IsValidCoordinate(eCoordType, &dfX, &dfY))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_COORDINATE;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	// 맵매칭 유효 거리
	if (!IsValidSearchRadius(nRadius))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_SEARCHRADIUS;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	// 방위각 유효성 검사
	if (!IsValidAngle(nAngle))
	{
		pstMatchLinkInfo->wErrorCode = INVALID_ANGLE;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
		return false;
	}

	pstMatchLinkInfo->wErrorCode = NO_ERROR;
	// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
	strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
	return true;
}

/**
 * @brief 맵 매칭 결과값
 * @param[in] wErrorCode 에러 코드
 * @param[in] stMatchEntry 매칭 링크 정보
 * @param[out] pstMatchLinkInfo 결과 정보
 * @return true, false
*/
bool CMapMatch::SetResponseValue(uint16 wErrorCode, MATCH_ENTRY stMatchEntry, 
		PMATCH_LINK_INFO pstMatchLinkInfo)
{
	memset(pstMatchLinkInfo, 0, MATCH_LINK_INFO_SIZE);

	if (wErrorCode != NO_ERROR)
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));
	}
	else
	{
		pstMatchLinkInfo->wErrorCode = wErrorCode;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));

		pstMatchLinkInfo->wErrorCode = NO_ERROR;
		// 에러 코드에 대응하는 메시지 문자열 조회 (2026-07-08 최정우 주석 추가)
		strcpy(pstMatchLinkInfo->szErrorMsg, m_cCodeMap.GetValue(ErrorCodeTable, NOE(ErrorCodeTable), pstMatchLinkInfo->wErrorCode));

		// 2026-07-10 최정우 주석 처리: MATCH_ENTRY 에만 존재하는 dfCost/dfAngleCost/dfAltAdj(24B) 때문에
		//   두 구조체 레이아웃이 어긋나, 통복사 시 qwLinkID 이후 필드(링크ID·도로유형·노드 등)가 전부 밀려
		//   쓰레기값이 됨 → 연속(Continue) 맵매칭이 항상 GetLinkInfo 실패로 무력화. 명시적 필드 복사로 교체.
		//memcpy(&pstMatchLinkInfo->dfMatchX, &stMatchEntry, MATCH_ENTRY_SIZE);
		//pstMatchLinkInfo->dfMatchX /= 360000.0;
		//pstMatchLinkInfo->dfMatchY /= 360000.0;
		//pstMatchLinkInfo->dfStNodeX /= 360000.0;
		//pstMatchLinkInfo->dfStNodeY /= 360000.0;
		//pstMatchLinkInfo->dfEdNodeX /= 360000.0;
		//pstMatchLinkInfo->dfEdNodeY /= 360000.0;

		// MATCH_ENTRY → MATCH_LINK_INFO 필드별 복사 (좌표·노드좌표는 /360000 역스케일) (2026-07-10 최정우 수정)
		pstMatchLinkInfo->dfMatchX			= stMatchEntry.dfMatchX / 360000.0;
		pstMatchLinkInfo->dfMatchY			= stMatchEntry.dfMatchY / 360000.0;
		pstMatchLinkInfo->dfSgmtMatchLen	= stMatchEntry.dfSgmtMatchLen;
		pstMatchLinkInfo->dfIntersectLenSgmt = stMatchEntry.dfIntersectLenSgmt;
		pstMatchLinkInfo->nDirAngleDiff		= stMatchEntry.nDirAngleDiff;
		pstMatchLinkInfo->qwLinkID			= stMatchEntry.qwLinkID;
		pstMatchLinkInfo->wLenFromLink		= stMatchEntry.wLenFromLink;
		pstMatchLinkInfo->nMaxSpeed			= stMatchEntry.nMaxSpeed;
		pstMatchLinkInfo->dfLen				= stMatchEntry.dfLen;
		pstMatchLinkInfo->nRoadRank			= stMatchEntry.nRoadRank;
		pstMatchLinkInfo->nConnect			= stMatchEntry.nConnect;
		pstMatchLinkInfo->nRoadType			= stMatchEntry.nRoadType;
		pstMatchLinkInfo->nLanes			= stMatchEntry.nLanes;
		memcpy(pstMatchLinkInfo->szRoadName, stMatchEntry.szRoadName, sizeof(pstMatchLinkInfo->szRoadName));
		pstMatchLinkInfo->qwStNodeID		= stMatchEntry.qwStNodeID;
		pstMatchLinkInfo->dfStNodeX			= stMatchEntry.dfStNodeX / 360000.0;
		pstMatchLinkInfo->dfStNodeY			= stMatchEntry.dfStNodeY / 360000.0;
		pstMatchLinkInfo->nStNodeType		= stMatchEntry.nStNodeType;
		pstMatchLinkInfo->qwEdNodeID		= stMatchEntry.qwEdNodeID;
		pstMatchLinkInfo->dfEdNodeX			= stMatchEntry.dfEdNodeX / 360000.0;
		pstMatchLinkInfo->dfEdNodeY			= stMatchEntry.dfEdNodeY / 360000.0;
		pstMatchLinkInfo->nEdNodeType		= stMatchEntry.nEdNodeType;
	}

	return (wErrorCode == NO_ERROR) ? true : false;
}

/**
 * @brief 측지계 코드 유효성 검사
 * @param[in] eCoordType 측지계 코드
 * @return true, false
*/
bool CMapMatch::IsValidCoordinateType(enum eCoordinateType& eCoordType)
{
	if ((eCoordType < EPSG3857) || (eCoordType > BESSELGEO))
		return false;

	return true;
}

/**
 * @brief 좌표 변환 및 유효성 검사
 * @param[in] eCoordType 측지계 코드
 * @param[in,out] dfX X 좌표
 * @param[in,out] dfY Y 좌표
 * @return true, false
*/
bool CMapMatch::IsValidCoordinate(enum eCoordinateType& eCoordType, 
		double *dfX, double *dfY)
{
	// 좌표 변환
	if (!m_cCoordinate.ConvertCoordinateToWGS84GEO(eCoordType, dfX, dfY))
		return false;

	// 좌표 유효성 검사
	if (!m_cCoordinate.IsValidWGS84GEO(*dfX, *dfY))
		return false;

	return true;
}

/**
 * @brief 검색 반경 유효성 검사
 * @param[in] nRadius 검색 반경
 * @return true, false
*/
bool CMapMatch::IsValidSearchRadius(sint16& nRadius)
{
	if ((nRadius < 0) || (nRadius > 250))
		return false;

	return true;
}

/**
 * @brief 방위각 유효성 검사
 * @param[in] nAngle 방위각
 * @return true, false
*/
bool CMapMatch::IsValidAngle(sint16& nAngle)
{
	if (nAngle == NO_ANGLE) return true;
	if ((nAngle < NO_ANGLE) || (nAngle > 359))
		return false;

	return true;
}

/**
 * @brief 최대 연속 측위 유효성 검사
 * @param[in] nSearchStep 최대 연속 측위 값
 * @return true, false
*/
bool CMapMatch::IsValidSearchStep(sint16& nSearchStep)
{
	if ((nSearchStep < 0) || (nSearchStep > 32767))
		return false;

	return true;
}
