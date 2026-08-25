/**
 * @file RawLogWorker.cpp
 * @brief 원시 GPS batch 맵매칭·DB 결과 갱신 워커 클래스 소스 파일
*/
#include "RawLogWorker.h"
#include "Clock.h"
#include "log4z.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <cctype>
#include <unistd.h>
#include <pthread.h>
#include <unordered_map>
#include <libpq-fe.h>

using namespace zsummer::log4z;

namespace {

// bulk release 재시도 횟수 (PK: TRIP_ID|GPS_SEQ). 워커 스레드 간 공유 (2026-07-10 최정우 주석 추가)
pthread_mutex_t g_retryCountMutex = PTHREAD_MUTEX_INITIALIZER;
unordered_map<string, int> g_mapReleaseRetryCount;

/**
 * @brief release 재시도 카운트 맵 키 생성
 * @param[in] strTripId 운행 ID (PK-1)
 * @param[in] strGpsSeq GPS 순번 (PK-2, 문자열)
 * @return TRIP_ID|GPS_SEQ 형식 키
 * @remark BulkReleaseRawLogs() 재시도 상한(nRetryMax) 판별용
 */
static string MakeReleaseRetryKey(const string& strTripId, const string& strGpsSeq)
{
	return strTripId + "|" + strGpsSeq;
}

/**
 * @brief release 재시도 횟수 1 증가 (스레드 안전)
 * @param[in] strKey MakeReleaseRetryKey() 로 생성한 키
 * @return 증가 후 재시도 횟수
 * @remark BulkReleaseRawLogs() 에서 PROCESSING→PENDING 해제 실패 시 누적
 */
static int BumpReleaseRetryCount(const string& strKey)
{
	pthread_mutex_lock(&g_retryCountMutex);
	int nCount = ++g_mapReleaseRetryCount[strKey];
	pthread_mutex_unlock(&g_retryCountMutex);
	return nCount;
}

/**
 * @brief release 재시도 카운트 제거 (스레드 안전)
 * @param[in] strKey MakeReleaseRetryKey() 로 생성한 키
 * @return void
 * @remark BulkUpdateRawLogs() 정상 완료(MATCHED/SKIP/ERROR) 시 호출
 */
static void ClearReleaseRetryCount(const string& strKey)
{
	pthread_mutex_lock(&g_retryCountMutex);
	g_mapReleaseRetryCount.erase(strKey);
	pthread_mutex_unlock(&g_retryCountMutex);
}

/**
 * @brief 커넥션 풀에서 DB 연결 핸들 확보 (일시 고갈 시 재시도)
 * @param[in] pcPool PostgreSQL 커넥션 풀
 * @param[in] nMaxAttempt 재시도 최대 횟수 ([database] retrymax, 회)
 * @param[in] nWaitMs 재시도 사이 대기 ([database] retrywait, ms)
 * @return PGconn*(성공), nullptr(실패)
 * @remark getConnection() 실패 시 nWaitMs 간격으로 최대 nMaxAttempt 회 시도 (2026-07-10 최정우 주석 추가)
 */
static PGconn* AcquirePoolConnection(CPostgrePool *pcPool, int nMaxAttempt, int nWaitMs)
{
	if (pcPool == nullptr)
		return nullptr;

	if (nMaxAttempt < 1)
		nMaxAttempt = 1;
	if (nWaitMs < 0)
		nWaitMs = 0;

	for (int nAttempt=1; nAttempt<=nMaxAttempt; ++nAttempt)
	{
		PGconn *pcConn = pcPool->getConnection();
		if (pcConn != nullptr)
			return pcConn;

		if ((nAttempt < nMaxAttempt) && (nWaitMs > 0))
			usleep(static_cast<useconds_t>(nWaitMs) * 1000);
	}

	return nullptr;
}

} // namespace

/**
 * @brief 생성자
*/
CRawLogWorker::CRawLogWorker()
{
	memset(reinterpret_cast<void *>(&m_stConfig), 0, sizeof(m_stConfig));
}

/**
 * @brief 소멸자
*/
CRawLogWorker::~CRawLogWorker()
{
}

/**
 * @brief 워커 공유 설정 및 스레드별 trip_id 세션 맵 초기화
 * @param[in] stConfig DB pool, SQL, ProcessManager, 워커 스레드 수
 * @return void
*/
void CRawLogWorker::SetConfig(const RAWLOG_WORKER_CONFIG& stConfig)
{
	m_stConfig = stConfig;

	if (m_stConfig.nWorkerThreads <= 0)
		m_stConfig.nWorkerThreads = 1;

	// [database] conn_retry — 기동 시 LoadConfig 값 보정 (2026-07-10 최정우 추가)
	if (m_stConfig.nConnRetryMax < 1)
		m_stConfig.nConnRetryMax = 1;
	if (m_stConfig.nConnRetryWait < 0)
		m_stConfig.nConnRetryWait = 0;

	m_vtTripSessions.clear();
	m_vtTripSessions.resize(static_cast<size_t>(m_stConfig.nWorkerThreads));
}

/**
 * @brief dtLastSeen 경과 trip_id 세션 만료 제거 (#6 TTL, 워커 스레드 self)
 * @param[in] nThreadId 워커 스레드 ID (자기 소유 세션 맵만 정리)
 * @param[in] nTtlSec TTL (초). 0 이하면 비활성
 * @param[in] pcConn TTL 만료 시점에 아직 주정차 세션이 열려 있으면 즉시 위반 INSERT, 4유형 중
 *   미확정(TRIP_END_DT NULL) 레코드가 있으면 마감 UPDATE 하는 데 씀(nullptr 이면 둘 다 스킵)
 * @return 제거된 세션 수
 * @remark 각 워커가 자기 맵(m_vtTripSessions[nThreadId])만 정리 → 락 불필요(소유권 유지).
 *         run() 배치 처리 직후 호출되어 모니터 스레드와의 동시 접근(데이터 레이스)을 제거한다.
 *         TTL 만료 = 그 이후로 GPS 자체가 안 온 것 — END 이벤트를 놓쳤든 단말이 아예 전송을 멈췄든
 *         서버 입장에서 원인은 구분 불가. 주정차는 "계속 정차 중이었다"로 간주해 마지막으로 확인된
 *         위치·시각까지의 체류시간으로 위반을 확정(사용자 지시, 2026-08-13). 나머지(이미 INSERT돼
 *         있는데 트립이 안 끝나 TRIP_END_DT 가 아직 NULL인 레코드, 4유형 전부 해당 — 개방형은
 *         게이트 통과 즉시 Y/0 확정이라 항상 이 케이스)는 N/3(AUDIT) + TRIP_END_DT(마지막 확인
 *         시각)로 마감(사용자 지시, 2026-08-13 — `UpdateAbnormalTripEnd()`, 최초엔 개방형
 *         한정·status=4 였다가 전 유형·status=3(AUDIT)으로 확대·정정). 참고: 세션 자체가 다른
 *         이유로 유실되는 경우(서버 재시작 등, in-memory라 영속 안 됨)는 이 경로로도 못 잡음 —
 *         별도 한계로 남음.
*/
int CRawLogWorker::ExpireTtlSessions(int nThreadId, int nTtlSec, PGconn *pcConn)
{
	if (nTtlSec <= 0)
		return 0;

	if (nThreadId < 0 || nThreadId >= static_cast<int>(m_vtTripSessions.size()))
		return 0;

	unordered_map<string, VEHICLE_TRIP_SESSION>& mapSessions =
		m_vtTripSessions[static_cast<size_t>(nThreadId)];

	const time_t dtNow = time(nullptr);
	int nRemoved = 0;
	vector<CHARGE_INSERT_ROW> vtExpiredParkingCharges;
	vector<TRIP_END_UPDATE_ROW> vtAbnormalEndUpdates;
	vector<RAW_LOG_UPDATE_ROW> vtExpiredPendingUpdates;			// 세션 소멸 전 보류(pending) 행 확정분 (2026-08-21 최정우 추가)

	for (unordered_map<string, VEHICLE_TRIP_SESSION>::iterator it=mapSessions.begin();
			it != mapSessions.end(); )
	{
		if (it->second.dtLastSeen > 0
			&& (dtNow - it->second.dtLastSeen) > static_cast<time_t>(nTtlSec))
		{
			LOGFMTD("[#%02d] session ttl expired!trip_id=[%s] last_seen=[%ld] ttl=[%d]",
				nThreadId, it->first.c_str(),
				static_cast<long>(it->second.dtLastSeen), nTtlSec);

			// 세션이 곧 지워지므로, 아직 보류(pending) 중인 1틱 지연 행이 있으면 먼저 확정
			//   (commit)한다 — 더 이상 "다음" GPS 가 안 올 것이므로 보정판단 없이 계산된 값 그대로.
			//   아래 bWasParking 등 스냅샷보다 먼저 실행해야 보류 행의 과금 반영이 상태에 반영됨
			//   (2026-08-21 최정우 추가)
			CommitPendingRow(nThreadId, &it->second, false, 0, &vtExpiredPendingUpdates, &vtExpiredParkingCharges);

			// 한 trip 에서 여러 세션(예: 주정차 폴리곤·면제도로/일반도로/폐쇄형/구간단속 LINE)이
			//   좌표상 겹쳐 동시에 TTL 만료될 수 있음(실측으로 확인됨, 2026-08-13) — Append*들이
			//   같은 nChargeSeq(trip_seq)를 그대로 쓰면 PK(trip_id,device_key,trip_seq) 충돌로
			//   뒤 INSERT가 ON CONFLICT DO NOTHING 에 조용히 유실됨. 실제 INSERT 성공한 것만
			//   골라 seq 를 증가시켜야 하므로 호출 전후로 bIn*RoadZone 상태 변화를 확인 — 매번
			//   "호출 전 스냅샷 → 호출 → 증가" 패턴을 반복 (2026-08-14 최정우 수정 — 폐쇄형·구간단속
			//   TTL-flush 함수 신규 추가로 5개가 됨)
			bool bWasParking = !it->second.vtParkRuns.empty();
			AppendExpiredParkingCharge(nThreadId, it->first, it->second, &vtExpiredParkingCharges);
			if (bWasParking)
				it->second.nChargeSeq += 1;

			bool bWasExempt = !it->second.vtExemptRuns.empty();
			AppendExpiredExemptZoneCharge(nThreadId, it->first, it->second, &vtExpiredParkingCharges);
			if (bWasExempt)
				it->second.nChargeSeq += 1;

			bool bWasNodeStep = !it->second.vtNodeStepRuns.empty();
			AppendExpiredNodeStepCharge(nThreadId, it->first, it->second, &vtExpiredParkingCharges);
			if (bWasNodeStep)
				it->second.nChargeSeq += 1;

			bool bWasOpen = !it->second.vtOpenRuns.empty();
			AppendExpiredOpenGateCharge(nThreadId, it->first, it->second, &vtExpiredParkingCharges);
			if (bWasOpen)
				it->second.nChargeSeq += 1;

			bool bWasClosedRoad = it->second.bInClosedRoad;
			AppendExpiredClosedRoadCharge(nThreadId, it->first, it->second, it->second.dtLastSeen, &vtExpiredParkingCharges);
			if (bWasClosedRoad)
				it->second.nChargeSeq += 1;

			bool bWasSpeedZone = it->second.bInSpeedZone;
			AppendExpiredSpeedZoneCharge(nThreadId, it->first, it->second, it->second.dtLastSeen, &vtExpiredParkingCharges);
			if (bWasSpeedZone)
				it->second.nChargeSeq += 1;

			// 미확정 레코드 마감(4유형 공용) — trip_id 하나당 1행이면 충분(WHERE 절이
			//   TRIP_END_DT IS NULL 로 알아서 대상만 걸러줌, 없으면 0건 영향으로 조용히 끝남)
			//   (2026-08-13 최정우 추가, 2026-08-13 수정 — 개방형 한정 해제)
			if (it->second.szTripId[0] != '\0')
			{
				TRIP_END_UPDATE_ROW stAbnormalRow;
				stAbnormalRow.strTripId = it->second.szTripId;
				stAbnormalRow.strTripEndDt = FormatDateTime14(it->second.dtLastSeen);
				stAbnormalRow.strUpdDt = FormatDateTime14(dtNow);
				vtAbnormalEndUpdates.push_back(stAbnormalRow);
			}

			mapSessions.erase(it++);
			++nRemoved;
		}
		else
		{
			// park_ttl — dtLastSeen(세션 전체)은 아직 신선해도(예: raw_vld=false GPS가 계속
			//   들어옴), 열려있는 주정차 세션의 마지막 신뢰(raw_vld=true) 확인으로부터 park_ttl
			//   초가 지났으면 좌표 확인 없이 그 시점 기준으로 강제 마감한다. 디바이스 세션 자체는
			//   지우지 않음 — GPS는 계속 들어오고 있어 다른 유형 세션·연속 매칭 컨텍스트는 그대로
			//   유지해야 함(위 TTL 만료 분기와 다른 점) (2026-08-19 최정우 추가)
			// 구역별 park_ttl 판정은 함수 안에서 한다(겹쳐 열린 구역마다 만료 시점이 다름)
			if (!it->second.vtParkRuns.empty())
				AppendStaleParkingCharge(nThreadId, it->first, &it->second, &vtExpiredParkingCharges);
			++it;
		}
	}

	if (!vtExpiredParkingCharges.empty() && (pcConn != nullptr))
		BulkInsertCharges(pcConn, vtExpiredParkingCharges);

	if (!vtAbnormalEndUpdates.empty() && (pcConn != nullptr))
		UpdateAbnormalTripEnd(pcConn, vtAbnormalEndUpdates);

	// 세션 소멸 전 확정된 보류(pending) 행의 rawgps_update 반영 (2026-08-21 최정우 추가)
	if (!vtExpiredPendingUpdates.empty() && (pcConn != nullptr))
		BulkUpdateRawLogs(pcConn, vtExpiredPendingUpdates);

	if (nRemoved > 0)
	{
		LOGFMTD("[#%02d] session ttl expired removed!count=[%d] ttl_sec=[%d]",
			nThreadId, nRemoved, nTtlSec);
	}

	return nRemoved;
}

/**
 * @brief TTL 만료 세션 중 아직 열려있는 주정차 세션을 위반 1건으로 마감 (2026-08-13 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 위반 확정 시 CHARGE_INSERT_ROW 1건 추가
 * @remark stRawLogInfo(현재 GPS 틱)가 없는 컨텍스트라 ProcessParkingCharge() 종료 블록과 로직은
 *   같지만 필드 출처가 다름(트립종료시각 대신 dtLastSeen, occur_dt는 세션의 dtParkEntryTime 그대로).
 *   실제 TRIP_EVENT=2 를 받은 적이 없어 [trip_end] UPDATE 가 이 trip_id 를 앞으로도 채워줄 일이
 *   없으므로, trip_end_dt 는 여기서 직접 dtLastSeen(마지막 확인 시각)으로 채운다. 확정 데이터가
 *   아님을 표시하기 위해 charge_yn='N'/charge_status='4'(SKIP) 명시 — 정상 이탈/트립종료 경로의
 *   'Y'/'0' 과 다름(사용자 지시, 2026-08-13).
*/
time_t CRawLogWorker::InterpolateZoneCrossingTime(PZONE_INFO pstZone,
		double dfInX, double dfInY, time_t dtIn, double dfOutX, double dfOutY, time_t dtOut)
{
	if (pstZone == nullptr) return dtIn;
	if (!CChargeDataLoader::IsPointInPolygon(dfInX, dfInY, pstZone->vtCoords)) return dtIn;
	if (CChargeDataLoader::IsPointInPolygon(dfOutX, dfOutY, pstZone->vtCoords)) return dtIn;

	double dfInDist = CChargeDataLoader::DistanceToPolygonBoundaryMeters(dfInX, dfInY, pstZone->vtCoords);
	double dfOutDist = CChargeDataLoader::DistanceToPolygonBoundaryMeters(dfOutX, dfOutY, pstZone->vtCoords);
	double dfTotal = dfInDist + dfOutDist;
	if (dfTotal <= 0.0) return dtIn;

	double dfFrac = dfInDist / dfTotal;					// In 이 경계에서 먼 비율만큼 Out 쪽으로 이동
	double dfDeltaSec = difftime(dtOut, dtIn) * dfFrac;
	return dtIn + static_cast<time_t>(dfDeltaSec + (dfDeltaSec >= 0.0 ? 0.5 : -0.5));
}

/**
 * @brief 폐쇄형/구간단속 게이트 통과 시각 보간 — 직전 확정 tick~현재 tick 사이 등속 직선 이동을
 *   가정, 게이트까지의 직선거리 비율로 그 구간 안 실제 통과 시각을 추정한다 (2026-08-25 최정우 추가)
 * @remark InterpolateZoneCrossingTime() 과 원리는 같으나(비율 보간) 그쪽은 폴리곤 경계까지의
 *   거리(주정차 전용), 이쪽은 게이트라는 "점"까지의 직선거리(폐쇄형/구간단속 전용) 기준이라
 *   구분함. dfPrevX/Y~dfCurX/Y 거리가 0이면(같은 좌표 재수신 등) 보간 근거가 없어 dtCur 을 그대로
 *   반환한다. 비율은 0~1 로 클램프 — 게이트가 두 tick 의 이동 경로 밖(직선상 연장선)에 있는 것처럼
 *   계산되는 경우(맵매칭 곡선경로 vs 직선거리 오차)에도 dtPrev/dtCur 범위를 벗어나지 않게 한다.
 */
time_t CRawLogWorker::InterpolateGateCrossingTime(double dfPrevX, double dfPrevY, time_t dtPrev,
		double dfCurX, double dfCurY, time_t dtCur, double dfGateX, double dfGateY)
{
	POINT stPrev, stCur, stGate;
	stPrev.dfX = dfPrevX;  stPrev.dfY = dfPrevY;
	stCur.dfX = dfCurX;    stCur.dfY = dfCurY;
	stGate.dfX = dfGateX;  stGate.dfY = dfGateY;

	double dfPrevToCur = HaversineMeters(stPrev, stCur);
	if (dfPrevToCur <= 0.0) return dtCur;

	double dfPrevToGate = HaversineMeters(stPrev, stGate);
	double dfFrac = dfPrevToGate / dfPrevToCur;
	if (dfFrac < 0.0) dfFrac = 0.0;
	if (dfFrac > 1.0) dfFrac = 1.0;

	double dfDeltaSec = difftime(dtCur, dtPrev) * dfFrac;
	return dtPrev + static_cast<time_t>(dfDeltaSec + (dfDeltaSec >= 0.0 ? 0.5 : -0.5));
}

bool CRawLogWorker::BuildParkRow(const PARK_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, double dfEndX, double dfEndY,
		const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);
	double dfDwellSec = difftime(dtEnd, stRun.dtEntryTime);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "4";							// PARKING
	pstRow->strChargeUnit = "2";							// POLYGON
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", dfEndY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", dfEndX);
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stRun.dfAccumDistM + 0.5));
	pstRow->strDistM = szDistM;

	if (dfDwellSec > 0.0)
	{
		double dfAvgSpeedKmh = (stRun.dfAccumDistM / dfDwellSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	// OCCUR_DT — 주정차는 "진입 시각"(RawLogWorker 관례). 일반도로만 진출 시각을 쓴다
	pstRow->strOccurDt = FormatDateTime14(stRun.dtEntryTime);
	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	pstRow->strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : pstRow->strOccurDt;

	pstRow->strTollgateId = "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strRegDt;
	pstRow->strChargeYn = pszChargeYn;
	pstRow->strChargeStatus = pszChargeStatus;

	// base_parking_fine 최소 from_min(분) 미만 체류는 등록 대상에서 제외 — min_sec=0 이면
	//   임계 비활성(테이블 비어있거나 미로드), 항상 등록 (사용자 지시, 2026-08-24 최정우 추가)
	int nMinSec = m_stConfig.pcChargeDataLoader->GetParkFineMinSec();
	return (nMinSec <= 0) || (dfDwellSec >= static_cast<double>(nMinSec));
}

void CRawLogWorker::AppendExpiredParkingCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (stSession.vtParkRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 겹쳐 진행 중이던 구역을 전부 마감한다. 체류 종료는 dtLastSeen — TTL 만료는 그 이후로 GPS가
	//   안 왔다는 뜻이라 "그 시점까지는 확실히 있었다"만 서버가 아는 전부 (2026-08-23 최정우 수정)
	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtParkRuns.size(); ++i)
	{
		const PARK_RUN_SESSION& stRun = stSession.vtParkRuns[i];
		CHARGE_INSERT_ROW stRow;
		bool bMeetsFineMin = BuildParkRow(stRun, stSession.szTripId, strDeviceKey, nSeq, stSession.dtLastSeen,
			stRun.dfLastX, stRun.dfLastY, "N", "3", &stRow);
		if (bMeetsFineMin) pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTI("[#%02d] parking expired!device=[%s] trip_id=[%s] road=[%s] dwell=[%s]s registered=[%d]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, stRun.szRoadID,
			stRow.strStaySeconds.c_str(), static_cast<int>(bMeetsFineMin));
	}
}

/**
 * @brief park_ttl — 열려있는 주정차 세션의 마지막 신뢰(raw_vld=true) 확인 이후 일정 시간이
 *   지나면 좌표 확인 없이 그 시점 기준으로 강제 마감 (2026-08-19 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in,out] pstSession 대상 세션 — 마감 후 park_* 필드만 초기화(디바이스 세션 자체는 유지)
 * @param[out] pvtOut CHARGE_INSERT_ROW 1건 추가
 * @remark AppendExpiredParkingCharge() 와 다른 점: 그건 디바이스 세션 전체가 TTL로 사라질 때
 *   (GPS 자체가 끊김) 쓰는 경로라 trip_end_dt 를 직접 채우고 세션을 지운다. 이 함수는 GPS는
 *   계속 들어오는데(raw_vld=false 라도) 신뢰 가능한 위치 확인만 오래 끊긴 경우라 트립이 계속
 *   진행 중일 수 있음 — trip_end_dt 는 비워두고([trip_end] UPDATE 가 나중에 채움), 세션도
 *   지우지 않고 park_* 필드만 리셋해 다음 구역 진입을 정상적으로 받을 수 있게 한다. 체류시간·
 *   종료위치는 ProcessParkingCharge() 의 raw_vld=false 마감 경로와 동일하게 마지막 신뢰
 *   시각·좌표(dtParkLastConfirmedTime/dfParkLastConfirmedX/Y) 기준 — 그래서 charge_yn/status 도
 *   그 경로와 동일하게 정상값('Y'/'0')을 쓴다(AppendExpiredParkingCharge 의 'N'/'3' 과 다름 —
 *   거긴 트립 자체가 유실된 상황이라 심사대상으로 표시하지만, 여긴 마지막 신뢰 시점까지는
 *   확실한 근거가 있는 정상 기록이기 때문).
*/
void CRawLogWorker::AppendStaleParkingCharge(int nThreadId, const string& strDeviceKey,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if ((pstSession == nullptr) || pstSession->vtParkRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// park_ttl 초과 — 마지막 신뢰(raw_vld=true) 확인 시점까지만 체류로 인정하고 강제 마감한다.
	//   디바이스 세션 자체는 지우지 않는다(GPS 는 계속 들어오고 다른 유형 세션은 유효).
	//   정상 마감 경로와 동일하게 Y/0 로 기록 (2026-08-23 최정우 수정 — 구역별로 처리)
	const time_t dtNow = time(nullptr);
	for (size_t i = 0; i < pstSession->vtParkRuns.size(); )
	{
		PARK_RUN_SESSION& stRun = pstSession->vtParkRuns[i];
		if ((stRun.dtLastConfirmedTime <= 0) || (m_stConfig.nParkTtlSec <= 0)
			|| ((dtNow - stRun.dtLastConfirmedTime) <= static_cast<time_t>(m_stConfig.nParkTtlSec)))
		{ ++i; continue; }

		CHARGE_INSERT_ROW stRow;
		bool bMeetsFineMin = BuildParkRow(stRun, pstSession->szTripId, strDeviceKey, pstSession->nChargeSeq,
			stRun.dtLastConfirmedTime, stRun.dfLastConfirmedX, stRun.dfLastConfirmedY,
			"Y", "0", &stRow);
		if (bMeetsFineMin) pvtOut->push_back(stRow);

		LOGFMTI("[#%02d] parking stale finalized!device=[%s] trip_id=[%s] road=[%s] dwell=[%s]s registered=[%d]",
			nThreadId, strDeviceKey.c_str(), pstSession->szTripId, stRun.szRoadID,
			stRow.strStaySeconds.c_str(), static_cast<int>(bMeetsFineMin));

		pstSession->nChargeSeq += 1;
		pstSession->vtParkRuns.erase(pstSession->vtParkRuns.begin() + i);
	}
}

/**
 * @brief TTL 만료 세션 중 아직 열려있는 면제도로 세션을 N/4 로 마감 (2026-08-13 최초 추가,
 *   2026-08-14 세 차례 재설계 — "모든 미등록 링크" 방식 폐기하고 zone 기반 판정으로 복귀,
 *   charge_type/from_id/to_id도 한때 "0"+링크ID로 바꿨다가 사용자 재지시로 "5"+zone road_id로 원복)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 세션이 진행 중이었으면 CHARGE_INSERT_ROW 1건 추가
 * @remark 정상 이탈·트립종료는 ProcessExemptZoneCharge() 가 이미 그 자리에서 N/4 로 기록하므로,
 *   이 함수는 그 신호(구역 이탈/트립종료)가 영영 안 오고 GPS 자체가 끊긴 경우(TTL 만료)만
 *   대신 마감해주는 보완 경로. charge_status=4(SKIP) — 면제도로는 애초에 과금 대상이 아니므로
 *   AUDIT(3)이 아니라 SKIP(4)(사용자 지시 계승).
*/
void CRawLogWorker::BuildExemptRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "5";							// 비과금도로 고유 charge_type(사용자 재지시, 2026-08-14)
	pstRow->strChargeUnit = "1";							// LINE 유형 — 폐쇄형·구간단속과 동일하게 LINK
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;					// base_roadlink 등록 road_id
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", stRun.dfLastY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", stRun.dfLastX);
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stRun.dfAccumDistM + 0.5));
	pstRow->strDistM = szDistM;

	double dfElapsedSec = difftime(dtEnd, stRun.dtEntryTime);
	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (stRun.dfAccumDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	pstRow->strOccurDt = FormatDateTime14(dtEnd);
	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	pstRow->strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : pstRow->strOccurDt;

	pstRow->strTollgateId = "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strOccurDt;
	pstRow->strChargeYn = "N";								// 면제도로는 항상 N/4
	pstRow->strChargeStatus = "4";
}

void CRawLogWorker::AppendExpiredExemptZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (stSession.vtExemptRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtExemptRuns.size(); ++i)
	{
		CHARGE_INSERT_ROW stRow;
		BuildExemptRow(stSession.vtExemptRuns[i], stSession.szTripId, strDeviceKey,
			nSeq, stSession.dtLastSeen, &stRow);
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTI("[#%02d] exempt zone expired!device=[%s] trip_id=[%s] road=[%s] dist_m=[%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId,
			stSession.vtExemptRuns[i].szRoadID, stRow.strDistM.c_str());
	}
}

/**
 * @brief TTL 만료 세션 중 아직 열려있는 일반도로(ROAD_KIND=0) 세션을 N/3(AUDIT) 로 마감 (2026-08-14 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 세션이 진행 중이었으면 CHARGE_INSERT_ROW 1건 추가
 * @remark 정상 이탈·트립종료는 ProcessNodeStepCharge() 가 이미 그 자리에서 Y/0 로 기록하므로,
 *   이 함수는 그 신호(링크 변경/트립종료)가 영영 안 오고 GPS 자체가 끊긴 경우(TTL 만료)만 대신
 *   마감해주는 보완 경로. 일반도로는 실제 과금 대상이라 폐쇄형/구간단속/주정차와 동일하게
 *   charge_status=3(AUDIT) — 비과금도로의 SKIP(4)과 다름(애초에 과금 대상이 아니었던 비과금도로와
 *   달리, 일반도로는 확정 못한 채 끝난 것뿐이라 "심사대상"이 맞음).
*/
void CRawLogWorker::BuildNodeStepRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "0";							// NODE_STEP(일반도로)
	pstRow->strChargeUnit = "0";							// NODE(사용자 지시, 2026-08-14 — 개방형과 동일 관례)
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", stRun.dfLastY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", stRun.dfLastX);
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stRun.dfAccumDistM + 0.5));
	pstRow->strDistM = szDistM;

	double dfElapsedSec = difftime(dtEnd, stRun.dtEntryTime);
	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (stRun.dfAccumDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";							// 일반도로는 구역 제한속도 개념 해당없음

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	// OCCUR_DT — 다른 4유형은 "진입 시각"이지만 일반도로만 사용자 지시로 "진출 시각"
	//   (2026-08-14) — 혼동해서 통일하지 말 것
	pstRow->strOccurDt = FormatDateTime14(dtEnd);

	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	if (pszTripStartDt != nullptr)
		pstRow->strTripStartDt = pszTripStartDt;
	else
		pstRow->strTripStartDt = pstRow->strOccurDt;

	pstRow->strTollgateId = "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strOccurDt;
	pstRow->strChargeYn = "Y";								// 실제 과금 대상(사용자 지시, 2026-08-14)
	pstRow->strChargeStatus = "0";
}

void CRawLogWorker::AppendExpiredNodeStepCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (stSession.vtNodeStepRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 겹쳐 진행 중이던 구역이 여럿일 수 있어 전부 마감한다 (2026-08-23 최정우 수정)
	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtNodeStepRuns.size(); ++i)
	{
		CHARGE_INSERT_ROW stRow;
		BuildNodeStepRow(stSession.vtNodeStepRuns[i], stSession.szTripId, strDeviceKey,
			nSeq, stSession.dtLastSeen, &stRow);
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTI("[#%02d] node step expired!device=[%s] trip_id=[%s] road=[%s] dist_m=[%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId,
			stSession.vtNodeStepRuns[i].szRoadID, stRow.strDistM.c_str());
	}
}

/**
 * @brief 개방형 과금 1행 생성 — 정상 이탈과 TTL 만료 공용 (2026-08-25 최정우 추가)
 * @remark dist_m·from/to 좌표·charge_yn/status 산출 방식이 stRun.bStartedByTrip 으로 갈린다:
 *   - false(구역 밖에서 정상 진입) — dist_m=구역 전체길이(ZONE_INFO.dfLengthM), from/to 좌표=
 *     구역 자체 등록 시작/끝점(dfFirstLat/Lon·dfLastLat/Lon, SPEED/CLOSED와 동일 관례),
 *     charge_yn/status=Y/0 고정(개방형 구역을 물리적으로 통과하려면 게이트를 지날 수밖에 없음)
 *   - true(트립이 이 구역 도로 위에서 시작) — dist_m=stRun.dfAccumDistM(출발~이탈 실관측 거리),
 *     from/to 좌표=실제 진입/이탈 매칭 좌표(NODE_STEP과 동일 관례), charge_yn/status 는
 *     stRun.bGateCrossed(이 run 동안 게이트를 실제로 지났는가)로 결정 — Y/0(지남) vs N/3(못 지남,
 *     이미 게이트를 지난 뒤 시작했다는 뜻)
*/
void CRawLogWorker::BuildOpenZoneRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, CHARGE_INSERT_ROW *pstRow)
{
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);

	pstRow->strTripId = strTripId;
	pstRow->strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", nChargeSeq);
	pstRow->strChargeSeq = szSeq;

	pstRow->strChargeType = "1";							// OPEN_ROAD
	pstRow->strChargeUnit = "0";							// NODE(실측 관례 유지)
	pstRow->strLinkId = "";
	pstRow->strFromId = stRun.szRoadID;					// 2026-08-25 변경 — 게이트ID→구역road_id(NODE_STEP과 동일 관례)
	pstRow->strToId = stRun.szRoadID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	double dfDistM;
	if (stRun.bStartedByTrip)
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stRun.dfEntryY);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stRun.dfEntryX);
		snprintf(szToLat, sizeof(szToLat), "%.06lf", stRun.dfLastY);
		snprintf(szToLon, sizeof(szToLon), "%.06lf", stRun.dfLastX);
		dfDistM = stRun.dfAccumDistM;
	}
	else if (pstZone != nullptr)
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZone->dfFirstLat);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZone->dfFirstLon);
		snprintf(szToLat, sizeof(szToLat), "%.06lf", pstZone->dfLastLat);
		snprintf(szToLon, sizeof(szToLon), "%.06lf", pstZone->dfLastLon);
		dfDistM = pstZone->dfLengthM;
	}
	else
	{
		szFromLat[0] = szFromLon[0] = szToLat[0] = szToLon[0] = '\0';
		dfDistM = 0.0;
	}
	pstRow->strFromLat = szFromLat;
	pstRow->strFromLon = szFromLon;
	pstRow->strToLat = szToLat;
	pstRow->strToLon = szToLon;

	pstRow->strZoneId = stRun.szRoadID;
	pstRow->strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfDistM + 0.5));
	pstRow->strDistM = szDistM;

	double dfElapsedSec = difftime(dtEnd, stRun.dtEntryTime);
	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (dfDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		pstRow->strSpeedKmh = szSpeedKmh;
	}
	pstRow->strSpeedLimitKmh = "";							// 개방형은 구역 제한속도 개념 해당없음(기존 관례 유지)

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	pstRow->strStaySeconds = szStaySeconds;

	pstRow->strOccurDt = FormatDateTime14(stRun.dtEntryTime);	// 진입 시각(다른 유형과 동일 관례)

	const char *pszTripStartDt = ExtractTripStartDt(strTripId.c_str());
	pstRow->strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : pstRow->strOccurDt;

	// tollgate_id — 이 run 동안 실제로 지난 게이트 ID(못 지났으면 빈 값)
	PGATE_INFO pstGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(stRun.szRoadID, 'M');
	pstRow->strTollgateId = (stRun.bGateCrossed && (pstGate != nullptr)) ? pstGate->szTollgateID : "";
	pstRow->strEntryTollgateId = "";
	pstRow->strExitTollgateId = "";
	pstRow->strRegDt = FormatDateTime14(time(nullptr));
	pstRow->strUpdDt = pstRow->strRegDt;

	// charge_yn/status — 정상진입 run 은 Y/0 고정, 트립시작 run 은 게이트 통과 여부로 결정
	//   (사용자 지시, 2026-08-25)
	if (!stRun.bStartedByTrip || stRun.bGateCrossed)
	{
		pstRow->strChargeYn = "Y";
		pstRow->strChargeStatus = "0";
	}
	else
	{
		pstRow->strChargeYn = "N";
		pstRow->strChargeStatus = "3";
	}
}

/**
 * @brief TTL 만료로 세션이 지워지기 직전, 아직 열려있는 개방형 run 이면 그 상태 그대로 1건 기록 —
 *   [trip_abend] UPDATE(query.sql)가 뒤이어 TRIP_END_DT IS NULL 인 이 행을 찾아 N/3(AUDIT)로
 *   정정한다(다른 유형과 동일한 2단계 처리, AppendExpiredNodeStepCharge와 동일 패턴) (2026-08-25 최정우 추가)
*/
void CRawLogWorker::AppendExpiredOpenGateCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (stSession.vtOpenRuns.empty() || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	int nSeq = stSession.nChargeSeq;
	for (size_t i = 0; i < stSession.vtOpenRuns.size(); ++i)
	{
		CHARGE_INSERT_ROW stRow;
		BuildOpenZoneRow(stSession.vtOpenRuns[i], stSession.szTripId, strDeviceKey,
			nSeq, stSession.dtLastSeen, &stRow);
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTI("[#%02d] open zone expired!device=[%s] trip_id=[%s] road=[%s] dist_m=[%s]",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId,
			stSession.vtOpenRuns[i].szRoadID, stRow.strDistM.c_str());
	}
}

/**
 * @brief TTL 만료 세션 중 입구만 통과하고 출구를 못 찾은 폐쇄형 세션을 N/3(AUDIT)로 마감
 *   (2026-08-14 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 세션이 입구 통과 상태였으면 CHARGE_INSERT_ROW 1건 추가
 * @remark 기존엔 이 함수 자체가 없어 "입구는 확인했지만 출구 신호가 영영 안 온" 세션이 TTL로
 *   조용히 사라지면 그 진입 사실 자체가 통째로 유실됐음(휴게소 장시간 정차 등). to_id/출구게이트는
 *   여전히 못 봤으니 지어내지 않고 비워두지만, dist_m/speed_kmh/to_lat·lon 은 2026-08-25부터
 *   dfClosedAccumDistM/dfClosedLastX·Y(ProcessClosedRoadCharge() 가 매 틱 갱신하는 실시간 누적거리·
 *   마지막 확인 위치)로 채운다 — 과거엔 이 세션 필드 자체가 없어 전부 비워뒀었음(최정우 수정).
*/
void CRawLogWorker::AppendExpiredClosedRoadCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (!stSession.bInClosedRoad || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stSession.szClosedRoadId);

	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stSession.szTripId;
	stRow.strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", stSession.nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "2";								// CLOSED_ROAD
	stRow.strChargeUnit = "1";								// LINK (실측 확인)
	stRow.strLinkId = "";

	stRow.strFromId = stSession.szEntryTollgateId;			// 입구는 확인됨
	stRow.strToId = "";										// 출구 미확인 — 지어내지 않음

	char szFromLat[32], szFromLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stSession.dfEntryFromLat);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stSession.dfEntryFromLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	// 출구 미확인이라도 실시간 누적거리·마지막 확인 위치는 이제 세션에 있음(2026-08-25 최정우
	//   추가 — ProcessClosedRoadCharge() 의 "구역 이탈" 보조판정과 같은 필드 재사용). dfClosedLastX/Y
	//   초기값이 진입 위치라 세션이 TTL로 지워질 때까지 한 틱도 못 받았어도 0m·진입 위치 그대로 나옴
	char szToLat[32], szToLon[32];
	snprintf(szToLat, sizeof(szToLat), "%.06lf", stSession.dfClosedLastY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", stSession.dfClosedLastX);
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = stSession.szClosedRoadId;
	stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stSession.dfClosedAccumDistM + 0.5));
	stRow.strDistM = szDistM;

	double dfDwellSec = difftime(dtEndTime, stSession.dtEntryTime);
	if (dfDwellSec > 0.0)
	{
		double dfAvgSpeedKmh = (stSession.dfClosedAccumDistM / dfDwellSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}
	stRow.strSpeedLimitKmh = "";							// 출구 링크를 특정 못 함 — 지어내지 않음

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	stRow.strOccurDt = FormatDateTime14(stSession.dtEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stSession.szTripId);
	if (pszTripStartDt != nullptr)
		stRow.strTripStartDt = pszTripStartDt;
	else
		stRow.strTripStartDt = stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = stSession.szEntryTollgateId;
	stRow.strExitTollgateId = "";

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	stRow.strTripEndDt = FormatDateTime14(dtEndTime);
	stRow.strChargeYn = "N";
	stRow.strChargeStatus = "3";							// AUDIT — 다른 TTL-flush 유형과 동일 관례

	pvtOut->push_back(stRow);

	LOGFMTI("[#%02d] closed road recorded (ttl expiry)!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
		"entry=[%s] exit=[unknown]",
		nThreadId, strDeviceKey.c_str(), stSession.szTripId, stSession.nChargeSeq,
		stSession.szClosedRoadId, stSession.szEntryTollgateId);
}

/**
 * @brief TTL 만료 세션 중 입구만 통과하고 출구를 못 찾은 구간단속 세션을 N/3(AUDIT)로 마감
 *   (2026-08-14 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 세션이 입구 통과 상태였으면 CHARGE_INSERT_ROW 1건 추가
 * @remark to_id(출구게이트)는 여전히 못 봤으니 비워두지만, dist_m/speed_kmh/to_lat·lon 은
 *   2026-08-25부터 dfSpeedAccumDistM/dfSpeedLastX·Y(ProcessSpeedZoneCharge() 가 매 틱 갱신하는
 *   실시간 누적거리·마지막 확인 위치)로 채운다 — ProcessClosedRoadCharge() 동일 근거.
 *   from_lat/lon은 실측 관례상 구역 등록 좌표(ZONE_INFO.dfFirstLat/Lon) 그대로 유지.
*/
void CRawLogWorker::AppendExpiredSpeedZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut)
{
	if (!stSession.bInSpeedZone || (pvtOut == nullptr)
		|| (m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stSession.szSpeedZoneRoadId);

	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stSession.szTripId;
	stRow.strDeviceKey = strDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", stSession.nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "3";								// SPEED
	stRow.strChargeUnit = "1";								// LINK (실측 확인)
	stRow.strLinkId = "";

	stRow.strFromId = stSession.szSpeedEntryTollgateId;		// 입구 게이트ID (2026-08-20 최정우 수정)
	stRow.strToId = "";										// 출구 미확인 — 지어내지 않음

	char szFromLat[32], szFromLon[32];
	if (pstZone != nullptr)
	{
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZone->dfFirstLat);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZone->dfFirstLon);
		stRow.strFromLat = szFromLat;
		stRow.strFromLon = szFromLon;
	}
	else
	{
		// 구역 캐시 유실 등으로 좌표를 못 구한 경우 — 아는 값이 없으므로 NULL(2026-08-20부터 허용)
		stRow.strFromLat = "";
		stRow.strFromLon = "";
	}
	char szToLat[32], szToLon[32];
	snprintf(szToLat, sizeof(szToLat), "%.06lf", stSession.dfSpeedLastY);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", stSession.dfSpeedLastX);
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = stSession.szSpeedZoneRoadId;
	stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(stSession.dfSpeedAccumDistM + 0.5));
	stRow.strDistM = szDistM;

	double dfElapsedSec = difftime(dtEndTime, stSession.dtSpeedEntryTime);
	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (stSession.dfSpeedAccumDistM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}
	if (pstZone != nullptr)
	{
		char szSpeedLimit[8];
		snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZone->dfSpeedLimitKmh + 0.5));
		stRow.strSpeedLimitKmh = szSpeedLimit;
	}

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	stRow.strOccurDt = FormatDateTime14(stSession.dtSpeedEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stSession.szTripId);
	if (pszTripStartDt != nullptr)
		stRow.strTripStartDt = pszTripStartDt;
	else
		stRow.strTripStartDt = stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = stSession.szSpeedEntryTollgateId;	// 2026-08-20 최정우 수정
	stRow.strExitTollgateId = "";								// 출구 미확인 — NULLIF(...,'')로 DB엔 NULL

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	stRow.strTripEndDt = FormatDateTime14(dtEndTime);
	stRow.strChargeYn = "N";
	stRow.strChargeStatus = "3";							// AUDIT — 다른 TTL-flush 유형과 동일 관례

	pvtOut->push_back(stRow);

	LOGFMTI("[#%02d] speed zone recorded (ttl expiry)!device=[%s] trip_id=[%s] seq=[%d] road=[%s]",
		nThreadId, strDeviceKey.c_str(), stSession.szTripId, stSession.nChargeSeq,
		stSession.szSpeedZoneRoadId);
}

/**
 * @brief 예약된 batch 전건 PROCESSING→PENDING release (#7/#8)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtBatch 예약된 GPS batch
 * @param[in] nThreadId 로그용 워커 ID (-1 이면 생략)
 * @return true(전건 release), false(실패·인자 무효)
*/
bool CRawLogWorker::ReleaseReservedBatch(PGconn *pcConn, const RAW_LOG_BATCH& vtBatch, int nThreadId)
{
	if (pcConn == nullptr || vtBatch.empty() || m_stConfig.strUpdateSQL.empty())
		return false;

	vector<RAW_LOG_UPDATE_ROW> vtRelease;
	vtRelease.reserve(vtBatch.size());

	for (size_t i=0; i<vtBatch.size(); ++i)
	{
		// batch 1건씩 release 행 목록 적재 (2026-07-08 최정우 주석 추가)
		AppendReleaseRowFromRawLog(&vtRelease, vtBatch[i]);
	}

	if (vtRelease.empty())
		return false;

	// 예약 batch PROCESSING→PENDING bulk release (2026-07-08 최정우 주석 추가)
	if (!BulkReleaseRawLogs(pcConn, vtRelease))
	{
		if (nThreadId >= 0)
		{
			LOGFMTE("[#%02d] batch reserve release failed!device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
				static_cast<int>(vtRelease.size()));
		}
		else
		{
			LOGFMTE("batch reserve release failed!device=[%s] trip_id=[%s] count=[%d]",
				vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
				static_cast<int>(vtRelease.size()));
		}
		return false;
	}

	if (nThreadId >= 0)
	{
		LOGFMTW("[#%02d] batch reserve released!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
			nThreadId, vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
			static_cast<int>(vtRelease.size()));
	}
	else
	{
		LOGFMTW("batch reserve released!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
			vtBatch[0].szDeviceKey, vtBatch[0].szTripID,
			static_cast<int>(vtRelease.size()));
	}

	return true;
}

/**
 * @brief TRIP_EVENT 값 유효 여부 (0/1/2)
 * @param[in] nTripEvent TRIP_EVENT SMALLINT
 * @return true(유효), false(실패)
*/
bool CRawLogWorker::IsValidTripEvent(sint16 nTripEvent)
{
	return (nTripEvent == TRIP_EVENT_START)
		|| (nTripEvent == TRIP_EVENT_NONE)
		|| (nTripEvent == TRIP_EVENT_END);
}

/**
 * @brief TRIP_ID 가 CAR_SEQ_NO 기반 형식인지 검사
 * @param[in] stRawLogInfo 원시 GPS
 * @return true(유효), false(실패)
 * @remark 형식: {CAR_SEQ_NO 6자리 숫자}_{YYYYMMDDHH24MISS} (2026-08-18 최정우 수정 —
 *   DEVICE_KEY 접두사 검사에서 CAR_SEQ_NO 6자리 숫자 접두사 검사로 변경. 클라이언트가
 *   BASE_CARINFO.CAR_SEQ_NO 를 6자리로 0-패딩해 trip_id 를 생성하는 방식으로 확인됨에
 *   따라, 실제 발급 방식에 맞춰 형식 검증을 수정. 특정 device 의 CAR_SEQ_NO 와
 *   일치하는지까지는 대조하지 않는 단순 패턴 검사(숫자 6자리 + '_')다.)
*/
bool CRawLogWorker::IsValidTripIdForDevice(const sRawLogInfo& stRawLogInfo)
{
	if (stRawLogInfo.szDeviceKey[0] == '\0' || stRawLogInfo.szTripID[0] == '\0')
		return false;

	for (int i = 0; i < 6; ++i)
	{
		if (!isdigit(static_cast<unsigned char>(stRawLogInfo.szTripID[i])))
			return false;
	}

	return (stRawLogInfo.szTripID[6] == '_');
}

/**
 * @brief 수집 데이터 2차 검증 (위치검증서버 방어)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[out] pnRejectStatus 거부 시 MATCH_STATUS (SKIP)
 * @return true(맵매칭 진행 가능), false(거부)
*/
bool CRawLogWorker::ValidateRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		sint16 *pnRejectStatus)
{
	if (pnRejectStatus == nullptr)
		return false;

	*pnRejectStatus = MATCH_STATUS_SKIP;

	if (stRawLogInfo.szDeviceKey[0] == '\0')
	{
		LOGFMTW("[#%02d] reject empty device_key!seq=[%u]",
			nThreadId, stRawLogInfo.dwSeqNo);
		return false;
	}

	if (stRawLogInfo.szTripID[0] == '\0')
	{
		LOGFMTW("[#%02d] reject empty trip_id!device=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.dwSeqNo);
		return false;
	}

	// trip_id 가 CAR_SEQ_NO_{ts} 형식인지 검사 (2026-07-08 최정우 주석 추가, 2026-08-18 최정우 수정)
	if (!IsValidTripIdForDevice(stRawLogInfo))
	{
		LOGFMTW("[#%02d] reject invalid trip_id format!device=[%s] trip_id=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
		return false;
	}

	// TRIP_EVENT 0/1/2 유효값 검사 (2026-07-08 최정우 주석 추가)
	if (!IsValidTripEvent(stRawLogInfo.nTripEvent))
	{
		LOGFMTW("[#%02d] reject invalid trip_event=[%d]!device=[%s] trip_id=[%s] seq=[%u]",
			nThreadId, static_cast<int>(stRawLogInfo.nTripEvent),
			stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
		return false;
	}

	return true;
}

/**
 * @brief GPS 좌표·RAW_VLD 유효성 검사 (맵매칭 제외 대상)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @return true(SKIP=3 처리), false(맵매칭 진행 가능)
 * @remark
 *   - GPS_LAT 또는 GPS_LON 이 NULL
 *   - RAW_VLD 가 FALSE 또는 NULL
*/
bool CRawLogWorker::ShouldSkipGpsInput(int nThreadId, const sRawLogInfo& stRawLogInfo, bool bIgnoreRawVld)
{
	if ((stRawLogInfo.bGpsLatNull) || (stRawLogInfo.bGpsLonNull))
	{
		LOGFMTW("[#%02d] reject null gps coord!device=[%s] trip_id=[%s] seq=[%u] lat_null=[%d] lon_null=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<int>(stRawLogInfo.bGpsLatNull),
			static_cast<int>(stRawLogInfo.bGpsLonNull));
		return true;
	}

	// RAW_VLD=false 는 주정차 판정에도 태우지 않는다(2026-08-22 사용자 확정). 실측에서 이런 좌표의
	//   ACCURACY_M 이 78~362m(평균 149m)였는데, RL-Z00001 폴리곤은 면적 12,624m^2 로 대각선이 약 110m
	//   라 "폴리곤 안에 있었다"는 사실 자체를 신뢰할 수 없다. 그 결과 실제 정차 2건(178초·75초)이
	//   기록되지 않지만, 이건 이 검사가 도입된 시점부터의 동작이고 규칙 변경과 무관하다.
	//   완화하려면 폴리곤 크기 대비 ACCURACY_M 임계값을 구역별로 둬야 해서 관리비용이 크다고 판단.
	//   ignore_rawvld=1 이면 이 검사를 건너뛴다 — 운영 데이터 전량 매칭 검증용 (2026-08-23 최정우 추가)
	if (!bIgnoreRawVld && ((!stRawLogInfo.bRawVldKnown) || (!stRawLogInfo.bRawVld)))
	{
		LOGFMTW("[#%02d] reject invalid raw_vld!device=[%s] trip_id=[%s] seq=[%u] known=[%d] raw_vld=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<int>(stRawLogInfo.bRawVldKnown),
			static_cast<int>(stRawLogInfo.bRawVld));
		return true;
	}

	return false;
}

/**
 * @brief 이동거리 환산속도 vs SPEED_KMH 정합성 검사 (2026-07-20 최정우 추가)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stSession 현재 trip_id 세션 (직전 매칭 성공 위치·시각 기준)
 * @param[out] pnImpliedSpeedKmh 이동거리 환산속도(km/h) — 로그·참고용, nullable
 * @return true(SKIP 대상 — 이동거리가 SPEED_KMH 대비 비정상), false(정상 또는 판단 불가)
 * @remark
 *   전제: config speed_factor>0, 직전 매칭 성공 위치 보유(bHasLastMatch), 직전 포인트가 정상 매칭
 *         (bLastPointOk — 아니면 갭이 정상 1구간보다 넓어져 평균/순간 비교 신뢰 못함, 2026-07-21 최정우 추가),
 *         SPEED_KMH 유효(NULL 아님), 직전 매칭 시각과의 간격이 (0, MM_CALC_MAX_GAP_SEC] 이내
 *   판정: 환산속도(직전 매칭 위치→현재 GPS 하버사인 거리 / 시간간격) > SPEED_KMH × speed_factor + speed_margin
 *   예) SPEED_KMH=37, factor=2.0, margin=25 → 상한 99km/h. 환산속도 187km/h → SKIP
*/
bool CRawLogWorker::ShouldSkipImplausibleSpeed(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, int *pnImpliedSpeedKmh)
{
	if (pnImpliedSpeedKmh != nullptr)
		*pnImpliedSpeedKmh = -1;

	if (m_stConfig.dfSpeedFactor <= 0.0)
		return false;			// 비활성
	if (!stSession.bHasLastMatch)
		return false;			// 비교 기준(직전 매칭 위치) 없음 — START 등
	if (!stSession.bLastPointOk)
		return false;			// 직전 포인트가 매칭 실패 — 갭이 정상 1구간보다 넓어져 평균/순간 비교 신뢰 못함 (2026-07-21 최정우 추가)
	if (stRawLogInfo.fSpeed < 0.0f)
		return false;			// SPEED_KMH NULL — 비교 불가

	double dfGapSec = difftime(stRawLogInfo.dtGPS, stSession.dtLastMatchGps);
	if (dfGapSec <= 0.0 || dfGapSec > static_cast<double>(MM_CALC_MAX_GAP_SEC))
		return false;			// 공백·역전 구간 — 판단 불신

	POINT stPrev; stPrev.dfX = stSession.dfLastMatchX; stPrev.dfY = stSession.dfLastMatchY;
	POINT stCur;  stCur.dfX = stRawLogInfo.dfX;         stCur.dfY = stRawLogInfo.dfY;
	// 직전 매칭 위치→현재 GPS 하버사인 거리(m) (2026-07-20 최정우 추가)
	double dfMoveM = HaversineMeters(stPrev, stCur);
	double dfImpliedKmh = (dfMoveM / dfGapSec) * 3.6;

	if (pnImpliedSpeedKmh != nullptr)
		*pnImpliedSpeedKmh = static_cast<int>(dfImpliedKmh + 0.5);

	double dfLimitKmh = static_cast<double>(stRawLogInfo.fSpeed) * m_stConfig.dfSpeedFactor
		+ static_cast<double>(m_stConfig.nSpeedMargin);
	if (dfImpliedKmh <= dfLimitKmh)
		return false;

	LOGFMTW("[#%02d] reject implausible speed!device=[%s] trip_id=[%s] seq=[%u] "
		"move=[%.1fm] gap=[%.1fs] implied=[%.1fkm/h] reported=[%.1fkm/h] limit=[%.1fkm/h]",
		nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
		dfMoveM, dfGapSec, dfImpliedKmh, static_cast<double>(stRawLogInfo.fSpeed), dfLimitKmh);
	return true;
}

/**
 * @brief Begin 맵매칭(초기 맵매칭) 필요 여부 판단
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stSession 현재 trip_id 세션
 * @param[out] pbFullReset true 이면 START 에 의한 전체 세션 초기화
 * @param[out] pbSeqRollback true 이면 GPS_SEQ 역전(과거·중복 seq) — 반환값은 false 이고
 *   호출측(ProcessRawLog)이 세션을 건드리지 않은 채 그 행만 SKIP 한다 (2026-08-23 최정우 추가)
 * @remark
 *   - TRIP_EVENT=0(START) 또는 TRIP_ID 변경 → 시작(BEGIN 강등)
 *   - GPS_SEQ<=dwLastGpsSeq(역전·동일 seq 재처리)는 더 이상 BEGIN 강등이 아니다 — pbSeqRollback 로 알린다
*/
bool CRawLogWorker::NeedsBeginReset(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, bool *pbFullReset, bool *pbSeqRollback)
{
	if ((pbFullReset == nullptr) || (pbSeqRollback == nullptr))
		return false;

	*pbFullReset = false;
	*pbSeqRollback = false;

	if (stRawLogInfo.nTripEvent == TRIP_EVENT_START)
	{
		*pbFullReset = true;
		return true;
	}

	// TRIP_ID 변경 = 새 주행 (이전 trip END 누락으로 세션 잔류 또는 START 누락) → 전체 리셋(갱신) (2026-07-08 최정우 추가)
	if ((stSession.szTripId[0] != '\0') && 
		(strcmp(stSession.szTripId, stRawLogInfo.szTripID) != 0) && 
		(stRawLogInfo.szTripID[0] != '\0'))
	{
		LOGFMTW("[#%02d] trip_id changed (missing END/START)!device=[%s] old=[%s] new=[%s] seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stSession.szTripId,
			stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo);
		*pbFullReset = true;
		return true;
	}

	// GPS_SEQ 역전·중복 — 이미 지나온 시점의 행이다. 세션은 건드리지 않고 신호만 올린다.
	//   (2026-08-23 최정우 수정 — 이전엔 return true 로 BEGIN 강등했으나 그게 오매칭 원인이었다.
	//    아래 ProcessRawLog 의 bSeqRollback 처리 주석 참고)
	if ((stSession.dwLastGpsSeq > 0) && 
		(stRawLogInfo.dwSeqNo <= stSession.dwLastGpsSeq))
	{
		LOGFMTW("[#%02d] gps_seq rollback!device=[%s] trip_id=[%s] seq=[%u] last_seq=[%u] -> SKIP(세션 유지)",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			stRawLogInfo.dwSeqNo, stSession.dwLastGpsSeq);
		*pbSeqRollback = true;
		return false;
	}

	return false;
}

/**
 * @brief 연속 맵매칭 세션을 시작 상태로 초기화
 * @param[in,out] stSession trip_id 세션
 * @param[in] bFullReset true 이면 START 누락 경고 플래그도 초기화
 * @return void
*/
void CRawLogWorker::ResetTripSessionForBegin(VEHICLE_TRIP_SESSION& stSession, bool bFullReset)
{
	stSession.qwLinkID = 0;
	stSession.dwLastGpsSeq = 0;

	// 시작 전환 시 직전 매칭·고도 앵커 폐기 → 끊긴/역전 구간 오계산 방지 (2026-07-08 최정우 추가)
	stSession.dfLastMatchX = 0.0;
	stSession.dfLastMatchY = 0.0;
	stSession.dtLastMatchGps = 0;
	stSession.bHasLastMatch = false;
	stSession.nPrevAltitude = NO_ALTITUDE;
	stSession.nPrevRoadType = ROAD_TYPE_NORMAL;
	stSession.bHasPrevAlt = false;
	stSession.dfLastMatchLinkPos = 0.0;
	stSession.bHasPrevLinkPos = false;
	stSession.nReverseStreak = 0;
	stSession.bLastPointOk = true;

	if (bFullReset)
	{
		stSession.bStartWarned = false;
		// 실제 신규 trip 시작(START/trip_id 변경)일 때만 과금 트랙 리셋 — GPS_SEQ 역전 재처리(bFullReset=false)는
		//   같은 trip 이 계속되는 것이라 리셋하면 안 됨(이미 지난 게이트·구역 재부과 위험) (2026-08-12 최정우 추가)
		stSession.nChargeSeq = 1;
		stSession.dtLastGpsEventTime = 0;						// 신규 trip 은 이전 trip 최댓값을 이어받으면 안 됨 (2026-08-25 최정우 추가)

		// 폐쇄형 진입 상태도 동일하게 신규 trip 시작 시에만 리셋 (2026-08-12 최정우 추가)
		stSession.bInClosedRoad = false;
		stSession.szEntryTollgateId[0] = '\0';
		stSession.szClosedRoadId[0] = '\0';
		stSession.dfClosedAccumDistM = 0.0;						// (2026-08-25 최정우 추가)
		stSession.qwClosedLastZoneLinkID = 0;						// (2026-08-25 최정우 추가)
		stSession.dtClosedLastZoneTime = 0;						// (2026-08-25 최정우 추가)
		stSession.bClosedEntryAmbiguous = false;					// (2026-08-25 최정우 추가)

		// 구간단속 진입 상태도 동일하게 신규 trip 시작 시에만 리셋 (2026-08-12 최정우 추가)
		stSession.bInSpeedZone = false;
		stSession.szSpeedZoneRoadId[0] = '\0';
		stSession.szSpeedEntryTollgateId[0] = '\0';
		stSession.dfSpeedAccumDistM = 0.0;						// (2026-08-25 최정우 추가)
		stSession.qwSpeedLastZoneLinkID = 0;						// (2026-08-25 최정우 추가)
		stSession.dtSpeedLastZoneTime = 0;						// (2026-08-25 최정우 추가)
		stSession.bSpeedEntryAmbiguous = false;					// (2026-08-25 최정우 추가)

		// 면제도로·일반도로 진행 세션도 신규 trip 시작 시에만 리셋 (2026-08-23 최정우 수정 — 벡터화)
		stSession.vtExemptRuns.clear();
		stSession.vtNodeStepRuns.clear();
		stSession.vtOpenRuns.clear();							// (2026-08-25 최정우 추가)
	}
}

/**
 * @brief TRIP_EVENT=END 신뢰 여부 판정(스퓨리어스 순서역전 검사) (2026-08-25 최정우 추가)
 * @remark gps_dt 가 이 세션에서 지금까지 확인된 최대 gps_dt(dtLastGpsEventTime)보다 과거면
 *   신뢰하지 않는다 — 단말이 보낸 순서역전·중복 도착 이벤트로 추정(실측 근거·오판 시 안전장치는
 *   [[project_duplicate_trip_end_event_speed_charge]] 참고). ProcessRawLog() 의 "현재 틱" 판정과
 *   CommitPendingRow() 의 "보류 중이던 옛 틱을 나중에 flush 할 때" 판정에 공용으로 쓴다 — 후자를
 *   빠뜨리면, 스퓨리어스 END 로 판정돼 보류 상태로 남아있던 행이 나중에 다른 이유로 flush 될 때
 *   그 안에서 또 nTripEvent==END 를 그대로 믿어버려 원래 막으려던 버그가 재발한다(2026-08-25
 *   OPEN/EXEMPT/NODE_STEP/PARKING 에 이 판정을 배선하던 중 발견). dtLastGpsEventTime 갱신(상태
 *   변경)은 이 함수가 아니라 ProcessRawLog() 가 매 틱 1회만 수행 — 이 함수는 순수 판정만 한다.
*/
bool CRawLogWorker::IsTrustedTripEnd(const sRawLogInfo& stRawLogInfo, const VEHICLE_TRIP_SESSION& stSession)
{
	if (stRawLogInfo.nTripEvent != TRIP_EVENT_END)
		return false;
	if ((stSession.dtLastGpsEventTime > 0) && (stRawLogInfo.dtGPS < stSession.dtLastGpsEventTime))
		return false;
	return true;
}

/**
 * @brief ThreadPool Runnable – trip_id batch 1건 처리
 * @param[in] nThreadId 워커 스레드 ID (세션 맵 인덱스)
 * @param[in] context RAW_LOG_BATCH 포인터 (동일 trip_id GPS 묶음)
 * @return void
 * @remark
 *   - 세션은 배치 임시(stWorkSession)로 맵매칭 후 bulk 성공 시에만 m_vtTripSessions 에 커밋
 *   - #7: 조기 종료 시 ReleaseReservedBatch() 로 PROCESSING 해제
*/
void CRawLogWorker::run(int nThreadId, void *context)
{
	RAW_LOG_BATCH *pvtBatch = reinterpret_cast<RAW_LOG_BATCH *>(context);
	if ((pvtBatch == nullptr) || (pvtBatch->empty()))
		return;

	PGconn *pcConn = nullptr;

	if (m_stConfig.pcPostgrePool == nullptr)
	{
		LOGFMTE("[#%02d] db pool is null!batch orphan until recover!device=[%s] count=[%d]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, static_cast<int>(pvtBatch->size()));
		return;
	}

	if (m_stConfig.strUpdateSQL.empty())
	{
		LOGFMTE("[#%02d] update sql is empty!batch orphan until recover!device=[%s] count=[%d]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, static_cast<int>(pvtBatch->size()));
		return;
	}

	// batch 처리용 DB 커넥션 획득 (#E-1: [database] retrymax/wait 재시도) (2026-07-10 최정우 추가)
	pcConn = AcquirePoolConnection(m_stConfig.pcPostgrePool,
		m_stConfig.nConnRetryMax, m_stConfig.nConnRetryWait);
	if (pcConn == nullptr)
	{
		LOGFMTE("[#%02d] db connection is null after retry!batch orphan until recover!device=[%s] count=[%d]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, static_cast<int>(pvtBatch->size()));
		return;
	}

	if ((nThreadId < 0) || (nThreadId >= static_cast<int>(m_vtTripSessions.size())))
	{
		LOGFMTE("[#%02d] session index out of range!", nThreadId);
		// 세션 인덱스 오류 시 batch 예약 release (2026-07-08 최정우 주석 추가)
		ReleaseReservedBatch(pcConn, *pvtBatch, nThreadId);
		// DB 커넥션 반환 (2026-07-08 최정우 주석 추가)
		ReleaseConnection(pcConn);
		return;
	}

	LOGFMTD("[#%02d] batch start!device=[%s] trip_id=[%s] count=[%d]",
		nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
		static_cast<int>(pvtBatch->size()));

	vector<RAW_LOG_UPDATE_ROW> vtUpdates;
	vtUpdates.reserve(pvtBatch->size());
	vector<CHARGE_INSERT_ROW> vtChargeInserts;					// 개방형 게이트 통과 — 배치 종료 시 일괄 INSERT (2026-08-12 최정우 추가)
	vector<TRIP_END_UPDATE_ROW> vtTripEndUpdates;				// 트립 종료 — 배치 종료 시 trip_end_dt UPDATE (2026-08-12 최정우 추가)

	// bulk 성공 전까지 m_vtTripSessions 미갱신: 커밋된 세션을 복사해 배치 임시 세션으로 사용
	// 세션 맵 키 = DEVICE_KEY (2026-07-08 최정우 수정): device 당 1세션 → END/START 누락 고아 세션 누적 방지.
	//   신규 TRIP_ID 는 ProcessRawLog/NeedsBeginReset 의 trip_id 변경 감지로 세션을 리셋(갱신)한다.
	unordered_map<string, VEHICLE_TRIP_SESSION>& mapSessions =
		m_vtTripSessions[static_cast<size_t>(nThreadId)];
	const string strDeviceKey = (*pvtBatch)[0].szDeviceKey;

	VEHICLE_TRIP_SESSION stWorkSession;
	unordered_map<string, VEHICLE_TRIP_SESSION>::iterator itSession = mapSessions.find(strDeviceKey);
	if (itSession != mapSessions.end())
		stWorkSession = itSession->second;

	bool bTripEnded = false;
	bool bProcessOk = true;
	for (size_t i=0; i<pvtBatch->size(); ++i)
	{
		// GPS 1건 검증·맵매칭·UPDATE 행 적재 (2026-07-08 최정우 주석 추가)
		if (!ProcessRawLog(nThreadId, (*pvtBatch)[i], &vtUpdates, &vtChargeInserts, &vtTripEndUpdates, &stWorkSession, &bTripEnded))
			bProcessOk = false;
	}

	// vtUpdates 에 없는 예약 행 release (AppendUpdateRow 실패 등 #4 배치 내 orphan)
	vector<RAW_LOG_UPDATE_ROW> vtOrphanRelease;
	for (size_t i=0; i<pvtBatch->size(); ++i)
	{
		char szGpsSeq[16];
		snprintf(szGpsSeq, sizeof(szGpsSeq), "%u", (*pvtBatch)[i].dwSeqNo);

		if ((*pvtBatch)[i].szTripID[0] == '\0')
		{
			LOGFMTE("[#%02d] orphan release skipped!invalid trip_id device=[%s] seq=[%u]",
				nThreadId, (*pvtBatch)[i].szDeviceKey, (*pvtBatch)[i].dwSeqNo);
			bProcessOk = false;
			continue;
		}

		// 1틱 지연커밋으로 세션에 정당하게 보류(pending) 중인 행은 orphan 이 아님 — vtUpdates 에는
		//   아직 없지만(다음 배치에서 확정) 유실된 게 아니므로 release 대상에서 제외해야 한다.
		//   그렇지 않으면 이 행이 PENDING(0)으로 되돌아가 나중에 새 행처럼 재조회되면서, 메모리에
		//   남아있는 보류 버퍼와 겹쳐 같은 GPS 를 두 번 처리하는 버그가 생김 (2026-08-21 최정우 추가)
		const bool bIsSessionPending = stWorkSession.bHasPendingCommit
			&& (strcmp(stWorkSession.stPendingRawLogInfo.szTripID, (*pvtBatch)[i].szTripID) == 0)
			&& (stWorkSession.stPendingRawLogInfo.dwSeqNo == (*pvtBatch)[i].dwSeqNo);

		if (!IsRowInUpdates(vtUpdates, (*pvtBatch)[i].szTripID, szGpsSeq) && !bIsSessionPending)
		{
			// vtUpdates 미포함 orphan 행 release 목록 적재 (2026-07-08 최정우 주석 추가)
			AppendReleaseRowFromRawLog(&vtOrphanRelease, (*pvtBatch)[i]);
		}
	}

	if (!vtOrphanRelease.empty())
	{
		// orphan 예약 행 PROCESSING→PENDING release (2026-07-08 최정우 주석 추가)
		if (!BulkReleaseRawLogs(pcConn, vtOrphanRelease))
		{
			LOGFMTE("[#%02d] orphan release failed!device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
				static_cast<int>(vtOrphanRelease.size()));
			bProcessOk = false;
		}
		else
		{
			LOGFMTW("[#%02d] orphan released!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
				static_cast<int>(vtOrphanRelease.size()));
		}
	}

	// 1틱 지연커밋 도입으로 이번 배치의 모든 행이 보류(pending)됐다면 vtUpdates 가 비어있을 수
	//   있음 — 그 경우 bulk UPDATE 자체는 할 게 없어 자동 성공 취급하고, 아래 세션 커밋(보류
	//   버퍼 포함)은 그대로 진행해야 다음 배치에서 이어서 확정(commit)된다 (2026-08-21 최정우 추가)
	bool bUpdateOk = true;
	if (!vtUpdates.empty())
	{
		// reserve(rawgps_select) 의 짝: 완료는 rawgps_update(1/3/4), 실패 시 release(0) 동일 SQL
		// 맵매칭 결과 bulk UPDATE (2026-07-08 최정우 주석 추가)
		if (!BulkUpdateRawLogs(pcConn, vtUpdates))
		{
			LOGFMTE("[#%02d] bulk update failed!device=[%s] trip_id=[%s] count=[%d]",
				nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
				static_cast<int>(vtUpdates.size()));
			bProcessOk = false;
			bUpdateOk = false;

			// PROCESSING 좀비 방지: match_status=0, INTERSECT_LEN/MATCH_* '' → 기존 컬럼 유지
			// bulk update 실패 시 동일 PK release (2026-07-08 최정우 주석 추가)
			if (!BulkReleaseRawLogs(pcConn, vtUpdates))
			{
				LOGFMTE("[#%02d] bulk release failed!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtUpdates.size()));
			}
			else
			{
				LOGFMTW("[#%02d] bulk release ok!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtUpdates.size()));
			}
			// stWorkSession 폐기 — 커밋된 세션(mapSessions) 유지
		}
	}

	if (bUpdateOk)
	{
		// 과금 bulk INSERT(개방형·폐쇄형·구간단속·주정차·면제도로·일반도로 공용) — rawgps_update
		//   성공 후에만 시도. 실패 시 map-match bulk update 실패와 동일하게 취급(배치 release·
		//   세션 미커밋) → 다음 poll 에서 재처리되며 세션의 진행 중 트랙(vtOpenRuns 등)도 커밋되지
		//   않아 재통과 시 정상 재부과됨 (2026-08-12 최정우 추가)
		bool bChargeOk = true;
		if (!vtChargeInserts.empty())
		{
			bChargeOk = BulkInsertCharges(pcConn, vtChargeInserts);
			if (!bChargeOk)
			{
				LOGFMTE("[#%02d] charge bulk insert failed!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtChargeInserts.size()));
			}
		}

		if (!bChargeOk)
		{
			bProcessOk = false;
			if (!vtUpdates.empty() && !BulkReleaseRawLogs(pcConn, vtUpdates))
			{
				LOGFMTE("[#%02d] bulk release failed!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtUpdates.size()));
			}
			else if (!vtUpdates.empty())
			{
				LOGFMTW("[#%02d] bulk release ok!PROCESSING→PENDING device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtUpdates.size()));
			}
			// stWorkSession 폐기 — 커밋된 세션(mapSessions) 유지
		}
		else
		{
			// 트립 종료 trip_end_dt UPDATE — best-effort(실패해도 배치 자체는 성공 처리).
			//   과금 INSERT 와 달리 금액에 영향 없는 참고 컬럼이라 실패해도 배치를 release 하지 않음 (2026-08-12 최정우 추가)
			if (!vtTripEndUpdates.empty() && !UpdateTripEndDt(pcConn, vtTripEndUpdates))
			{
				LOGFMTE("[#%02d] trip_end update failed!device=[%s] trip_id=[%s] count=[%d]",
					nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID,
					static_cast<int>(vtTripEndUpdates.size()));
			}

			// DB 반영 성공 후에만 세션 커밋 (bulk 실패·release 시 연속 맵매칭 맥락 보존)
			// bTripEnded 이면 MATCHED/ERROR/SKIP 무관 trip_id 세션 제거
			if (bTripEnded)
				mapSessions.erase(strDeviceKey);					// (2026-07-08 최정우 수정) 키 = DEVICE_KEY
			else
				mapSessions[strDeviceKey] = stWorkSession;			// (2026-07-08 최정우 수정) 키 = DEVICE_KEY
		}
	}

	if (!bProcessOk)
	{
		LOGFMTE("[#%02d] batch process failed!device=[%s] trip_id=[%s]",
			nThreadId, (*pvtBatch)[0].szDeviceKey, (*pvtBatch)[0].szTripID);
	}

	// 자기 스레드 세션만 TTL 만료 정리 (락 불필요, 모니터 스레드 레이스 제거)
	// trip_id 세션 TTL 만료 제거 (2026-07-08 최정우 주석 추가)
	ExpireTtlSessions(nThreadId, m_stConfig.nTtlSec, pcConn);

	// batch 처리 후 DB 커넥션 반환 (2026-07-08 최정우 주석 추가)
	ReleaseConnection(pcConn);
}

/**
 * @brief 커넥션 반환 전 미완료 트랜잭션 ROLLBACK 가드 후 pool 반환 (#14)
 * @param[in] pcConn DB 커넥션
 * @return void
 * @remark 현재 워커는 autocommit 단일 문장이라 in-트랜잭션 상태가 되지 않지만,
 *         향후 명시적 BEGIN/COMMIT(예: #10 과금 INSERT 동시 커밋) 도입 대비 방어 가드.
 *         Fetcher::ReleaseConnection 과 동일 패턴.
*/
void CRawLogWorker::ReleaseConnection(PGconn *pcConn)
{
	if ((pcConn == nullptr) || (m_stConfig.pcPostgrePool == nullptr))
		return;

	PGTransactionStatusType nTxnStatus = PQtransactionStatus(pcConn);
	if ((nTxnStatus == PQTRANS_INTRANS) || (nTxnStatus == PQTRANS_INERROR))
	{
		// 미완료 트랜잭션 ROLLBACK (2026-07-08 최정우 주석 추가)
		PQexec(pcConn, "ROLLBACK");
	}

	// DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
	m_stConfig.pcPostgrePool->releaseConnection(pcConn);
}

/**
 * @brief Runnable 종료 콜백 (ThreadPool stop 시 호출)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] context 호출 컨텍스트 (미사용)
 * @return void
 * @remark #8: 진행 중 batch 는 run() 완료 시점까지 처리. 큐 잔여는 Server drain 이 release
*/
void CRawLogWorker::stop(int nThreadId, void *context)
{
	(void)nThreadId;
	(void)context;
}

/**
 * @brief 보류(pending) 중인 1틱 지연 행을 확정(commit) — 반대편 짝 링크 1틱 오매칭 보정 + 과금
 *   함수 호출 + rawgps_update 큐잉
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in,out] pstSession 배치 임시 세션 — bHasPendingCommit=false 로 소비
 * @param[in] bHasNextLinkID 보정판단용 "다음" 확정 링크 존재 여부(false=보정 시도 안 함)
 * @param[in] qwNextLinkID 보정판단용 "다음" 확정 링크 ID
 * @param[out] pvtUpdates rawgps_update bulk UPDATE 대상 행 목록
 * @param[out] pvtChargeInserts charge_insert bulk INSERT 대상 행 목록
 * @return void
 * @remark 보류 행이 없으면(bHasPendingCommit=false) 아무 것도 안 하고 반환.
 *   보정 조건: 보류 행이 bReverseSuspect(역행의심)이고, 보류 행의 링크가 마지막으로 신뢰
 *   커밋된 링크(qwLastConfirmedLinkID)와 다르며, "다음" 확정 링크가 다시 그 마지막 신뢰
 *   링크로 돌아왔을 때 — 즉 "역행의심으로 다른 링크에 1틱 튀었다가 바로 다음 GPS에서 직전
 *   링크로 복귀"하는 패턴이면 GPS 노이즈로 판단해 MATCH_STATUS=SKIP(미과금) 처리한다.
 *   좌표·MATCH_LINK_ID 자체는 다른 저신뢰 SKIP(bClampLowConf 등)과 동일하게 참고용으로 DB에
 *   그대로 남긴다(무엇으로 오매칭됐었는지 추적 가능하도록, 값을 지어내 덮어쓰지 않음).
 *   과금 함수(ProcessOpenGateCharge 등)는 "직전 매칭 위치·시각"을 세션에서 읽어 이동거리·
 *   속도를 계산하는데, 그 값(dfLastMatchX/Y 등)은 RunMapMatch 가 매 행마다 실시간으로 이미
 *   최신 위치로 전진시켜놨으므로, 보류 행 처리 "당시" 스냅샷(dfPendingPrevMatchX/Y 등)으로
 *   잠깐 바꿔치기한 후 호출하고 끝나면 즉시 원복한다 — 그렇지 않으면 몇 틱 지난 최신 위치를
 *   "직전 위치"로 오인해 이동거리·속도가 틀어진다. 세션의 다른 과금 상태(bInClosedRoad 등)는
 *   건드리지 않음 (2026-08-21 최정우 추가)
*/
void CRawLogWorker::CommitPendingRow(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		bool bHasNextLinkID, uint64 qwNextLinkID,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if ((pstSession == nullptr) || !pstSession->bHasPendingCommit)
		return;

	const sRawLogInfo stRawLogInfo = pstSession->stPendingRawLogInfo;			// 지역 복사(commit 중 세션 필드 재사용 대비)
	MATCH_LINK_INFO stMatchLinkInfo = pstSession->stPendingMatchLinkInfo;		// 지역 복사(보정 시 nFinalStatus 만 별도 변수로 바꿈)
	// 보류 중이던 이 행 자체의 TRIP_EVENT=END 신뢰 여부 — ProcessRawLog() 가 처음 이 행을 봤을 때
	//   스퓨리어스로 판정해 보류 상태로 남겨뒀을 수 있는데, 여기서 다시 stRawLogInfo.nTripEvent 를
	//   직접 보면 그 판정이 무시되고 원래 막으려던 버그가 재발한다 — IsTrustedTripEnd() 로 동일하게
	//   재판정(dtLastGpsEventTime 은 이미 더 이후 틱까지 반영돼 있어 오히려 더 안전) (2026-08-25 최정우 추가)
	const bool bTrustedTripEnd = IsTrustedTripEnd(stRawLogInfo, *pstSession);
	sint16 nFinalStatus = pstSession->nPendingFinalStatus;
	bool bMatched = (nFinalStatus == MATCH_STATUS_MATCHED);

	// ── 트립 첫 점(BEGIN) 반대방향 오매칭 보정 (2026-08-22 최정우 추가) ──
	//   BEGIN 은 heading 을 무시하고 거리만으로 판정한다(bIgnoreHeading, 2026-08-19). 왕복분리
	//   도로는 짝 링크가 10m 남짓 옆에 나란히 있어 거리차가 무의미한데도 더 가까운 쪽이 뽑힌다.
	//   실측 trip 000376_20260819094414 seq1 — 정답 2040426801 이 7.68m, 반대방향 2040426701 이
	//   4.16m 라 반대방향이 채택됐다(heading 276° vs 채택 링크 방위각 99°, 177° 어긋남).
	//
	//   첫 점 heading 으로 고치려던 접근은 버렸다 — 전국 21트립 실측에서 첫 점 heading 이
	//   이후 점들의 평균 방향과 41° 어긋나 신뢰할 수 없고, 애초에 BEGIN 후보 목록은
	//   그리드 셀당 최선 1건만 담아(GridSgmtMapMatch) 짝 링크가 목록에 오르지도 않는다.
	//
	//   대신 "이미 확정된 다음 점의 링크"를 편향 기준으로 BEGIN 을 다시 태운다. 그 링크가
	//   보류 행 링크의 왕복분리 짝(qwOppositeLinkID)일 때만 — 즉 첫 점이 건너편에 붙은 것이
	//   분명할 때만 — 재매칭한다. 진입 가능한 다른 링크로 넘어간 정상 전이는 건드리지 않는다.
	//   (전국 21트립 검증: 보정 대상 1건, 정상 전이 9건은 미개입)
	if (bMatched && (pstSession->qwLastConfirmedLinkID == 0)
		&& bHasNextLinkID && (qwNextLinkID != stMatchLinkInfo.qwLinkID)
		&& (m_stConfig.pcDataLoader != nullptr) && (m_stConfig.pcProcessManager != nullptr))
	{
		PLINK_INFO pstPendingLink = m_stConfig.pcDataLoader->GetLinkInfo(stMatchLinkInfo.qwLinkID);
		if ((pstPendingLink != nullptr) && (pstPendingLink->qwOppositeLinkID == qwNextLinkID))
		{
			MATCH_LINK_INFO stRematched;
			CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
			if (cPM.RematchBeginBiased(stRawLogInfo, qwNextLinkID, &stRematched)
				&& (stRematched.qwLinkID == qwNextLinkID))
			{
				LOGFMTW("[#%02d] begin opposite-link corrected!device=[%s] trip_id=[%s] seq=[%u] "
					"link=[%llu] -> [%llu] (next_confirmed=[%llu])",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
					static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
					static_cast<unsigned long long>(stRematched.qwLinkID),
					static_cast<unsigned long long>(qwNextLinkID));
				stMatchLinkInfo = stRematched;
			}
		}
	}

	// ── 왕복분리 반대편 링크 N틱 연속 오매칭 보정 (2026-08-24 최정우 확장) ──
	//   기존엔 "짝 링크로 1틱만 튀었다가 바로 다음 틱에 복귀"만 잡았다. 짝 링크로 튀었다가
	//   opp_streakmax 틱 이내에 원래 링크로 돌아오면, 그 사이 커밋된 MATCH_STATUS 를 한꺼번에
	//   SKIP 으로 재기록한다(실측 000376_20260819140532 G5·G6, 2틱 연속 — 저속 구간은 방위각을
	//   안 써서(가중치 0) 왕복분리 반대편이 물리적으로 붙어있으면 GPS 튐만으로 여러 틱 연속
	//   반대편에 붙을 수 있음). qwLastConfirmedLinkID 는 매 틱 갱신돼버려 스트릭 기준으로 못 쓰므로,
	//   스트릭 시작 시점의 진짜 확정 링크를 qwOppStreakAnchorLinkID 에 별도로 고정해둔다.
	//   과금 함수는 아래 공통 흐름에서 매 틱 정상 호출한다(보정 여부와 무관) — 확정 전에 과금을
	//   보류하는 방식도 시도했으나, 정상 주행 중 반대편을 스치기만 하고 다른 링크로 넘어가는(오매칭
	//   아닌) 흔한 경우까지 매번 과금을 놓치는 것으로 실측 확인돼(트립 1개에 4건) 기존 정책(원래
	//   1틱 보정도 항상 즉시 과금을 확정해왔음)을 그대로 유지 — DB 표시(match_status)만 사후
	//   재기록한다.
	if (bMatched && (m_stConfig.pcDataLoader != nullptr))
	{
		uint64 qwAnchor = (pstSession->qwOppStreakAnchorLinkID != 0)
			? pstSession->qwOppStreakAnchorLinkID : pstSession->qwLastConfirmedLinkID;

		if ((qwAnchor != 0) && (stMatchLinkInfo.qwLinkID == qwAnchor) && !pstSession->vtOppStreakUpdateIdx.empty())
		{
			// 원래 링크로 복귀 확정 — 스트릭에 쌓인 틱들의 DB 기록만 SKIP 으로 재기록
			//   (과금은 이미 그 시점 매칭대로 처리됐으므로 되돌리지 않는다)
			for (size_t i = 0; i < pstSession->vtOppStreakUpdateIdx.size(); ++i)
			{
				size_t idx = pstSession->vtOppStreakUpdateIdx[i];
				if (idx >= pvtUpdates->size()) continue;
				(*pvtUpdates)[idx].strMatchStatus = "3";
				(*pvtUpdates)[idx].strMatchLat.clear();
				(*pvtUpdates)[idx].strMatchLon.clear();
				(*pvtUpdates)[idx].strMatchLinkId.clear();
				(*pvtUpdates)[idx].strIntersectLen.clear();
			}
			LOGFMTW("[#%02d] opposite-link %zu-tick flip DB-corrected!device=[%s] trip_id=[%s] "
				"anchor_link=[%llu] (charge already processed as-matched, not reverted)",
				nThreadId, pstSession->vtOppStreakUpdateIdx.size(), stRawLogInfo.szDeviceKey,
				stRawLogInfo.szTripID, static_cast<unsigned long long>(qwAnchor));
			pstSession->qwOppStreakAnchorLinkID = 0;
			pstSession->vtOppStreakUpdateIdx.clear();
		}
		else if ((qwAnchor != 0) && (stMatchLinkInfo.qwLinkID != qwAnchor))
		{
			PLINK_INFO pstAnchorLink = m_stConfig.pcDataLoader->GetLinkInfo(qwAnchor);
			bool bIsOpposite = (pstAnchorLink != nullptr) && (pstAnchorLink->qwOppositeLinkID != 0)
				&& (pstAnchorLink->qwOppositeLinkID == stMatchLinkInfo.qwLinkID);

			if (bIsOpposite && (static_cast<int>(pstSession->vtOppStreakUpdateIdx.size()) < m_stConfig.nOppStreakMax))
			{
				if (pstSession->qwOppStreakAnchorLinkID == 0)
					pstSession->qwOppStreakAnchorLinkID = qwAnchor;
				// 이번 틱의 pvtUpdates 인덱스는 AppendUpdateRow 호출 직후에 기록한다
			}
			else
			{
				// 반대편이 아니거나 스트릭 한도 초과 — 진짜 전이로 인정, 스트릭만 해제(DB 재기록 없음)
				pstSession->qwOppStreakAnchorLinkID = 0;
				pstSession->vtOppStreakUpdateIdx.clear();
			}
		}
	}
	// 이번 틱이 스트릭에 새로 편입되는지 — 위 블록 판정 직후, 과금 호출로 qwLastConfirmedLinkID 가
	//   갱신되기 "전"에 미리 계산해둔다 (2026-08-24 최정우 추가)
	const bool bJoinedOppStreak = (pstSession->qwOppStreakAnchorLinkID != 0)
		&& (stMatchLinkInfo.qwLinkID != pstSession->qwOppStreakAnchorLinkID);

	// ── 트립 시작(또는 장시간 SKIP 후) 첫 매칭이 왕복분리 어느 쪽인지 불확실한 구간 보정
	//   (2026-08-24 최정우 추가, 실측 21트립 중 3건꼴로 재현 확인: 000376_20260819140532 G5·G6 등) ──
	//   진짜 앵커가 아직 없을 때 첫 매칭 성공 링크(A)를 곧바로 신뢰하지 않고 "잠정 후보"로만 잡는다.
	//   왕복분리 반대편(B)이 뒤이어 나타나면 둘 다 후보로 계속 추적하다가, ①한쪽이 opp_streakmax
	//   틱에 먼저 도달하거나 ②A/B 어느 쪽도 아닌 제3의 링크가 나타나면 그 시점까지 더 많이 쌓인
	//   쪽을 승자로 확정하고, 진 쪽의 DB 기록만 SKIP 으로 재기록한다(과금은 그 시점 매칭대로 항상
	//   정상 처리 — 위 반대편 스트릭 보정과 동일 정책). 승자가 정해지면 qwLastConfirmedLinkID 는
	//   아래 공통 흐름(과금 처리 뒤 무조건 대입)에서 자연히 이번 틱 링크로 앵커가 되므로 여기서
	//   별도로 세팅하지 않는다.
	int nJoinedStartCand = 0;			// 0=해당없음, 1=후보A 편입, 2=후보B 편입
	if (bMatched && (m_stConfig.pcDataLoader != nullptr)
		&& (pstSession->qwLastConfirmedLinkID == 0) && (pstSession->qwOppStreakAnchorLinkID == 0))
	{
		auto ResolveStartAmbiguity = [&](vector<size_t>& vtLoserIdx, uint64 qwLoserLink)
		{
			for (size_t i = 0; i < vtLoserIdx.size(); ++i)
			{
				size_t idx = vtLoserIdx[i];
				if (idx >= pvtUpdates->size()) continue;
				(*pvtUpdates)[idx].strMatchStatus = "3";
				(*pvtUpdates)[idx].strMatchLat.clear();
				(*pvtUpdates)[idx].strMatchLon.clear();
				(*pvtUpdates)[idx].strMatchLinkId.clear();
				(*pvtUpdates)[idx].strIntersectLen.clear();
			}
			if (!vtLoserIdx.empty())
				LOGFMTW("[#%02d] trip-start ambiguous link resolved!device=[%s] trip_id=[%s] "
					"loser_link=[%llu] loser_streak=[%zu] (DB-corrected, charge not reverted)",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					static_cast<unsigned long long>(qwLoserLink), vtLoserIdx.size());
			pstSession->qwStartCandLinkA = 0;
			pstSession->qwStartCandLinkB = 0;
			pstSession->vtStartCandIdxA.clear();
			pstSession->vtStartCandIdxB.clear();
		};

		if (pstSession->qwStartCandLinkA == 0)
		{
			pstSession->qwStartCandLinkA = stMatchLinkInfo.qwLinkID;
			nJoinedStartCand = 1;
		}
		else if (stMatchLinkInfo.qwLinkID == pstSession->qwStartCandLinkA)
		{
			if (static_cast<int>(pstSession->vtStartCandIdxA.size()) >= m_stConfig.nOppStreakMax)
				ResolveStartAmbiguity(pstSession->vtStartCandIdxB, pstSession->qwStartCandLinkB);
			else
				nJoinedStartCand = 1;
		}
		else if (pstSession->qwStartCandLinkB == 0)
		{
			PLINK_INFO pstCandA = m_stConfig.pcDataLoader->GetLinkInfo(pstSession->qwStartCandLinkA);
			if ((pstCandA != nullptr) && (pstCandA->qwOppositeLinkID != 0)
				&& (pstCandA->qwOppositeLinkID == stMatchLinkInfo.qwLinkID))
			{
				pstSession->qwStartCandLinkB = stMatchLinkInfo.qwLinkID;
				nJoinedStartCand = 2;
			}
			else
			{
				// A도 아니고 반대편도 아닌 제3의 링크 — B가 없었으니 A가 그냥 승리(정리만)
				ResolveStartAmbiguity(pstSession->vtStartCandIdxB, 0);
			}
		}
		else if (stMatchLinkInfo.qwLinkID == pstSession->qwStartCandLinkB)
		{
			if (static_cast<int>(pstSession->vtStartCandIdxB.size()) >= m_stConfig.nOppStreakMax)
				ResolveStartAmbiguity(pstSession->vtStartCandIdxA, pstSession->qwStartCandLinkA);
			else
				nJoinedStartCand = 2;
		}
		else
		{
			// 제3의 링크 등장 — A/B 중 더 많이 쌓인 쪽이 승리
			if (pstSession->vtStartCandIdxA.size() >= pstSession->vtStartCandIdxB.size())
				ResolveStartAmbiguity(pstSession->vtStartCandIdxB, pstSession->qwStartCandLinkB);
			else
				ResolveStartAmbiguity(pstSession->vtStartCandIdxA, pstSession->qwStartCandLinkA);
		}
	}

	if (bMatched)
	{
		const double dfCurX = pstSession->dfLastMatchX;
		const double dfCurY = pstSession->dfLastMatchY;
		const time_t dtCurGps = pstSession->dtLastMatchGps;
		const bool bCurHas = pstSession->bHasLastMatch;

		pstSession->dfLastMatchX = pstSession->dfPendingPrevMatchX;
		pstSession->dfLastMatchY = pstSession->dfPendingPrevMatchY;
		pstSession->dtLastMatchGps = pstSession->dtPendingPrevMatchGps;
		pstSession->bHasLastMatch = pstSession->bPendingHadLastMatch;

		ProcessOpenGateCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts, bTrustedTripEnd);
		ProcessClosedRoadCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessSpeedZoneCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessExemptZoneCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts, bTrustedTripEnd);
		ProcessNodeStepCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts, bTrustedTripEnd);

		pstSession->dfLastMatchX = dfCurX;
		pstSession->dfLastMatchY = dfCurY;
		pstSession->dtLastMatchGps = dtCurGps;
		pstSession->bHasLastMatch = bCurHas;

		// 트립 시작 모호구간 후보(A/B)에 아직 편입 중이면(nJoinedStartCand!=0) 앵커를 확정하지
		//   않는다 — 여기서 그냥 대입해버리면 qwLastConfirmedLinkID 가 0이 아니게 돼 바로 다음
		//   틱부터 "앵커 없음" 조건이 깨져 위 블록이 다시는 못 돈다 (2026-08-24 최정우 추가)
		if (nJoinedStartCand == 0)
		{
			pstSession->qwLastConfirmedLinkID = stMatchLinkInfo.qwLinkID;
			pstSession->dtLastConfirmedLinkTime = stRawLogInfo.dtGPS;
		}
	}
	else
	{
		// SKIP 틱 — 확정매칭 전이로 못 잡은 잔여 출구 케이스를 raw GPS 좌표로 한 번 더 확인
		//   (사용자 지시, 2026-08-24 최정우 추가)
		CheckClosedRoadExitByRawGps(nThreadId, stRawLogInfo, pstSession, pvtChargeInserts);
		CheckSpeedZoneExitByRawGps(nThreadId, stRawLogInfo, pstSession, pvtChargeInserts);
	}

	bool bAppended = AppendUpdateRow(pvtUpdates, stRawLogInfo, nFinalStatus, pstSession->nPendingIntersectLen,
		pstSession->bPendingHasCoords ? &stMatchLinkInfo.dfMatchY : nullptr,
		pstSession->bPendingHasCoords ? &stMatchLinkInfo.dfMatchX : nullptr,
		pstSession->bPendingHasCoords ? stMatchLinkInfo.qwLinkID : 0);

	if (bJoinedOppStreak && bAppended)
		pstSession->vtOppStreakUpdateIdx.push_back(pvtUpdates->size() - 1);

	if (bAppended)
	{
		if (nJoinedStartCand == 1)
			pstSession->vtStartCandIdxA.push_back(pvtUpdates->size() - 1);
		else if (nJoinedStartCand == 2)
			pstSession->vtStartCandIdxB.push_back(pvtUpdates->size() - 1);
	}

	pstSession->bHasPendingCommit = false;
}

/**
 * @brief GPS 1건 처리 – 검증·맵매칭·결과 행 적재 (배치 종료 시 rawgps_update)
 * @param[in] nThreadId 워커 스레드 ID
 * @param[in] stRawLogInfo 원시 GPS 정보 (TRIP_ID 는 수집서버 적재분)
 * @param[out] pvtUpdates bulk UPDATE 대상 행 목록
 * @param[in,out] pstSession 배치 임시 세션 (bulk 성공 전까지 m_vtTripSessions 미반영)
 * @param[out] pbTripEnded TRIP END(2) 시 true 설정 — 일치 결과 무관, bulk 성공 후 세션 제거 (#9)
 * @return true(처리·적재 성공), false(인자 null·적재 실패)
 * @remark 2026-07-08 최정우 추가
 *   - TRIP_ID 없음/불일치, TRIP_EVENT 비정상 → SKIP
 *   - TRIP_EVENT=0(START) 또는 GPS_SEQ<=dwLastGpsSeq → 세션 초기화 후 시작
 *   - 맵매칭 실패 → ERROR, 성공 → MATCHED
 *   - TRIP_EVENT=2(END) 이면 MATCHED/ERROR/SKIP 무관 pbTripEnded=true (#9)
 * @remark 세션 갱신은 pstSession(배치 임시)에만 적용. run() 이 bulk 성공 시 커밋.
 * @remark 2026-08-21 최정우 수정 — 정상 매칭(bMatched && !bUntrustedMatch)된 행은 즉시
 *   커밋하지 않고 세션에 1틱 보류(CommitPendingRow 참고), 반대편 짝 링크 1틱 오매칭 보정 도입
*/
bool CRawLogWorker::ProcessRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		vector<TRIP_END_UPDATE_ROW> *pvtTripEndUpdates,
		VEHICLE_TRIP_SESSION *pstSession, bool *pbTripEnded)
{
	if ((pvtUpdates == nullptr) || (pvtChargeInserts == nullptr) || (pvtTripEndUpdates == nullptr)
		|| (pstSession == nullptr) || (pbTripEnded == nullptr))
		return false;

	sint16 nRejectStatus = MATCH_STATUS_SKIP;
	// device_key·trip_id·trip_event 2차 검증 (2026-07-08 최정우 주석 추가)
	if (!ValidateRawLog(nThreadId, stRawLogInfo, &nRejectStatus))
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, nRejectStatus);

	VEHICLE_TRIP_SESSION& stSession = *pstSession;
	stSession.dtLastSeen = time(nullptr);

	bool bFullReset = false;
	bool bSeqRollback = false;			// GPS_SEQ 역전 — 아래에서 이 행만 SKIP (2026-08-23 최정우 추가)
	// 시작(세션 초기화) 필요 여부 판단 (2026-07-08 최정우 주석 추가)
	if (NeedsBeginReset(nThreadId, stRawLogInfo, stSession, &bFullReset, &bSeqRollback))
	{
		if (bFullReset)
		{
			LOGFMTD("[#%02d] trip START reset!device=[%s] trip_id=[%s] seq=[%u]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRawLogInfo.dwSeqNo);
		}

		// 연속 맵매칭 세션 시작 상태로 초기화 (2026-07-08 최정우 주석 추가)
		ResetTripSessionForBegin(stSession, bFullReset);
	}
	else if (!bSeqRollback && !stSession.bStartWarned && stRawLogInfo.nTripEvent != TRIP_EVENT_START)
	{
		LOGFMTW("[#%02d] trip missing START!device=[%s] trip_id=[%s] seq=[%u] event=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			stRawLogInfo.dwSeqNo, static_cast<int>(stRawLogInfo.nTripEvent));
		stSession.bStartWarned = true;
	}

	// 현재 배치 TRIP_ID 를 세션에 기록(다음 trip 변경 감지 기준). 키는 DEVICE_KEY 라 trip 이 바뀌면 위에서 리셋됨 (2026-07-08 최정우 추가)
	strncpy(stSession.szTripId, stRawLogInfo.szTripID, sizeof(stSession.szTripId) - 1);
	stSession.szTripId[sizeof(stSession.szTripId) - 1] = '\0';

	// TRIP_EVENT=END 스퓨리어스(순서역전) 검사 — 이 END 의 gps_dt 가 이 trip 에서 지금까지 확인된
	//   최대 gps_dt(dtLastGpsEventTime)보다 과거면 신뢰하지 않는다(단말이 보낸 순서역전·중복 도착
	//   이벤트로 추정). 실측 000376_20260819140856 seq19(gps_dt 가 직전 seq18보다 과거인 END) —
	//   이 스퓨리어스 END 가 그대로 처리되면서 열려있던 구간단속 세션이 강제마감→리셋되고, 뒤이은
	//   정상 주행이 같은 구역 재진입으로 오판돼 AUDIT 행이 중복 적재됐다. 오판(진짜 END를 스퓨리어스로
	//   착각)해도 세션이 안 지워질 뿐 데이터 유실은 없음 — 결국 TTL 만료로 안전하게 마감된다(사용자
	//   확정, 2026-08-25). dtLastGpsEventTime==0(신규 trip 첫 행)이면 비교 대상이 없어 항상 신뢰한다.
	//   (2026-08-25 최정우 추가)
	bool bTrustedTripEnd = IsTrustedTripEnd(stRawLogInfo, stSession);
	if (!bTrustedTripEnd && (stRawLogInfo.nTripEvent == TRIP_EVENT_END))
	{
		LOGFMTW("[#%02d] spurious END skip!device=[%s] trip_id=[%s] seq=[%u] gps_dt=[%ld] last_gps_dt=[%ld]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<long>(stRawLogInfo.dtGPS), static_cast<long>(stSession.dtLastGpsEventTime));
	}
	if (stRawLogInfo.dtGPS > stSession.dtLastGpsEventTime)
		stSession.dtLastGpsEventTime = stRawLogInfo.dtGPS;

	// 트립종료 직전, 아직 보류(pending) 중인 1틱 지연 행이 있으면 먼저 확정(commit)한다 — 바로
	//   아래 AppendExpiredClosedRoadCharge/AppendExpiredSpeedZoneCharge 가 세션의 bInClosedRoad/
	//   bInSpeedZone 을 읽기 "전"에 보류 행의 과금 반영이 먼저 끝나 있어야 정확하다. 이 시점엔
	//   이번 행(TRIP_EVENT=END) 자신의 매칭 결과를 아직 몰라 보정판단용 "다음" 링크가 없음(보정
	//   시도 안 함, 보류 행을 계산된 값 그대로 커밋) (2026-08-21 최정우 추가)
	if (bTrustedTripEnd)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);

	// 트립 종료(TRIP_EVENT=2) — 매칭 성공/실패 무관하게 그 trip_id 의 PRIM_CHARGEHAND 전 행에
	//   trip_end_dt 를 나중에(run() 이 배치 종료 시) UPDATE 하도록 적재 (2026-08-12 최정우 추가)
	//   UPD_DT 도 TRIP_END_DT 와 동일하게 GPS 시각(wall-clock 아님) 사용 — 사용자 지시(2026-08-14):
	//   "구역 이탈"과 "트립 정상종료"가 같은 tick 에 겹치는 경우(예: 일반도로 구간 안에서 트립이
	//   끝나는 경우) 진출시각과 종료시각이 같은 값이니 UPD_DT 도 그 값으로 맞추는 게 자연스러움 —
	//   5유형 공용 UPDATE라 전부에 동일 적용됨. bTrustedTripEnd 로 스퓨리어스 END 는 제외(2026-08-25)
	if (bTrustedTripEnd)
	{
		TRIP_END_UPDATE_ROW stEndRow;
		stEndRow.strTripId = stRawLogInfo.szTripID;
		stEndRow.strTripEndDt = FormatDateTime14(stRawLogInfo.dtGPS);
		stEndRow.strUpdDt = FormatDateTime14(stRawLogInfo.dtGPS);
		pvtTripEndUpdates->push_back(stEndRow);

		// 트립이 끝나는 시점에 아직 입구만 통과하고 출구를 못 찾은 폐쇄형/구간단속 세션이 열려
		//   있으면, 곧이어 세션 자체가 삭제(TRIP_EVENT=2 → pbTripEnded=true → run()이 배치 성공 후
		//   mapSessions.erase)되면서 그 진행 중이던 통행 기록이 통째로 사라진다 — TTL 만료 때와
		//   동일한 형태(N/3 AUDIT, 못 본 출구게이트는 NULL로 저장 — NULLIF(...,'') 처리)로 여기서
		//   먼저 기록해둔다(2026-08-20 최정우 추가, 사용자 지시 — 이슈②·③이 공유하던 근본원인 해결).
		//   snapshot-후-increment 패턴은 ExpireTtlSessions()와 동일 이유(한 tick에 폐쇄형·구간단속이
		//   동시에 열려있을 수 있어 trip_seq PK 충돌 방지)
		//   마감 후에는 진행 플래그를 내린다. 두 Append 함수는 세션을 const 참조로 받아 스스로
		//   플래그를 못 내리는데, 한 트립에 TRIP_EVENT=END 행이 둘 이상 오면(실측
		//   000376_20260819140856 의 seq19·seq23 — seq19 는 시각이 뒤로 간 도착 이벤트 행이다)
		//   같은 진입이 두 번 마감돼 AUDIT 행이 중복 적재된다. 실측에서 구간단속 RL-Z00003 이
		//   진입시각 20260819140940 으로 2건(체류 9초·21초) 쌓였다. 둘 다 charge_yn=N 이라
		//   요금이 이중으로 나가지는 않았지만, 출구를 봤다면 이중 과금이 될 수 있는 구조다.
		//   트립 종료 시 세션은 어차피 배치 끝에 삭제되므로 여기서 미리 내려도 안전하다
		//   (2026-08-23 최정우 수정)
		bool bWasClosedRoad = stSession.bInClosedRoad;
		AppendExpiredClosedRoadCharge(nThreadId, stRawLogInfo.szDeviceKey, stSession, stRawLogInfo.dtGPS, pvtChargeInserts);
		if (bWasClosedRoad)
		{
			stSession.nChargeSeq += 1;
			stSession.bInClosedRoad = false;
		}

		bool bWasSpeedZone = stSession.bInSpeedZone;
		AppendExpiredSpeedZoneCharge(nThreadId, stRawLogInfo.szDeviceKey, stSession, stRawLogInfo.dtGPS, pvtChargeInserts);
		if (bWasSpeedZone)
		{
			stSession.nChargeSeq += 1;
			stSession.bInSpeedZone = false;
		}
	}

	// ── GPS_SEQ 역전 행 — 세션 앵커를 유지한 채 이 행만 SKIP (2026-08-23 최정우 추가) ──
	//   예전에는 NeedsBeginReset() 이 여기서 true 를 돌려 세션 앵커(직전 링크·위치·고도)를 통째로
	//   버리고 BEGIN 으로 강등했다. 그런데 BEGIN 은 heading 을 아예 안 보고 거리만으로 판정하므로
	//   (bIgnoreHeading=true, BeginMapMatch.cpp), 왕복분리 도로에서는 10m 옆 건너편 차로가 그냥
	//   더 가깝다는 이유로 채택된다 — 실측 000376_20260819140856 seq18 에서 반대차로 2.80m 가
	//   정답차로 7.12m 를 이겨 오매칭됐고, 그 잘못된 앵커가 seq20 까지 번졌다.
	//   방위각으로는 막을 수 없다: GISUtil::SgmtMatch() 가 양방향 단일 링크 지원을 위해 세그먼트
	//   방위각의 정·역(+180°) 중 더 잘 맞는 쪽을 채택해서, 실제 167° 어긋난 건너편 차로가 13° 로
	//   접혀 하드컷(MM_DIR_MAX_DEG=120°)을 그냥 통과한다. 즉 이 쌍에서 유일한 방어선은 위상
	//   (직전 링크에서 도달 가능한가)인데 BEGIN 에는 그 제약이 없다.
	//   과거 행 하나 때문에 뒤따르는 정상 행이 그 방어선을 잃을 이유가 없다. 역전 행 자신은
	//   이미 지나온 시점이라 재매칭 가치가 없으므로 SKIP(3) 로 남긴다.
	//   트립종료(TRIP_EVENT=END) 기록·보류행 커밋은 바로 위에서 이미 끝냈다.
	if (bSeqRollback)
	{
		if (bTrustedTripEnd)
			*pbTripEnded = true;
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// GPS 좌표·RAW_VLD 유효성 검사 — SKIP(3). 세션·DB 좌표 미저장 (2026-07-10 최정우 수정)
	if (ShouldSkipGpsInput(nThreadId, stRawLogInfo, m_stConfig.nIgnoreRawVld != 0))
	{
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (bTrustedTripEnd)
			*pbTripEnded = true;

		// 주정차 판정은 RAW_VLD 와 무관하게 원시 GPS 좌표로 수행한다 (2026-08-22 사용자 확정).
		//   근거: 차량이 멈추면 GPS 가 측위를 놓쳐 ACCURACY_M 이 급격히 나빠지고 RAW_VLD=false 가
		//   되는데(실측: DRIVE_STATUS=PARKED 497건의 평균 정확도 64m, ON_ROAD 는 7m), 하필 주정차
		//   판정이 가장 필요한 순간이 그때다. 이 행들을 버리면 도착 정차가 통째로 누락된다
		//   (실측: 000376_20260819094414 의 도착 정차 17점·79초가 전부 RAW_VLD=false 였음).
		//   맵매칭(다른 3종 과금)은 종전대로 SKIP — 좌표를 링크에 붙이는 일은 정확도가 필요하지만,
		//   "폴리곤 안에 있었나"는 그보다 훨씬 큰 공간 판정이라 성격이 다르다.
		//   GPS_LAT/LON 자체가 NULL 이면 판정 불가라 제외.
		if (!stRawLogInfo.bGpsLatNull && !stRawLogInfo.bGpsLonNull)
			ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts, bTrustedTripEnd);
		// 보정판단용 "다음" 링크 없음(이 행은 raw_vld=false라 신뢰 못함) — 보류 행 계산된 값 그대로 커밋 (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// config radius_skip — ACCURACY_M 초과 시 SKIP. 세션 앵커 미갱신 (2026-07-10 최정우 수정)
	// 검색반경 아님. 0=비활성 (2026-07-08 최정우)
#if 0
	 if (m_stConfig.nRadiusSkipM > 0
	 	&& stRawLogInfo.nAccuracyM >= 0
	 	&& stRawLogInfo.nAccuracyM > m_stConfig.nRadiusSkipM)
#endif
	if ((m_stConfig.nRadiusSkip > 0) && 
		(stRawLogInfo.nAccuracyM >= 0) && 
		(stRawLogInfo.nAccuracyM > m_stConfig.nRadiusSkip))
	{
		LOGFMTW("[#%02d] reject accuracy_m over skip!device=[%s] trip_id=[%s] seq=[%u] accuracy_m=[%d] radius_skip=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			static_cast<int>(stRawLogInfo.nAccuracyM), m_stConfig.nRadiusSkip);
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (bTrustedTripEnd)
			*pbTripEnded = true;

		// 정확도 SKIP — 최근접 있으면 참고용 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
		MATCH_LINK_INFO stNear;
		memset(reinterpret_cast<void *>(&stNear), 0, MATCH_LINK_INFO_SIZE);
		stNear.dfIntersectLenSgmt = -1.0;
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		// 보정판단용 "다음" 링크 없음(정확도 SKIP이라 신뢰 못함) — 보류 행 계산된 값 그대로 커밋 (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
		if (cPM.FindNearestSegment(stRawLogInfo, &stNear))
		{
			int nNearLen = CalcIntersectLen(stRawLogInfo, stNear.dfMatchX, stNear.dfMatchY);
			return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP, nNearLen,
				&stNear.dfMatchY, &stNear.dfMatchX, stNear.qwLinkID);
		}
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// 이동거리 환산속도 vs SPEED_KMH 정합성 검사 — 이상치 GPS SKIP. 세션 앵커 미갱신 (2026-07-20 최정우 추가)
	int nImpliedSpeedKmh = -1;
	if (ShouldSkipImplausibleSpeed(nThreadId, stRawLogInfo, stSession, &nImpliedSpeedKmh))
	{
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (bTrustedTripEnd)
			*pbTripEnded = true;

		// 정합성 SKIP — 최근접 있으면 참고용 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
		MATCH_LINK_INFO stNear;
		memset(reinterpret_cast<void *>(&stNear), 0, MATCH_LINK_INFO_SIZE);
		stNear.dfIntersectLenSgmt = -1.0;
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		// 보정판단용 "다음" 링크 없음(정합성 SKIP이라 신뢰 못함) — 보류 행 계산된 값 그대로 커밋 (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
		if (cPM.FindNearestSegment(stRawLogInfo, &stNear))
		{
			int nNearLen = CalcIntersectLen(stRawLogInfo, stNear.dfMatchX, stNear.dfMatchY);
			return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP, nNearLen,
				&stNear.dfMatchY, &stNear.dfMatchX, stNear.qwLinkID);
		}
		return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP);
	}

	// (D) 장시간 공백 시 세션 앵커 폐기 → 연속성 끊고 초기(Begin) 재획득 (2026-07-15 최정우 추가)
	//   직전 "매칭 성공" 이후 gap 이 MM_SESSION_RESET_GAP_SEC 초과면 위치 불확실 → 앵커·링크 리셋
	if (stSession.bHasLastMatch && (stSession.dtLastMatchGps > 0))
	{
		double dfSessGapSec = difftime(stRawLogInfo.dtGPS, stSession.dtLastMatchGps);
		if (dfSessGapSec > static_cast<double>(MM_SESSION_RESET_GAP_SEC))
		{
			LOGFMTD("[#%02d] session gap reset! device=[%s] trip_id=[%s] seq=[%u] gap=[%.0fs]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
				stRawLogInfo.dwSeqNo, dfSessGapSec);
			stSession.qwLinkID = 0;
			stSession.dfLastMatchX = 0.0;
			stSession.dfLastMatchY = 0.0;
			stSession.dtLastMatchGps = 0;
			stSession.bHasLastMatch = false;
			stSession.bHasPrevAlt = false;
		}
	}

	sint16 nFinalStatus = MATCH_STATUS_MATCHED;
	MATCH_LINK_INFO stMatchLinkInfo;
	memset(reinterpret_cast<void *>(&stMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	stMatchLinkInfo.dfIntersectLenSgmt = -1.0;

	// 1 GPS 맵매칭 처리시간 측정 (nMatchTimeoutMs>0 시 초과 포인트 ERROR 격리)
	// 맵매칭 처리 시간 측정 시작 (2026-07-08 최정우 주석 추가)
	CClock cMatchClock;
	cMatchClock.Start();
	// ProcessManager 경유 시작/Continue 맵매칭 (2026-07-08 최정우 주석 추가)
	bool bMatched = RunMapMatch(nThreadId, stRawLogInfo, &stSession, &stMatchLinkInfo);
	// 맵매칭 처리 시간 측정 종료 (2026-07-08 최정우 주석 추가)
	cMatchClock.Stop();

	// 반경 밖·진단반경 초과 최근접 — MATCHED 아님, SKIP(3)·세션 미갱신·MATCH_LAT/LON·INTERSECT_LEN 저장 (2026-07-10 최정우 수정)
	const bool bOut = (!bMatched) && stMatchLinkInfo.bOutOfRadius;
	// 연속 역행 미확정(reverse_confirm 미만) — SKIP·세션 앵커 고정. RunMapMatch 가 이미 nReverseStreak 갱신.
	//   bReverseSuspect(위치+heading 둘 다 역행) 기준으로 GPS 노이즈로 인한 오탐을 줄임 (2026-07-21 최정우 수정)
	const bool bReverseSkip = bMatched && stMatchLinkInfo.bReverseSuspect
		&& (stSession.nReverseStreak < m_stConfig.nReverseConfirm);

	if (!bMatched && bOut)
	{
		// SKIP: MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)만 DB 저장, 세션 앵커 미갱신
		LOGFMTW("[#%02d] out-of-radius skip! device=[%s] trip_id=[%s] seq=[%u] "
			"intersect_len=[%.1fm] match_lat=[%.06lf] match_lon=[%.06lf]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			stMatchLinkInfo.dfIntersectLenSgmt, stMatchLinkInfo.dfMatchY, stMatchLinkInfo.dfMatchX);
		nFinalStatus = MATCH_STATUS_SKIP;
	}
	else if (!bMatched)
	{
		// 실패: DEVICE_KEY·좌표(위경도)·에러코드·에러메시지(CodeMap 변환값) 로그
		const char *pszErrMsg = (stMatchLinkInfo.szErrorMsg[0] != '\0')
			? stMatchLinkInfo.szErrorMsg : "unknown";
		LOGFMTW("[#%02d] map match failed! device=[%s] trip_id=[%s] seq=[%u] "
			"lat=[%.06lf] lon=[%.06lf] err=[%d] msg=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
			stRawLogInfo.dfY, stRawLogInfo.dfX,
			static_cast<int>(stMatchLinkInfo.wErrorCode), pszErrMsg);
		nFinalStatus = MATCH_STATUS_ERROR;
	}
	else
	{
		// bMatched == true 인 경우, 아래 두 검사(타임아웃/역행 SKIP)는 각각 독립적으로 실행되어야 한다.
		//
		// [버그였던 예전 코드]                          [문제]
		//   else if (nMatchTimeoutMs > 0) { ... }         "타임아웃 검사 기능이 켜져 있다"는 조건만으로
		//   else if (bReverseSkip)        { ... }         이 가지로 들어가 버려서, 실제로는 200ms를
		//                                                  안 넘겨 안에서 아무 것도 안 해도 바로 아래
		//                                                  else if(bReverseSkip) 는 검사조차 못 받고
		//                                                  건너뛰어짐 → reverse_confirm 기반 SKIP이
		//                                                  timeout 기능이 켜져 있는 한 항상 무력화됨.
		//
		// [수정] else if 로 나란히 두지 않고, if 두 개로 분리해서 둘 다 매번 검사되게 함
		//   (2026-07-21 최정우 수정 — 인위적 역행 테스트로 발견)
		if (m_stConfig.nMatchTimeoutMs > 0)
		{
			const double dfElapsedMs = cMatchClock.GetElapsedTime() * 1000.0;
			if (dfElapsedMs > static_cast<double>(m_stConfig.nMatchTimeoutMs))
			{
				LOGFMTW("[#%02d] map match timeout! elapsed=[%.1fms] threshold=[%dms] seq=[%u] device=[%s] trip_id=[%s]",
					nThreadId, dfElapsedMs, m_stConfig.nMatchTimeoutMs,
					stRawLogInfo.dwSeqNo, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID);
				nFinalStatus = MATCH_STATUS_ERROR;
				bMatched = false;   // MATCH_LAT/LON 미기록(격리)
			}
		}

		if (bMatched && bReverseSkip)
		{
			// 연속 역행이 reverse_confirm 미만 — 노이즈 취급, SKIP 저장(좌표는 원본 그대로)·세션 앵커 미갱신 (2026-07-21 최정우 추가)
			LOGFMTW("[#%02d] reverse hit! device=[%s] trip_id=[%s] seq=[%u] streak=[%d/%d] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				stSession.nReverseStreak, m_stConfig.nReverseConfirm,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// 경계 클램프 + GPS와 거리가 먼 저신뢰 매칭 — SKIP 처리. 여러 GPS_SEQ 가 같은 꺾임점으로
		//   뭉개져 실제로는 계속 이동 중인데도 MATCH_LAT/LON 이 정지한 것처럼 보이는 오탐(예: 주정차
		//   오판) 을 다운스트림에서 MATCHED 로 신뢰하지 않도록 함. 세션 앵커(qwLinkID 등)는 그대로
		//   갱신 — 엔진 내부 연속 매칭 추적은 방해하지 않고, DB 저장값만 SKIP 으로 표시 (2026-07-21 최정우 추가)
		if (bMatched && stMatchLinkInfo.bClampLowConf)
		{
			LOGFMTW("[#%02d] clamp low-confidence! device=[%s] trip_id=[%s] seq=[%u] "
				"intersect_len=[%.1fm] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				stMatchLinkInfo.dfIntersectLenSgmt,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}

		// 같은 링크 역행인데 heading 없음/각도 애매해 "확실한 노이즈"로 단정 못 하는 경우 — SKIP 처리.
		//   좌표는 계산된 값 그대로 저장(원본 GPS 대비 위치는 정상), 신뢰도만 낮게 표시 (2026-07-22 최정우 추가)
		if (bMatched && stMatchLinkInfo.bAmbiguousReverse)
		{
			LOGFMTW("[#%02d] ambiguous reverse! device=[%s] trip_id=[%s] seq=[%u] link=[%llu] -> SKIP",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
		}
	}

	// 신뢰 못 하는 좌표(역행 미확정·클램프 저신뢰·역행 판단불가)는 다음 포인트의 HEADING/SPEED/
	//   이상속도 검사 기준으로 쓰지 않음 (2026-07-22 최정우 수정 — 역행 판단불가 케이스 추가)
	const bool bUntrustedMatch = bReverseSkip || stMatchLinkInfo.bClampLowConf || stMatchLinkInfo.bAmbiguousReverse;

	stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
	// 다음 포인트의 이상속도 검사 신뢰도 판단용 — 앵커 갱신 여부와 동일 조건 (2026-07-21 최정우 추가)
	stSession.bLastPointOk = (bMatched && !bUntrustedMatch);

	// 주정차 판정 — 원시 GPS·속도가 기본이지만, 규칙2(매칭 좌표도 폴리곤 내)·규칙4(매칭 좌표가
	//   폴리곤 밖이면 통과로 보고 해제)를 위해 매칭 결과가 필요해 맵매칭 "이후"로 옮겼다.
	//   맵매칭 성공/실패와 무관하게 항상 평가하는 성질은 그대로 — 실패 시 bMatchTrusted=false 로
	//   넘겨 원시 좌표만으로 규칙1 판정한다 (2026-08-22 최정우 수정, 원래는 맵매칭 전 호출)
	{
		const bool bParkMatchOk = (bMatched && !bUntrustedMatch);
		ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts, bTrustedTripEnd,
			bParkMatchOk, bParkMatchOk ? stMatchLinkInfo.dfMatchX : 0.0,
			bParkMatchOk ? stMatchLinkInfo.dfMatchY : 0.0);
	}

	// ── 매칭 성공(MATCHED) 시에만 세션 앵커 갱신 — SKIP/ERROR 는 직전 성공 앵커 유지 (2026-07-10 최정우 수정) ──
	//   · XY·시각: HEADING/SPEED 보정용 (ALTITUDE_M NULL이어도 갱신)
	//   · 고도: ALTITUDE_M 유효 시에만 nPrevAltitude·nPrevRoadType 저장
	//     (직전 매칭 좌표에 Z 없음 — GPS 고도를 앵커로 기억)
	//   · 직전 고도 없이 현재만 있으면 bHasPrevAlt=false → 고도 점수 스킵
	//   · 역행 미확정(bReverseSkip)·클램프 저신뢰 시에도 이 앵커는 갱신하지 않음 — 다음 포인트가
	//     오염된 좌표를 기준으로 HEADING/SPEED 를 잘못 계산하지 않도록 (2026-07-21 최정우 수정)
	//   · 이 앵커는 맵매칭 엔진(RunMapMatch, 다음 GPS 의 고도보조 점수)이 실시간으로 계속 써야
	//     해서 절대 지연 불가 — 과금 함수 호출·DB 반영만 아래에서 1틱 보류한다 (2026-08-21 최정우 수정)
	if (bMatched && !bUntrustedMatch)
	{
		// 과금 함수용 "직전 매칭 위치·시각" 스냅샷 — 바로 아래서 최신값으로 덮어쓰기 전에, 이번
		//   행을 보류(pending) 커밋할 때 쓸 "그 당시" 값을 미리 저장해둔다. 기존엔 이 스냅샷이
		//   필요 없었다(과금 함수를 그 자리에서 곧바로 호출했으므로) — 1틱 지연커밋 도입으로
		//   보류 시점엔 세션 앵커가 이미 몇 틱 전진해있어 스냅샷이 꼭 필요함 (2026-08-21 최정우 추가)
		const double dfPrevMatchX = stSession.dfLastMatchX;
		const double dfPrevMatchY = stSession.dfLastMatchY;
		const time_t dtPrevMatchGps = stSession.dtLastMatchGps;
		const bool bPrevHasMatch = stSession.bHasLastMatch;

		stSession.dfLastMatchX = stMatchLinkInfo.dfMatchX;
		stSession.dfLastMatchY = stMatchLinkInfo.dfMatchY;
		stSession.dtLastMatchGps = stRawLogInfo.dtGPS;
		stSession.bHasLastMatch = true;
		if (stRawLogInfo.nAltitudeM >= 0)
		{
			stSession.nPrevAltitude = stRawLogInfo.nAltitudeM;
			stSession.nPrevRoadType = stMatchLinkInfo.nRoadType;
			stSession.bHasPrevAlt = true;
		}

		// 기존에 보류돼 있던 행을 먼저 확정(commit) — 이번 행의 확정 링크를 "다음" 참고로 보정
		//   판단(반대편 짝 링크 1틱 오매칭이면 SKIP 처리) (2026-08-21 최정우 추가)
		CommitPendingRow(nThreadId, &stSession, true, stMatchLinkInfo.qwLinkID, pvtUpdates, pvtChargeInserts);

		// 이번 행은 즉시 과금 처리·DB 반영하지 않고 세션에 1틱 보류 — 다음 GPS 가 오면 그때 위
		//   로직으로 확정된다(반대편 짝 링크 1틱 오매칭 보정, [[project_mapmatch_opposite_link_and_begin_heading]]
		//   과 별개 후속 조치) (2026-08-21 최정우 추가)
		stSession.bHasPendingCommit = true;
		stSession.stPendingRawLogInfo = stRawLogInfo;
		stSession.stPendingMatchLinkInfo = stMatchLinkInfo;
		stSession.nPendingFinalStatus = nFinalStatus;
		stSession.nPendingIntersectLen = CalcIntersectLen(stRawLogInfo,
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY);
		stSession.bPendingHasCoords = true;
		stSession.dfPendingPrevMatchX = dfPrevMatchX;
		stSession.dfPendingPrevMatchY = dfPrevMatchY;
		stSession.dtPendingPrevMatchGps = dtPrevMatchGps;
		stSession.bPendingHadLastMatch = bPrevHasMatch;

		// 트립 종료(TRIP_EVENT=2) — 더 이상 "다음" GPS 가 안 올 수 있으므로 보정판단 없이 즉시 확정 (2026-08-21 최정우 추가)
		//   bTrustedTripEnd 로 스퓨리어스 END 는 제외(2026-08-25)
		if (bTrustedTripEnd)
		{
			CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);
			*pbTripEnded = true;
		}

		return true;
	}

	// SKIP/ERROR(또는 bUntrustedMatch) 확정 — 기존과 동일하게 즉시 커밋. 보류 중이던 행이 있으면
	//   "다음" 링크 참고 없이(이 행은 신뢰 못하는 매칭이라 보정판단에 못 씀) 계산된 값 그대로
	//   확정한다 (2026-08-21 최정우 추가)
	CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);

	// INTERSECT_LEN: GPS↔세그먼트 교차점(MATCH_LAT/LON) 하버사인 거리(m) → 정수 반올림
	const bool bHasCoords = (bMatched || bOut);
	int nIntersectLen = -1;
	if (bHasCoords)
		nIntersectLen = CalcIntersectLen(stRawLogInfo,
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY);

	// DB 저장: MATCHED/SKIP 시 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·MATCH_LINK_ID. ERROR 는 미저장
	if (!AppendUpdateRow(pvtUpdates, stRawLogInfo, nFinalStatus, nIntersectLen,
		bHasCoords ? &stMatchLinkInfo.dfMatchY : nullptr,
		bHasCoords ? &stMatchLinkInfo.dfMatchX : nullptr,
		bHasCoords ? stMatchLinkInfo.qwLinkID : 0))
		return false;

	// END 이벤트면 MATCHED/ERROR/SKIP 무관 세션 종료 (bulk 성공 후 mapSessions.erase)
	//   bTrustedTripEnd 로 스퓨리어스 END 는 제외(2026-08-25)
	if (bTrustedTripEnd)
		*pbTripEnded = true;

	return true;
}

/**
 * @brief vtUpdates 에 PK 행 존재 여부 (배치 orphan 판별)
 * @param[in] vtUpdates bulk UPDATE 대상 목록
 * @param[in] strTripId 운행 ID (PK-1)
 * @param[in] strGpsSeq GPS 순번 (PK-2, 문자열)
 * @return true(포함), false(미포함)
*/
bool CRawLogWorker::IsRowInUpdates(const vector<RAW_LOG_UPDATE_ROW>& vtUpdates,
		const string& strTripId, const string& strGpsSeq)
{
	for (size_t i=0; i<vtUpdates.size(); ++i)
	{
		if (vtUpdates[i].strTripId == strTripId
			&& vtUpdates[i].strGpsSeq == strGpsSeq)
			return true;
	}
	return false;
}

/**
 * @brief 미처리 예약 행 release 1건 적재 [rawgps_update] $3=0
 * @param[out] pvtRelease release 대상 행 목록
 * @param[in] stRawLogInfo 원시 GPS (PK 추출용)
 * @return true(적재 성공), false(pvtRelease null·trip_id 무효)
 * @remark AppendUpdateRow 실패 등 vtUpdates 미포함 행의 PROCESSING 해제용 (#4)
*/
bool CRawLogWorker::AppendReleaseRowFromRawLog(vector<RAW_LOG_UPDATE_ROW> *pvtRelease,
		const sRawLogInfo& stRawLogInfo)
{
	if (pvtRelease == nullptr)
		return false;

	if (stRawLogInfo.szTripID[0] == '\0')
		return false;

	char szSeqNo[16];
	snprintf(szSeqNo, sizeof(szSeqNo), "%u", stRawLogInfo.dwSeqNo);

	RAW_LOG_UPDATE_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strGpsSeq = szSeqNo;
	stRow.strMatchStatus = "0";
	pvtRelease->push_back(stRow);
	return true;
}

/**
 * @brief RAW_LOG_INFO → MAP_MATCH_INPUT 변환 후 스레드별 ProcessManager 맵매칭
 * @param[in] nThreadId 워커 스레드 ID (ProcessManager 인덱스)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in,out] pstSession trip_id 세션 (qwLinkID 연속 맵매칭용)
 * @return true(매칭 성공), false(실패)
*/
bool CRawLogWorker::RunMapMatch(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, MATCH_LINK_INFO *pstMatchLinkInfo)
{
	if (pstSession == nullptr || pstMatchLinkInfo == nullptr
		|| m_stConfig.pcProcessManager == nullptr)
		return false;

	if (nThreadId < 0 || nThreadId >= m_stConfig.nWorkerThreads)
		return false;

	memset(reinterpret_cast<void *>(pstMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);
	pstMatchLinkInfo->dfIntersectLenSgmt = -1.0;
	CProcessManager& cProcessManager = m_stConfig.pcProcessManager[nThreadId];

	// ── HEADING/SPEED 보정: DB 적재값 우선, NULL(미적재) 이면 직전 매칭좌표로 계산 (2026-07-08 최정우 추가) ──
	//   · 전제 : 세션에 직전 "매칭 성공" 좌표(dfLastMatchX/Y)와 그 GPS 시각(dtLastMatchGps) 보유 시에만
	//   · nAngle < 0(NO_ANGLE) / fSpeed < 0(NO_SPEED) 가 곧 DB NULL 을 의미(파서에서 그렇게 세팅)
	// · 차이 가드: 직전 매칭점과의 시간간격이 (0, MM_CALC_MAX_GAP_SEC] 일 때만 계산(끊긴 구간 오계산 방지)
	//   · 원본 stRawLogInfo 는 불변 유지, 보정본(stAdjusted)으로 맵매칭 입력
	sRawLogInfo stAdjusted = stRawLogInfo;
	ALT_MATCH_CTX stAltCtx;
	if ((pstSession->bHasLastMatch) && 
		((stAdjusted.nAngle < 0) || (stAdjusted.fSpeed < 0.0f)))
	{
		double dfGapSec = difftime(stAdjusted.dtGPS, pstSession->dtLastMatchGps);
		if (dfGapSec > 0.0 && dfGapSec <= static_cast<double>(MM_CALC_MAX_GAP_SEC))
		{
			POINT stPrev; stPrev.dfX = pstSession->dfLastMatchX; stPrev.dfY = pstSession->dfLastMatchY;
			POINT stCur;  stCur.dfX  = stAdjusted.dfX;           stCur.dfY  = stAdjusted.dfY;
			// 직전·현재 GPS 하버사인 수평 이동거리(m) (2026-07-08 최정우 주석 추가)
			double dfMoveM = HaversineMeters(stPrev, stCur);
			stAltCtx.dfHorizMove = dfMoveM;

			if (stAdjusted.fSpeed < 0.0f)
				stAdjusted.fSpeed = static_cast<float>(dfMoveM / dfGapSec * 3.6);

			// 방위각 계산: 하한(MM_CALC_MIN_DIST) ≤ 이동거리, 상한([mapmatch] distance) ≥ 이동거리 일 때만 (2026-07-15 최정우 수정)
			//   상한 초과(예: 터널·수신두절 후 큰 점프)면 직선 이동방향이 실제 진행방향과 달라 heading 미사용
			if (stAdjusted.nAngle < 0 && dfMoveM >= MM_CALC_MIN_DIST &&
				((m_stConfig.nHeadingMaxDist <= 0) || (dfMoveM <= static_cast<double>(m_stConfig.nHeadingMaxDist))))
				// 직전·현재 좌표로 방위각(degree) 보정 (2026-07-08 최정우 주석 추가)
				stAdjusted.nAngle = m_cGISUtil.GetDirAngleDegree(stPrev, stCur);
		}
	}
	else if (pstSession->bHasLastMatch)
	{
		POINT stPrev; stPrev.dfX = pstSession->dfLastMatchX; stPrev.dfY = pstSession->dfLastMatchY;
		POINT stCur;  stCur.dfX  = stAdjusted.dfX;           stCur.dfY  = stAdjusted.dfY;
		// 고도 점수용 수평 이동거리(m) (2026-07-08 최정우 주석 추가)
		stAltCtx.dfHorizMove = HaversineMeters(stPrev, stCur);
	}

	// ── 고도 앵커 → 연속 맵매칭 컨텍스트 (Begin 미적용) ──
	//   · 전제: bHasPrevAlt (직전 매칭 성공 시 ALTITUDE_M 있었음)
	//   · dfHorizMove: 직전 매칭 XY → 현재 GPS XY 하버사인(m) — 경사 판정용
	//   · Δalt = 현재 ALTITUDE_M − nPrevAltitude (ProcessManager/ContinueMapMatch에서 사용)
	// · 예) seq10 매칭·고도100m 저장 → seq11 고도106m·같은 고가 → 차이=8 이내 보너스 −3
	if (pstSession->bHasPrevAlt)
	{
		stAltCtx.nPrevAltitude = pstSession->nPrevAltitude;
		stAltCtx.nPrevRoadType = pstSession->nPrevRoadType;
		stAltCtx.bHasPrevAlt = true;
	}

	// 직전 매칭 위치(같은 링크 내 역행 페널티용) — 고도 앵커와 무관하게 독립 전달 (2026-07-20 최정우 추가)
	if (pstSession->bHasPrevLinkPos)
	{
		stAltCtx.dfPrevLinkPos = pstSession->dfLastMatchLinkPos;
		stAltCtx.bHasPrevLinkPos = true;
		// 같은 링크 노이즈 보정(1m 전진) 시, 이번 후보 자신의 계산값이 아니라 마지막으로
		//   신뢰했던 실제 매칭 좌표를 기준점으로 삼기 위해 함께 전달 (2026-07-22 최정우 추가)
		//   단 "신뢰 가능한 매칭"이 한 번이라도 있었을 때만이다. bHasPrevLinkPos 는 매칭 성공이면
		//   무조건 서지만 dfLastMatchX/Y 는 신뢰 매칭에서만 갱신되므로, 트립 앞부분이 전부
		//   SKIP 이면 0 인 채로 넘어가 보정 좌표가 (0,0) 이 된다 (2026-08-23 최정우 수정)
		stAltCtx.bHasPrevMatchPos = pstSession->bHasLastMatch;
		if (pstSession->bHasLastMatch)
		{
			stAltCtx.dfPrevMatchX = pstSession->dfLastMatchX;
			stAltCtx.dfPrevMatchY = pstSession->dfLastMatchY;
		}
	}

	// 연속 맵매칭 링크는 "맵매칭 성공(반경 내 MATCHED)" 시에만 세션에 반영한다.
	//   SKIP(정확도/반경 밖)·ERROR 시 직전 성공 링크를 그대로 유지 → 다음 GPS 는 마지막 성공 링크
	//   기준으로 연속 맵매칭을 이어간다. (ProcessRawLog 는 실패 시 로컬 링크를 0으로 리셋하므로
	//   세션 링크에 반영되지 않도록 로컬 복사본으로 호출) (2026-07-10 최정우 수정)
	uint64 qwLinkID = pstSession->qwLinkID;
	bool bMatched = cProcessManager.ProcessRawLog(stAdjusted, qwLinkID, pstMatchLinkInfo,
		(stAltCtx.bHasPrevAlt || stAltCtx.bHasPrevLinkPos) ? &stAltCtx : nullptr);
	if (bMatched)
	{
		double dfNewLinkPos = static_cast<double>(pstMatchLinkInfo->wLenFromLink)
			+ pstMatchLinkInfo->dfSgmtMatchLen;

		// 연속 역행 스트릭 갱신 — bReverseSuspect(위치 역행 + heading 도 역방향 일치) 연속 횟수를 센다.
		//   GPS 노이즈성 흔들림(heading은 여전히 정방향)은 스트릭에 안 잡히고 실제 역행(heading도 반대)만
		//   잡히게 함 (2026-07-21 최정우 수정 — heading 대조 결합)
		//   reverse_confirm 미만이면 노이즈로 보고 앵커(dfLastMatchLinkPos) 고정 — 다음 포인트도
		//   같은 기준점과 비교돼 판정이 안 흔들린다. reverse_confirm 이상이면 실제 이동으로 확정하고
		//   앵커를 지금 위치로 재설정해 정상 추적을 재개한다 (2026-07-21 최정우 추가 — dip 판정 대체)
		if (pstMatchLinkInfo->bReverseSuspect)
			pstSession->nReverseStreak += 1;
		else
			pstSession->nReverseStreak = 0;

		const bool bConfirmed = (pstSession->nReverseStreak >= m_stConfig.nReverseConfirm);

		pstSession->qwLinkID = qwLinkID;		// 성공 시에만 링크 전진(다음 점 연속 매칭 기준)
		if (!pstMatchLinkInfo->bReverseSuspect || bConfirmed)
			pstSession->dfLastMatchLinkPos = dfNewLinkPos;
		pstSession->bHasPrevLinkPos = true;
	}
	return bMatched;
}

/**
 * @brief YYYYMMDDHH24MISS 문자열 생성 (로컬 시각) (2026-08-12 최정우 추가)
 * @param[in] dtValue time_t
 * @return 14자 일시 문자열 (dtValue<=0 이면 빈 문자열)
 * @remark CRawLogFetcher::ParseDateTime() 의 역변환 — PRIM_CHARGEHAND occur_dt/trip_start_dt 저장용
*/
string CRawLogWorker::FormatDateTime14(time_t dtValue)
{
	if (dtValue <= 0)
		return string();

	struct tm stTm;
	localtime_r(&dtValue, &stTm);

	char szBuf[32];
	snprintf(szBuf, sizeof(szBuf), "%04d%02d%02d%02d%02d%02d",
		stTm.tm_year + 1900, stTm.tm_mon + 1, stTm.tm_mday,
		stTm.tm_hour, stTm.tm_min, stTm.tm_sec);
	return string(szBuf);
}

/**
 * @brief TRIP_ID({6자리 숫자}_{YYYYMMDDHH24MISS}) 에서 시각 부분만 추출 (2026-08-19 최정우 추가)
 * @param[in] szTripId TRIP_ID 문자열
 * @return 성공 시 첫 '_' 다음 문자열 포인터(szTripId 내부를 가리킴 — 수명은 szTripId 와 동일),
 *   '_'가 없거나 그 뒤에 아무것도 없으면 nullptr
 * @remark 기존엔 DEVICE_KEY 길이만큼 건너뛰는 방식(TRIP_ID = DEVICE_KEY_시각 가정)이었으나,
 *   TRIP_ID 포맷이 CAR_SEQ_NO(6자리)_시각 로 바뀌면서 DEVICE_KEY 와 길이가 달라져 엉뚱한
 *   위치부터 잘리는 버그가 있었음(원격 DB 실측으로 확인 — 예: trip_id=000376_20260819094414,
 *   device_key=CAR000434(9자) 인데 trip_start_dt=60819094414 로 저장됨). TRIP_ID 안의 첫
 *   '_' 위치를 직접 찾는 방식으로 교체 — 포맷이 다시 바뀌어도(접두어 길이 변경) 안전.
*/
const char* CRawLogWorker::ExtractTripStartDt(const char *szTripId)
{
	if ((szTripId == nullptr) || (szTripId[0] == '\0'))
		return nullptr;

	const char *pszUnderscore = strchr(szTripId, '_');
	if ((pszUnderscore == nullptr) || (pszUnderscore[1] == '\0'))
		return nullptr;

	return pszUnderscore + 1;
}

/**
 * @brief 개방형 구역의 M게이트를 이 진행(run) 동안 실제로 지났는지 갱신(래치 — 한 번 true 면
 *   재확인 안 함) (2026-08-25 최정우 추가)
 * @remark 단순히 "지금 위치가 게이트 지점 이후인가"만 보면 안 된다 — case C(트립이 이미 게이트를
 *   지난 뒤 시작)는 시작하자마자 그 조건을 항상 만족해버려서 첫 틱부터 "통과함"으로 오판된다
 *   (실측 000376/900202 합성테스트로 발견, 2026-08-25). 그래서 "이 run 안에서 게이트 이전
 *   상태를 실제로 거쳐왔는가"(bSeenBeforeGate)를 먼저 확인하고, 그 상태에서 게이트 위치에
 *   도달했을 때만 "진짜 통과"로 확정한다. 판정 순서:
 *   (1) 경유 링크(aqwPathLinkIDs)에 게이트 링크가 있으면 이미 완전히 지나온 것으로 확정
 *       (그 경유 링크에 도달했다는 것 자체가 그 이전엔 게이트 이전 위치였다는 뜻이라 안전)
 *   (2) 이번 확정 링크가 게이트 링크가 아닌 이 구역의 다른 링크면 — 개방형 구역은 단방향
 *       코리도라는 전제로 "게이트 이전" 상태로 기록만 해둔다(RL-Z00004 등 실측 구조상 게이트
 *       앞뒤로 링크가 나뉘어 있음)
 *   (3) 이번 확정 링크가 게이트 자신의 링크면 "링크 시작점부터 게이트까지 거리" vs "링크
 *       시작점부터 현재 매칭 위치까지 거리"를 비교(-3m 오차 허용) — 아직 못 미쳤으면 "게이트
 *       이전" 기록만, 이미 도달/통과했으면 bSeenBeforeGate 가 true 일 때만 "진짜 통과"로 확정
 *   HaversineMeters() 가 private static 이라 CollectGateCandidates() 처럼 파일지역 static 함수로
 *   못 두고 멤버 함수로 둠
*/
void CRawLogWorker::UpdateOpenGateCrossed(const MATCH_LINK_INFO& stMatchLinkInfo, ZONE_RUN_SESSION *pstRun)
{
	if (pstRun->bGateCrossed) return;
	if (m_stConfig.pcChargeDataLoader == nullptr) return;

	PGATE_INFO pstGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(pstRun->szRoadID, 'M');
	if (pstGate == nullptr) return;			// 이 구역엔 등록된 M게이트가 없음(방어적)

	// aqwPathLinkIDs 의 마지막 원소는 이번 확정 링크(stMatchLinkInfo.qwLinkID)와 항상 같다
	//   (CollectGateCandidatesOnIntermediateLinks() 주석 참고) — 그래서 "이미 완전히 지나온
	//   경유 링크"만 보려면 마지막 하나는 반드시 제외해야 한다. 안 그러면 이번 확정 링크가
	//   게이트 링크 자신인 매 틱마다 이 루프가 자기 자신과 매치돼 무조건 true 가 되어 버려서,
	//   case C(이미 게이트 지난 뒤 출발) 의 두 번째 틱부터 곧바로 "통과함"으로 오판된다(실측
	//   000376/900202 합성테스트로 발견, 2026-08-25)
	for (uint8 i = 0; (i + 1) < stMatchLinkInfo.nPathLinkCount; ++i)
	{
		if (stMatchLinkInfo.aqwPathLinkIDs[i] == pstGate->qwLinkID)
		{ pstRun->bGateCrossed = true; return; }
	}

	if (stMatchLinkInfo.qwLinkID != pstGate->qwLinkID)
	{
		pstRun->bSeenBeforeGate = true;			// 게이트 아닌 구역 내 다른(=상류) 링크 위
		return;
	}

	POINT stLinkStart, stGatePos;
	stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
	stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
	stGatePos.dfX = pstGate->dfLon;
	stGatePos.dfY = pstGate->dfLat;
	double dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);
	double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

	if (dfCurPosOnLink < (dfGatePosOnLink - 3.0))
	{
		pstRun->bSeenBeforeGate = true;			// 아직 게이트 전 — 나중에 지나면 확정할 근거
		return;
	}

	if (pstRun->bSeenBeforeGate)
		pstRun->bGateCrossed = true;				// 게이트 전이었다가 지금 지남 확인 — 진짜 통과
	// bSeenBeforeGate 가 false 면(=이 run 안에서 게이트 전 상태를 본 적이 없음) 지금 위치가
	//   게이트 지난 곳이어도 확정 안 함 — 트립이 이미 게이트를 지난 뒤 시작했을 가능성(case C)
}

/**
 * @brief 개방형(ROAD_KIND=1) 구역 진입/이탈 판정 (2026-08-25 최정우 재작성)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — vtOpenRuns/nChargeSeq 갱신
 * @param[out] pvtChargeInserts 정상 이탈·트립종료 시 1행씩 적재 (일반도로 NODE_STEP과 동일 구조)
 * @return void
 * @remark 원래는 게이트 통과 순간의 점 이벤트였으나, 주행거리(dist_m)·주행시간(stay_seconds)을
 *   함께 적재하기 위해 일반도로와 동일한 "구역 진입~이탈" 구조로 전환(사용자 지시). 게이트는
 *   여전히 있고 UpdateOpenGateCrossed()가 이 진행(run) 동안 실제로 지났는지를 매 틱 갱신 —
 *   BuildOpenZoneRow()가 이 값과 bStartedByTrip 으로 dist_m 산출 방식·charge_yn/status 를 정한다.
 *   트립종료(TRIP_EVENT=2)는 일반도로와 동일하게 디바운스 없이 즉시 마감(다음 GPS가 안 올 수 있음)
*/
void CRawLogWorker::ProcessOpenGateCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경유 링크 포함 전체 경로에서 개방형 구역을 "전부" 수집(NODE_STEP과 동일 패턴, 2026-08-25 최정우 추가)
	vector<PZONE_INFO> vtZones;
	{
		vector<PZONE_INFO> vtOne;
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
			m_stConfig.pcChargeDataLoader->GetOpenZonesByLinkId(stMatchLinkInfo.qwLinkID, &vtOne);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
				m_stConfig.pcChargeDataLoader->GetOpenZonesByLinkId(
					stMatchLinkInfo.aqwPathLinkIDs[i], &vtOne);
		}
		for (size_t i = 0; i < vtOne.size(); ++i)			// road_id 기준 중복 제거
		{
			bool bDup = false;
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(vtZones[e]->szRoadID, vtOne[i]->szRoadID) == 0) { bDup = true; break; }
			}
			if (!bDup) vtZones.push_back(vtOne[i]);
		}
	}

	const bool bTripEnding = bTrustedTripEnd;

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtOpenRuns.size(); )
	{
		ZONE_RUN_SESSION& stRun = pstSession->vtOpenRuns[si];

		UpdateOpenGateCrossed(stMatchLinkInfo, &stRun);

		bool bSameZone = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
		}

		// 누적 이동거리 — bStartedByTrip(부분거리) run 에만 의미 있지만, 정상진입 run 도 계산 자체는
		//   해둔다(사용 안 하고 버릴 뿐, 분기 단순화)
		if (bSameZone)
		{
			stRun.nExitTicks = 0;
			POINT stPrev, stCur;
			stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
			stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;
			stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
			stRun.dfLastX = stMatchLinkInfo.dfMatchX;
			stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		}
		else if (stRun.nExitTicks == 0)
		{
			// 이탈 디바운스 스트릭의 첫 "밖" tick — ProcessNodeStepCharge() 동일 근거(dfFirstOutX/Y
			//   필드 주석 참고, 2026-08-25 최정우 추가)
			stRun.dfFirstOutX = stMatchLinkInfo.dfMatchX;
			stRun.dfFirstOutY = stMatchLinkInfo.dfMatchY;
			stRun.dtFirstOut = stRawLogInfo.dtGPS;
		}

		if (bSameZone && !bTripEnding) { ++si; continue; }		// 계속 진행 중

		// node_exitcnt 재사용 — 순간 오매칭 1틱으로 세션이 쪼개지는 것 방지(NODE_STEP과 동일 원리).
		//   트립종료는 디바운스 없이 즉시 마감(다음 틱이 안 옴)
		if (!bTripEnding)
		{
			stRun.nExitTicks += 1;
			if (stRun.nExitTicks < m_stConfig.nNodeExitCnt) { ++si; continue; }
		}

		// 이탈 지점 보정 — 구역 안에서 마지막으로 달린 링크의 종료 노드까지 거리를 채운다(NODE_STEP과
		//   동일 원리). 거리 보정 자체는 bStartedByTrip(부분거리) run 에만 의미 있음 — 정상진입 run 은
		//   dist_m 이 구역 전체길이 고정값이라 무관. 단 "시각" 보간(dtOpenExitTime)은 두 run 유형
		//   모두에 적용한다 — 정상진입 run 도 stay_seconds/speed_kmh 는 dtEnd 기준으로 계산되므로
		//   똑같이 부정확할 수 있음(사용자 지시, 2026-08-25 최정우 추가 — CLOSED/SPEED 의
		//   InterpolateGateCrossingTime() 동일 로직을 "구역 경계 노드"를 목표점 삼아 재사용)
		time_t dtOpenExitTime = (stRun.dtLastInZoneTime != 0) ? stRun.dtLastInZoneTime : stRawLogInfo.dtGPS;
		if (!bSameZone && (stRun.qwLastLinkID != 0) && (m_stConfig.pcDataLoader != nullptr))
		{
			PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(stRun.qwLastLinkID);
			if (pstLastLink != nullptr)
			{
				POINT stFrom, stNode;
				stFrom.dfX = stRun.dfLastX;  stFrom.dfY = stRun.dfLastY;
				stNode.dfX = static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0;
				stNode.dfY = static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0;

				if (stRun.bStartedByTrip)
				{
					double dfTail = HaversineMeters(stFrom, stNode);
					if ((dfTail > 0.0) && (dfTail <= pstLastLink->dfLen + 1.0))
					{
						stRun.dfAccumDistM += dfTail;
						stRun.dfLastX = stNode.dfX;
						stRun.dfLastY = stNode.dfY;
					}
				}

				// "밖" 기준점은 세션 범용 직전tick이 아니라 dfFirstOutX/Y 를 써야 한다 —
				//   ProcessNodeStepCharge() 동일 근거(node_exitcnt 디바운스 구간 오염 방지,
				//   2026-08-25 최정우 추가)
				if (stRun.dtFirstOut != 0)
				{
					dtOpenExitTime = InterpolateGateCrossingTime(
						stFrom.dfX, stFrom.dfY, stRun.dtLastInZoneTime,
						stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut,
						stNode.dfX, stNode.dfY);
				}
			}
		}

		CHARGE_INSERT_ROW stRow;
		BuildOpenZoneRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, dtOpenExitTime, &stRow);
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] open zone exit recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"started_by_trip=[%d] gate_crossed=[%d] dist_m=[%s] avg_speed=[%s] trip_ending=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, static_cast<int>(stRun.bStartedByTrip), static_cast<int>(stRun.bGateCrossed),
			stRow.strDistM.c_str(), stRow.strSpeedKmh.c_str(), static_cast<int>(bTripEnding));

		pstSession->nChargeSeq += 1;
		pstSession->vtOpenRuns.erase(pstSession->vtOpenRuns.begin() + si);
	}

	// ── ② 새로 진입한 구역 세션 개시 ─────────────────────────────────────────
	if (bTripEnding)
		return;											// 종료 틱에서는 새로 열지 않는다

	// 트립 자체가 이 구역 도로 위에서 시작했는지 — TRIP_EVENT=START 행의 매칭 링크가 이미 구역
	//   안이면 게이트를 지나온 적 없는 상태로 진행이 시작된 것(사용자 지시, 2026-08-25 추가)
	const bool bTripStarting = (stRawLogInfo.nTripEvent == TRIP_EVENT_START);

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtOpenRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtOpenRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;

		ZONE_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		// 진입 경계(구역에 들어온 링크의 시작 노드) 통과 시각 보간 — 위 dtOpenExitTime 동일 근거
		//   참고. 트립시작(bTripStarting) run 은 겨냥할 "직전(구역 밖) tick" 자체가 없어 대상 아님
		//   (2026-08-25 최정우 추가, 사용자 지시)
		if (!bTripStarting && pstSession->bHasLastMatch)
		{
			stRun.dtEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				stMatchLinkInfo.dfStNodeX, stMatchLinkInfo.dfStNodeY);
		}
		else
		{
			stRun.dtEntryTime = stRawLogInfo.dtGPS;
		}
		stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
		stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
		stRun.dfAccumDistM = 0.0;
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;
		stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
		stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		stRun.bStartedByTrip = bTripStarting;

		if (bTripStarting)
		{
			// 트립시작 run 은 "이미 지나온 뒤 시작"(case C)과 "지금 막 그 지점에서 시작"(case B)을
			//   구분해야 한다 — 이전 틱이 없어 UpdateOpenGateCrossed()의 "reached-or-passed"(한쪽
			//   방향) 판정을 그대로 쓰면 게이트보다 훨씬 뒤에서 시작해도 무조건 "지났다"로 오판된다.
			//   대칭 허용오차(±3m)로 "지금 막 그 지점"만 인정 — 그보다 뚜렷이 앞서면(이미 지남)
			//   미인정, 뚜렷이 뒤면(아직 못 지남) 당연히 미인정(사용자 지시, 2026-08-25 추가)
			PGATE_INFO pstGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(stRun.szRoadID, 'M');
			if ((pstGate != nullptr) && (pstGate->qwLinkID == stMatchLinkInfo.qwLinkID))
			{
				POINT stLinkStart, stGatePos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stGatePos.dfX = pstGate->dfLon;
				stGatePos.dfY = pstGate->dfLat;
				double dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;
				if (fabs(dfCurPosOnLink - dfGatePosOnLink) <= 3.0)
					stRun.bGateCrossed = true;
			}
			// 게이트가 다른 링크면(=이 링크엔 아예 없음) 이 시작 틱만으론 판단 불가 — false 유지,
			//   뒤이은 틱에서 UpdateOpenGateCrossed()가 정상 갱신(그 시점부턴 "이 run 안에서 계속
			//   있었다"는 전제가 성립하므로 한쪽 방향 판정이 맞다)
		}
		else
		{
			// 정상진입(case A) run — 직전 틱까지 구역 밖에 있었다는 게 보장되므로 한쪽 방향
			//   판정(경유 링크 포함)을 그대로 써도 안전
			UpdateOpenGateCrossed(stMatchLinkInfo, &stRun);
		}
		pstSession->vtOpenRuns.push_back(stRun);

		LOGFMTI("[#%02d] open zone entry!device=[%s] trip_id=[%s] road=[%s] started_by_trip=[%d] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			static_cast<int>(bTripStarting), pstSession->vtOpenRuns.size());
	}
}

/**
 * @brief link_id+gate_div 로 게이트 후보 수집 — 'B'(양방향 겸용) 게이트도 함께 포함 (2026-08-13 최정우 추가)
 * @remark base_tollgate.gate_div CHECK 제약이 M/I/O/B 를 전부 허용하는데, 코드에선 I/O만 취급하고
 *   있었음 — 'B' 는 "이 한 게이트가 자기 road_id 기준으로 입구·출구 양쪽 다 될 수 있다"는 의미로
 *   해석해 cGateDiv 조회에 항상 같이 포함시킴. 폐쇄형/구간단속 둘 다 사용
 * @param[in] qwLinkID 매칭 링크 ID
 * @param[in] cGateDiv 찾는 방향('I' 또는 'O')
 * @param[out] pvtOut 일치하는 게이트 포인터 목록(cGateDiv 일치 + 'B' 전부)
*/
static void CollectGateCandidates(CChargeDataLoader *pcLoader, const uint64 qwLinkID,
		const char cGateDiv, vector<PGATE_INFO> *pvtOut)
{
	pvtOut->clear();
	vector<PGATE_INFO> vtExact, vtBoth;
	pcLoader->GetGatesByLinkId(qwLinkID, cGateDiv, &vtExact);
	pcLoader->GetGatesByLinkId(qwLinkID, 'B', &vtBoth);
	pvtOut->insert(pvtOut->end(), vtExact.begin(), vtExact.end());
	pvtOut->insert(pvtOut->end(), vtBoth.begin(), vtBoth.end());
}

/**
 * @brief 경유(이미 완전히 통과한) 링크에서만 게이트 후보 수집 — stMatchLinkInfo.aqwPathLinkIDs 중
 *   마지막(=이번 확정 링크, stMatchLinkInfo.qwLinkID 와 동일) 하나를 제외한 앞쪽 링크들만 순회
 *   (2026-08-20 최정우 추가)
 * @remark 진출(exit) 판정은 원래 "이번 확정 링크 안에서 게이트 지점을 실제로 지났는지" 링크 내
 *   위치(wLenFromLink+dfSgmtMatchLen) 비교가 필요했는데, 경유 링크는 정의상 이미 링크 전체를
 *   통과 완료한 구간이라 그 위의 게이트는 위치 비교 없이 무조건 "통과"로 확정할 수 있음 — GPS
 *   수신 주기가 늘어나 짧은 게이트 링크가 두 GPS 포인트 사이에 통째로 끼어버리는 경우 보완
 * @param[in] nPathCount stMatchLinkInfo.nPathLinkCount(0 또는 1이면 경유 링크 없음 — 결과 항상 빈 목록)
*/
static void CollectGateCandidatesOnIntermediateLinks(CChargeDataLoader *pcLoader,
		const uint64 *paqwPathLinkIDs, const uint8 nPathCount, const char cGateDiv,
		vector<PGATE_INFO> *pvtOut)
{
	pvtOut->clear();
	if (nPathCount < 2)
		return;
	for (uint8 i = 0; i < static_cast<uint8>(nPathCount - 1); ++i)
	{
		vector<PGATE_INFO> vtOne;
		CollectGateCandidates(pcLoader, paqwPathLinkIDs[i], cGateDiv, &vtOne);
		pvtOut->insert(pvtOut->end(), vtOne.begin(), vtOne.end());
	}
}

/**
 * @brief 이번 확정 링크(경유 링크 포함)가 그 구역의 link_ids 안에 있는지 — 게이트를 못 찾은 채
 *   구역을 "확정 이탈"했는지 판정하는 보조 수단 (2026-08-25 최정우 추가)
 * @remark CLOSED_ROAD/SPEED 전용 — 진행 중인 구역의 road_id 를 이미 알고 있는 상태에서 "그 구역
 *   안에 계속 있는가"만 확인하면 되므로, NODE_STEP/EXEMPT/OPEN처럼 link_id→구역 전체 역인덱스를
 *   따로 만들 필요 없이 GetZoneByRoadId() 로 얻은 그 zone 하나의 vtLinkIds 만 선형 탐색한다.
*/
static bool IsMatchedLinkInZone(PZONE_INFO pstZone, const MATCH_LINK_INFO& stMatchLinkInfo)
{
	if ((pstZone == nullptr) || pstZone->vtLinkIds.empty())
		return false;

	for (size_t z = 0; z < pstZone->vtLinkIds.size(); ++z)
	{
		if (pstZone->vtLinkIds[z] == stMatchLinkInfo.qwLinkID)
			return true;
		for (uint8 p = 0; p < stMatchLinkInfo.nPathLinkCount; ++p)
		{
			if (pstZone->vtLinkIds[z] == stMatchLinkInfo.aqwPathLinkIDs[p])
				return true;
		}
	}
	return false;
}

/**
 * @brief 폐쇄형(CLOSED_ROAD) 입/출구 게이트 판정 (2026-08-12 최정우 추가, 2026-08-13 재작성)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — 진입 상태(bInClosedRoad 등) 갱신
 * @param[out] pvtChargeInserts 출구 통과 시 1행 적재 (bulk INSERT 는 run() 이 배치 종료 시 일괄 실행)
 * @return void
 * @remark 2026-08-13 재작성 — 사용자 질문으로 발견된 3가지 한계 대응:
 *   (1) 한 링크에 같은 방향(I 또는 O) 게이트가 2개 이상 있는 경우 — 기존 GetGateByLinkId(첫 매치만
 *       반환)를 GetGatesByLinkId(전부 수집) 기반 CollectGateCandidates() 로 교체, 활성 세션의
 *       road_id 와 실제로 일치하는 게이트를 후보 중에서 직접 찾음(개방형 멀티게이트 수정과 동일 원리)
 *   (2) 링크는 다르지만 출구→도로→입구 패턴이 연속으로 여러 번 존재 — 원래도 지원됨(세션이 매
 *       구간마다 완전히 리셋되므로), 이번 재작성으로도 유지
 *   (3) 한 게이트가 출구+입구를 겸하는 경우(gate_div='B', base_tollgate CHECK 제약엔 이미 있었지만
 *       코드가 안 씀) — CollectGateCandidates() 가 'B' 게이트를 I/O 조회 양쪽에 항상 포함
 *   추가로 "방금 출구 처리한 link_id 에서 즉시 재진입 금지" 가드를 link_id 만이 아니라 **road_id
 *   까지 같이 비교**하도록 정정 — 기존엔 link_id 만 비교해서 "같은 링크의 다른 구역 입구 게이트"까지
 *   같이 막아버리는 문제가 있었음(예: A구역 출구와 B구역 입구가 같은 링크에 있는 경우, A 출구 직후
 *   B 입구까지 막힘). road_id 도 같아야만(=진짜 자기 자신 재진입) 막도록 수정.
 *   이 가드 덕분에 이제 "같은 tick 에서 A구역 출구 처리 직후 곧바로 B구역 입구 처리"까지 자연스럽게
 *   이어짐(진출 처리 후 return 하지 않고 바로 진입 후보 검사로 넘어가는 구조로 변경).
*/
void CRawLogWorker::ProcessClosedRoadCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 진출 처리 먼저 시도 — 이미 진입해 있는 상태일 때만. 이 링크의 출구(O/B) 게이트 후보 전부를
	//   모아 활성 세션의 road_id 와 실제로 일치하는 것을 찾음(같은 링크에 다른 구역 출구 게이트가
	//   섞여 있어도 정확히 골라냄) (2026-08-13 최정우 재작성)
	if (pstSession->bInClosedRoad)
	{
		// 실시간 누적거리·마지막 위치 갱신 — 게이트 확정 여부와 무관하게 매 틱 항상 최신으로
		//   유지한다. 게이트 기반 정상 종료는 여전히 ZONE_INFO.dfLengthM(고정값)을 쓰지만, 게이트를
		//   못 찾고 구역을 벗어나거나(아래 else 분기) TTL/트립종료로 강제마감되는 경우
		//   (AppendExpiredClosedRoadCharge)엔 이 실측값이 유일한 근거라 "게이트 대기 중"이었던
		//   틱들도 빠짐없이 누적돼 있어야 한다(실측 000376_20260819140856 RL-Z00003 — 게이트를
		//   찾긴 했지만 위치 미도달로 대기만 하다 트립이 끝난 케이스에서 처음엔 이 갱신이 빠져
		//   dist_m=0으로 남는 문제 발견, 2026-08-25 최정우 추가)
		{
			POINT stPrevPos, stCurPos;
			stPrevPos.dfX = pstSession->dfClosedLastX;  stPrevPos.dfY = pstSession->dfClosedLastY;
			stCurPos.dfX = stMatchLinkInfo.dfMatchX;    stCurPos.dfY = stMatchLinkInfo.dfMatchY;
			pstSession->dfClosedAccumDistM += HaversineMeters(stPrevPos, stCurPos);
			pstSession->dfClosedLastX = stMatchLinkInfo.dfMatchX;
			pstSession->dfClosedLastY = stMatchLinkInfo.dfMatchY;
		}

		// "구역 안에서 마지막으로 확인된 링크/시각" 갱신 — qwLastConfirmedLinkID 가 트립 시작
		//   후보(A/B) 판정 중이라 아직 0인 상태로 진입~이탈이 한 tick 만에 벌어지면(실측
		//   000376_20260819140856) 아래 bExitOnPrevLink 판정이 qwLastConfirmedLinkID 만으론
		//   출구 게이트를 못 찾는다 — 그 대체 근거로 매 틱 갱신(2026-08-25 최정우 추가)
		{
			PZONE_INFO pstTrackingZoneForUpdate = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);
			if (IsMatchedLinkInZone(pstTrackingZoneForUpdate, stMatchLinkInfo))
			{
				pstSession->qwClosedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
				pstSession->dtClosedLastZoneTime = stRawLogInfo.dtGPS;
			}
		}

		// 경유(이미 완전 통과) 링크들을 먼저 확인 — 발견되면 위치 판정 없이 무조건 통과로 확정
		//   (2026-08-20 최정우 추가, 근거는 CollectGateCandidatesOnIntermediateLinks() 주석 참고)
		PGATE_INFO pstExitGate = nullptr;
		bool bExitOnIntermediate = false;

		vector<PGATE_INFO> vtIntermediateExit;
		CollectGateCandidatesOnIntermediateLinks(m_stConfig.pcChargeDataLoader,
			stMatchLinkInfo.aqwPathLinkIDs, stMatchLinkInfo.nPathLinkCount, 'O', &vtIntermediateExit);
		for (size_t i = 0; i < vtIntermediateExit.size(); ++i)
		{
			if (strcmp(pstSession->szClosedRoadId, vtIntermediateExit[i]->szRoadID) == 0)
			{
				pstExitGate = vtIntermediateExit[i];
				bExitOnIntermediate = true;
				break;
			}
		}

		if (pstExitGate == nullptr)
		{
			vector<PGATE_INFO> vtExitCandidates;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtExitCandidates);

			for (size_t i = 0; i < vtExitCandidates.size(); ++i)
			{
				if (strcmp(pstSession->szClosedRoadId, vtExitCandidates[i]->szRoadID) == 0)
				{
					pstExitGate = vtExitCandidates[i];
					break;
				}
			}
		}

		// 직전 확정 링크에 출구 게이트가 있었는데 그 안에서 위치 3m 이내로 못 잡은 경우 — 다음
		//   틱이 아예 다른 링크로 확정됐다는 것 자체가 그 링크를 끝까지 지나왔다는 뜻이므로 위치
		//   판정 없이 확정한다. 자세한 배경은 ProcessSpeedZoneCharge() 동일 로직 주석 참고
		//   (2026-08-24 최정우 추가)
		// 후보 링크 두 개를 순서대로 시도 — (1) qwLastConfirmedLinkID, (2) qwClosedLastZoneLinkID.
		//   qwLastConfirmedLinkID 는 트립 시작 후보(A/B) 판정이 아직 안 끝났으면 0에 머물러 있어
		//   못 쓰는 경우가 있다(실측 000376_20260819140856 — 트립 첫 tick 이 이미 구역 안이라
		//   그 tick 이 바로 진입+구역 내 유일한 링크였는데, 다음 tick 에 완전히 다른 링크로 넘어가며
		//   출구를 못 잡고 "게이트 미확인 이탈"로 빠짐). qwClosedLastZoneLinkID 는 그 대체용으로
		//   "실제로 이 구역 안에서 마지막으로 확인됐던 링크"를 추적한 값이라 이 경우에도 유효
		//   (2026-08-25 최정우 추가, 사용자 지시 — "다음 맵매칭 좌표/링크로 진출 확인 가능")
		bool bExitOnPrevLink = false;
		uint64 qwExitPrevLinkID = 0;
		time_t dtExitPrevLinkTime = 0;
		if (pstExitGate == nullptr)
		{
			const uint64 aqwPrevCand[2] = { pstSession->qwLastConfirmedLinkID, pstSession->qwClosedLastZoneLinkID };
			const time_t adtPrevCandTime[2] = { pstSession->dtLastConfirmedLinkTime, pstSession->dtClosedLastZoneTime };
			for (int c = 0; (c < 2) && (pstExitGate == nullptr); ++c)
			{
				if ((aqwPrevCand[c] == 0) || (aqwPrevCand[c] == stMatchLinkInfo.qwLinkID))
					continue;

				vector<PGATE_INFO> vtPrevExit;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, aqwPrevCand[c], 'O', &vtPrevExit);
				for (size_t i = 0; i < vtPrevExit.size(); ++i)
				{
					if (strcmp(pstSession->szClosedRoadId, vtPrevExit[i]->szRoadID) == 0)
					{
						pstExitGate = vtPrevExit[i];
						bExitOnPrevLink = true;
						qwExitPrevLinkID = aqwPrevCand[c];
						dtExitPrevLinkTime = adtPrevCandTime[c];
						break;
					}
				}
			}
		}

		if (pstExitGate != nullptr)
		{
			// 위치(구간 내 지점) 비교는 "이번 확정 링크 안에서 아직 진행 중"인 경우에만 필요 —
			//   경유 링크에서 찾았으면 이미 링크 전체를 지났으므로 생략 (2026-08-20 최정우 추가)
			if (!bExitOnIntermediate && !bExitOnPrevLink)
			{
				POINT stLinkStart, stGatePos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stGatePos.dfX = pstExitGate->dfLon;
				stGatePos.dfY = pstExitGate->dfLat;
				double dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				// 출구 게이트 지점을 아직 안 지났으면(약간의 오차 허용 -3m) 이번 tick 은 통과 전 —
				//   누적거리만 반영하고 종료 대기, 진입 후보 검사도 아직은 무의미하므로 여기서 끝 (2026-08-12 최정우 추가)
				if (dfCurPosOnLink < (dfGatePosOnLink - 3.0))
					return;
			}

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "2";							// CLOSED_ROAD
			stRow.strChargeUnit = "1";							// LINK (실측 확인)
			stRow.strLinkId = "";

			stRow.strFromId = pstSession->szEntryTollgateId;
			stRow.strToId = pstExitGate->szTollgateID;

			// 입/출구 게이트 이상(둘이 같거나 하나라도 비어있음) — 정상 과금 대상 아님으로 표시.
			//   charge_status=3(AUDIT=심사대상) — 완전 배제(SKIP=4)가 아니라 사람이 재확인하도록 표시
			//   (사용자 지시, 2026-08-13 — 원래 4(SKIP)였다가 3(AUDIT)으로 정정)
			bool bGateAnomaly = (pstSession->szEntryTollgateId[0] == '\0') ||
				(pstExitGate->szTollgateID[0] == '\0') ||
				(strcmp(pstSession->szEntryTollgateId, pstExitGate->szTollgateID) == 0);
			stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
			stRow.strChargeStatus = bGateAnomaly ? "3" : "0";
			if (bGateAnomaly)
			{
				LOGFMTW("[#%02d] closed road gate anomaly!device=[%s] trip_id=[%s] entry=[%s] exit=[%s] -> charge_yn=N",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					pstSession->szEntryTollgateId, pstExitGate->szTollgateID);
			}

			// 출구 게이트 통과 시각 보간 — 직전 확정 tick~이번 tick 사이에서 게이트를 지났을 시각을
			//   추정한다(사용자 지시, 2026-08-25 최정우 추가). bExitOnPrevLink 로 확정된 경우 이번
			//   틱의 stMatchLinkInfo 는 이미 다른(무관한) 링크를 가리키지만, 보간 기준으로는 오히려
			//   "그 무관한 링크까지 이동한 시각"이 곧 게이트를 확실히 지난 시점이라 유효한 종점이 됨.
			//   직전 tick 이 없으면(트립 첫 tick) 예전처럼 dtExitPrevLinkTime/현재시각 그대로 사용
			//   (원래 방식: 2026-08-24 최정우 추가, dtExitPrevLinkTime 로 일반화 2026-08-25)
			time_t dtExitTime;
			if (pstSession->bHasLastMatch)
			{
				dtExitTime = InterpolateGateCrossingTime(
					pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
					pstExitGate->dfLon, pstExitGate->dfLat);
			}
			else
			{
				dtExitTime = bExitOnPrevLink ? dtExitPrevLinkTime : stRawLogInfo.dtGPS;
			}

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
			snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
			if (bExitOnPrevLink)
			{
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
			}
			else
			{
				snprintf(szToLat, sizeof(szToLat), "%.06lf", stMatchLinkInfo.dfEdNodeY);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", stMatchLinkInfo.dfEdNodeX);
			}
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szClosedRoadId;
			PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);
			stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

			// 구간거리 — 원래는 실시간 GPS 누적이 아니라 base_roadlink.coords 폴리라인 실거리
			//   (ZONE_INFO.dfLengthM, [zone_select] SQL 에서 하버사인 합산으로 미리 계산됨) 사용
			//   (2026-08-12 최정우 수정). 단, 진입 자체가 애매(bClosedEntryAmbiguous — 트립이 이미
			//   폐쇄형 도로 위에서 시작해 진입 게이트를 못 잡은 경우)했으면 "구역 전체를 다 지났다"는
			//   전제가 성립하지 않으므로, 대신 실제 출발 지점(dfEntryFromLat/Lon)~출구 게이트 간
			//   실거리를 쓴다(사용자 지시, 2026-08-25 최정우 추가)
			double dfLengthM;
			if (pstSession->bClosedEntryAmbiguous)
			{
				POINT stStartPos, stGatePos;
				stStartPos.dfX = pstSession->dfEntryFromLon;  stStartPos.dfY = pstSession->dfEntryFromLat;
				stGatePos.dfX = pstExitGate->dfLon;           stGatePos.dfY = pstExitGate->dfLat;
				dfLengthM = HaversineMeters(stStartPos, stGatePos);
			}
			else
			{
				dfLengthM = (pstZone != nullptr) ? pstZone->dfLengthM : 0.0;
			}
			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
			stRow.strDistM = szDistM;

			double dfDwellSec = difftime(dtExitTime, pstSession->dtEntryTime);
			// 주행은 실제로 있었으므로 경과시간이 0(또는 반올림 시 0이 되는 소수)으로 남으면 안 됨 —
			//   최소 1초로 보정(사용자 지시, 2026-08-25 최정우 추가)
			if (dfDwellSec < 1.0)
				dfDwellSec = 1.0;

			// 평균속도 — 구역 실거리 ÷ 입구~출구 경과시간(사용자 지시, 2026-08-14 — 개방형 제외 전
			//   유형 평균속도로 통일. 기존엔 개방형과 동일하게 순간속도를 썼으나, 구역 개념이 있는
			//   유형은 구간단속과 동일하게 평균속도가 맞다는 지시로 변경)
			{
				double dfAvgSpeedKmh = (dfLengthM / dfDwellSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}

			// 제한속도 — 진입 애매(bClosedEntryAmbiguous) 시엔 이 레코드가 어느 구간의 제한속도를
			//   대표하는지 정의할 근거 자체가 없음(입구 링크를 특정 못 함) — 0으로 둔다(사용자 지시,
			//   2026-08-25 최정우 추가). 그 외엔 기존과 동일하게 매칭 링크(또는 bExitOnPrevLink 시
			//   직전 링크)의 실제 도로 제한속도 사용
			if (pstSession->bClosedEntryAmbiguous)
			{
				stRow.strSpeedLimitKmh = "0";
			}
			else
			{
				uint8 nSpeedLimitForRow = stMatchLinkInfo.nMaxSpeed;
				if (bExitOnPrevLink && (m_stConfig.pcDataLoader != nullptr))
				{
					PLINK_INFO pstPrevLink = m_stConfig.pcDataLoader->GetLinkInfo(qwExitPrevLinkID);
					if (pstPrevLink != nullptr)
						nSpeedLimitForRow = pstPrevLink->nMaxSpeed;
				}
				char szSpeedLimit[8];
				snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(nSpeedLimitForRow));
				stRow.strSpeedLimitKmh = szSpeedLimit;
			}

			// stay_seconds — 입구~출구 체류시간(초), 사용자 지시(2026-08-14 — 개방형 제외 전 유형 공통화).
			//   위 dfDwellSec(평균속도 계산에 이미 씀)과 동일한 경과시간 재사용
			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtEntryTime);		// 입구 통과 시각(실측 패턴과 일치)

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			if (pszTripStartDt != nullptr)
				stRow.strTripStartDt = pszTripStartDt;
			else
				stRow.strTripStartDt = stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szEntryTollgateId;
			stRow.strExitTollgateId = pstExitGate->szTollgateID;

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			pvtChargeInserts->push_back(stRow);

			LOGFMTI("[#%02d] closed road exit charge queued!device=[%s] trip_id=[%s] seq=[%d] entry=[%s] exit=[%s] dist_m=[%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				pstSession->szEntryTollgateId, pstExitGate->szTollgateID, szDistM);

			pstSession->nChargeSeq += 1;
			pstSession->bInClosedRoad = false;
			pstSession->szEntryTollgateId[0] = '\0';
			pstSession->szClosedRoadId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 — 이 링크가 바로 다음 구역의
			//   입구 게이트도 겸하는 경우(사용자 질문 케이스) 같은 tick 에 바로 이어서 처리 (2026-08-13 최정우 추가)
		}
		else
		{
			// 게이트를 못 찾음 — 이 구역(link_ids)을 완전히 벗어났는지로 "확정 이탈" 여부만 보조
			//   판정한다(실측 누적거리·위치는 위에서 이미 매 틱 갱신됨). 아직 구역 안이면 계속
			//   대기, 완전히 벗어났으면 게이트 미확인이라 여전히 N/3(AUDIT)이지만 dist_m/speed_kmh/
			//   stay_seconds는 0 대신 실측값으로 채워 마감한다(실측 000376_20260819140856 — 게이트를
			//   못 찾고 구역을 나갔는데 전부 0으로 비어있던 문제, 2026-08-25 최정우 추가)
			PZONE_INFO pstTrackingZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);

			if (IsMatchedLinkInZone(pstTrackingZone, stMatchLinkInfo))
				return;			// 아직 구역 안 — 다음 틱 대기

			// 링크가 2개 이상인 구역은 "지금 이 링크가 link_ids 에 없다"만으로 확정 이탈 처리하면
			//   안 된다 — 교차로 부근에서 잠깐 인접한 비구성원 링크로 튀었다가 같은 구역의 남은
			//   링크로 되돌아오는 매칭 흔들림이 실측으로 확인됨(000376_20260819141002 RL-Z00006,
			//   6개 링크 중 3번째 링크 대신 잠깐 옆 링크로 튀었다가 복귀 — 이걸 확정 이탈로 오판해
			//   실제 출구 게이트(TG00011)에 도달하기 훨씬 전에 1266m/144s 짜리 정상 통과를
			//   459m/24s 로 조기 마감시켜버림). 링크 1개짜리 구역(RL-Z00005 등)은 "그 링크를
			//   벗어남=구역을 벗어남"이 유일한 해석이라 안전하지만, 여러 링크로 구성된 구역은
			//   TTL/트립종료 마감(AppendExpiredClosedRoadCharge)에 맡기는 게 더 안전함 — 그쪽도
			//   이미 실측 누적거리를 쓰므로 dist_m=0 으로 비는 문제는 없다 (2026-08-25 최정우 추가,
			//   전체 재맵매칭 회귀검증 중 발견)
			if ((pstTrackingZone != nullptr) && (pstTrackingZone->vtLinkIds.size() > 1))
				return;			// 여러 링크 구역 — 일시적 이탈일 수 있음, 확정 짓지 않고 계속 대기

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "2";							// CLOSED_ROAD
			stRow.strChargeUnit = "1";
			stRow.strLinkId = "";
			stRow.strFromId = pstSession->szEntryTollgateId;
			stRow.strToId = "";								// 출구 미확인 — 지어내지 않음

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
			snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
			snprintf(szToLat, sizeof(szToLat), "%.06lf", stMatchLinkInfo.dfMatchY);
			snprintf(szToLon, sizeof(szToLon), "%.06lf", stMatchLinkInfo.dfMatchX);
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szClosedRoadId;
			stRow.strZoneName = (pstTrackingZone != nullptr) ? pstTrackingZone->szRoadNm : "";

			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(pstSession->dfClosedAccumDistM + 0.5));
			stRow.strDistM = szDistM;

			double dfDwellSec = difftime(stRawLogInfo.dtGPS, pstSession->dtEntryTime);
			if (dfDwellSec > 0.0)
			{
				double dfAvgSpeedKmh = (pstSession->dfClosedAccumDistM / dfDwellSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}
			stRow.strSpeedLimitKmh = "";						// 출구 링크를 특정 못 함 — 지어내지 않음

			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtEntryTime);

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szEntryTollgateId;
			stRow.strExitTollgateId = "";

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			stRow.strChargeYn = "N";							// 출구 게이트 미확인 — 항상 AUDIT
			stRow.strChargeStatus = "3";

			pvtChargeInserts->push_back(stRow);

			LOGFMTW("[#%02d] closed road exit unconfirmed(zone left)!device=[%s] trip_id=[%s] seq=[%d] "
				"entry=[%s] road=[%s] dist_m=[%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				pstSession->szEntryTollgateId, pstSession->szClosedRoadId, szDistM);

			pstSession->nChargeSeq += 1;
			pstSession->bInClosedRoad = false;
			pstSession->szEntryTollgateId[0] = '\0';
			pstSession->szClosedRoadId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 — 게이트 확정 이탈과 동일 관례
		}
	}

	// 진입 처리 — 이미 다른 구역에 진입해 있는 상태면(bInClosedRoad=true 로 위 블록에서 못 닫혔으면)
	//   새 진입을 받지 않음(한 번에 하나의 폐쇄형 구역만 추적 가능) (2026-08-13 최정우 재작성)
	if (pstSession->bInClosedRoad)
		return;

	// 경유 링크 포함 전체 경로에서 입구 후보 수집 — 진입은 위치 판정이 필요 없어(어느 지점에서
	//   발견되든 그 즉시 진입 확정) 경유/최종 구분 없이 단순 병합 (2026-08-20 최정우 추가)
	vector<PGATE_INFO> vtEntryCandidates;
	{
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
		{
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'I', &vtEntryCandidates);
		}
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
			{
				vector<PGATE_INFO> vtOne;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.aqwPathLinkIDs[i], 'I', &vtOne);
				vtEntryCandidates.insert(vtEntryCandidates.end(), vtOne.begin(), vtOne.end());
			}
		}
	}

	for (size_t i = 0; i < vtEntryCandidates.size(); ++i)
	{
		PGATE_INFO pstEntryGate = vtEntryCandidates[i];
		bool bAmbiguousStart = false;			// 아래 참고 (2026-08-25 최정우 추가)

		// 이번 확정(최종) 링크에서 찾은 후보만 위치 검사 — 경유(이미 완전 통과) 링크에서 찾은
		//   후보는 무조건 확정(위치를 잴 의미가 없음, 위 출구 판정과 동일 원리).
		//   같은 링크에 같은 road_id 출구(O/B) 게이트가 있으면(TG00007/08 처럼 입/출구가 한 링크에
		//   같이 있는 구조) 그 출구 지점을 이미 지났으면 진입 후보에서 제외 — 안 그러면 출구 처리
		//   직후에도 그 링크에 계속 머무는 동안(다음 tick들) 매번 진입~진출이 반복 재발화됨.
		//   근본원인: 출구 판정엔 "아직 도달 전인가" 위치 확인이 있는데 진입 판정엔 전혀 없었음 —
		//   진입 쪽엔 "이미 지나쳤는가"를 확인해야 재발화를 막을 수 있음(2026-08-20 최정우 수정 —
		//   세션에 남기는 방식은 트립 끝까지 재진입 자체를 막아버리는 별개 버그였음, 지역변수로
		//   1tick만 막는 방식도 이 재발화(여러 tick 동안 반복)까진 못 막아 폐기)
		if (pstEntryGate->qwLinkID == stMatchLinkInfo.qwLinkID)
		{
			vector<PGATE_INFO> vtSameLinkExit;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtSameLinkExit);

			PGATE_INFO pstSameLinkExit = nullptr;
			for (size_t e = 0; e < vtSameLinkExit.size(); ++e)
			{
				if (strcmp(vtSameLinkExit[e]->szRoadID, pstEntryGate->szRoadID) == 0)
				{
					pstSameLinkExit = vtSameLinkExit[e];
					break;
				}
			}

			if (pstSameLinkExit != nullptr)
			{
				POINT stLinkStart, stExitPos, stEntryPos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stExitPos.dfX = pstSameLinkExit->dfLon;
				stExitPos.dfY = pstSameLinkExit->dfLat;
				double dfExitPosOnLink = HaversineMeters(stLinkStart, stExitPos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				if (dfCurPosOnLink >= (dfExitPosOnLink - 3.0))
					continue;			// 이미 이 링크의 출구 지점을 지났음 — 재진입 아님

				// 세션에 확정된 직전 링크가 전혀 없는(=이 트립에서 지금까지 얻은 유일한 정보) 상태에서,
				//   첫 매칭 위치가 입구 지점보다 출구 지점에 더 가까우면 — 지금 막 이 게이트를 지난 게
				//   아니라 이미 그 전부터(이전 트립 등에서) 구역 안에 있었을 가능성이 높다. 입구
				//   게이트ID를 이 게이트로 확정해 잘못 귀속시키지 말고 비워둔다 — 아래 bGateAnomaly
				//   로직이 자동으로 AUDIT(N/3) 처리해준다(실측 000376_20260819140856 — 트립 첫 GPS가
				//   이미 입구 TG00007로부터 343m·출구 TG00008까지는 23m 남은 지점이었는데 "지금 막
				//   TG00007 진입"으로 오기록됐었음, 2026-08-25 최정우 추가)
				stEntryPos.dfX = pstEntryGate->dfLon;
				stEntryPos.dfY = pstEntryGate->dfLat;
				double dfEntryPosOnLink = HaversineMeters(stLinkStart, stEntryPos);
				if ((pstSession->qwLastConfirmedLinkID == 0)
					&& (dfCurPosOnLink > ((dfEntryPosOnLink + dfExitPosOnLink) / 2.0)))
				{
					bAmbiguousStart = true;
					LOGFMTW("[#%02d] closed road entry ambiguous!device=[%s] trip_id=[%s] road=[%s] "
						"pos=[%.1fm] entry_gate_pos=[%.1fm] exit_gate_pos=[%.1fm] -> entry left blank",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						pstEntryGate->szRoadID, dfCurPosOnLink, dfEntryPosOnLink, dfExitPosOnLink);
				}
			}
		}

		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
		if ((pstZone == nullptr) || (strcmp(pstZone->szRoadKind, "2") != 0))
			continue;

		pstSession->bInClosedRoad = true;
		pstSession->bClosedEntryAmbiguous = bAmbiguousStart;
		if (bAmbiguousStart)
		{
			pstSession->szEntryTollgateId[0] = '\0';
			// 진입 게이트 자체가 없으니 "입구 게이트 링크의 from_node"라는 기존 의미가 성립하지
			//   않음 — 실제 시작 지점(이 트립이 관측된 첫 매칭 좌표)을 대신 담아 나중에 dist_m
			//   실거리 계산 기준으로 쓴다(사용자 지시, 2026-08-25 최정우 추가)
			pstSession->dfEntryFromLat = stMatchLinkInfo.dfMatchY;
			pstSession->dfEntryFromLon = stMatchLinkInfo.dfMatchX;
		}
		else
		{
			strncpy(pstSession->szEntryTollgateId, pstEntryGate->szTollgateID, sizeof(pstSession->szEntryTollgateId) - 1);
			pstSession->szEntryTollgateId[sizeof(pstSession->szEntryTollgateId) - 1] = '\0';
			pstSession->dfEntryFromLat = stMatchLinkInfo.dfStNodeY;
			pstSession->dfEntryFromLon = stMatchLinkInfo.dfStNodeX;
		}
		strncpy(pstSession->szClosedRoadId, pstEntryGate->szRoadID, sizeof(pstSession->szClosedRoadId) - 1);
		pstSession->szClosedRoadId[sizeof(pstSession->szClosedRoadId) - 1] = '\0';
		// 진입 게이트 통과 시각 보간 — 진입이 애매하지 않을 때만(실제 게이트 위치가 있어야 보간
		//   근거가 있음) 직전 확정 tick~이번 tick 사이에서 게이트를 지났을 시각을 추정한다. 직전
		//   tick 이 없으면(트립 첫 tick) 지금 tick 시각을 그대로 씀(사용자 지시, 2026-08-25 최정우 추가)
		if (!bAmbiguousStart && pstSession->bHasLastMatch)
		{
			pstSession->dtEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				pstEntryGate->dfLon, pstEntryGate->dfLat);
		}
		else
		{
			pstSession->dtEntryTime = stRawLogInfo.dtGPS;
		}
		// 게이트 미확인 이탈 시 실측 dist_m 산출용 — 진입 시점 위치부터 누적 시작 (2026-08-25 최정우 추가)
		pstSession->dfClosedLastX = stMatchLinkInfo.dfMatchX;
		pstSession->dfClosedLastY = stMatchLinkInfo.dfMatchY;
		pstSession->dfClosedAccumDistM = 0.0;
		// 진입 tick 의 링크 자체가 "구역 안에서 마지막으로 확인된 링크"의 첫 값 — 위 qwClosedLastZoneLinkID
		//   주석 참고(2026-08-25 최정우 추가)
		pstSession->qwClosedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
		pstSession->dtClosedLastZoneTime = stRawLogInfo.dtGPS;

		LOGFMTI("[#%02d] closed road entry!device=[%s] trip_id=[%s] gate=[%s] road=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			bAmbiguousStart ? "unknown" : pstEntryGate->szTollgateID, pstEntryGate->szRoadID);
		break;			// 한 tick 엔 하나만 진입
	}
}

/**
 * @brief 구간단속(SPEED) 입/출구 게이트 판정 (2026-08-12 최정우 추가, 2026-08-13 재작성)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — 진입 상태(bInSpeedZone 등) 갱신. 폐쇄형과 별도
 *   독립 트랙(같은 도로 위에 겹쳐 동시 진행 가능)
 * @param[out] pvtChargeInserts 출구 통과 시 1행 적재
 * @return void
 * @remark
 *   - 이 링크에 입구(I) 게이트가 있고 아직 구간단속 구역에 안 들어가 있으면, 그 게이트 road_id 의
 *     구역이 road_kind='3'(구간단속)일 때만 진입 처리(폐쇄형 road_kind='2' 등 제외)
 *   - 진출 시 from_id=to_id=구역 road_id(게이트ID 아님, 실측 확인), from/to_lat·lon=구역
 *     coords 폴리라인의 첫/마지막 정점(ZONE_INFO.dfFirstLat/Lon·dfLastLat/Lon)
 *   - speed_kmh 는 순간속도가 아니라 "구역 실거리 ÷ 입구~출구 경과시간" 평균속도
 *   - speed_limit_kmh 는 구역 자체 등록값(base_roadlink.speed_limit_kmh) 사용
 *   - charge_yn/charge_status 는 기본 Y/0 — 다른 유형과 동일하게 처리(사용자 지시, 2026-08-13).
 *     원래는 위반 여부와 무관하게 항상 N/4 고정이었음(실측 2건 전부 위반(78/84>60)인데도 N/4로
 *     확인됨 — 통행료 파이프라인 비대상이라는 근거였음). 이번 변경으로 그 실측 선례와는 달라짐 —
 *     구간단속의 TTL 만료·세션유실 시 N/4 처리는 아직 별도 미구현(폐쇄형과 동일한 잔여 이슈)
 *   - 2026-08-13 재작성: 폐쇄형과 동일한 3가지 한계 대응(한 링크 내 동일방향 게이트 2개 이상,
 *     road_id 까지 비교하는 재진입 가드로 "같은 링크의 다른 구역" 통과 허용, gate_div='B' 겸용
 *     게이트 지원) — CollectGateCandidates() 재사용, 상세 근거는 ProcessClosedRoadCharge() 참고
*/
void CRawLogWorker::ProcessSpeedZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 진출 처리 먼저 시도 — ProcessClosedRoadCharge() 와 동일 패턴 (2026-08-13 최정우 재작성)
	if (pstSession->bInSpeedZone)
	{
		// 실시간 누적거리·마지막 위치 갱신 — ProcessClosedRoadCharge() 와 동일 근거·위치(게이트
		//   확정 여부와 무관하게 매 틱 항상 최신 유지) (2026-08-25 최정우 추가)
		{
			POINT stPrevPos, stCurPos;
			stPrevPos.dfX = pstSession->dfSpeedLastX;  stPrevPos.dfY = pstSession->dfSpeedLastY;
			stCurPos.dfX = stMatchLinkInfo.dfMatchX;   stCurPos.dfY = stMatchLinkInfo.dfMatchY;
			pstSession->dfSpeedAccumDistM += HaversineMeters(stPrevPos, stCurPos);
			pstSession->dfSpeedLastX = stMatchLinkInfo.dfMatchX;
			pstSession->dfSpeedLastY = stMatchLinkInfo.dfMatchY;
		}

		// "구역 안에서 마지막으로 확인된 링크/시각" 갱신 — ProcessClosedRoadCharge() 동일 근거·
		//   위치 참고(2026-08-25 최정우 추가)
		{
			PZONE_INFO pstTrackingZoneForUpdate = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szSpeedZoneRoadId);
			if (IsMatchedLinkInZone(pstTrackingZoneForUpdate, stMatchLinkInfo))
			{
				pstSession->qwSpeedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
				pstSession->dtSpeedLastZoneTime = stRawLogInfo.dtGPS;
			}
		}

		// 경유(이미 완전 통과) 링크들을 먼저 확인 — ProcessClosedRoadCharge() 와 동일 원리
		//   (2026-08-20 최정우 추가)
		PGATE_INFO pstExitGate = nullptr;
		bool bExitOnIntermediate = false;

		vector<PGATE_INFO> vtIntermediateExit;
		CollectGateCandidatesOnIntermediateLinks(m_stConfig.pcChargeDataLoader,
			stMatchLinkInfo.aqwPathLinkIDs, stMatchLinkInfo.nPathLinkCount, 'O', &vtIntermediateExit);
		for (size_t i = 0; i < vtIntermediateExit.size(); ++i)
		{
			if (strcmp(pstSession->szSpeedZoneRoadId, vtIntermediateExit[i]->szRoadID) == 0)
			{
				pstExitGate = vtIntermediateExit[i];
				bExitOnIntermediate = true;
				break;
			}
		}

		if (pstExitGate == nullptr)
		{
			vector<PGATE_INFO> vtExitCandidates;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtExitCandidates);

			for (size_t i = 0; i < vtExitCandidates.size(); ++i)
			{
				if (strcmp(pstSession->szSpeedZoneRoadId, vtExitCandidates[i]->szRoadID) == 0)
				{
					pstExitGate = vtExitCandidates[i];
					break;
				}
			}
		}

		// 직전 확정 링크(이번 틱에 이미 완전히 벗어난 링크)에 출구 게이트가 있었는데, 그 안에서
		//   GPS 틱이 게이트 위치 3m 이내로 못 들어와 못 잡은 경우 — 다음 틱이 아예 다른 링크로
		//   확정됐다는 것 자체가 그 링크를 끝까지 지나왔다는 뜻이므로 위치 판정 없이 확정한다.
		//   경유경로(aqwPathLinkIDs)로도 못 잡는 이유: 두 확정 링크가 그래프상 1-hop 인접이면
		//   재구성 경로가 생기지 않아(경유 링크 개념 자체가 없음) 직전 링크가 경로에 아예 안 실린다
		//   (실측 000370_20260824103155 RL-Z00003 — 출구 TG00013 이 링크 끝단(길이 331.7m 중
		//   310m 지점)에 있는데 마지막 GPS 는 그보다 21.6m 못 미친 289m 지점, 3초 뒤 다음 틱은
		//   이미 다음 링크로 확정 — 게이트를 "지나간 순간"을 찍은 GPS 가 그냥 없었을 뿐 실제로는
		//   통과함. 이걸 못 잡아 트립종료까지 세션이 안 닫히고 TTL 로 강제마감돼 stay_seconds 에
		//   이탈 이후 시간까지 섞여 들어감) (2026-08-24 최정우 추가)
		// 후보 링크 두 개를 순서대로 시도 — ProcessClosedRoadCharge() 동일 근거 참고. qwLastConfirmedLinkID
		//   가 트립 시작 후보 판정 중이라 0에 머물러 있으면 qwSpeedLastZoneLinkID 로 대체
		//   (2026-08-25 최정우 추가, 사용자 지시 — "다음 맵매칭 좌표/링크로 진출 확인 가능")
		bool bExitOnPrevLink = false;
		uint64 qwExitPrevLinkID = 0;
		time_t dtExitPrevLinkTime = 0;
		if (pstExitGate == nullptr)
		{
			const uint64 aqwPrevCand[2] = { pstSession->qwLastConfirmedLinkID, pstSession->qwSpeedLastZoneLinkID };
			const time_t adtPrevCandTime[2] = { pstSession->dtLastConfirmedLinkTime, pstSession->dtSpeedLastZoneTime };
			for (int c = 0; (c < 2) && (pstExitGate == nullptr); ++c)
			{
				if ((aqwPrevCand[c] == 0) || (aqwPrevCand[c] == stMatchLinkInfo.qwLinkID))
					continue;

				vector<PGATE_INFO> vtPrevExit;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, aqwPrevCand[c], 'O', &vtPrevExit);
				for (size_t i = 0; i < vtPrevExit.size(); ++i)
				{
					if (strcmp(pstSession->szSpeedZoneRoadId, vtPrevExit[i]->szRoadID) == 0)
					{
						pstExitGate = vtPrevExit[i];
						bExitOnPrevLink = true;
						qwExitPrevLinkID = aqwPrevCand[c];
						dtExitPrevLinkTime = adtPrevCandTime[c];
						break;
					}
				}
			}
		}

		if (pstExitGate != nullptr)
		{
			if (!bExitOnIntermediate && !bExitOnPrevLink)
			{
				POINT stLinkStart, stGatePos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stGatePos.dfX = pstExitGate->dfLon;
				stGatePos.dfY = pstExitGate->dfLat;
				double dfGatePosOnLink = HaversineMeters(stLinkStart, stGatePos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				if (dfCurPosOnLink < (dfGatePosOnLink - 3.0))
					return;
			}

			PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szSpeedZoneRoadId);

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "3";							// SPEED
			stRow.strChargeUnit = "1";							// LINK (실측 확인)
			stRow.strLinkId = "";

			// from_id/to_id — 입/출구 게이트ID (2026-08-20 최정우 수정, 사용자 지시 — 기존엔 구역
			//   road_id를 넣었으나(2026-08-12 실측 확인 기반) 게이트ID 자체로 변경. 구역 road_id는
			//   zone_id 컬럼에 계속 남음
			stRow.strFromId = pstSession->szSpeedEntryTollgateId;
			stRow.strToId = pstExitGate->szTollgateID;

			// 진입 애매(bSpeedEntryAmbiguous — 트립이 이미 구간단속 구역 위에서 시작해 진입 게이트를
			//   못 잡은 경우)했으면 from_lat/lon·dist_m 모두 "구역 전체를 지났다"는 전제인 구역
			//   등록값(dfFirstLat/Lon, dfLengthM) 대신 실제 출발 지점 기준으로 계산한다(사용자 지시,
			//   2026-08-25 최정우 추가) — ProcessClosedRoadCharge() bClosedEntryAmbiguous 동일 근거
			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			double dfLengthM;
			if (pstSession->bSpeedEntryAmbiguous)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfSpeedEntryFromLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfSpeedEntryFromLon);
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);

				POINT stStartPos, stGatePos;
				stStartPos.dfX = pstSession->dfSpeedEntryFromLon;  stStartPos.dfY = pstSession->dfSpeedEntryFromLat;
				stGatePos.dfX = pstExitGate->dfLon;                stGatePos.dfY = pstExitGate->dfLat;
				dfLengthM = HaversineMeters(stStartPos, stGatePos);
			}
			else if (pstZone != nullptr)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZone->dfFirstLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZone->dfFirstLon);
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstZone->dfLastLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstZone->dfLastLon);
				dfLengthM = pstZone->dfLengthM;
			}
			else
			{
				szFromLat[0] = szFromLon[0] = szToLat[0] = szToLon[0] = '\0';
				dfLengthM = 0.0;
			}
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szSpeedZoneRoadId;
			stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
			stRow.strDistM = szDistM;

			// 출구 게이트 통과 시각 보간 — ProcessClosedRoadCharge() 동일 근거·방식 참고
			//   (2026-08-25 최정우 추가). 직전 tick 이 없으면 예전 방식 그대로(2026-08-24 최정우
			//   추가, dtExitPrevLinkTime 로 일반화 2026-08-25)
			time_t dtExitTime;
			if (pstSession->bHasLastMatch)
			{
				dtExitTime = InterpolateGateCrossingTime(
					pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
					stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
					pstExitGate->dfLon, pstExitGate->dfLat);
			}
			else
			{
				dtExitTime = bExitOnPrevLink ? dtExitPrevLinkTime : stRawLogInfo.dtGPS;
			}
			(void)qwExitPrevLinkID;		// 구간단속은 제한속도를 구역 등록값으로만 쓰므로 링크ID 자체는 미사용

			double dfElapsedSec = difftime(dtExitTime, pstSession->dtSpeedEntryTime);
			// 주행은 실제로 있었으므로 경과시간이 0(또는 반올림 시 0이 되는 소수)으로 남으면 안 됨 —
			//   최소 1초로 보정(사용자 지시, 2026-08-25 최정우 추가)
			if (dfElapsedSec < 1.0)
				dfElapsedSec = 1.0;

			// 평균속도 — 구역 실거리 ÷ 입구~출구 경과시간. 구간단속은 순간속도가 아니라 구간 전체
			//   평균속도가 제한속도 초과 여부(위반 판정) 기준이기 때문 (2026-08-12 최정우 추가)
			{
				double dfAvgSpeedKmh = (dfLengthM / dfElapsedSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}

			// 제한속도 — 구역 자체 등록값(base_roadlink.speed_limit_kmh, 실측 확인) 사용. 개방형·폐쇄형은
			//   매칭 링크의 값을 썼지만, 구간단속은 구역 전체 제한속도가 별도 등록돼 있어 이쪽을 씀 (2026-08-12 최정우 추가)
			if (pstZone != nullptr)
			{
				char szSpeedLimit[8];
				snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZone->dfSpeedLimitKmh + 0.5));
				stRow.strSpeedLimitKmh = szSpeedLimit;
			}

			// stay_seconds — 입구~출구 체류시간(초), 사용자 지시(2026-08-14 — 개방형 제외 전 유형 공통화).
			//   위 dfElapsedSec(평균속도 계산에 이미 씀)과 동일한 경과시간 재사용
			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtSpeedEntryTime);

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			if (pszTripStartDt != nullptr)
				stRow.strTripStartDt = pszTripStartDt;
			else
				stRow.strTripStartDt = stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szSpeedEntryTollgateId;	// 2026-08-20 최정우 수정 — 게이트ID 기록으로 변경
			stRow.strExitTollgateId = pstExitGate->szTollgateID;

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			// charge_yn/charge_status — 다른 유형과 동일하게 기본 Y/0 (사용자 지시, 2026-08-13 —
			//   원래는 통행료 파이프라인 비대상이라 위반 여부 무관 항상 N/4 고정이었음, 실측 선례와 달라짐).
			//   단, 입구 게이트를 확정 못 한(진입 판정이 애매해 비워둔) 경우는 CLOSED_ROAD와 동일하게
			//   AUDIT(N/3) — "다른 유형과 동일하게"라는 원 지시 취지에 맞춤(2026-08-25 최정우 추가,
			//   ProcessClosedRoadCharge() bGateAnomaly 참고)
			bool bGateAnomaly = (pstSession->szSpeedEntryTollgateId[0] == '\0') ||
				(pstExitGate->szTollgateID[0] == '\0') ||
				(strcmp(pstSession->szSpeedEntryTollgateId, pstExitGate->szTollgateID) == 0);
			stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
			stRow.strChargeStatus = bGateAnomaly ? "3" : "0";

			pvtChargeInserts->push_back(stRow);

			LOGFMTI("[#%02d] speed zone exit charge queued!device=[%s] trip_id=[%s] seq=[%d] road=[%s] dist_m=[%s] avg_speed=[%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				pstSession->szSpeedZoneRoadId, szDistM, stRow.strSpeedKmh.c_str());

			pstSession->nChargeSeq += 1;
			pstSession->bInSpeedZone = false;
			pstSession->szSpeedZoneRoadId[0] = '\0';
			pstSession->szSpeedEntryTollgateId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 (2026-08-13 최정우 추가, 폐쇄형과 동일 이유)
		}
		else
		{
			// 게이트를 못 찾음 — ProcessClosedRoadCharge() 동일 로직·근거 참고: 이 구역(link_ids)을
			//   완전히 벗어났는지로 "확정 이탈" 판정(실측 누적거리·위치는 위에서 이미 매 틱 갱신됨).
			//   아직 구역 안이면 계속 대기, 벗어났으면 게이트 미확인 AUDIT(N/3)이지만 dist_m/
			//   speed_kmh/stay_seconds는 실측값으로 채운다 (2026-08-25 최정우 추가)
			PZONE_INFO pstTrackingZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szSpeedZoneRoadId);

			if (IsMatchedLinkInZone(pstTrackingZone, stMatchLinkInfo))
				return;			// 아직 구역 안 — 다음 틱 대기

			// 여러 링크 구역은 일시적 이탈로 확정 짓지 않음 — ProcessClosedRoadCharge() 동일 근거
			//   참고(2026-08-25 최정우 추가, 전체 재맵매칭 회귀검증 중 RL-Z00006 에서 발견)
			if ((pstTrackingZone != nullptr) && (pstTrackingZone->vtLinkIds.size() > 1))
				return;			// 여러 링크 구역 — 일시적 이탈일 수 있음, 확정 짓지 않고 계속 대기

			CHARGE_INSERT_ROW stRow;
			stRow.strTripId = stRawLogInfo.szTripID;
			stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

			char szSeq[16];
			snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
			stRow.strChargeSeq = szSeq;

			stRow.strChargeType = "3";							// SPEED
			stRow.strChargeUnit = "1";
			stRow.strLinkId = "";
			stRow.strFromId = pstSession->szSpeedEntryTollgateId;
			stRow.strToId = "";								// 출구 미확인 — 지어내지 않음

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			if (pstTrackingZone != nullptr)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstTrackingZone->dfFirstLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstTrackingZone->dfFirstLon);
			}
			else
			{
				szFromLat[0] = szFromLon[0] = '\0';
			}
			snprintf(szToLat, sizeof(szToLat), "%.06lf", stMatchLinkInfo.dfMatchY);
			snprintf(szToLon, sizeof(szToLon), "%.06lf", stMatchLinkInfo.dfMatchX);
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szSpeedZoneRoadId;
			stRow.strZoneName = (pstTrackingZone != nullptr) ? pstTrackingZone->szRoadNm : "";

			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(pstSession->dfSpeedAccumDistM + 0.5));
			stRow.strDistM = szDistM;

			double dfElapsedSec = difftime(stRawLogInfo.dtGPS, pstSession->dtSpeedEntryTime);
			if (dfElapsedSec > 0.0)
			{
				double dfAvgSpeedKmh = (pstSession->dfSpeedAccumDistM / dfElapsedSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}
			if (pstTrackingZone != nullptr)
			{
				char szSpeedLimit[8];
				snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstTrackingZone->dfSpeedLimitKmh + 0.5));
				stRow.strSpeedLimitKmh = szSpeedLimit;
			}

			char szStaySeconds[16];
			snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
			stRow.strStaySeconds = szStaySeconds;

			stRow.strOccurDt = FormatDateTime14(pstSession->dtSpeedEntryTime);

			const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
			stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

			stRow.strTollgateId = "";
			stRow.strEntryTollgateId = pstSession->szSpeedEntryTollgateId;
			stRow.strExitTollgateId = "";

			stRow.strRegDt = FormatDateTime14(time(nullptr));
			stRow.strUpdDt = stRow.strRegDt;

			stRow.strChargeYn = "N";							// 출구 게이트 미확인 — 항상 AUDIT
			stRow.strChargeStatus = "3";

			pvtChargeInserts->push_back(stRow);

			LOGFMTW("[#%02d] speed zone exit unconfirmed(zone left)!device=[%s] trip_id=[%s] seq=[%d] "
				"entry=[%s] road=[%s] dist_m=[%s]",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
				pstSession->szSpeedEntryTollgateId, pstSession->szSpeedZoneRoadId, szDistM);

			pstSession->nChargeSeq += 1;
			pstSession->bInSpeedZone = false;
			pstSession->szSpeedZoneRoadId[0] = '\0';
			pstSession->szSpeedEntryTollgateId[0] = '\0';
			// return 하지 않고 아래 진입 후보 검사로 계속 진행 — 게이트 확정 이탈과 동일 관례
		}
	}

	// 진입 처리 — 위 블록에서 못 닫혔으면(다른 구역에 여전히 진입 중) 새 진입 안 받음 (2026-08-13 최정우 재작성)
	if (pstSession->bInSpeedZone)
		return;

	// 경유 링크 포함 전체 경로에서 입구 후보 수집 (2026-08-20 최정우 추가, 폐쇄형과 동일 원리)
	vector<PGATE_INFO> vtEntryCandidates;
	{
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
		{
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'I', &vtEntryCandidates);
		}
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
			{
				vector<PGATE_INFO> vtOne;
				CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.aqwPathLinkIDs[i], 'I', &vtOne);
				vtEntryCandidates.insert(vtEntryCandidates.end(), vtOne.begin(), vtOne.end());
			}
		}
	}

	for (size_t i = 0; i < vtEntryCandidates.size(); ++i)
	{
		PGATE_INFO pstEntryGate = vtEntryCandidates[i];
		bool bAmbiguousStart = false;			// ProcessClosedRoadCharge() 동일 로직 참고 (2026-08-25 최정우 추가)

		// 이번 확정(최종) 링크에서 찾은 후보만 위치 검사 — ProcessClosedRoadCharge() 와 동일 원리
		//   (2026-08-20 최정우 수정, 상세 근거는 그 함수의 동일 블록 주석 참고)
		if (pstEntryGate->qwLinkID == stMatchLinkInfo.qwLinkID)
		{
			vector<PGATE_INFO> vtSameLinkExit;
			CollectGateCandidates(m_stConfig.pcChargeDataLoader, stMatchLinkInfo.qwLinkID, 'O', &vtSameLinkExit);

			PGATE_INFO pstSameLinkExit = nullptr;
			for (size_t e = 0; e < vtSameLinkExit.size(); ++e)
			{
				if (strcmp(vtSameLinkExit[e]->szRoadID, pstEntryGate->szRoadID) == 0)
				{
					pstSameLinkExit = vtSameLinkExit[e];
					break;
				}
			}

			if (pstSameLinkExit != nullptr)
			{
				POINT stLinkStart, stExitPos, stEntryPos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stExitPos.dfX = pstSameLinkExit->dfLon;
				stExitPos.dfY = pstSameLinkExit->dfLat;
				double dfExitPosOnLink = HaversineMeters(stLinkStart, stExitPos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				if (dfCurPosOnLink >= (dfExitPosOnLink - 3.0))
					continue;			// 이미 이 링크의 출구 지점을 지났음 — 재진입 아님

				// ProcessClosedRoadCharge() 동일 로직·근거 참고 (2026-08-25 최정우 추가)
				stEntryPos.dfX = pstEntryGate->dfLon;
				stEntryPos.dfY = pstEntryGate->dfLat;
				double dfEntryPosOnLink = HaversineMeters(stLinkStart, stEntryPos);
				if ((pstSession->qwLastConfirmedLinkID == 0)
					&& (dfCurPosOnLink > ((dfEntryPosOnLink + dfExitPosOnLink) / 2.0)))
				{
					bAmbiguousStart = true;
					LOGFMTW("[#%02d] speed zone entry ambiguous!device=[%s] trip_id=[%s] road=[%s] "
						"pos=[%.1fm] entry_gate_pos=[%.1fm] exit_gate_pos=[%.1fm] -> entry left blank",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
						pstEntryGate->szRoadID, dfCurPosOnLink, dfEntryPosOnLink, dfExitPosOnLink);
				}
			}
		}

		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
		if ((pstZone == nullptr) || (strcmp(pstZone->szRoadKind, "3") != 0))
			continue;

		pstSession->bInSpeedZone = true;
		pstSession->bSpeedEntryAmbiguous = bAmbiguousStart;
		if (bAmbiguousStart)
		{
			pstSession->szSpeedEntryTollgateId[0] = '\0';
			// ProcessClosedRoadCharge() bClosedEntryAmbiguous 동일 근거 참고 — 실제 시작 지점을
			//   담아 나중에 dist_m 실거리 계산 기준으로 씀(2026-08-25 최정우 추가)
			pstSession->dfSpeedEntryFromLat = stMatchLinkInfo.dfMatchY;
			pstSession->dfSpeedEntryFromLon = stMatchLinkInfo.dfMatchX;
		}
		else
		{
			strncpy(pstSession->szSpeedEntryTollgateId, pstEntryGate->szTollgateID, sizeof(pstSession->szSpeedEntryTollgateId) - 1);
			pstSession->szSpeedEntryTollgateId[sizeof(pstSession->szSpeedEntryTollgateId) - 1] = '\0';
		}
		strncpy(pstSession->szSpeedZoneRoadId, pstEntryGate->szRoadID, sizeof(pstSession->szSpeedZoneRoadId) - 1);
		pstSession->szSpeedZoneRoadId[sizeof(pstSession->szSpeedZoneRoadId) - 1] = '\0';
		// ProcessClosedRoadCharge() 동일 근거 참고 — 진입 게이트 통과 시각 보간 (2026-08-25 최정우 추가)
		if (!bAmbiguousStart && pstSession->bHasLastMatch)
		{
			pstSession->dtSpeedEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				pstEntryGate->dfLon, pstEntryGate->dfLat);
		}
		else
		{
			pstSession->dtSpeedEntryTime = stRawLogInfo.dtGPS;
		}
		// 게이트 미확인 이탈 시 실측 dist_m 산출용 (2026-08-25 최정우 추가, ProcessClosedRoadCharge() 동일)
		pstSession->dfSpeedLastX = stMatchLinkInfo.dfMatchX;
		pstSession->dfSpeedLastY = stMatchLinkInfo.dfMatchY;
		pstSession->dfSpeedAccumDistM = 0.0;
		// ProcessClosedRoadCharge() 동일 근거 참고 (2026-08-25 최정우 추가)
		pstSession->qwSpeedLastZoneLinkID = stMatchLinkInfo.qwLinkID;
		pstSession->dtSpeedLastZoneTime = stRawLogInfo.dtGPS;

		LOGFMTI("[#%02d] speed zone entry!device=[%s] trip_id=[%s] gate=[%s] road=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			bAmbiguousStart ? "unknown" : pstEntryGate->szTollgateID, pstEntryGate->szRoadID);
		break;			// 한 tick 엔 하나만 진입
	}
}

/**
 * @brief 폐쇄형 — SKIP 틱(맵매칭 실패) raw GPS 기준 출구 판정
 * @remark ProcessClosedRoadCharge() 의 확정매칭 전이 기반 출구판정으로도 못 잡는 잔여 케이스 보완 —
 *   구역 시작점 기준 raw GPS 거리가 출구 게이트 거리보다 MM_RAWGPS_EXIT_MARGIN_M 이상 크면
 *   확정 링크 없이도 출구로 확정한다. 확정매칭이 있었을 때 쓰는 stMatchLinkInfo 기반 값(매칭
 *   링크 종점 좌표·제한속도 등)이 없어, 그 대신 구역 등록값(ZONE_INFO)으로 대체한다
 *   (2026-08-24 최정우 추가, 사용자 지시)
*/
void CRawLogWorker::CheckClosedRoadExitByRawGps(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if (!pstSession->bInClosedRoad || (m_stConfig.pcChargeDataLoader == nullptr)
		|| m_stConfig.strChargeInsertSQL.empty() || stRawLogInfo.bGpsLatNull || stRawLogInfo.bGpsLonNull)
		return;

	PGATE_INFO pstExitGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(pstSession->szClosedRoadId, 'O');
	if (pstExitGate == nullptr)
		return;
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);
	if (pstZone == nullptr)
		return;

	POINT stZoneStart, stGatePos, stRaw;
	stZoneStart.dfX = pstZone->dfFirstLon;  stZoneStart.dfY = pstZone->dfFirstLat;
	stGatePos.dfX = pstExitGate->dfLon;     stGatePos.dfY = pstExitGate->dfLat;
	stRaw.dfX = stRawLogInfo.dfX;           stRaw.dfY = stRawLogInfo.dfY;

	double dfGateFromStart = HaversineMeters(stZoneStart, stGatePos);
	double dfRawFromStart = HaversineMeters(stZoneStart, stRaw);
	if (dfRawFromStart < (dfGateFromStart + MM_RAWGPS_EXIT_MARGIN_M))
		return;

	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "2";								// CLOSED_ROAD
	stRow.strChargeUnit = "1";
	stRow.strLinkId = "";

	stRow.strFromId = pstSession->szEntryTollgateId;
	stRow.strToId = pstExitGate->szTollgateID;

	bool bGateAnomaly = (pstSession->szEntryTollgateId[0] == '\0') ||
		(pstExitGate->szTollgateID[0] == '\0') ||
		(strcmp(pstSession->szEntryTollgateId, pstExitGate->szTollgateID) == 0);
	stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
	stRow.strChargeStatus = bGateAnomaly ? "3" : "0";

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = pstSession->szClosedRoadId;
	stRow.strZoneName = pstZone->szRoadNm;

	double dfLengthM = pstZone->dfLengthM;
	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
	stRow.strDistM = szDistM;

	double dfDwellSec = difftime(stRawLogInfo.dtGPS, pstSession->dtEntryTime);
	if (dfDwellSec > 0.0)
	{
		double dfAvgSpeedKmh = (dfLengthM / dfDwellSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}

	char szSpeedLimit[8];
	snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZone->dfSpeedLimitKmh + 0.5));
	stRow.strSpeedLimitKmh = szSpeedLimit;

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfDwellSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	stRow.strOccurDt = FormatDateTime14(pstSession->dtEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
	stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = pstSession->szEntryTollgateId;
	stRow.strExitTollgateId = pstExitGate->szTollgateID;

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	pvtChargeInserts->push_back(stRow);

	LOGFMTI("[#%02d] closed road exit charge queued (raw gps)!device=[%s] trip_id=[%s] seq=[%d] "
		"entry=[%s] exit=[%s] dist_m=[%s] margin_over=[%.1f]m",
		nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
		pstSession->szEntryTollgateId, pstExitGate->szTollgateID, szDistM,
		dfRawFromStart - dfGateFromStart);

	pstSession->nChargeSeq += 1;
	pstSession->bInClosedRoad = false;
}

/**
 * @brief 구간단속 — SKIP 틱(맵매칭 실패) raw GPS 기준 출구 판정
 * @remark CheckClosedRoadExitByRawGps() 와 동일 원리, 자세한 배경은 그쪽 주석 참고 (2026-08-24 최정우 추가)
*/
void CRawLogWorker::CheckSpeedZoneExitByRawGps(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if (!pstSession->bInSpeedZone || (m_stConfig.pcChargeDataLoader == nullptr)
		|| m_stConfig.strChargeInsertSQL.empty() || stRawLogInfo.bGpsLatNull || stRawLogInfo.bGpsLonNull)
		return;

	PGATE_INFO pstExitGate = m_stConfig.pcChargeDataLoader->GetGateByRoadId(pstSession->szSpeedZoneRoadId, 'O');
	if (pstExitGate == nullptr)
		return;
	PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szSpeedZoneRoadId);
	if (pstZone == nullptr)
		return;

	POINT stZoneStart, stGatePos, stRaw;
	stZoneStart.dfX = pstZone->dfFirstLon;  stZoneStart.dfY = pstZone->dfFirstLat;
	stGatePos.dfX = pstExitGate->dfLon;     stGatePos.dfY = pstExitGate->dfLat;
	stRaw.dfX = stRawLogInfo.dfX;           stRaw.dfY = stRawLogInfo.dfY;

	double dfGateFromStart = HaversineMeters(stZoneStart, stGatePos);
	double dfRawFromStart = HaversineMeters(stZoneStart, stRaw);
	if (dfRawFromStart < (dfGateFromStart + MM_RAWGPS_EXIT_MARGIN_M))
		return;

	PZONE_INFO pstZoneForRow = pstZone;
	CHARGE_INSERT_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

	char szSeq[16];
	snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
	stRow.strChargeSeq = szSeq;

	stRow.strChargeType = "3";								// SPEED
	stRow.strChargeUnit = "1";
	stRow.strLinkId = "";

	stRow.strFromId = pstSession->szSpeedEntryTollgateId;
	stRow.strToId = pstExitGate->szTollgateID;

	char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
	snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZoneForRow->dfFirstLat);
	snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZoneForRow->dfFirstLon);
	snprintf(szToLat, sizeof(szToLat), "%.06lf", pstExitGate->dfLat);
	snprintf(szToLon, sizeof(szToLon), "%.06lf", pstExitGate->dfLon);
	stRow.strFromLat = szFromLat;
	stRow.strFromLon = szFromLon;
	stRow.strToLat = szToLat;
	stRow.strToLon = szToLon;

	stRow.strZoneId = pstSession->szSpeedZoneRoadId;
	stRow.strZoneName = pstZoneForRow->szRoadNm;

	double dfLengthM = pstZoneForRow->dfLengthM;
	char szDistM[16];
	snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
	stRow.strDistM = szDistM;

	double dfElapsedSec = difftime(stRawLogInfo.dtGPS, pstSession->dtSpeedEntryTime);
	if (dfElapsedSec > 0.0)
	{
		double dfAvgSpeedKmh = (dfLengthM / dfElapsedSec) * 3.6;
		char szSpeedKmh[16];
		snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
		stRow.strSpeedKmh = szSpeedKmh;
	}

	char szSpeedLimit[8];
	snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZoneForRow->dfSpeedLimitKmh + 0.5));
	stRow.strSpeedLimitKmh = szSpeedLimit;

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d", static_cast<int>(dfElapsedSec + 0.5));
	stRow.strStaySeconds = szStaySeconds;

	stRow.strOccurDt = FormatDateTime14(pstSession->dtSpeedEntryTime);

	const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
	stRow.strTripStartDt = (pszTripStartDt != nullptr) ? pszTripStartDt : stRow.strOccurDt;

	stRow.strTollgateId = "";
	stRow.strEntryTollgateId = pstSession->szSpeedEntryTollgateId;
	stRow.strExitTollgateId = pstExitGate->szTollgateID;

	stRow.strRegDt = FormatDateTime14(time(nullptr));
	stRow.strUpdDt = stRow.strRegDt;

	// ProcessSpeedZoneCharge() 와 동일 근거 — 입구 게이트 미확정 시 AUDIT(N/3) (2026-08-25 최정우 추가)
	bool bGateAnomaly = (pstSession->szSpeedEntryTollgateId[0] == '\0') ||
		(pstExitGate->szTollgateID[0] == '\0') ||
		(strcmp(pstSession->szSpeedEntryTollgateId, pstExitGate->szTollgateID) == 0);
	stRow.strChargeYn = bGateAnomaly ? "N" : "Y";
	stRow.strChargeStatus = bGateAnomaly ? "3" : "0";

	pvtChargeInserts->push_back(stRow);

	LOGFMTI("[#%02d] speed zone exit charge queued (raw gps)!device=[%s] trip_id=[%s] seq=[%d] "
		"road=[%s] dist_m=[%s] margin_over=[%.1f]m",
		nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
		pstSession->szSpeedZoneRoadId, szDistM, dfRawFromStart - dfGateFromStart);

	pstSession->nChargeSeq += 1;
	pstSession->bInSpeedZone = false;
}

/**
 * @brief 면제도로 진입/이탈 판정 (2026-08-13 최초 추가, 2026-08-14 세 차례 재설계)
 * @remark 2026-08-14 사용자 재지시로 zone 기반 판정으로 복귀 — base_roadlink 에 등록된 면제도로
 *   구역(road_kind=5, link_ids)에 매칭 링크가 속하는지로 판정("모든 미등록 링크" 방식은 폐기).
 *   charge_type="5"(비과금도로 고유값 — 한때 "0"(일반도로와 통합)으로 바꿨다가 사용자 재지시로
 *   원복됨). from_id/to_id는 zone의 road_id(base_roadlink 등록값, 링크 ID 아님 — 이것도 한때
 *   링크ID였다가 재지시로 원복). zone_id/zone_name도 동일하게 실제 매칭된 구역의 road_id/road_nm.
 *   from/to_lat·lon은 진입/진출 시점의 매칭 위치. stay_seconds는 진출-진입 체류시간(초), 평균속도는
 *   speed_kmh로 분리 기록.
 *   이탈(다른 구역/미등록 링크로 이동) 또는 트립 종료 시 항상 `charge_yn='N'`/`charge_status='4'`
 *   (SKIP)로 1건 기록. 진행 중 GPS가 끊겨 세션이 TTL로 강제 마감되는 경우는
 *   AppendExpiredExemptZoneCharge() 가 별도 INSERT 처리.
*/

void CRawLogWorker::ProcessExemptZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경유 링크 포함 전체 경로에서 면제구역을 "전부" 수집 — 짧은 면제구역 링크가 두 GPS 사이에
	//   통째로 끼어 안 잡히는 경우 보완 + 한 링크가 여러 구역에 속하는 경우 대응
	//   (2026-08-20 추가 → 2026-08-23 복수 구역 지원)
	vector<PZONE_INFO> vtZones;
	{
		vector<PZONE_INFO> vtOne;
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
			m_stConfig.pcChargeDataLoader->GetExemptZonesByLinkId(stMatchLinkInfo.qwLinkID, &vtOne);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
				m_stConfig.pcChargeDataLoader->GetExemptZonesByLinkId(
					stMatchLinkInfo.aqwPathLinkIDs[i], &vtOne);
		}
		for (size_t i = 0; i < vtOne.size(); ++i)
		{
			bool bDup = false;
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(vtZones[e]->szRoadID, vtOne[i]->szRoadID) == 0) { bDup = true; break; }
			}
			if (!bDup) vtZones.push_back(vtOne[i]);
		}
	}

	const bool bTripEnding = bTrustedTripEnd;

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtExemptRuns.size(); )
	{
		ZONE_RUN_SESSION& stRun = pstSession->vtExemptRuns[si];

		// 누적 이동거리는 재진입 유예 대기 중에도 갱신한다 — 유예 구간의 이동거리·시간을
		//   dist_m·stay_seconds 에 포함(사용자 지시, 2026-08-14)
		POINT stPrev, stCur;
		stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
		stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;
		stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;

		bool bSameZone = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
		}

		if (bSameZone && !bTripEnding)
		{
			stRun.dtExitCandidateTime = 0;				// 재진입 확인 — 유예 대기 해제
			++si; continue;
		}

		if (!bTripEnding && vtZones.empty())
		{
			// "무존"(다음 행선지 미확인) — exempt_regrace 초 동안 확정 마감을 보류하고 재진입을 기다림.
			//   다른 구역이 이미 확인된 상태라면 유예 없이 곧바로 마감한다
			if (stRun.dtExitCandidateTime == 0)
				stRun.dtExitCandidateTime = stRawLogInfo.dtGPS;

			double dfGraceElapsedSec = difftime(stRawLogInfo.dtGPS, stRun.dtExitCandidateTime);
			if (dfGraceElapsedSec < static_cast<double>(m_stConfig.nExemptRegraceSec))
			{ ++si; continue; }
		}

		CHARGE_INSERT_ROW stRow;
		BuildExemptRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, stRawLogInfo.dtGPS, &stRow);
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] exempt zone exit recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dist_m=[%s] trip_ending=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRow.strDistM.c_str(), static_cast<int>(bTripEnding));

		pstSession->nChargeSeq += 1;
		pstSession->vtExemptRuns.erase(pstSession->vtExemptRuns.begin() + si);
	}

	// ── ② 새로 진입한 구역 세션 개시 ─────────────────────────────────────────
	if (bTripEnding)
		return;

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtExemptRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtExemptRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;

		ZONE_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		stRun.dtEntryTime = stRawLogInfo.dtGPS;
		stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
		stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
		stRun.dfAccumDistM = 0.0;
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;
		stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
		pstSession->vtExemptRuns.push_back(stRun);

		LOGFMTI("[#%02d] exempt zone entry!device=[%s] trip_id=[%s] road=[%s] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			pstSession->vtExemptRuns.size());
	}
}

/**
 * @brief 일반도로(ROAD_KIND=0, NODE_STEP) 진입/이탈 판정 (2026-08-14 최정우 추가)
 * @remark 비과금도로와 동일 구조 — 게이트가 없어 매칭 링크 ID로 구역 소속 여부를 직접 조회
 *   (GetNodeStepZoneByLinkId, ChargeDataLoader가 link_ids로 미리 구성해둔 역인덱스). 비과금도로와
 *   달리 실제 과금 대상이라, 이탈(다른 링크로 이동) 또는 트립 종료 시 항상 `charge_yn='Y'`/
 *   `charge_status='0'`(정상)으로 1건 기록 — 구역 진입 시점부터 이탈까지 누적거리를 dist_m 으로
 *   적재(사용자 지시, 2026-08-14 — 다른 유형과 겹쳐도 무조건 별도 부과, 폐쇄형/비과금도로와 동일
 *   패턴 재사용). 진행 중 GPS가 끊겨 세션이 TTL로 강제 마감되는 경우는
 *   AppendExpiredNodeStepCharge() 가 별도 INSERT 처리.
 * @remark 컬럼 매핑(사용자 지시, 2026-08-14) — 다른 4유형과 다른 점 위주:
 *   `charge_unit='0'`(NODE, 개방형과 동일 관례 — LINE 판정이지만 단위는 NODE로 지시받음),
 *   `occur_dt`=**진출 시각**(다른 4유형은 전부 "진입 시각"이라 헷갈리기 쉬움 — 일반도로만 예외),
 *   `upd_dt`=occur_dt와 동일(진출 시각, `reg_dt`=INSERT 실행 wall-clock 시각과는 다름),
 *   `trip_end_dt`는 이 함수에서 직접 안 채움 — TRIP_EVENT=2(트립 정상종료) 시 4유형 공용
 *   `[trip_end]` UPDATE 가 별도로 채움(사용자 지시의 "운행 정상 종료 시 종료 시각"과 동일 결과,
 *   기존 공용 메커니즘 재사용).
*/
void CRawLogWorker::ProcessNodeStepCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts, bool bTrustedTripEnd)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경유 링크 포함 전체 경로에서 일반도로 구역을 "전부" 수집한다. 한 링크가 여러 구역에 속할 수
	//   있어(인접 구역의 경계 링크 등) 복수 조회 API 를 쓴다 (2026-08-23 최정우 수정)
	vector<PZONE_INFO> vtZones;
	{
		vector<PZONE_INFO> vtOne;
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
			m_stConfig.pcChargeDataLoader->GetNodeStepZonesByLinkId(stMatchLinkInfo.qwLinkID, &vtOne);
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
				m_stConfig.pcChargeDataLoader->GetNodeStepZonesByLinkId(
					stMatchLinkInfo.aqwPathLinkIDs[i], &vtOne);
		}
		for (size_t i = 0; i < vtOne.size(); ++i)			// road_id 기준 중복 제거
		{
			bool bDup = false;
			for (size_t e = 0; e < vtZones.size(); ++e)
			{
				if (strcmp(vtZones[e]->szRoadID, vtOne[i]->szRoadID) == 0) { bDup = true; break; }
			}
			if (!bDup) vtZones.push_back(vtOne[i]);
		}
	}

	// 트립 종료(TRIP_EVENT=2) — 구역 위인 채로 끝나면 "이탈" 신호가 영영 안 옴, 즉시 강제 마감.
	//   ProcessRawLog() 가 스퓨리어스(순서역전) END 검사까지 마친 bTrustedTripEnd 를 그대로 씀
	//   (2026-08-25 최정우 수정)
	const bool bTripEnding = bTrustedTripEnd;

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtNodeStepRuns.size(); )
	{
		ZONE_RUN_SESSION& stRun = pstSession->vtNodeStepRuns[si];

		bool bSameZone = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
		}

		// 누적 이동거리는 구역 안에 있을 때만 더한다 — 이탈한 틱의 구역 밖 매칭점을 포함하지 않기 위함
		if (bSameZone)
		{
			stRun.nExitTicks = 0;						// 정상 유지 — 디바운스 해제 (2026-08-24 최정우 추가)
			POINT stPrev, stCur;
			stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
			stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;
			stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
			stRun.dfLastX = stMatchLinkInfo.dfMatchX;
			stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		}
		else if (stRun.nExitTicks == 0)
		{
			// 이탈 디바운스 스트릭의 첫 "밖" tick — 디바운스가 확정되는 마지막 tick(이미 구역에서
			//   한참 멀어진 지점)이 아니라 이 tick을 나중에 경계 노드 통과시각 보간의 "밖" 기준점으로
			//   써야 한다(위 dfFirstOutX/Y 필드 주석 참고, 2026-08-25 최정우 추가)
			stRun.dfFirstOutX = stMatchLinkInfo.dfMatchX;
			stRun.dfFirstOutY = stMatchLinkInfo.dfMatchY;
			stRun.dtFirstOut = stRawLogInfo.dtGPS;
		}

		if (bSameZone && !bTripEnding) { ++si; continue; }	// 계속 진행 중

		// node_exitcnt 회 연속 확인 후에만 이탈 확정 — 순간 오매칭 1틱으로 세션이 쪼개지는 것 방지
		//   (실측 000376_20260819094414 M45, 왕복분리 반대편 오매칭으로 RL-Z00002 세션이 40~44/
		//   46~ 로 쪼개짐). 트립종료는 디바운스 없이 즉시 마감(다음 틱이 안 옴) (2026-08-24 최정우 추가)
		if (!bTripEnding)
		{
			stRun.nExitTicks += 1;
			if (stRun.nExitTicks < m_stConfig.nNodeExitCnt) { ++si; continue; }
		}

		// ── 이탈 지점 보정 ──
		//   GPS 표본은 구역 경계에 맞춰 찍히지 않아, 마지막 매칭점에서 끊으면 통행거리가 짧게
		//   계산된다. 구역 안에서 마지막으로 달린 링크의 종료 노드까지 채운다(구역 끝 좌표든
		//   중간 교차로든 결국 그 노드로 수렴). 트립종료·TTL 로 구역 안에서 끝난 경우는
		//   실제로 거기까지만 달린 것이라 보정하지 않는다.
		if (!bSameZone && (stRun.qwLastLinkID != 0) && (m_stConfig.pcDataLoader != nullptr))
		{
			PLINK_INFO pstLastLink = m_stConfig.pcDataLoader->GetLinkInfo(stRun.qwLastLinkID);
			if (pstLastLink != nullptr)
			{
				POINT stFrom, stNode;
				stFrom.dfX = stRun.dfLastX;  stFrom.dfY = stRun.dfLastY;
				stNode.dfX = static_cast<double>(pstLastLink->dwEdNodeX) / 360000.0;
				stNode.dfY = static_cast<double>(pstLastLink->dwEdNodeY) / 360000.0;

				double dfTail = HaversineMeters(stFrom, stNode);
				// 링크 길이를 넘으면 매칭이 튄 것으로 보고 버린다(과다 계상 방지)
				if ((dfTail > 0.0) && (dfTail <= pstLastLink->dfLen + 1.0))
				{
					stRun.dfAccumDistM += dfTail;
					stRun.dfLastX = stNode.dfX;
					stRun.dfLastY = stNode.dfY;

					LOGFMTI("[#%02d] node step exit clipped to node!device=[%s] trip_id=[%s] "
						"road=[%s] link=[%llu] tail=[%.1f]m total=[%.1f]m",
						nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
						static_cast<unsigned long long>(stRun.qwLastLinkID),
						dfTail, stRun.dfAccumDistM);
				}

				// 종료 노드 통과 시각 보간 — ProcessOpenGateCharge()/ProcessClosedRoadCharge() 동일
				//   근거(InterpolateGateCrossingTime() 참고, 목표점만 게이트 대신 이 노드). "밖" 기준점은
				//   세션 범용 직전tick(dfLastMatchX/Y)이 아니라 dfFirstOutX/Y 를 써야 한다 — node_exitcnt
				//   디바운스로 이탈 확정이 몇 틱 뒤에 일어나므로, 세션 범용 직전tick은 이미 그 디바운스
				//   구간 안의 tick(구역에서 한참 멀어진 지점)이라 엉뚱한 보간이 나온다(2026-08-25
				//   최정우 추가, 사용자 지시 — dfFirstOutX/Y 필드 주석 참고)
				if (stRun.dtFirstOut != 0)
				{
					stRun.dtLastInZoneTime = InterpolateGateCrossingTime(
						stFrom.dfX, stFrom.dfY, stRun.dtLastInZoneTime,
						stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut,
						stNode.dfX, stNode.dfY);
				}
			}
		}

		// node_exitcnt 디바운스로 이탈 확정이 몇 틱 늦게 일어나므로(이미 다른 구역에 들어가 있을 수
		//   있음), occur_dt·stay_seconds 는 이번 틱(dtGPS)이 아니라 구역 안에서 실제로 마지막 확정
		//   매칭됐던 시각(dtLastInZoneTime, 위에서 보간됐으면 그 값)을 써야 한다 — 안 그러면 디바운스
		//   대기 시간이 stay_seconds 에 섞여 다음 구역 범위와 겹친다(실측 000376_20260819094414 —
		//   실제 마지막 확정은 seq38인데 디바운스 대기 중 seq42~44 는 이미 RL-Z00003 로 넘어가 있어
		//   occur_dt 를 그대로 쓰면 두 구역 G범위가 겹쳐 보임). 세션이 진입 즉시 이 값을 채우므로
		//   0 은 실질적으로 없지만 방어적으로 폴백 유지 (2026-08-24 최정우 추가)
		time_t dtNodeStepExitTime = (stRun.dtLastInZoneTime != 0) ? stRun.dtLastInZoneTime : stRawLogInfo.dtGPS;

		CHARGE_INSERT_ROW stRow;
		BuildNodeStepRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, dtNodeStepExitTime, &stRow);
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] node step exit recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dist_m=[%s] avg_speed=[%s] trip_ending=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRow.strDistM.c_str(), stRow.strSpeedKmh.c_str(),
			static_cast<int>(bTripEnding));

		pstSession->nChargeSeq += 1;
		pstSession->vtNodeStepRuns.erase(pstSession->vtNodeStepRuns.begin() + si);
	}

	// ── ② 새로 진입한 구역 세션 개시 ─────────────────────────────────────────
	if (bTripEnding)
		return;											// 종료 틱에서는 새로 열지 않는다

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtNodeStepRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtNodeStepRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;

		ZONE_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		// 진입 경계(구역에 들어온 링크의 시작 노드) 통과 시각 보간 — ProcessOpenGateCharge() 동일
		//   근거 참고. 직전 tick 이 없으면(트립 첫 tick) 그대로 원시 시각 사용
		//   (2026-08-25 최정우 추가, 사용자 지시)
		if (pstSession->bHasLastMatch)
		{
			stRun.dtEntryTime = InterpolateGateCrossingTime(
				pstSession->dfLastMatchX, pstSession->dfLastMatchY, pstSession->dtLastMatchGps,
				stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY, stRawLogInfo.dtGPS,
				stMatchLinkInfo.dfStNodeX, stMatchLinkInfo.dfStNodeY);
		}
		else
		{
			stRun.dtEntryTime = stRawLogInfo.dtGPS;
		}
		stRun.dfEntryX = stMatchLinkInfo.dfMatchX;
		stRun.dfEntryY = stMatchLinkInfo.dfMatchY;
		stRun.dfAccumDistM = 0.0;
		stRun.dfLastX = stMatchLinkInfo.dfMatchX;
		stRun.dfLastY = stMatchLinkInfo.dfMatchY;
		stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
		pstSession->vtNodeStepRuns.push_back(stRun);

		LOGFMTI("[#%02d] node step entry!device=[%s] trip_id=[%s] road=[%s] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			pstSession->vtNodeStepRuns.size());
	}
}

/**
 * @brief 주정차 판정 — 맵매칭 전 raw GPS 기준, 구역 진입/이탈만 판단 (2026-08-13 최정우 추가)
 * @remark 다른 3종(개방형·폐쇄형·구간단속)과 달리 매칭 결과(MATCH_LINK_INFO)를 전혀 안 씀 — 맵매칭은
 *   가장 가까운 도로 링크로 좌표를 강제 스냅시켜 도로 밖 주정차 위치를 왜곡하고, 정지 상태에서는
 *   스냅 자체가 불안정하기 때문(호출측이 RunMapMatch 호출 "전"에 이 함수를 실행).
 *   판정은 구역판정(위치, ACCURACY_M 적응형 버퍼로 GPS 오차 흡수) 하나뿐 — 서행/정차 구분(속도)은
 *   안 씀. dist_m(누적거리)·speed_kmh(평균속도)를 어차피 같이 기록하므로 "이게 실제 정차인지
 *   그냥 지나간 건지"는 과금서버가 그 값들로 판단 가능 — MapMatchSvr가 진입 시점에 속도로 미리
 *   걸러버리면 그 판단을 중복으로 하는 셈이라 제거함(사용자 지적, 2026-08-13 — park_dwell 제거와
 *   같은 논리). DRIVE_STATUS 도 안 씀(엔진 on 상태 정차 위반을 장비가 ON_ROAD 로 계속 보고할 위험)
 *   — [[project_parking_match_pseudocode]] 2026-08-13 개정 참고.
 *   구역 이탈은 park_exitcnt 회 연속 확인 후에만 확정(디바운스) — GPS 튐으로 인한 세션 오종료 방지.
 *   체류시간(stay_seconds)은 임계값 없이 항상 사실대로 기록만 함 — 위반 확정(유예시간 초과 등)은
 *   과금서버 책임이라 이 서버가 게이팅하지 않음(사용자 지시, 2026-08-13).
*/
void CRawLogWorker::ProcessParkingCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		bool bTrustedTripEnd, bool bMatchTrusted, double dfMatchX, double dfMatchY)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 경계 통과 시각 보간용 "직전 틱" 스냅샷 — 이번 틱으로 갱신하기 "전" 값을 먼저 떼어간다.
	//   early return 경로가 아래에 여럿 있어(bTripEnding 등) 함수 맨 앞에서 한 번만 갱신해
	//   호출마다 반드시 실행되게 한다 (사용자 지시, 2026-08-24 최정우 추가)
	const bool bPrevRawKnown = pstSession->bHasLastRawTick;
	const double dfPrevRawX = pstSession->dfLastRawTickX;
	const double dfPrevRawY = pstSession->dfLastRawTickY;
	const time_t dtPrevRaw = pstSession->dtLastRawTick;
	pstSession->dfLastRawTickX = stRawLogInfo.dfX;
	pstSession->dfLastRawTickY = stRawLogInfo.dfY;
	pstSession->dtLastRawTick = stRawLogInfo.dtGPS;
	pstSession->bHasLastRawTick = true;

	// ── 좌표 정확도 상한 (park_accmax, 0=비활성) ─────────────────────────────────
	//   측위에 실패한 단말은 셀 기반 대체 위치로 수백 m 점프한 뒤 그 좌표에 얼어붙는다.
	//   실측(실주행 11트립, analysis/rawvld_realcheck.py — 앞뒤 신뢰점 사이를 도로 형상을
	//   따라 보간해 참위치를 추정하는 방식, leave-one-out 검증 링크일치 93.9%):
	//     accuracy_m 51~100  추정오차 중앙 34.7m — 폴리곤 판정 17건 전부 허위 진입(일치 0)
	//     accuracy_m 101~    추정오차 중앙 189m·최대 486m, 좌표 동결률 64.6%
	//                        — 허위진입 20건·누락 20건
	//     accuracy_m 16~50   추정오차 중앙 8.6m — 폴리곤 오판정 0건이라 살린다
	//   park_buf(버퍼 상한)로는 못 막는다. 버퍼는 "얼마나 여유를 줄까"이지 "이 좌표를 믿을까"가
	//   아니라서, 좌표 자체가 200m 틀리면 버퍼를 좁혀도 엉뚱한 자리에서 판정한다.
	//
	//   맵매칭에는 영향이 없다 — 걸러지는 행(accuracy_m>50)은 전부 RAW_VLD=false 이고
	//   (경계가 15라 구조적으로 그렇다) 이미 ShouldSkipGpsInput() 에서 맵매칭을 SKIP 한다.
	//   반대로 매칭을 타는 RAW_VLD=true 행은 accuracy_m<=15 라 이 게이트에 걸릴 수 없다.
	//   "RAW_VLD=false 도 주정차 판정은 수행한다"(2026-08-22 사용자 확정)는 유지된다 —
	//   그 근거였던 "멈추면 GPS 가 나빠지는데 하필 그때 판정이 필요하다"는 16~50 구간에서
	//   여전히 유효하고, 못 쓸 좌표만 걷어낸다.
	//   ACCURACY_M 이 NULL(-1)이면 판단 근거가 없으므로 걸러내지 않는다.
	//
	//   적용 범위 — "세션 개시"에만 건다. 처음엔 함수 진입부에서 통째로 return 했는데
	//   실측에서 반례가 나왔다(000376_20260819094414): 정상 좌표로 폴리곤에 들어가 정차한
	//   진짜 도착 정차인데(seq 133 까지 정확도 12·폴리곤 내), 정차 후 정확도가 78~346 으로
	//   나빠졌고 트립 종료(TRIP_EVENT=END) 행도 정확도 321 이었다. 그 행에서 return 하는 바람에
	//   ①의 세션 마감이 실행되지 못해 199초짜리 진짜 체류가 통째로 사라졌다.
	//   그래서 아래처럼 나눈다.
	//     ① 진행 중 세션 — 갱신은 건너뛰되(위치·거리 오염·허위 이탈틱 방지) 마감은 그대로 한다
	//     ② 세션 개시   — 나쁜 좌표로는 새 세션을 열지 않는다 (허위 진입 차단)
	//   즉 나쁜 좌표를 "정보 없음"으로 다루는 것이지 "구역 밖"으로 다루는 게 아니다.
	//   (2026-08-23 최정우 추가)
	const bool bAccTrusted = (m_stConfig.nParkAccMax <= 0) || (stRawLogInfo.nAccuracyM < 0)
		|| (stRawLogInfo.nAccuracyM <= m_stConfig.nParkAccMax);

	// 구역판정 버퍼 — ACCURACY_M 적응형(포인트마다 오차만큼), 상한은 park_buf 로 캡(이상치 방지)
	double dfBufM = static_cast<double>(m_stConfig.nParkBuf);
	if ((stRawLogInfo.nAccuracyM >= 0) && (static_cast<double>(stRawLogInfo.nAccuracyM) < dfBufM))
		dfBufM = static_cast<double>(stRawLogInfo.nAccuracyM);

	// ── 판정 규칙 (2026-08-22 재작성 → 2026-08-23 복수 구역 지원 → 2026-08-25 규칙4 정지 예외) ──
	//   규칙1  원시 좌표가 폴리곤 내 + 맵매칭 실패                    → 주정차
	//   규칙2  원시 좌표가 폴리곤 내 + 매칭 성공 + 매칭 좌표도 같은 폴리곤 내 → 주정차
	//   규칙4  원시 좌표는 폴리곤 내인데 매칭 좌표는 그 폴리곤 밖     → 통과 중이므로 제외
	//   폴리곤이 겹쳐 설정될 수 있어(시간대별 규제가 다른 구역 등) 포함하는 구역을 전부 다룬다.
	//
	//   규칙4는 "매칭 좌표가 raw 좌표보다 신뢰할 만하다"는 전제인데, 이 전제는 차량이 실제로
	//   움직여야(방위각·궤적으로 후보 링크를 구분) 성립한다. 정지 상태(SPEED_KMH<1)에서는 그
	//   구분 신호 자체가 없어 근처 아무 링크에나 확신 있게(bReverseSkip/bClampLowConf/
	//   bAmbiguousReverse 어디에도 안 걸리는 채로) 잘못 스냅될 수 있다 — 실측
	//   000370_20260819093236 seq29~71(2분 9초, DRIVE_STATUS=PARKED·SPEED_KMH=0 내내, raw
	//   좌표는 RL-Z00001 폴리곤 안 그대로)에서 매칭이 인접 링크에 확신 있게 스냅되는 바람에
	//   규칙4가 전체 구간을 "통과 중"으로 오판 → PARKING 레코드가 한 건도 안 남음(진입
	//   후보(park_entrycnt)조차 못 쌓임) 확인. 정지 중엔 규칙4를 적용하지 않고 규칙1/2(raw
	//   기준)로만 판정한다 (2026-08-25 최정우 추가).
	const bool bLikelyStationary = (stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f);

	vector<PZONE_INFO> vtZones;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
		stRawLogInfo.dfX, stRawLogInfo.dfY, dfBufM, &vtZones);

	if (bMatchTrusted && !bLikelyStationary && !vtZones.empty())
	{
		vector<PZONE_INFO> vtMatch;
		m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(dfMatchX, dfMatchY, 0.0, &vtMatch);
		vector<PZONE_INFO> vtKeep;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			for (size_t m = 0; m < vtMatch.size(); ++m)
			{
				if (strcmp(vtZones[e]->szRoadID, vtMatch[m]->szRoadID) == 0)
				{ vtKeep.push_back(vtZones[e]); break; }
			}
		}
		vtZones.swap(vtKeep);							// 규칙4 — 매칭 좌표가 그 구역 밖이면 제외
	}

	// 속도 상한 — park_speedmax=0 이면 비활성(기본). 위반 판정 정책은 과금서버 몫이라 엔진은
	//   구역 진입 사실만 등록한다 (2026-08-22 사용자 확정)
	const bool bSpeedGate = (m_stConfig.nParkSpeedMax > 0);
	const bool bSlowEnough = (!bSpeedGate) || (stRawLogInfo.fSpeed < 0.0f)
		|| (stRawLogInfo.fSpeed <= static_cast<float>(m_stConfig.nParkSpeedMax));
	const bool bSpeedExit = !vtZones.empty() && !bSlowEnough;   // 재가속 이탈은 유예를 건너뛴다
	if (!bSlowEnough)
		vtZones.clear();

	// ProcessRawLog() 가 스퓨리어스(순서역전) END 검사까지 마친 bTrustedTripEnd 를 그대로 씀
	//   (2026-08-25 최정우 수정)
	const bool bTripEnding = bTrustedTripEnd;
	const bool bThisRowTrusted = stRawLogInfo.bRawVldKnown && stRawLogInfo.bRawVld;
	// DRIVE_STATUS=PARKED + 속도 0 이면 좌표 변화는 GPS 튐이다 — 거리로 세지 않고 체류만 연장
	const bool bParkedStill = (stRawLogInfo.nDriveStatus == DRIVE_STATUS_PARKED)
		&& (stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f);

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtParkRuns.size(); )
	{
		PARK_RUN_SESSION& stRun = pstSession->vtParkRuns[si];

		// 정확도 미달 좌표는 "정보 없음" — 위치·누적거리를 오염시키지 않고 이탈 디바운스도
		//   올리지 않는다. 다만 트립 종료 행이면 마감은 해야 하므로 통과시킨다 (2026-08-23 최정우 추가)
		if (!bAccTrusted && !bTripEnding) { ++si; continue; }

		bool bSameZone = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(stRun.szRoadID, vtZones[e]->szRoadID) == 0) { bSameZone = true; break; }
		}

		if (bSameZone)
		{
			POINT stPrev, stCur;
			stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
			stCur.dfX = stRawLogInfo.dfX;  stCur.dfY = stRawLogInfo.dfY;
			if (!bParkedStill)
				stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
			stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
			stRun.dfLastInZoneX = stRawLogInfo.dfX;
			stRun.dfLastInZoneY = stRawLogInfo.dfY;
		}
		stRun.dfLastX = stRawLogInfo.dfX;				// 하버사인 기준점은 항상 최신 좌표
		stRun.dfLastY = stRawLogInfo.dfY;
		if (bThisRowTrusted)
		{
			stRun.dtLastConfirmedTime = stRawLogInfo.dtGPS;
			stRun.dfLastConfirmedX = stRawLogInfo.dfX;
			stRun.dfLastConfirmedY = stRawLogInfo.dfY;
		}

		if (bSameZone && !bTripEnding)
		{
			stRun.nExitTicks = 0;						// 정상 유지 — 디바운스·유예 해제
			stRun.dtExitCandidateTime = 0;
			++si; continue;
		}

		if (!bTripEnding)
		{
			// 디바운스·유예 시작 "전", 구역 밖으로 처음 찍힌 원시좌표 — 경계 통과 시각 보간의
			//   Out 쪽 앵커. 이후 재확인 없이 그대로 유지(디바운스 도중 다시 잠깐 원존이어도
			//   nExitTicks 는 0으로 안 돌아가는 한 갱신 안 함) (2026-08-24 최정우 추가)
			if (stRun.nExitTicks == 0)
			{
				stRun.dfFirstOutX = stRawLogInfo.dfX;
				stRun.dfFirstOutY = stRawLogInfo.dfY;
				stRun.dtFirstOut = stRawLogInfo.dtGPS;
			}
			stRun.nExitTicks += 1;						// park_exitcnt 회 연속 확인 후에만 이탈 확정
			if (stRun.nExitTicks < m_stConfig.nParkExitCnt) { ++si; continue; }

			// "무존"(다음 행선지 미확인) 상태에서만 재진입 유예를 준다. 재가속으로 인한 이탈은
			//   확정적 사실이라 유예 없이 즉시 마감한다
			if (vtZones.empty() && !bSpeedExit)
			{
				if (stRun.dtExitCandidateTime == 0)
					stRun.dtExitCandidateTime = stRawLogInfo.dtGPS;
				double dfGraceElapsedSec = difftime(stRawLogInfo.dtGPS, stRun.dtExitCandidateTime);
				if (dfGraceElapsedSec < static_cast<double>(m_stConfig.nParkRegraceSec))
				{ ++si; continue; }
			}
		}

		// 체류 종료는 "마지막으로 조건을 만족한 시각" — 디바운스·유예에 쓴 시간을 위반에 넣지 않는다.
		//   트립종료(bTripEnding)로 강제마감된 경우는 그 뒤에 관측된 "밖" 좌표 자체가 없어 보간
		//   근거가 없으므로 무보정(dtLastInZoneTime 그대로) (2026-08-24 최정우 추가)
		time_t dtParkEnd = stRun.dtLastInZoneTime;
		if (!bTripEnding)
		{
			PZONE_INFO pstExitZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(stRun.szRoadID);
			dtParkEnd = InterpolateZoneCrossingTime(pstExitZone,
				stRun.dfLastInZoneX, stRun.dfLastInZoneY, stRun.dtLastInZoneTime,
				stRun.dfFirstOutX, stRun.dfFirstOutY, stRun.dtFirstOut);
		}
		CHARGE_INSERT_ROW stRow;
		bool bMeetsFineMin = BuildParkRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, dtParkEnd,
			stRun.dfLastInZoneX, stRun.dfLastInZoneY, "Y", "0", &stRow);
		if (bMeetsFineMin) pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] parking dwell recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dwell=[%s]s dist_m=[%s] avg_speed=[%s] trip_ending=[%d] registered=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRow.strStaySeconds.c_str(), stRow.strDistM.c_str(),
			stRow.strSpeedKmh.c_str(), static_cast<int>(bTripEnding), static_cast<int>(bMeetsFineMin));

		pstSession->nChargeSeq += 1;
		pstSession->vtParkRuns.erase(pstSession->vtParkRuns.begin() + si);
	}

	if (bTripEnding)
	{
		pstSession->vtParkCands.clear();
		return;
	}

	// 정확도 미달 좌표로는 새 세션을 열지 않는다. 후보 카운터도 건드리지 않는다 —
	//   "정보 없음"이므로 좋은 좌표의 연속을 끊지도, 늘리지도 않는다 (2026-08-23 최정우 추가)
	if (!bAccTrusted)
		return;

	// ── ② 세션 개시 — 구역별로 park_entrycnt 회 연속 충족해야 연다 ────────────
	//   1~2점(0~3초)짜리는 체류시간 산출이 불가능하고 GPS 튐과 구분되지 않는다
	for (size_t ci = 0; ci < pstSession->vtParkCands.size(); )
	{
		bool bStill = false;
		for (size_t e = 0; e < vtZones.size(); ++e)
		{
			if (strcmp(pstSession->vtParkCands[ci].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bStill = true; break; }
		}
		if (bStill) ++ci;
		else pstSession->vtParkCands.erase(pstSession->vtParkCands.begin() + ci);   // 연속 끊김
	}

	for (size_t e = 0; e < vtZones.size(); ++e)
	{
		bool bOpen = false;
		for (size_t si = 0; si < pstSession->vtParkRuns.size(); ++si)
		{
			if (strcmp(pstSession->vtParkRuns[si].szRoadID, vtZones[e]->szRoadID) == 0)
			{ bOpen = true; break; }
		}
		if (bOpen) continue;							// 이미 진행 중

		PARK_CANDIDATE *pstCand = nullptr;
		for (size_t ci = 0; ci < pstSession->vtParkCands.size(); ++ci)
		{
			if (strcmp(pstSession->vtParkCands[ci].szRoadID, vtZones[e]->szRoadID) == 0)
			{ pstCand = &pstSession->vtParkCands[ci]; break; }
		}
		if (pstCand == nullptr)
		{
			PARK_CANDIDATE stNew;
			strncpy(stNew.szRoadID, vtZones[e]->szRoadID, sizeof(stNew.szRoadID) - 1);
			stNew.szRoadID[sizeof(stNew.szRoadID) - 1] = '\0';
			// 연속의 첫 좌표 — 세션 진입 시각이 된다. 직전 틱(밖)이 있으면 그 사이에서 실제 경계
			//   통과 시각을 보간 — 버퍼만으로 판정됐거나(원시좌표는 폴리곤 밖) 직전 틱이 없으면
			//   (트립 첫 틱 등) 보정 근거가 없어 원시 GPS 시각 그대로 (2026-08-24 최정우 추가)
			stNew.dtTime = bPrevRawKnown
				? InterpolateZoneCrossingTime(vtZones[e], stRawLogInfo.dfX, stRawLogInfo.dfY, stRawLogInfo.dtGPS,
					dfPrevRawX, dfPrevRawY, dtPrevRaw)
				: stRawLogInfo.dtGPS;
			stNew.dfX = stRawLogInfo.dfX;
			stNew.dfY = stRawLogInfo.dfY;
			pstSession->vtParkCands.push_back(stNew);
			pstCand = &pstSession->vtParkCands.back();
		}
		pstCand->nTicks += 1;
		if (pstCand->nTicks < m_stConfig.nParkEntryCnt) continue;

		PARK_RUN_SESSION stRun;
		strncpy(stRun.szRoadID, vtZones[e]->szRoadID, sizeof(stRun.szRoadID) - 1);
		stRun.szRoadID[sizeof(stRun.szRoadID) - 1] = '\0';
		stRun.dtEntryTime = pstCand->dtTime;			// 연속의 첫 좌표부터 체류 시작
		stRun.dfEntryX = pstCand->dfX;
		stRun.dfEntryY = pstCand->dfY;
		stRun.dfLastX = stRawLogInfo.dfX;
		stRun.dfLastY = stRawLogInfo.dfY;
		stRun.dtLastInZoneTime = stRawLogInfo.dtGPS;
		stRun.dfLastInZoneX = stRawLogInfo.dfX;
		stRun.dfLastInZoneY = stRawLogInfo.dfY;
		stRun.dtLastConfirmedTime = stRawLogInfo.dtGPS;
		stRun.dfLastConfirmedX = stRawLogInfo.dfX;
		stRun.dfLastConfirmedY = stRawLogInfo.dfY;
		pstSession->vtParkRuns.push_back(stRun);

		LOGFMTI("[#%02d] parking zone entry!device=[%s] trip_id=[%s] road=[%s] cnt=[%d] speed=[%d] open=[%zu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRun.szRoadID,
			pstCand->nTicks, static_cast<int>(stRawLogInfo.fSpeed), pstSession->vtParkRuns.size());

		for (size_t ci = 0; ci < pstSession->vtParkCands.size(); ++ci)
		{
			if (strcmp(pstSession->vtParkCands[ci].szRoadID, stRun.szRoadID) == 0)
			{ pstSession->vtParkCands.erase(pstSession->vtParkCands.begin() + ci); break; }
		}
	}
}

/**
 * @brief 하버사인 거리 계산 (WGS84 경위도 → m) (2026-07-08 최정우 추가)
 * @param[in] stA 좌표 A (dfX=경도, dfY=위도, 단위 도)
 * @param[in] stB 좌표 B
 * @return 두 점 사이 지표 거리(m)
 * @remark a = sin²(Δlat/2) + cos(lat1)·cos(lat2)·sin²(Δlon/2), d = 2R·asin(√a)
*/
double CRawLogWorker::HaversineMeters(const POINT& stA, const POINT& stB)
{
	const double dfR = 6378137.0;								// WGS84 장반경(m)
	double dfLat1 = RAD(stA.dfY);
	double dfLat2 = RAD(stB.dfY);
	double dfDLat = RAD(stB.dfY - stA.dfY);
	double dfDLon = RAD(stB.dfX - stA.dfX);

	double dfA = sin(dfDLat / 2.0) * sin(dfDLat / 2.0)
		+ cos(dfLat1) * cos(dfLat2) * sin(dfDLon / 2.0) * sin(dfDLon / 2.0);
	if (dfA > 1.0) dfA = 1.0;									// 부동소수 오차 클램프
	return 2.0 * dfR * asin(sqrt(dfA));
}

/**
 * @brief INTERSECT_LEN 산출 — GPS 좌표와 세그먼트 교차점(MATCH) 사이 거리(m)
 * @param[in] stRawLogInfo 원시 GPS (dfX=경도, dfY=위도, 도)
 * @param[in] dfMatchLon 세그먼트 교차점 경도 (MATCH_LON)
 * @param[in] dfMatchLat 세그먼트 교차점 위도 (MATCH_LAT)
 * @return 반올림 정수 거리(m), GPS 무효 시 -1
*/
int CRawLogWorker::CalcIntersectLen(const sRawLogInfo& stRawLogInfo,
		double dfMatchLon, double dfMatchLat)
{
	if (stRawLogInfo.bGpsLatNull || stRawLogInfo.bGpsLonNull)
		return -1;

	POINT stGps;
	POINT stMatch;
	stGps.dfX = stRawLogInfo.dfX;
	stGps.dfY = stRawLogInfo.dfY;
	stMatch.dfX = dfMatchLon;
	stMatch.dfY = dfMatchLat;
	return static_cast<int>(HaversineMeters(stGps, stMatch) + 0.5);
}

/**
 * @brief PostgreSQL text[] 리터럴용 문자열 이스케이프
 * @param[in] strValue 원본 문자열
 * @return 이스케이프된 문자열
*/
string CRawLogWorker::EscapePgArrayText(const string& strValue)
{
	string strEscaped;
	strEscaped.reserve(strValue.size() + 4);

	for (size_t i=0; i<strValue.size(); ++i)
	{
		const char c = strValue[i];
		if (c == '\\' || c == '"')
			strEscaped += '\\';
		strEscaped += c;
	}

	return strEscaped;
}

/**
 * @brief PostgreSQL text[] 리터럴 생성
 * @param[in] vtValues text 배열 원소 목록
 * @return PostgreSQL text[] 리터럴 (예: {"a","b"})
*/
string CRawLogWorker::BuildPgTextArray(const vector<string>& vtValues)
{
	string strArray = "{";
	for (size_t i=0; i<vtValues.size(); ++i)
	{
		if (i > 0)
			strArray += ",";
		strArray += "\"";
		// text[] 원소 PostgreSQL 이스케이프 (2026-07-08 최정우 주석 추가)
		strArray += EscapePgArrayText(vtValues[i]);
		strArray += "\"";
	}
	strArray += "}";
	return strArray;
}

/**
 * @brief bulk UPDATE 1행 적재
 * @param[out] pvtUpdates bulk UPDATE 대상 목록
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] nStatus MATCH_STATUS (1/3/4/0)
 * @param[in] nIntersectLen GPS↔세그먼트 교차점 거리(m, INTERSECT_LEN), -1 이면 미갱신
 * @param[in] pdfMatchLat 매칭 위도 (MATCHED 시), nullptr 이면 미갱신
 * @param[in] pdfMatchLon 매칭 경도 (MATCHED 시), nullptr 이면 미갱신
 * @return true(적재 성공), false(pvtUpdates null·trip_id 무효)
 * @remark invalid trip_id 시 false — run() orphan release 가 PK 없으면 복구 대기
*/
bool CRawLogWorker::AppendUpdateRow(vector<RAW_LOG_UPDATE_ROW> *pvtUpdates,
		const sRawLogInfo& stRawLogInfo, sint16 nStatus, int nIntersectLen,
		const double *pdfMatchLat, const double *pdfMatchLon, uint64 qwMatchLinkId)
{
	if (pvtUpdates == nullptr)
		return false;

	if (stRawLogInfo.szTripID[0] == '\0')
	{
		LOGFMTE("worker update error! invalid trip_id! seq=[%u] device=[%s]",
			stRawLogInfo.dwSeqNo, stRawLogInfo.szDeviceKey);
		return false;
	}

	char szSeqNo[16];
	char szStatus[8];
	char szIntersectLen[16];
	char szMatchLat[32];
	char szMatchLon[32];
	char szMatchLinkId[24];

	snprintf(szSeqNo, sizeof(szSeqNo), "%u", stRawLogInfo.dwSeqNo);
	snprintf(szStatus, sizeof(szStatus), "%d", static_cast<int>(nStatus));
	if (nIntersectLen >= 0)
		snprintf(szIntersectLen, sizeof(szIntersectLen), "%d", nIntersectLen);
	else
		szIntersectLen[0] = '\0';

	// 좌표가 제공되면 상태(MATCHED/SKIP) 무관 저장. 반경 밖 SKIP 도 최근접 좌표 기록 (2026-07-10 최정우 수정)
	if ((pdfMatchLat != nullptr) && (pdfMatchLon != nullptr))
	{
		snprintf(szMatchLat, sizeof(szMatchLat), "%.06lf", *pdfMatchLat);
		snprintf(szMatchLon, sizeof(szMatchLon), "%.06lf", *pdfMatchLon);
	}
	else
	{
		szMatchLat[0] = '\0';
		szMatchLon[0] = '\0';
	}

	// 매칭 링크 ID (0=미제공 → 빈 문자열, SQL CASE 에서 상태별 처리) (2026-07-15 최정우 추가)
	if (qwMatchLinkId != 0)
		snprintf(szMatchLinkId, sizeof(szMatchLinkId), "%llu",
			static_cast<unsigned long long>(qwMatchLinkId));
	else
		szMatchLinkId[0] = '\0';

	RAW_LOG_UPDATE_ROW stRow;
	stRow.strTripId = stRawLogInfo.szTripID;
	stRow.strGpsSeq = szSeqNo;
	stRow.strMatchStatus = szStatus;
	stRow.strIntersectLen = szIntersectLen;
	stRow.strMatchLat = szMatchLat;
	stRow.strMatchLon = szMatchLon;
	stRow.strMatchLinkId = szMatchLinkId;
	pvtUpdates->push_back(stRow);
	return true;
}

/**
 * @brief PQexec UPDATE/COMMAND 영향 행 수
 * @param[in] pcResult PQ 실행 결과
 * @return 영향 받은 행 수 (없으면 0)
 * @remark PGRES_COMMAND_OK 여도 WHERE 불일치 시 0 가능 (#5)
*/
int CRawLogWorker::GetPgCmdTuples(PGresult *pcResult)
{
	if (pcResult == nullptr)
		return 0;

	const char *pszAffected = PQcmdTuples(pcResult);
	if ((pszAffected == nullptr) || (pszAffected[0] == '\0'))
		return 0;

	return atoi(pszAffected);
}

/**
 * @brief UPDATE 영향 행 수가 기대값과 일치하는지 검증
 * @param[in] pcResult PQ 실행 결과
 * @param[in] nExpected 기대 갱신 행 수
 * @param[in] pszLogTag 로그 태그 (nullptr 이면 "워커")
 * @return true(일치), false(불일치·pcResult null)
*/
bool CRawLogWorker::CheckPgUpdateAffected(PGresult *pcResult, int nExpected,
		const char *pszLogTag)
{
	// PQcmdTuples 로 영향 행 수 추출 (2026-07-08 최정우 주석 추가)
	const int nAffected = GetPgCmdTuples(pcResult);
	if (nAffected == nExpected)
		return true;

	LOGFMTW("%s partial update! expected=[%d] affected=[%d]",
		(pszLogTag != nullptr) ? pszLogTag : "worker",
		nExpected, nAffected);
	return false;
}

/**
 * @brief prim_rawgps 처리 결과 일괄 갱신 [rawgps_update]
 * @param[in] pcConn DB 커넥션
 * @param[in] vtUpdates bulk UPDATE 대상 행 목록
 * @return true(전건 갱신), false(실행 오류·부분 갱신·인자 무효)
 * @remark
 *   - WHERE MATCH_STATUS=2 인 행만 갱신 (예약된 batch)
 *   - $4=INTERSECT_LEN[] : GPS↔세그먼트 교차점 거리(m). MATCH_LAT/LON 과 함께 AppendUpdateRow 값 사용
 *   - 별도 release SQL 없음. 실패 복구는 BulkReleaseRawLogs() 가 $4=0 으로 동일 SQL 호출
 *   - PGRES_COMMAND_OK 뿐 아니라 PQcmdTuples == vtUpdates.size() 검증 (#5)
 */
bool CRawLogWorker::BulkUpdateRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates)
{
	if (pcConn == nullptr || vtUpdates.empty())
		return false;

	vector<string> vtTripId;
	vector<string> vtGpsSeq;
	vector<string> vtMatchStatus;
	vector<string> vtIntersectLen;
	vector<string> vtMatchLat;
	vector<string> vtMatchLon;
	vector<string> vtMatchLinkId;

	vtTripId.reserve(vtUpdates.size());
	vtGpsSeq.reserve(vtUpdates.size());
	vtMatchStatus.reserve(vtUpdates.size());
	vtIntersectLen.reserve(vtUpdates.size());
	vtMatchLat.reserve(vtUpdates.size());
	vtMatchLon.reserve(vtUpdates.size());
	vtMatchLinkId.reserve(vtUpdates.size());

	for (size_t i=0; i<vtUpdates.size(); ++i)
	{
		const RAW_LOG_UPDATE_ROW& stRow = vtUpdates[i];
		vtTripId.push_back(stRow.strTripId);
		vtGpsSeq.push_back(stRow.strGpsSeq);
		vtMatchStatus.push_back(stRow.strMatchStatus);
		vtIntersectLen.push_back(stRow.strIntersectLen);
		vtMatchLat.push_back(stRow.strMatchLat);
		vtMatchLon.push_back(stRow.strMatchLon);
		vtMatchLinkId.push_back(stRow.strMatchLinkId);
	}

	// rawgps_update text[] 파라미터 리터럴 생성 (2026-07-08 최정우 주석 추가)
	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strGpsSeqArray = BuildPgTextArray(vtGpsSeq);
	string strMatchStatusArray = BuildPgTextArray(vtMatchStatus);
	string strIntersectLenArray = BuildPgTextArray(vtIntersectLen);
	string strMatchLatArray = BuildPgTextArray(vtMatchLat);
	string strMatchLonArray = BuildPgTextArray(vtMatchLon);
	string strMatchLinkIdArray = BuildPgTextArray(vtMatchLinkId);

	// 파라미터 순서 = PRIM_RAWGPS 컬럼 순서 ($1 TRIP_ID, $2 GPS_SEQ, $3 MATCH_LAT,
	//   $4 MATCH_LON, $5 INTERSECT_LEN, $6 MATCH_LINK_ID, $7 MATCH_STATUS)
	const char *pszParams[7] =
	{
		strTripIdArray.c_str(),
		strGpsSeqArray.c_str(),
		strMatchLatArray.c_str(),
		strMatchLonArray.c_str(),
		strIntersectLenArray.c_str(),
		strMatchLinkIdArray.c_str(),
		strMatchStatusArray.c_str()
	};

	const int nParamLengths[7] =
	{
		static_cast<int>(strTripIdArray.size()),
		static_cast<int>(strGpsSeqArray.size()),
		static_cast<int>(strMatchLatArray.size()),
		static_cast<int>(strMatchLonArray.size()),
		static_cast<int>(strIntersectLenArray.size()),
		static_cast<int>(strMatchLinkIdArray.size()),
		static_cast<int>(strMatchStatusArray.size())
	};
	const int nParamFormats[7] = { 0, 0, 0, 0, 0, 0, 0 };

	// rawgps_update bulk UPDATE 실행 (2026-07-08 최정우 주석 추가)
	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strUpdateSQL.c_str(),
		7, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	const int nExpected = static_cast<int>(vtUpdates.size());
	bool bOk = false;

	if (nExecStatus != PGRES_COMMAND_OK)
	{
		LOGFMTE("worker bulk update error! count=[%d] msg=[%s]",
			nExpected, PQresultErrorMessage(pcResult));
	}
	else if (!CheckPgUpdateAffected(pcResult, nExpected, "worker bulk update"))
		bOk = false;
	else
	{
		bOk = true;
		for (size_t i=0; i<vtUpdates.size(); ++i)
		{
			const string& strStatus = vtUpdates[i].strMatchStatus;
			if ((strStatus == "1") || (strStatus == "3") || (strStatus == "4"))
			{
				ClearReleaseRetryCount(MakeReleaseRetryKey(vtUpdates[i].strTripId,
					vtUpdates[i].strGpsSeq));
			}
		}
	}

	PQclear(pcResult);
	return bOk;
}

/**
 * @brief bulk update 실패 시 예약 해제 [rawgps_update] — reserve 의 release 경로
 * @param[in] pcConn DB 커넥션
 * @param[in] vtUpdates release 대상 PK 목록 (match_status 등은 내부에서 0으로 치환)
 * @return true(전건 release), false(실행 오류·부분 release·인자 무효)
 * @remark
 *   - rawgps_select 가 PROCESSING(2) 로 예약한 PK 목록을 PENDING(0) 으로 되돌린다.
 *   - 동일 [rawgps_update] SQL: $4 전부 '', $5~$7 전부 '' (MATCH_*·INTERSECT_LEN 미변경)
 *   - SQL CASE: status 0 은 MATCH_LAT/LON ELSE 분기 → 기존 DB 값 유지
 *   - 다음 poll 에서 PENDING 으로 재예약·재맵매칭 (기동 복구 없이 런타임 복구)
 *   - BulkUpdateRawLogs() 경유 — PQcmdTuples 전건 검증 (#5)
 */
bool CRawLogWorker::BulkReleaseRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates)
{
	if ((pcConn == nullptr) || (vtUpdates.empty()))
		return false;

	vector<RAW_LOG_UPDATE_ROW> vtPending;
	vector<RAW_LOG_UPDATE_ROW> vtError;
	vtPending.reserve(vtUpdates.size());
	vtError.reserve(vtUpdates.size());

	for (size_t i=0; i<vtUpdates.size(); ++i)
	{
		RAW_LOG_UPDATE_ROW stRow = vtUpdates[i];
		stRow.strIntersectLen.clear();
		stRow.strMatchLat.clear();
		stRow.strMatchLon.clear();
		stRow.strMatchLinkId.clear();

		const string strRetryKey = MakeReleaseRetryKey(stRow.strTripId, stRow.strGpsSeq);
		const int nRetryMax = m_stConfig.nRetryMax;
		const int nRetryCount = (nRetryMax > 0)
			? BumpReleaseRetryCount(strRetryKey) : 0;

		if ((nRetryMax > 0) && (nRetryCount >= nRetryMax))
		{
			stRow.strMatchStatus = "4";
			vtError.push_back(stRow);
			LOGFMTW("release retry exhausted!→ERROR trip_id=[%s] seq=[%s] count=[%d/%d]",
				stRow.strTripId.c_str(), stRow.strGpsSeq.c_str(),
				nRetryCount, nRetryMax);
		}
		else
		{
			stRow.strMatchStatus = "0";
			vtPending.push_back(stRow);
		}
	}

	bool bOk = true;
	if (!vtPending.empty())
		bOk = BulkUpdateRawLogs(pcConn, vtPending) && bOk;
	if (!vtError.empty())
		bOk = BulkUpdateRawLogs(pcConn, vtError) && bOk;

	return bOk;
}

/**
 * @brief 개방형 게이트 통과 bulk INSERT [charge_insert] (2026-08-12 최정우 추가)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtCharges bulk INSERT 대상 행 목록 (ProcessOpenGateCharge 가 적재)
 * @return true(성공), false(실행 오류·인자 무효)
 * @remark
 *   - rawgps_update 와 동일한 UNNEST text[] 파라미터 패턴 (BuildPgTextArray)
 *   - PK(trip_id, device_key, trip_seq) 충돌은 ON CONFLICT DO NOTHING(query.sql) — 재시도 배치의
 *     중복 INSERT 방어용
 *   - 호출측(run())이 실패 시 rawgps_update 와 동일하게 배치 release·세션 미커밋 처리 —
 *     게이트 통과가 누락 없이 다음 poll 에서 재시도되도록 함
*/
bool CRawLogWorker::BulkInsertCharges(PGconn *pcConn, const vector<CHARGE_INSERT_ROW>& vtCharges)
{
	if ((pcConn == nullptr) || vtCharges.empty() || m_stConfig.strChargeInsertSQL.empty())
		return false;

	vector<string> vtTripId, vtDeviceKey, vtChargeSeq, vtChargeType, vtChargeUnit, vtLinkId,
		vtFromId, vtToId, vtFromLat, vtFromLon, vtToLat, vtToLon, vtZoneId, vtZoneName,
		vtDistM, vtSpeedKmh, vtSpeedLimitKmh, vtOccurDt, vtTripStartDt, vtTollgateId,
		vtEntryTollgateId, vtExitTollgateId, vtRegDt, vtUpdDt, vtChargeYn, vtChargeStatus,
		vtStaySeconds, vtTripEndDt;

	for (size_t i=0; i<vtCharges.size(); ++i)
	{
		const CHARGE_INSERT_ROW& stRow = vtCharges[i];
		vtTripId.push_back(stRow.strTripId);
		vtDeviceKey.push_back(stRow.strDeviceKey);
		vtChargeSeq.push_back(stRow.strChargeSeq);
		vtChargeType.push_back(stRow.strChargeType);
		vtChargeUnit.push_back(stRow.strChargeUnit);
		vtLinkId.push_back(stRow.strLinkId);
		vtFromId.push_back(stRow.strFromId);
		vtToId.push_back(stRow.strToId);
		vtFromLat.push_back(stRow.strFromLat);
		vtFromLon.push_back(stRow.strFromLon);
		vtToLat.push_back(stRow.strToLat);
		vtToLon.push_back(stRow.strToLon);
		vtZoneId.push_back(stRow.strZoneId);
		vtZoneName.push_back(stRow.strZoneName);
		vtDistM.push_back(stRow.strDistM);
		vtSpeedKmh.push_back(stRow.strSpeedKmh);
		vtSpeedLimitKmh.push_back(stRow.strSpeedLimitKmh);
		vtOccurDt.push_back(stRow.strOccurDt);
		vtTripStartDt.push_back(stRow.strTripStartDt);
		vtTollgateId.push_back(stRow.strTollgateId);
		vtEntryTollgateId.push_back(stRow.strEntryTollgateId);
		vtExitTollgateId.push_back(stRow.strExitTollgateId);
		vtRegDt.push_back(stRow.strRegDt);
		vtUpdDt.push_back(stRow.strUpdDt);
		vtChargeYn.push_back(stRow.strChargeYn);
		vtChargeStatus.push_back(stRow.strChargeStatus);
		vtStaySeconds.push_back(stRow.strStaySeconds);
		vtTripEndDt.push_back(stRow.strTripEndDt);
	}

	// 파라미터 순서(query.sql [charge_insert] UNNEST 컬럼 순서와 반드시 일치)
	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strDeviceKeyArray = BuildPgTextArray(vtDeviceKey);
	string strChargeSeqArray = BuildPgTextArray(vtChargeSeq);
	string strChargeTypeArray = BuildPgTextArray(vtChargeType);
	string strChargeUnitArray = BuildPgTextArray(vtChargeUnit);
	string strLinkIdArray = BuildPgTextArray(vtLinkId);
	string strFromIdArray = BuildPgTextArray(vtFromId);
	string strToIdArray = BuildPgTextArray(vtToId);
	string strFromLatArray = BuildPgTextArray(vtFromLat);
	string strFromLonArray = BuildPgTextArray(vtFromLon);
	string strToLatArray = BuildPgTextArray(vtToLat);
	string strToLonArray = BuildPgTextArray(vtToLon);
	string strZoneIdArray = BuildPgTextArray(vtZoneId);
	string strZoneNameArray = BuildPgTextArray(vtZoneName);
	string strDistMArray = BuildPgTextArray(vtDistM);
	string strSpeedKmhArray = BuildPgTextArray(vtSpeedKmh);
	string strSpeedLimitKmhArray = BuildPgTextArray(vtSpeedLimitKmh);
	string strOccurDtArray = BuildPgTextArray(vtOccurDt);
	string strTripStartDtArray = BuildPgTextArray(vtTripStartDt);
	string strTollgateIdArray = BuildPgTextArray(vtTollgateId);
	string strEntryTollgateIdArray = BuildPgTextArray(vtEntryTollgateId);
	string strExitTollgateIdArray = BuildPgTextArray(vtExitTollgateId);
	string strRegDtArray = BuildPgTextArray(vtRegDt);
	string strUpdDtArray = BuildPgTextArray(vtUpdDt);
	string strChargeYnArray = BuildPgTextArray(vtChargeYn);
	string strChargeStatusArray = BuildPgTextArray(vtChargeStatus);
	string strStaySecondsArray = BuildPgTextArray(vtStaySeconds);				// (2026-08-13 최정우 추가)
	string strTripEndDtArray = BuildPgTextArray(vtTripEndDt);					// (2026-08-13 최정우 추가)

	const char *pszParams[28] =
	{
		strTripIdArray.c_str(), strDeviceKeyArray.c_str(), strChargeSeqArray.c_str(),
		strChargeTypeArray.c_str(), strChargeUnitArray.c_str(), strLinkIdArray.c_str(),
		strFromIdArray.c_str(), strToIdArray.c_str(), strFromLatArray.c_str(),
		strFromLonArray.c_str(), strToLatArray.c_str(), strToLonArray.c_str(),
		strZoneIdArray.c_str(), strZoneNameArray.c_str(), strDistMArray.c_str(),
		strSpeedKmhArray.c_str(), strSpeedLimitKmhArray.c_str(), strOccurDtArray.c_str(),
		strTripStartDtArray.c_str(), strTollgateIdArray.c_str(), strEntryTollgateIdArray.c_str(),
		strExitTollgateIdArray.c_str(), strRegDtArray.c_str(), strUpdDtArray.c_str(),
		strChargeYnArray.c_str(), strChargeStatusArray.c_str(), strStaySecondsArray.c_str(),
		strTripEndDtArray.c_str()
	};

	const int nParamLengths[28] =
	{
		static_cast<int>(strTripIdArray.size()), static_cast<int>(strDeviceKeyArray.size()),
		static_cast<int>(strChargeSeqArray.size()), static_cast<int>(strChargeTypeArray.size()),
		static_cast<int>(strChargeUnitArray.size()), static_cast<int>(strLinkIdArray.size()),
		static_cast<int>(strFromIdArray.size()), static_cast<int>(strToIdArray.size()),
		static_cast<int>(strFromLatArray.size()), static_cast<int>(strFromLonArray.size()),
		static_cast<int>(strToLatArray.size()), static_cast<int>(strToLonArray.size()),
		static_cast<int>(strZoneIdArray.size()), static_cast<int>(strZoneNameArray.size()),
		static_cast<int>(strDistMArray.size()), static_cast<int>(strSpeedKmhArray.size()),
		static_cast<int>(strSpeedLimitKmhArray.size()), static_cast<int>(strOccurDtArray.size()),
		static_cast<int>(strTripStartDtArray.size()), static_cast<int>(strTollgateIdArray.size()),
		static_cast<int>(strEntryTollgateIdArray.size()), static_cast<int>(strExitTollgateIdArray.size()),
		static_cast<int>(strRegDtArray.size()), static_cast<int>(strUpdDtArray.size()),
		static_cast<int>(strChargeYnArray.size()), static_cast<int>(strChargeStatusArray.size()),
		static_cast<int>(strStaySecondsArray.size()), static_cast<int>(strTripEndDtArray.size())
	};
	const int nParamFormats[28] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strChargeInsertSQL.c_str(),
		28, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	bool bOk = (nExecStatus == PGRES_COMMAND_OK) || (nExecStatus == PGRES_TUPLES_OK);
	if (!bOk)
	{
		LOGFMTE("worker charge bulk insert error! count=[%d] msg=[%s]",
			static_cast<int>(vtCharges.size()), PQresultErrorMessage(pcResult));
	}

	PQclear(pcResult);
	return bOk;
}

/**
 * @brief TTL 만료(비정상 종료) 시 미확정 레코드 마감 bulk UPDATE [trip_abend], 4유형 공용
 *   (2026-08-13 최정우 추가, 2026-08-13 수정 — 개방형 한정에서 전 유형으로 확대)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtRows UPDATE 대상 행 목록(ExpireTtlSessions 가 세션 만료마다 적재)
 * @return true(성공), false(실행 오류·인자 무효)
 * @remark WHERE TRIP_ID/TRIP_END_DT IS NULL 매칭 — 그 trip_id 에 아직 안 끝난(TRIP_END_DT NULL)
 *   레코드가 없거나 이미 [trip_end] 로 정상 마감됐으면 0건 영향으로 조용히 끝남(오류 아님)
*/
bool CRawLogWorker::UpdateAbnormalTripEnd(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows)
{
	if ((pcConn == nullptr) || vtRows.empty() || m_stConfig.strAbnormalTripEndSQL.empty())
		return false;

	vector<string> vtTripId, vtTripEndDt, vtUpdDt;
	for (size_t i=0; i<vtRows.size(); ++i)
	{
		vtTripId.push_back(vtRows[i].strTripId);
		vtTripEndDt.push_back(vtRows[i].strTripEndDt);
		vtUpdDt.push_back(vtRows[i].strUpdDt);
	}

	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strTripEndDtArray = BuildPgTextArray(vtTripEndDt);
	string strUpdDtArray = BuildPgTextArray(vtUpdDt);

	const char *pszParams[3] = { strTripIdArray.c_str(), strTripEndDtArray.c_str(), strUpdDtArray.c_str() };
	const int nParamLengths[3] =
	{
		static_cast<int>(strTripIdArray.size()),
		static_cast<int>(strTripEndDtArray.size()),
		static_cast<int>(strUpdDtArray.size())
	};
	const int nParamFormats[3] = { 0, 0, 0 };

	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strAbnormalTripEndSQL.c_str(),
		3, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	bool bOk = (nExecStatus == PGRES_COMMAND_OK) || (nExecStatus == PGRES_TUPLES_OK);
	if (!bOk)
	{
		LOGFMTE("worker abnormal trip end update error! count=[%d] msg=[%s]",
			static_cast<int>(vtRows.size()), PQresultErrorMessage(pcResult));
	}

	PQclear(pcResult);
	return bOk;
}

/**
 * @brief 트립 종료 시 trip_end_dt bulk UPDATE [trip_end] (2026-08-12 최정우 추가)
 * @param[in] pcConn DB 커넥션
 * @param[in] vtRows UPDATE 대상 행 목록(ProcessRawLog 가 TRIP_EVENT=2 감지 시 적재)
 * @return true(성공), false(실행 오류·인자 무효)
 * @remark WHERE TRIP_ID 매칭 — 해당 trip_id 로 적재된 PRIM_CHARGEHAND 행이 없으면 0건
 *   영향으로 조용히 끝남(과금 없는 트립도 정상 케이스라 오류 아님)
*/
bool CRawLogWorker::UpdateTripEndDt(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows)
{
	if ((pcConn == nullptr) || vtRows.empty() || m_stConfig.strTripEndUpdateSQL.empty())
		return false;

	vector<string> vtTripId, vtTripEndDt, vtUpdDt;
	for (size_t i=0; i<vtRows.size(); ++i)
	{
		vtTripId.push_back(vtRows[i].strTripId);
		vtTripEndDt.push_back(vtRows[i].strTripEndDt);
		vtUpdDt.push_back(vtRows[i].strUpdDt);
	}

	string strTripIdArray = BuildPgTextArray(vtTripId);
	string strTripEndDtArray = BuildPgTextArray(vtTripEndDt);
	string strUpdDtArray = BuildPgTextArray(vtUpdDt);

	const char *pszParams[3] = { strTripIdArray.c_str(), strTripEndDtArray.c_str(), strUpdDtArray.c_str() };
	const int nParamLengths[3] =
	{
		static_cast<int>(strTripIdArray.size()),
		static_cast<int>(strTripEndDtArray.size()),
		static_cast<int>(strUpdDtArray.size())
	};
	const int nParamFormats[3] = { 0, 0, 0 };

	PGresult *pcResult = PQexecParams(pcConn, m_stConfig.strTripEndUpdateSQL.c_str(),
		3, nullptr, pszParams, nParamLengths, nParamFormats, 0);

	if (pcResult == nullptr)
		return false;

	ExecStatusType nExecStatus = PQresultStatus(pcResult);
	bool bOk = (nExecStatus == PGRES_COMMAND_OK) || (nExecStatus == PGRES_TUPLES_OK);
	if (!bOk)
	{
		LOGFMTE("worker trip_end update error! count=[%d] msg=[%s]",
			static_cast<int>(vtRows.size()), PQresultErrorMessage(pcResult));
	}

	PQclear(pcResult);
	return bOk;
}
