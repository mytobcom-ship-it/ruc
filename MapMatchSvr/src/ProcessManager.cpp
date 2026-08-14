/**
 * @file ProcessManager.cpp
 * @brief 작업용 클래스 소스 파일
*/
#include "ProcessManager.h"

namespace {

void FillMatchTraceCtx(MATCH_TRACE_CTX& stTraceCtx, int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MAP_MATCH_INPUT& stMapMatchInput, uint64 qwPrevLinkId, bool bContinue,
		const ALT_MATCH_CTX *pstAltCtx)
{
	memset(reinterpret_cast<void *>(&stTraceCtx), 0, sizeof(stTraceCtx));
	stTraceCtx.nThreadId = nThreadId;
	strncpy(stTraceCtx.szDeviceKey, stRawLogInfo.szDeviceKey, sizeof(stTraceCtx.szDeviceKey) - 1);
	strncpy(stTraceCtx.szTripId, stRawLogInfo.szTripID, sizeof(stTraceCtx.szTripId) - 1);
	stTraceCtx.dwSeqNo = stRawLogInfo.dwSeqNo;
	stTraceCtx.dfGpsLat = stRawLogInfo.dfY;
	stTraceCtx.dfGpsLon = stRawLogInfo.dfX;
	stTraceCtx.nRadius = stMapMatchInput.nRadius;
	stTraceCtx.nSpeed = stMapMatchInput.nSpeed;
	stTraceCtx.nHeading = stMapMatchInput.nAngle;
	stTraceCtx.nAltitudeM = stMapMatchInput.nAltitudeM;
	stTraceCtx.bContinue = bContinue;
	stTraceCtx.qwPrevLinkId = qwPrevLinkId;
	stTraceCtx.nMatchedStep = 0;
	if (pstAltCtx != nullptr && pstAltCtx->bHasPrevAlt)
	{
		stTraceCtx.nPrevAltitude = pstAltCtx->nPrevAltitude;
		stTraceCtx.nPrevRoadType = pstAltCtx->nPrevRoadType;
	}
}

} // namespace

/**
 * @brief 생성자
*/
CProcessManager::CProcessManager() : 
	m_pcDataLoader(nullptr), 
	m_pcMapMatch(nullptr),
	m_nThreadId(0), 
	m_nCoordinateType(0), 
	m_nRadius(0), 
	m_dfRadiusScale(2.5),
	m_nRadiusMin(20),
	m_nRadiusMax(50),
	m_qwLinkID(0), 
	m_dwMaxDistance(0)
{
	// init
	memset(reinterpret_cast<void *>(m_szStartDate), 0, sizeof(m_szStartDate));
	memset(reinterpret_cast<void *>(m_szDriveID), 0, sizeof(m_szDriveID));
	memset(reinterpret_cast<void *>(m_szOperID), 0, sizeof(m_szOperID));
	m_vtMapMatchInfoList.clear();
}

/**
 * @brief 소멸자
*/
CProcessManager::~CProcessManager()
{
	if (m_pcMapMatch != nullptr)
	{
		delete m_pcMapMatch;
		m_pcMapMatch = nullptr;
	}
}

/**
 * @brief 작업 초기화 및 맵 매칭 작업
 * @param[in] nThreadId 쓰레드 ID
 * @param[in] pcDataLoader 형상정보 데이터 클래스
 * @param[in] nCoordinateType GPS 좌표 측지계
 * @param[in] nRadius config radius — ACCURACY_M NULL 시 검색 반경 폴백 (m) (2026-07-08 최정우)
 * @param[in] dwMaxDistance 연속 맵 매칭시 Heading 유효거리
 * @param[in] dfRadiusScale config radius_scale — 검색반경 = scale × ACCURACY_M (2026-07-08 최정우)
 * @param[in] nRadiusMin config radius_min — 적응형 검색 반경 하한 (m) (2026-07-08 최정우)
 * @param[in] nRadiusMax config radius_max — 적응형 검색 반경 상한 (m) (2026-07-08 최정우)
 * @param[in] stAltitudeConfig config altitude_* — 연속 맵매칭 고도 보조 점수
 * @return true(성공), false(실패)
*/
bool CProcessManager::Initialize(const int nThreadId, CDataLoader *pcDataLoader,
		const uint8& nCoordinateType, const sint16& nRadius, const uint32& dwMaxDistance,
		const double& dfRadiusScale, const sint16& nRadiusMin, const sint16& nRadiusMax,
		const ALTITUDE_SCORE_CONFIG& stAltitudeConfig)
{
	m_nThreadId = nThreadId;					// 쓰레드 ID

	m_pcDataLoader = pcDataLoader;
	if (m_pcDataLoader == nullptr)
	{
		LOGFMTE("[#%02d] data loader is null!", m_nThreadId);
		return false;
	}

	m_nCoordinateType = nCoordinateType;		// GPS 좌표 측지계
	m_nRadius = nRadius;						// 검색 반경 폴백 (ACCURACY_M NULL)
	m_dwMaxDistance = dwMaxDistance;			// 연속 맵매칭시 Heading 유효거리
	m_dfRadiusScale = (dfRadiusScale > 0.0) ? dfRadiusScale : 2.5;
	m_nRadiusMin = (nRadiusMin > 0) ? nRadiusMin : 20;
	m_nRadiusMax = (nRadiusMax >= m_nRadiusMin) ? nRadiusMax : m_nRadiusMin;
	m_stAltitudeConfig = stAltitudeConfig;

	if (m_pcMapMatch != nullptr)
	{
		delete m_pcMapMatch;
		m_pcMapMatch = nullptr;
	}

	m_pcMapMatch = new (std::nothrow)CMapMatch;
	if (m_pcMapMatch == nullptr)
	{
		LOGFMTE("[#%02d] map match instance is null!", m_nThreadId);
		return false;
	}

	// MapMatch 인스턴스에 형상 데이터 연결 (2026-07-08 최정우 주석 추가)
	if (!m_pcMapMatch->Initialize(m_pcDataLoader))
	{
		delete m_pcMapMatch;
		m_pcMapMatch = nullptr;
		LOGFMTE("[#%02d] map match initialize failed!", m_nThreadId);
		return false;
	}

	// 연속 맵매칭 고도 보조 점수 config 적용 (2026-07-08 최정우 주석 추가)
	m_pcMapMatch->SetAltitudeConfig(m_stAltitudeConfig);

	return true;
}

/**
 * @brief ACCURACY_M 기반 적응형 맵매칭 검색 반경 (m)
 * @param[in] nAccuracyM 수평 오차 ACCURACY_M (m). NO_ACCURACY 이면 config radius 폴백
 * @return 검색 반경 (m)
 * @remark 2026-07-08 최정우 추가
 *
 * 공식:
 *   ACCURACY_M NULL  → 검색반경 = radius
 *   ACCURACY_M 있음  → 검색반경 = max(radius_min, min(round(radius_scale × ACCURACY_M), radius_max))
 *
 * 예) radius=50, radius_scale=2.5, radius_min=20, radius_max=50
 *     ACCURACY_M=10  → round(2.5×10)=25  → 25m
 *     ACCURACY_M=30  → round(2.5×30)=75  → min(75,50)=50m
 *     ACCURACY_M=NULL → 50m (radius 폴백)
*/
sint16 CProcessManager::CalcAdaptiveRadius(sint16 nAccuracyM) const
{
	if (nAccuracyM < 0)
		return m_nRadius;

	int nAdaptive = static_cast<int>(m_dfRadiusScale * static_cast<double>(nAccuracyM) + 0.5);
	nAdaptive = std::max(static_cast<int>(m_nRadiusMin),
			std::min(nAdaptive, static_cast<int>(m_nRadiusMax)));
	return static_cast<sint16>(nAdaptive);
}

/**
 * @brief RAW_LOG_INFO → MAP_MATCH_INPUT 변환
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pstMapMatchInput 맵매칭 입력
 * @param[in] qwLinkID 직전 링크 ID (0이면 시작 후보)
 * @param[in] pstAltCtx 연속 맵매칭 고도 컨텍스트 (nullable)
 * @return void
 * @remark
 *   고도 보조 점수(bUseAltScore) 활성 조건 (모두 충족):
 *     1) qwLinkID ≠ 0 (연속 맵매칭)
 *     2) pstAltCtx·bHasPrevAlt (직전 성공 시 GPS 고도 앵커 있음)
 *     3) alt_weight > 0
 *   예) 직전100m·현재106m·같은 ROAD_TYPE → ContinueMapMatch에서 dfCost −10 가산
*/
void CProcessManager::BuildMapMatchInput(const sRawLogInfo& stRawLogInfo,
		MAP_MATCH_INPUT *pstMapMatchInput, uint64 qwLinkID,
		const ALT_MATCH_CTX *pstAltCtx) const
{
	if (pstMapMatchInput == nullptr)
		return;

	*pstMapMatchInput = MAP_MATCH_INPUT();
	pstMapMatchInput->nCoordinateType = (stRawLogInfo.nCoordinateType != 0)
		? stRawLogInfo.nCoordinateType : m_nCoordinateType;
	// ACCURACY_M 기반 적응형 검색 반경 산출 (2026-07-08 최정우 주석 추가)
	pstMapMatchInput->nRadius = CalcAdaptiveRadius(stRawLogInfo.nAccuracyM);
	pstMapMatchInput->dfX = stRawLogInfo.dfX;
	pstMapMatchInput->dfY = stRawLogInfo.dfY;
	pstMapMatchInput->nAngle = (stRawLogInfo.nAngle >= 0) ? stRawLogInfo.nAngle : static_cast<sint16>(NO_ANGLE);
	pstMapMatchInput->nSpeed = (stRawLogInfo.fSpeed >= 0.0f)
		? static_cast<sint16>(stRawLogInfo.fSpeed + 0.5f) : static_cast<sint16>(NO_SPEED);
	pstMapMatchInput->qwLinkID = 0;
	pstMapMatchInput->nAltitudeM = stRawLogInfo.nAltitudeM;
	pstMapMatchInput->nDriveStatus = stRawLogInfo.nDriveStatus;

	// 연속 맵매칭 + 직전 고도 앵커 + config 활성 시 고도 보조 점수 ON
	if (qwLinkID != 0 && pstAltCtx != nullptr && pstAltCtx->bHasPrevAlt
		&& m_stAltitudeConfig.dfWeight > 0.0)
	{
		pstMapMatchInput->bUseAltScore = true;
		pstMapMatchInput->nPrevAltitude = pstAltCtx->nPrevAltitude;
		pstMapMatchInput->nPrevRoadType = pstAltCtx->nPrevRoadType;
		pstMapMatchInput->dfHorizMove = pstAltCtx->dfHorizMove;
	}

	// 연속 맵매칭 + 직전 매칭 위치 보유 시 역행 페널티용 위치 전달 (2026-07-20 최정우 추가)
	if (qwLinkID != 0 && pstAltCtx != nullptr && pstAltCtx->bHasPrevLinkPos)
	{
		pstMapMatchInput->dfPrevLinkPos = pstAltCtx->dfPrevLinkPos;
		pstMapMatchInput->bHasPrevLinkPos = true;
		// 같은 링크 노이즈 보정 기준점(직전 신뢰 매칭 좌표) 함께 전달 (2026-07-22 최정우 추가)
		pstMapMatchInput->dfPrevMatchX = pstAltCtx->dfPrevMatchX;
		pstMapMatchInput->dfPrevMatchY = pstAltCtx->dfPrevMatchY;
	}
}

/**
 * @brief 실시간 GPS 1건 맵매칭 (시작 / Continue)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in,out] qwInOutLinkID 직전 링크 ID (device_key 세션). 성공 시 갱신
 * @param[out] pstMatchLinkInfo 맵매칭 결과
 * @return true(매칭 성공), false(실패)
*/
bool CProcessManager::ProcessRawLog(const sRawLogInfo& stRawLogInfo, uint64& qwInOutLinkID,
		MATCH_LINK_INFO *pstMatchLinkInfo, const ALT_MATCH_CTX *pstAltCtx)
{
	if (m_pcMapMatch == nullptr || pstMatchLinkInfo == nullptr)
		return false;

	memset(reinterpret_cast<void *>(pstMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	pstMatchLinkInfo->dfIntersectLenSgmt = -1.0;

	const uint64 qwPrevLinkId = qwInOutLinkID;

	MAP_MATCH_INPUT stMapMatchInput;
	// RAW GPS → MAP_MATCH_INPUT 변환 (적응형 검색 반경 포함) (2026-07-08 최정우 주석 추가)
	BuildMapMatchInput(stRawLogInfo, &stMapMatchInput, qwInOutLinkID, pstAltCtx);

	// 1차: 적응형 반경으로 Continue→시작 시도 (2026-07-10 최정우 수정)
	if (AttemptMatch(stRawLogInfo, stMapMatchInput, qwInOutLinkID, qwPrevLinkId, pstMatchLinkInfo, pstAltCtx))
		return true;

	// widen-on-miss: 적응형 반경이 상한보다 작으면 넓은 반경으로 1회 확장 재시도 (2026-07-10 최정우 추가)
	//   경계 밖(예: 반경 1m 초과)에 링크가 있어 놓친 경우 구제. 정밀(좁은 반경) 우선 + 실패 시에만 확장.
	const sint16 nWideRadius = std::max(m_nRadius, m_nRadiusMax);
	if (stMapMatchInput.nRadius < nWideRadius)
	{
		qwInOutLinkID = qwPrevLinkId;					// 연속 재시도 위해 직전 링크 복원
		// 입력 재구성(반경 외 nAngle/고도 컨텍스트 원복) 후 검색 반경만 확장 (2026-07-10 최정우 추가)
		BuildMapMatchInput(stRawLogInfo, &stMapMatchInput, qwInOutLinkID, pstAltCtx);
		stMapMatchInput.nRadius = nWideRadius;
		LOGFMTD("[#%02d] widen-on-miss retry!device=[%s] seq=[%u] radius=[%d]",
			m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
			static_cast<int>(nWideRadius));
		if (AttemptMatch(stRawLogInfo, stMapMatchInput, qwInOutLinkID, qwPrevLinkId, pstMatchLinkInfo, pstAltCtx))
			return true;
	}

	qwInOutLinkID = 0;

	// 정식 매칭 실패 — ① 진단반경(MM_DIAG_RADIUS) 이내 최근접 (2026-07-10 최정우 수정)
	//   MATCHED 아님 → SKIP·세션 미갱신, DB에 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
	if (FindNearestSegment(stRawLogInfo, pstMatchLinkInfo))
	{
		pstMatchLinkInfo->bOutOfRadius = true;
		LOGFMTD("[#%02d] out-of-radius nearest captured!device=[%s] seq=[%u] intersect_len=[%.1fm]",
			m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
			pstMatchLinkInfo->dfIntersectLenSgmt);
		return false;
	}

	// ② 그리드에 후보 있으나 진단반경 초과 — 반경 무시 기하 최근접 (2026-07-10 최정우 수정)
	//   동일하게 SKIP·세션 미갱신·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
	if (FindGeomNearestSegment(stRawLogInfo, pstMatchLinkInfo))
	{
		pstMatchLinkInfo->bOutOfRadius = true;
		LOGFMTW("[#%02d] beyond-diag nearest captured!device=[%s] seq=[%u] intersect_len=[%.1fm] diag_max=[%dm]",
			m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
			pstMatchLinkInfo->dfIntersectLenSgmt, MM_DIAG_RADIUS);
		return false;
	}

	LOGFMTW("[#%02d] map match failed!device=[%s] seq=[%u] err=[%u]",
		m_nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo,
		pstMatchLinkInfo->wErrorCode);
	return false;
}

/**
 * @brief 반경 밖 최근접 세그먼트 탐색(진단용) — MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 확보
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pstMatchLinkInfo 최근접 세그먼트 결과(좌표·INTERSECT_LEN·링크정보)
 * @return true(최근접 후보 발견), false(진단 반경 내 후보 없음)
 * @remark 방위각 무시, 시작(연속 아님), 진단반경 MM_DIAG_RADIUS 이내.
 *         MATCHED 아님 — SKIP·세션 미갱신·DB 참고용만 (2026-07-10 최정우 수정)
*/
bool CProcessManager::FindNearestSegment(const sRawLogInfo& stRawLogInfo,
		MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if ((m_pcMapMatch == nullptr) || (pstMatchLinkInfo == nullptr))
		return false;

	MAP_MATCH_INPUT stDiagInput;
	// 입력 재구성(고도 컨텍스트 없음, 시작) 후 방위각 무시·최대 반경으로 최근접 탐색 (2026-07-10 최정우 추가)
	BuildMapMatchInput(stRawLogInfo, &stDiagInput, 0, nullptr);
	stDiagInput.qwLinkID = 0;
	stDiagInput.nAngle = NO_ANGLE;					// 순수 기하 최근접(방위각 필터 미적용)
	stDiagInput.nRadius = MM_DIAG_RADIUS;			// 반경 밖 후보까지 포함하도록 최대 반경

	MATCH_TRACE_CTX stTraceCtx;
	FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stDiagInput, 0, false, nullptr);
	// GRID 기반 시작 으로 최근접 세그먼트 1개 획득 (좌표는 /360000 역스케일되어 채워짐) (2026-07-10 최정우 수정)
	return m_pcMapMatch->BeginMapMatch(stDiagInput, pstMatchLinkInfo, &stTraceCtx);
}

/**
 * @brief 진단반경 초과여도 기하 최근접 세그먼트 1건 (SKIP 참고용) (2026-07-10 최정우 수정)
 * @remark 정식·진단반경 매칭 실패 후 호출. MATCHED 아님, 세션 링크·앵커 미갱신.
*/
bool CProcessManager::FindGeomNearestSegment(const sRawLogInfo& stRawLogInfo,
		MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if ((m_pcMapMatch == nullptr) || (pstMatchLinkInfo == nullptr))
		return false;

	MAP_MATCH_INPUT stDiagInput;
	BuildMapMatchInput(stRawLogInfo, &stDiagInput, 0, nullptr);
	stDiagInput.qwLinkID = 0;
	stDiagInput.nAngle = NO_ANGLE;
	stDiagInput.nRadius = MM_DIAG_RADIUS;

	return m_pcMapMatch->BeginGeomNearest(stDiagInput, pstMatchLinkInfo);
}

/**
 * @brief 지정 반경(stMapMatchInput.nRadius)으로 연속(Continue)→초기(시작) 1회 시도 (2026-07-10 최정우 추가)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in,out] stMapMatchInput 맵매칭 입력 (nRadius 등 호출측에서 세팅). 내부에서 qwLinkID/nAngle 갱신
 * @param[in,out] qwInOutLinkID 직전 링크 ID. 연속 실패 시 0, 성공 시 매칭 링크로 갱신
 * @param[in] qwPrevLinkId 트레이스용 직전 링크 ID
 * @param[out] pstMatchLinkInfo 맵매칭 결과
 * @param[in] pstAltCtx 고도 컨텍스트 (nullable)
 * @return true(매칭 성공), false(실패)
 * @remark widen-on-miss 재시도에서 동일 로직을 반경만 바꿔 재사용하기 위한 분리
*/
bool CProcessManager::AttemptMatch(const sRawLogInfo& stRawLogInfo, MAP_MATCH_INPUT& stMapMatchInput,
		uint64& qwInOutLinkID, uint64 qwPrevLinkId, MATCH_LINK_INFO *pstMatchLinkInfo,
		const ALT_MATCH_CTX *pstAltCtx)
{
	MATCH_TRACE_CTX stTraceCtx;

	if (qwInOutLinkID != 0)
	{
		stMapMatchInput.qwLinkID = qwInOutLinkID;
		FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stMapMatchInput, qwPrevLinkId, true, pstAltCtx);
		// 직전 링크 기준 연속 맵매칭 시도 (2026-07-08 최정우 주석 추가)
		if (m_pcMapMatch->ContinueMapMatch(stMapMatchInput, pstMatchLinkInfo, &stTraceCtx))
		{
			qwInOutLinkID = pstMatchLinkInfo->qwLinkID;
			LOGFMTD("[#%02d] continue map match ok!device=[%s] link=[%llu]",
				m_nThreadId, stRawLogInfo.szDeviceKey,
				static_cast<unsigned long long>(qwInOutLinkID));
			return true;
		}

		qwInOutLinkID = 0;
		stMapMatchInput.qwLinkID = 0;
		stMapMatchInput.nAngle = NO_ANGLE;
	}

	// 연속 실패·초기 세션 — GRID 기반 Begin 맵매칭 (2026-07-08 최정우 주석 추가)
	// 직전 성공 링크가 있으면 연결성 편향 전달 → 나란한 도로 오매칭 억제 (2026-07-15 최정우 추가)
	stMapMatchInput.qwBiasLinkID = qwPrevLinkId;
	FillMatchTraceCtx(stTraceCtx, m_nThreadId, stRawLogInfo, stMapMatchInput, qwPrevLinkId, false, pstAltCtx);
	if (m_pcMapMatch->BeginMapMatch(stMapMatchInput, pstMatchLinkInfo, &stTraceCtx))
	{
		qwInOutLinkID = pstMatchLinkInfo->qwLinkID;
		LOGFMTD("[#%02d] begin map match ok!device=[%s] link=[%llu]",
			m_nThreadId, stRawLogInfo.szDeviceKey,
			static_cast<unsigned long long>(qwInOutLinkID));
		return true;
	}

	return false;
}

/**
 * @brief 진행 각도 계산
 * @param[in] stMatchPt 매칭 X,Y 좌표
 * @param[in] stPoint 요청 X,Y 좌표
 * @param[out] pnHeading 진행 각도(방위각)
 * @return true(성공), false(실패)
*/
bool CProcessManager::GetDirAzimuth(POINT& stMatchPt, POINT& stPoint, sint16 *pnHeading)
{
	int nHeading = 0;

	if ((stPoint.dfY - stMatchPt.dfY) > 0)
		nHeading = DEG(atan((stPoint.dfX - stMatchPt.dfX) / (stPoint.dfY - stMatchPt.dfY)));
	else if (((stPoint.dfY - stMatchPt.dfY) < 0) && ((stPoint.dfX - stMatchPt.dfX) > 0))
		nHeading = DEG(atan((stPoint.dfX - stMatchPt.dfX) / (stPoint.dfY - stMatchPt.dfY))) + 180;
	else if (((stPoint.dfY - stMatchPt.dfY) < 0) && ((stPoint.dfX - stMatchPt.dfX) < 0))
		nHeading = DEG(atan((stPoint.dfX - stMatchPt.dfX) / (stPoint.dfY - stMatchPt.dfY))) - 180;
	else if (((stPoint.dfY - stMatchPt.dfY < 0) && (stPoint.dfX == stMatchPt.dfX)))
		nHeading = 180;
	else if ((stPoint.dfY == stMatchPt.dfY) && ((stPoint.dfX - stMatchPt.dfX) > 0))
		nHeading = 90;
	else if ((stPoint.dfY == stMatchPt.dfY) && ((stPoint.dfX - stMatchPt.dfX) < 0))
		nHeading = -90;
	else
		return false;

	nHeading = 90 - nHeading;
	while (nHeading < 0)
		nHeading += 360;

	*pnHeading = nHeading;
	return true;
}
