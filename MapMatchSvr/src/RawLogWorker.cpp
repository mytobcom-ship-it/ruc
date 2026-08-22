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
void CRawLogWorker::BuildParkRow(const PARK_RUN_SESSION& stRun, const string& strTripId,
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
		BuildParkRow(stRun, stSession.szTripId, strDeviceKey, nSeq, stSession.dtLastSeen,
			stRun.dfLastX, stRun.dfLastY, "N", "3", &stRow);
		pvtOut->push_back(stRow);
		nSeq += 1;

		LOGFMTI("[#%02d] parking expired!device=[%s] trip_id=[%s] road=[%s] dwell=[%s]s",
			nThreadId, strDeviceKey.c_str(), stSession.szTripId, stRun.szRoadID,
			stRow.strStaySeconds.c_str());
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
		BuildParkRow(stRun, pstSession->szTripId, strDeviceKey, pstSession->nChargeSeq,
			stRun.dtLastConfirmedTime, stRun.dfLastConfirmedX, stRun.dfLastConfirmedY,
			"Y", "0", &stRow);
		pvtOut->push_back(stRow);

		LOGFMTI("[#%02d] parking stale finalized!device=[%s] trip_id=[%s] road=[%s] dwell=[%s]s",
			nThreadId, strDeviceKey.c_str(), pstSession->szTripId, stRun.szRoadID,
			stRow.strStaySeconds.c_str());

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
 * @brief TTL 만료 세션 중 입구만 통과하고 출구를 못 찾은 폐쇄형 세션을 N/3(AUDIT)로 마감
 *   (2026-08-14 최정우 추가)
 * @param[in] nThreadId 로그용 워커 ID
 * @param[in] strDeviceKey 세션 맵 키(=DEVICE_KEY)
 * @param[in] stSession 만료 직전 세션(제거 전 스냅샷)
 * @param[out] pvtOut 세션이 입구 통과 상태였으면 CHARGE_INSERT_ROW 1건 추가
 * @remark 기존엔 이 함수 자체가 없어 "입구는 확인했지만 출구 신호가 영영 안 온" 세션이 TTL로
 *   조용히 사라지면 그 진입 사실 자체가 통째로 유실됐음(휴게소 장시간 정차 등). 다른 TTL-flush
 *   함수(주정차·일반도로·면제도로)와 달리 폐쇄형은 실시간 위치 추적을 안 하고(구간거리를
 *   ZONE_INFO.dfLengthM 사전계산값으로 씀) "직전 매칭 위치" 자체를 세션에 안 갖고 있어, 출구 위치·
 *   실제 이동거리를 알 방법이 없음 — 모르는 값을 지어내지 않고 dist_m/speed_kmh/to_id/to_lat·lon은
 *   전부 비워두고, 확실히 아는 것(입구 게이트 ID·구역·진입시각)만 기록.
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
	stRow.strToLat = "";									// 출구 미확인 — 지어내지 않음(2026-08-20부터 NULL 허용, NULLIF(...,'')로 DB엔 NULL)
	stRow.strToLon = "";

	stRow.strZoneId = stSession.szClosedRoadId;
	stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	stRow.strDistM = "";									// 출구 미확인 — 실제 이동거리 불명
	stRow.strSpeedKmh = "";
	stRow.strSpeedLimitKmh = "";

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d",
		static_cast<int>(difftime(dtEndTime, stSession.dtEntryTime) + 0.5));
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
 * @remark 폐쇄형과 동일 이유(실시간 위치 미추적)로 dist_m/speed_kmh/to_lat·lon은 비워둠.
 *   from_id/to_id·from_lat/lon은 실측 관례상 게이트ID가 아니라 구역 road_id·구역 등록 좌표
 *   (ZONE_INFO.dfFirstLat/Lon)라 이 부분은 이미 알고 있음 — to_id는 진입 구역과 동일값을 넣지
 *   않고 비워서 "이탈 미확인" 상태를 명확히 구분.
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
	stRow.strToLat = "";									// 출구 미확인 — 지어내지 않음(NULLIF(...,'')로 DB엔 NULL)
	stRow.strToLon = "";

	stRow.strZoneId = stSession.szSpeedZoneRoadId;
	stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

	stRow.strDistM = "";									// 출구 미확인 — 실제 이동거리 불명
	stRow.strSpeedKmh = "";
	if (pstZone != nullptr)
	{
		char szSpeedLimit[8];
		snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(pstZone->dfSpeedLimitKmh + 0.5));
		stRow.strSpeedLimitKmh = szSpeedLimit;
	}

	char szStaySeconds[16];
	snprintf(szStaySeconds, sizeof(szStaySeconds), "%d",
		static_cast<int>(difftime(dtEndTime, stSession.dtSpeedEntryTime) + 0.5));
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
bool CRawLogWorker::ShouldSkipGpsInput(int nThreadId, const sRawLogInfo& stRawLogInfo)
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
	if ((!stRawLogInfo.bRawVldKnown) || (!stRawLogInfo.bRawVld))
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
 * @remark
 *   - TRIP_EVENT=0(START) 또는 GPS_SEQ<=dwLastGpsSeq(역전·동일 seq 재처리) → 시작
*/
bool CRawLogWorker::NeedsBeginReset(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, bool *pbFullReset)
{
	if (pbFullReset == nullptr)
		return false;

	*pbFullReset = false;

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

	if ((stSession.dwLastGpsSeq > 0) && 
		(stRawLogInfo.dwSeqNo <= stSession.dwLastGpsSeq))
	{
		LOGFMTW("[#%02d] gps_seq rollback!device=[%s] trip_id=[%s] seq=[%u] last_seq=[%u]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			stRawLogInfo.dwSeqNo, stSession.dwLastGpsSeq);
		return true;
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
		// 실제 신규 trip 시작(START/trip_id 변경)일 때만 게이트 트랙 리셋 — GPS_SEQ 역전 재처리(bFullReset=false)는
		//   같은 trip 이 계속되는 것이라 리셋하면 안 됨(이미 지난 게이트 재부과 위험) (2026-08-12 최정우 추가)
		stSession.vtActiveGateIds.clear();
		stSession.nChargeSeq = 1;

		// 폐쇄형 진입 상태도 동일하게 신규 trip 시작 시에만 리셋 (2026-08-12 최정우 추가)
		stSession.bInClosedRoad = false;
		stSession.szEntryTollgateId[0] = '\0';
		stSession.szClosedRoadId[0] = '\0';

		// 구간단속 진입 상태도 동일하게 신규 trip 시작 시에만 리셋 (2026-08-12 최정우 추가)
		stSession.bInSpeedZone = false;
		stSession.szSpeedZoneRoadId[0] = '\0';
		stSession.szSpeedEntryTollgateId[0] = '\0';

		// 면제도로·일반도로 진행 세션도 신규 trip 시작 시에만 리셋 (2026-08-23 최정우 수정 — 벡터화)
		stSession.vtExemptRuns.clear();
		stSession.vtNodeStepRuns.clear();
	}
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
		// 개방형 게이트 통과 bulk INSERT — rawgps_update 성공 후에만 시도. 실패 시 map-match
		//   bulk update 실패와 동일하게 취급(배치 release·세션 미커밋) → 다음 poll 에서 재처리되며
		//   게이트 진입 마킹(vtActiveGateIds)도 커밋되지 않아 재통과 시 정상 재부과됨 (2026-08-12 최정우 추가)
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

	// 반대편(짝) 링크 1틱 오매칭 보정 — bReverseSuspect 는 여기서 기준으로 못 씀: 반대편 짝
	//   링크는 그 방향으로는 "정상 순방향"으로 매칭되므로(TryOppositeLinkCandidate() 가 별도
	//   재귀 평가로 붙인 후보라 자기 자신 기준 역행이 아님) 최종 선택된 항목의 bReverseSuspect는
	//   보통 false 로 남는다. 대신 LINK_INFO.qwOppositeLinkID(물리적 왕복분리 짝, CreateData
	//   ComputeOppositeLinkPairs 사전계산)로 "직전 확정 링크의 짝 링크로 1틱만 튀었다가 바로
	//   직전 확정 링크로 복귀"하는 패턴인지 직접 확인한다 (2026-08-21 최정우 수정 — 최초
	//   구현에서 bReverseSuspect 기준으로는 실측 케이스가 보정되지 않는 것을 확인)
	if (bMatched && (pstSession->qwLastConfirmedLinkID != 0)
		&& (stMatchLinkInfo.qwLinkID != pstSession->qwLastConfirmedLinkID)
		&& bHasNextLinkID && (qwNextLinkID == pstSession->qwLastConfirmedLinkID)
		&& (m_stConfig.pcDataLoader != nullptr))
	{
		PLINK_INFO pstConfirmedLink = m_stConfig.pcDataLoader->GetLinkInfo(pstSession->qwLastConfirmedLinkID);
		if ((pstConfirmedLink != nullptr) && (pstConfirmedLink->qwOppositeLinkID != 0)
			&& (pstConfirmedLink->qwOppositeLinkID == stMatchLinkInfo.qwLinkID))
		{
			LOGFMTW("[#%02d] opposite-link 1-tick flip corrected!device=[%s] trip_id=[%s] seq=[%u] "
				"link=[%llu] prev_confirmed_link=[%llu] -> SKIP(uncharged)",
				nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, stRawLogInfo.dwSeqNo,
				static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID),
				static_cast<unsigned long long>(pstSession->qwLastConfirmedLinkID));
			nFinalStatus = MATCH_STATUS_SKIP;
			bMatched = false;
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

		ProcessOpenGateCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessClosedRoadCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessSpeedZoneCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessExemptZoneCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);
		ProcessNodeStepCharge(nThreadId, stRawLogInfo, stMatchLinkInfo, pstSession, pvtChargeInserts);

		pstSession->dfLastMatchX = dfCurX;
		pstSession->dfLastMatchY = dfCurY;
		pstSession->dtLastMatchGps = dtCurGps;
		pstSession->bHasLastMatch = bCurHas;

		pstSession->qwLastConfirmedLinkID = stMatchLinkInfo.qwLinkID;
	}

	AppendUpdateRow(pvtUpdates, stRawLogInfo, nFinalStatus, pstSession->nPendingIntersectLen,
		pstSession->bPendingHasCoords ? &stMatchLinkInfo.dfMatchY : nullptr,
		pstSession->bPendingHasCoords ? &stMatchLinkInfo.dfMatchX : nullptr,
		pstSession->bPendingHasCoords ? stMatchLinkInfo.qwLinkID : 0);

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
	// 시작(세션 초기화) 필요 여부 판단 (2026-07-08 최정우 주석 추가)
	if (NeedsBeginReset(nThreadId, stRawLogInfo, stSession, &bFullReset))
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
	else if (!stSession.bStartWarned && stRawLogInfo.nTripEvent != TRIP_EVENT_START)
	{
		LOGFMTW("[#%02d] trip missing START!device=[%s] trip_id=[%s] seq=[%u] event=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			stRawLogInfo.dwSeqNo, static_cast<int>(stRawLogInfo.nTripEvent));
		stSession.bStartWarned = true;
	}

	// 현재 배치 TRIP_ID 를 세션에 기록(다음 trip 변경 감지 기준). 키는 DEVICE_KEY 라 trip 이 바뀌면 위에서 리셋됨 (2026-07-08 최정우 추가)
	strncpy(stSession.szTripId, stRawLogInfo.szTripID, sizeof(stSession.szTripId) - 1);
	stSession.szTripId[sizeof(stSession.szTripId) - 1] = '\0';

	// 트립종료 직전, 아직 보류(pending) 중인 1틱 지연 행이 있으면 먼저 확정(commit)한다 — 바로
	//   아래 AppendExpiredClosedRoadCharge/AppendExpiredSpeedZoneCharge 가 세션의 bInClosedRoad/
	//   bInSpeedZone 을 읽기 "전"에 보류 행의 과금 반영이 먼저 끝나 있어야 정확하다. 이 시점엔
	//   이번 행(TRIP_EVENT=END) 자신의 매칭 결과를 아직 몰라 보정판단용 "다음" 링크가 없음(보정
	//   시도 안 함, 보류 행을 계산된 값 그대로 커밋) (2026-08-21 최정우 추가)
	if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
		CommitPendingRow(nThreadId, &stSession, false, 0, pvtUpdates, pvtChargeInserts);

	// 트립 종료(TRIP_EVENT=2) — 매칭 성공/실패 무관하게 그 trip_id 의 PRIM_CHARGEHAND 전 행에
	//   trip_end_dt 를 나중에(run() 이 배치 종료 시) UPDATE 하도록 적재 (2026-08-12 최정우 추가)
	//   UPD_DT 도 TRIP_END_DT 와 동일하게 GPS 시각(wall-clock 아님) 사용 — 사용자 지시(2026-08-14):
	//   "구역 이탈"과 "트립 정상종료"가 같은 tick 에 겹치는 경우(예: 일반도로 구간 안에서 트립이
	//   끝나는 경우) 진출시각과 종료시각이 같은 값이니 UPD_DT 도 그 값으로 맞추는 게 자연스러움 —
	//   5유형 공용 UPDATE라 전부에 동일 적용됨
	if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
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
		bool bWasClosedRoad = stSession.bInClosedRoad;
		AppendExpiredClosedRoadCharge(nThreadId, stRawLogInfo.szDeviceKey, stSession, stRawLogInfo.dtGPS, pvtChargeInserts);
		if (bWasClosedRoad)
			stSession.nChargeSeq += 1;

		bool bWasSpeedZone = stSession.bInSpeedZone;
		AppendExpiredSpeedZoneCharge(nThreadId, stRawLogInfo.szDeviceKey, stSession, stRawLogInfo.dtGPS, pvtChargeInserts);
		if (bWasSpeedZone)
			stSession.nChargeSeq += 1;
	}

	// GPS 좌표·RAW_VLD 유효성 검사 — SKIP(3). 세션·DB 좌표 미저장 (2026-07-10 최정우 수정)
	if (ShouldSkipGpsInput(nThreadId, stRawLogInfo))
	{
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		stSession.bLastPointOk = false;			// (2026-07-21 최정우 추가)
		if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
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
			ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts);
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
		if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
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
		if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
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
		ProcessParkingCharge(nThreadId, stRawLogInfo, &stSession, pvtChargeInserts,
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
		if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
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
	if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
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
		stAltCtx.dfPrevMatchX = pstSession->dfLastMatchX;
		stAltCtx.dfPrevMatchY = pstSession->dfLastMatchY;
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
 * @brief 개방형(OPEN_ROAD) 게이트 통과 판정 — 엣지 감지(진입 시 1건 적재) (2026-08-12 최정우 추가)
 * @param[in] nThreadId 워커 스레드 ID (로그용)
 * @param[in] stRawLogInfo 원시 GPS
 * @param[in] stMatchLinkInfo 신뢰 가능한 맵매칭 결과(호출측이 bMatched && !bUntrustedMatch 확인 후 호출)
 * @param[in,out] pstSession 배치 임시 세션 — vtActiveGateIds/nChargeSeq 갱신
 * @param[out] pvtChargeInserts 신규 진입 시 게이트별 1행씩 적재 (bulk INSERT 는 run() 이 배치 종료 시 일괄 실행)
 * @return void
 * @remark
 *   - 같은 link_id 에 개방형(M) 게이트가 2개 이상 있을 수 있어 GetGatesByLinkId 로 전부 조회,
 *     게이트마다 독립적으로 진입/유지 판정(2026-08-13 최정우 수정 — 원래 GetGateByLinkId 단일
 *     조회는 첫 게이트만 처리하고 나머지는 조용히 부과 누락되는 한계가 있었음)
 *   - vtActiveGateIds 에 없는 게이트면 "신규 진입" → 적재 + 목록에 추가
 *   - vtActiveGateIds 에 이미 있으면 같은 통과 구간 연속 매칭 중 → 스킵(중복 방지)
 *   - 이번 매칭에 안 잡히는(=이 링크에서 벗어난) 기존 활성 게이트는 목록에서 제거 — 나중에
 *     같은 게이트로 재진입해도 정상 재부과되게 함
*/
void CRawLogWorker::ProcessOpenGateCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// gate_div='M'(본선/개방형) 명시 — 폐쇄형 입/출구 게이트(I/O)가 같은 link_id 를 공유하는
	//   사례가 있어(TG00007/08) div 필터 없이 조회하면 폐쇄형 게이트 통과가 개방형으로도 잘못
	//   처리됨(2026-08-12 최정우 발견·수정) — 실측 테스트 중 로그로 확인된 버그
	// 이번 확정 링크 하나만이 아니라 직전 확정 링크부터 경유한 전체 경로(aqwPathLinkIDs)를 순회해
	//   게이트 후보 병합 — GPS 수신 주기가 늘어나(1초→3초) 두 GPS 포인트 사이 이동거리가 커지면
	//   짧은 게이트 링크가 통째로 건너뛰어져 어느 쪽 매칭 결과에도 안 잡히는 경우를 보완
	//   (2026-08-20 최정우 추가). nPathLinkCount==0(경로 정보 없음)이면 최종 링크 1개만 쓰는
	//   기존 동작으로 자연 축소됨
	vector<PGATE_INFO> vtGates;
	{
		uint8 nPathCount = stMatchLinkInfo.nPathLinkCount;
		if (nPathCount == 0)
		{
			m_stConfig.pcChargeDataLoader->GetGatesByLinkId(stMatchLinkInfo.qwLinkID, 'M', &vtGates);
		}
		else
		{
			for (uint8 i = 0; i < nPathCount; ++i)
			{
				vector<PGATE_INFO> vtOne;
				m_stConfig.pcChargeDataLoader->GetGatesByLinkId(stMatchLinkInfo.aqwPathLinkIDs[i], 'M', &vtOne);
				for (size_t g = 0; g < vtOne.size(); ++g)
				{
					bool bDup = false;
					for (size_t e = 0; e < vtGates.size(); ++e)
					{
						if (strcmp(vtGates[e]->szTollgateID, vtOne[g]->szTollgateID) == 0) { bDup = true; break; }
					}
					if (!bDup)
						vtGates.push_back(vtOne[g]);
				}
			}
		}
	}

	// 이번 매칭 링크의 게이트 목록에 더 이상 없는 기존 활성 게이트는 제거(엣지 감지) — 링크가
	//   바뀌었거나 게이트 링크를 완전히 벗어난 경우 (2026-08-13 최정우 수정)
	for (size_t i = 0; i < pstSession->vtActiveGateIds.size(); )
	{
		bool bStillOnLink = false;
		for (size_t g = 0; g < vtGates.size(); ++g)
		{
			if (pstSession->vtActiveGateIds[i] == vtGates[g]->szTollgateID) { bStillOnLink = true; break; }
		}
		if (!bStillOnLink)
			pstSession->vtActiveGateIds.erase(pstSession->vtActiveGateIds.begin() + i);
		else
			++i;
	}

	if (vtGates.empty())
		return;										// 게이트 링크 아님(또는 M 이 아닌 다른 div)

	for (size_t nGateIdx = 0; nGateIdx < vtGates.size(); ++nGateIdx)
	{
		PGATE_INFO pstGate = vtGates[nGateIdx];

		bool bAlreadyActive = false;
		for (size_t i = 0; i < pstSession->vtActiveGateIds.size(); ++i)
		{
			if (pstSession->vtActiveGateIds[i] == pstGate->szTollgateID) { bAlreadyActive = true; break; }
		}
		if (bAlreadyActive)
			continue;									// 같은 게이트 연속 매칭 중 — 중복 부과 방지

		// 신규 진입 — PRIM_CHARGEHAND 1행 적재 (2026-08-12 최정우 추가)
		CHARGE_INSERT_ROW stRow;
		stRow.strTripId = stRawLogInfo.szTripID;
		stRow.strDeviceKey = stRawLogInfo.szDeviceKey;

		char szSeq[16];
		snprintf(szSeq, sizeof(szSeq), "%d", pstSession->nChargeSeq);
		stRow.strChargeSeq = szSeq;

		// charge_unit/link_id/from_id/to_id/tollgate_id — 실측(59.11.91.162 운영 DB, charge_type=1
		//   기존 23건 전수) 컨벤션 그대로 반영 (2026-08-12 최정우 수정): charge_unit=0(NODE),
		//   from_id=to_id=게이트ID(TG00009 등), tollgate_id 비움.
		//   from_lat/lon·to_lat/lon 은 실측과 이미 일치(매칭 링크 시작·종료 노드 좌표) — 그대로 유지.
		//   link_id 는 실측(23건)은 비어있었지만, 이미 확보된 값(게이트 자체의 link_id — GetGateByLinkId
		//   조회 키와 동일값이라 매칭 결과 link_id 와 항상 같음)이라 참고용으로 채워 넣기로 결정 (2026-08-12 최정우 수정)
		stRow.strChargeType = "1";							// OPEN_ROAD
		stRow.strChargeUnit = "0";							// NODE (실측 확인)

		char szLinkId[24];
		snprintf(szLinkId, sizeof(szLinkId), "%llu", static_cast<unsigned long long>(pstGate->qwLinkID));
		stRow.strLinkId = szLinkId;

		stRow.strFromId = pstGate->szTollgateID;
		stRow.strToId = pstGate->szTollgateID;

		char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
		snprintf(szFromLat, sizeof(szFromLat), "%.06lf", stMatchLinkInfo.dfStNodeY);
		snprintf(szFromLon, sizeof(szFromLon), "%.06lf", stMatchLinkInfo.dfStNodeX);
		snprintf(szToLat, sizeof(szToLat), "%.06lf", stMatchLinkInfo.dfEdNodeY);
		snprintf(szToLon, sizeof(szToLon), "%.06lf", stMatchLinkInfo.dfEdNodeX);
		stRow.strFromLat = szFromLat;
		stRow.strFromLon = szFromLon;
		stRow.strToLat = szToLat;
		stRow.strToLon = szToLon;

		// 구역명 — base_roadlink 캐시(road_id 키) 조회. 못 찾아도 과금 자체는 진행(zone_name 참고용 컬럼) (2026-08-12 최정우 추가)
		stRow.strZoneId = pstGate->szRoadID;
		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstGate->szRoadID);
		stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

		// 순간속도 — 직전 매칭 위치·시각(세션 앵커, 호출측이 갱신 전에 넘겨줌)과 이번 장비 위치·시각
		//   사이 거리/시간으로 계산. 직전 매칭이 없거나(트립 시작 첫 포인트) 시간차 0 이하면 계산 불가 →
		//   빈 값(DB 기본값 0) 유지 (2026-08-12 최정우 추가)
		if (pstSession->bHasLastMatch && !stRawLogInfo.bGpsLatNull && !stRawLogInfo.bGpsLonNull)
		{
			double dfGapSec = difftime(stRawLogInfo.dtGPS, pstSession->dtLastMatchGps);
			if (dfGapSec > 0.0)
			{
				POINT stPrev, stCur;
				stPrev.dfX = pstSession->dfLastMatchX;
				stPrev.dfY = pstSession->dfLastMatchY;
				stCur.dfX = stRawLogInfo.dfX;
				stCur.dfY = stRawLogInfo.dfY;
				double dfMoveM = HaversineMeters(stPrev, stCur);
				double dfSpeedKmh = (dfMoveM / dfGapSec) * 3.6;

				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}
		}

		// 제한속도 — 매칭 링크 기준 (2026-08-12 최정우 추가)
		char szSpeedLimit[8];
		snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(stMatchLinkInfo.nMaxSpeed));
		stRow.strSpeedLimitKmh = szSpeedLimit;

		stRow.strOccurDt = FormatDateTime14(stRawLogInfo.dtGPS);

		// trip_start_dt — TRIP_ID({6자리 숫자}_{YYYYMMDDHH24MISS})에서 시각 부분 추출.
		//   형식이 어긋나면(방어적) occur_dt 로 대체 — trip_start_dt 는 NOT NULL 컬럼이라 빈 값
		//   불가 (2026-08-12 최정우 추가, 2026-08-19 최정우 수정 — ExtractTripStartDt() 로 교체,
		//   자세한 사유는 그 함수 주석 참고)
		const char *pszTripStartDt = ExtractTripStartDt(stRawLogInfo.szTripID);
		if (pszTripStartDt != nullptr)
			stRow.strTripStartDt = pszTripStartDt;
		else
			stRow.strTripStartDt = stRow.strOccurDt;

		// tollgate_id — 개방형은 게이트 통과 자체가 곧 과금 이벤트라 진입~진출 2단계 구조(폐쇄형·
		//   구간단속처럼 진입 시점엔 모르고 진출 때 채우는 방식)가 필요 없이, 이 INSERT 시점에 이미
		//   게이트 ID를 알고 있음 — 그래서 UPDATE 없이 여기서 바로 채움(2026-08-20 최정우 수정,
		//   사용자 지시. 기존엔 from_id/to_id 에 게이트ID가 이미 들어간다는 이유로 비워뒀었음
		//   (2026-08-12) — tollgate_id 컬럼 자체로도 조회할 수 있게 하기 위해 채우는 걸로 변경)
		stRow.strTollgateId = pstGate->szTollgateID;

		// reg_dt/upd_dt — 등록 시각(현재)으로 동일하게 채움. DB DEFAULT(now()) 에 맡기지 않고 명시
		//   지정 — 두 컬럼이 항상 같은 값이어야 한다는 요건을 SQL/트랜잭션 타이밍에 의존하지 않고 보장 (2026-08-12 최정우 추가)
		stRow.strRegDt = FormatDateTime14(time(nullptr));
		stRow.strUpdDt = stRow.strRegDt;

		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] open gate charge queued!device=[%s] trip_id=[%s] seq=[%d] gate=[%s] link=[%llu]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			pstGate->szTollgateID, static_cast<unsigned long long>(stMatchLinkInfo.qwLinkID));

		pstSession->vtActiveGateIds.push_back(pstGate->szTollgateID);
		pstSession->nChargeSeq += 1;
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

		if (pstExitGate != nullptr)
		{
			// 위치(구간 내 지점) 비교는 "이번 확정 링크 안에서 아직 진행 중"인 경우에만 필요 —
			//   경유 링크에서 찾았으면 이미 링크 전체를 지났으므로 생략 (2026-08-20 최정우 추가)
			if (!bExitOnIntermediate)
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

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstSession->dfEntryFromLat);
			snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstSession->dfEntryFromLon);
			snprintf(szToLat, sizeof(szToLat), "%.06lf", stMatchLinkInfo.dfEdNodeY);
			snprintf(szToLon, sizeof(szToLon), "%.06lf", stMatchLinkInfo.dfEdNodeX);
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szClosedRoadId;
			PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstSession->szClosedRoadId);
			stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

			// 구간거리 — 실시간 GPS 누적이 아니라 base_roadlink.coords 폴리라인 실거리(ZONE_INFO.dfLengthM,
			//   [zone_select] SQL 에서 하버사인 합산으로 미리 계산됨) 사용 (2026-08-12 최정우 수정)
			double dfLengthM = (pstZone != nullptr) ? pstZone->dfLengthM : 0.0;
			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
			stRow.strDistM = szDistM;

			double dfDwellSec = difftime(stRawLogInfo.dtGPS, pstSession->dtEntryTime);

			// 평균속도 — 구역 실거리 ÷ 입구~출구 경과시간(사용자 지시, 2026-08-14 — 개방형 제외 전
			//   유형 평균속도로 통일. 기존엔 개방형과 동일하게 순간속도를 썼으나, 구역 개념이 있는
			//   유형은 구간단속과 동일하게 평균속도가 맞다는 지시로 변경)
			if (dfDwellSec > 0.0)
			{
				double dfAvgSpeedKmh = (dfLengthM / dfDwellSec) * 3.6;
				char szSpeedKmh[16];
				snprintf(szSpeedKmh, sizeof(szSpeedKmh), "%d", static_cast<int>(dfAvgSpeedKmh + 0.5));
				stRow.strSpeedKmh = szSpeedKmh;
			}

			char szSpeedLimit[8];
			snprintf(szSpeedLimit, sizeof(szSpeedLimit), "%d", static_cast<int>(stMatchLinkInfo.nMaxSpeed));
			stRow.strSpeedLimitKmh = szSpeedLimit;

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
				POINT stLinkStart, stExitPos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stExitPos.dfX = pstSameLinkExit->dfLon;
				stExitPos.dfY = pstSameLinkExit->dfLat;
				double dfExitPosOnLink = HaversineMeters(stLinkStart, stExitPos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				if (dfCurPosOnLink >= (dfExitPosOnLink - 3.0))
					continue;			// 이미 이 링크의 출구 지점을 지났음 — 재진입 아님
			}
		}

		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
		if ((pstZone == nullptr) || (strcmp(pstZone->szRoadKind, "2") != 0))
			continue;

		pstSession->bInClosedRoad = true;
		strncpy(pstSession->szEntryTollgateId, pstEntryGate->szTollgateID, sizeof(pstSession->szEntryTollgateId) - 1);
		pstSession->szEntryTollgateId[sizeof(pstSession->szEntryTollgateId) - 1] = '\0';
		strncpy(pstSession->szClosedRoadId, pstEntryGate->szRoadID, sizeof(pstSession->szClosedRoadId) - 1);
		pstSession->szClosedRoadId[sizeof(pstSession->szClosedRoadId) - 1] = '\0';
		pstSession->dfEntryFromLat = stMatchLinkInfo.dfStNodeY;
		pstSession->dfEntryFromLon = stMatchLinkInfo.dfStNodeX;
		pstSession->dtEntryTime = stRawLogInfo.dtGPS;

		LOGFMTI("[#%02d] closed road entry!device=[%s] trip_id=[%s] gate=[%s] road=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			pstEntryGate->szTollgateID, pstEntryGate->szRoadID);
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

		if (pstExitGate != nullptr)
		{
			if (!bExitOnIntermediate)
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

			char szFromLat[32], szFromLon[32], szToLat[32], szToLon[32];
			if (pstZone != nullptr)
			{
				snprintf(szFromLat, sizeof(szFromLat), "%.06lf", pstZone->dfFirstLat);
				snprintf(szFromLon, sizeof(szFromLon), "%.06lf", pstZone->dfFirstLon);
				snprintf(szToLat, sizeof(szToLat), "%.06lf", pstZone->dfLastLat);
				snprintf(szToLon, sizeof(szToLon), "%.06lf", pstZone->dfLastLon);
			}
			else
			{
				szFromLat[0] = szFromLon[0] = szToLat[0] = szToLon[0] = '\0';
			}
			stRow.strFromLat = szFromLat;
			stRow.strFromLon = szFromLon;
			stRow.strToLat = szToLat;
			stRow.strToLon = szToLon;

			stRow.strZoneId = pstSession->szSpeedZoneRoadId;
			stRow.strZoneName = (pstZone != nullptr) ? pstZone->szRoadNm : "";

			double dfLengthM = (pstZone != nullptr) ? pstZone->dfLengthM : 0.0;
			char szDistM[16];
			snprintf(szDistM, sizeof(szDistM), "%d", static_cast<int>(dfLengthM + 0.5));
			stRow.strDistM = szDistM;

			// 평균속도 — 구역 실거리 ÷ 입구~출구 경과시간. 구간단속은 순간속도가 아니라 구간 전체
			//   평균속도가 제한속도 초과 여부(위반 판정) 기준이기 때문 (2026-08-12 최정우 추가)
			double dfElapsedSec = difftime(stRawLogInfo.dtGPS, pstSession->dtSpeedEntryTime);
			if (dfElapsedSec > 0.0)
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
			//   원래는 통행료 파이프라인 비대상이라 위반 여부 무관 항상 N/4 고정이었음, 실측 선례와 달라짐)
			stRow.strChargeYn = "Y";
			stRow.strChargeStatus = "0";

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
				POINT stLinkStart, stExitPos;
				stLinkStart.dfX = stMatchLinkInfo.dfStNodeX;
				stLinkStart.dfY = stMatchLinkInfo.dfStNodeY;
				stExitPos.dfX = pstSameLinkExit->dfLon;
				stExitPos.dfY = pstSameLinkExit->dfLat;
				double dfExitPosOnLink = HaversineMeters(stLinkStart, stExitPos);
				double dfCurPosOnLink = static_cast<double>(stMatchLinkInfo.wLenFromLink) + stMatchLinkInfo.dfSgmtMatchLen;

				if (dfCurPosOnLink >= (dfExitPosOnLink - 3.0))
					continue;			// 이미 이 링크의 출구 지점을 지났음 — 재진입 아님
			}
		}

		PZONE_INFO pstZone = m_stConfig.pcChargeDataLoader->GetZoneByRoadId(pstEntryGate->szRoadID);
		if ((pstZone == nullptr) || (strcmp(pstZone->szRoadKind, "3") != 0))
			continue;

		pstSession->bInSpeedZone = true;
		strncpy(pstSession->szSpeedZoneRoadId, pstEntryGate->szRoadID, sizeof(pstSession->szSpeedZoneRoadId) - 1);
		pstSession->szSpeedZoneRoadId[sizeof(pstSession->szSpeedZoneRoadId) - 1] = '\0';
		strncpy(pstSession->szSpeedEntryTollgateId, pstEntryGate->szTollgateID, sizeof(pstSession->szSpeedEntryTollgateId) - 1);
		pstSession->szSpeedEntryTollgateId[sizeof(pstSession->szSpeedEntryTollgateId) - 1] = '\0';
		pstSession->dtSpeedEntryTime = stRawLogInfo.dtGPS;

		LOGFMTI("[#%02d] speed zone entry!device=[%s] trip_id=[%s] gate=[%s] road=[%s]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
			pstEntryGate->szTollgateID, pstEntryGate->szRoadID);
		break;			// 한 tick 엔 하나만 진입
	}
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
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
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

	const bool bTripEnding = (stRawLogInfo.nTripEvent == TRIP_EVENT_END);

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
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts)
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

	// 트립 종료(TRIP_EVENT=2) — 구역 위인 채로 끝나면 "이탈" 신호가 영영 안 옴, 즉시 강제 마감
	const bool bTripEnding = (stRawLogInfo.nTripEvent == TRIP_EVENT_END);

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
			POINT stPrev, stCur;
			stPrev.dfX = stRun.dfLastX;  stPrev.dfY = stRun.dfLastY;
			stCur.dfX = stMatchLinkInfo.dfMatchX;  stCur.dfY = stMatchLinkInfo.dfMatchY;
			stRun.dfAccumDistM += HaversineMeters(stPrev, stCur);
			stRun.dfLastX = stMatchLinkInfo.dfMatchX;
			stRun.dfLastY = stMatchLinkInfo.dfMatchY;
			stRun.qwLastLinkID = stMatchLinkInfo.qwLinkID;
		}

		if (bSameZone && !bTripEnding) { ++si; continue; }	// 계속 진행 중

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
			}
		}

		CHARGE_INSERT_ROW stRow;
		BuildNodeStepRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, stRawLogInfo.dtGPS, &stRow);
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
		stRun.dtEntryTime = stRawLogInfo.dtGPS;
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
		bool bMatchTrusted, double dfMatchX, double dfMatchY)
{
	if ((m_stConfig.pcChargeDataLoader == nullptr) || m_stConfig.strChargeInsertSQL.empty())
		return;

	// 구역판정 버퍼 — ACCURACY_M 적응형(포인트마다 오차만큼), 상한은 park_buf 로 캡(이상치 방지)
	double dfBufM = static_cast<double>(m_stConfig.nParkBuf);
	if ((stRawLogInfo.nAccuracyM >= 0) && (static_cast<double>(stRawLogInfo.nAccuracyM) < dfBufM))
		dfBufM = static_cast<double>(stRawLogInfo.nAccuracyM);

	// ── 판정 규칙 (2026-08-22 재작성 → 2026-08-23 복수 구역 지원) ──────────────────
	//   규칙1  원시 좌표가 폴리곤 내 + 맵매칭 실패                    → 주정차
	//   규칙2  원시 좌표가 폴리곤 내 + 매칭 성공 + 매칭 좌표도 같은 폴리곤 내 → 주정차
	//   규칙4  원시 좌표는 폴리곤 내인데 매칭 좌표는 그 폴리곤 밖     → 통과 중이므로 제외
	//   폴리곤이 겹쳐 설정될 수 있어(시간대별 규제가 다른 구역 등) 포함하는 구역을 전부 다룬다.
	vector<PZONE_INFO> vtZones;
	m_stConfig.pcChargeDataLoader->GetParkingZonesContaining(
		stRawLogInfo.dfX, stRawLogInfo.dfY, dfBufM, &vtZones);

	if (bMatchTrusted && !vtZones.empty())
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

	const bool bTripEnding = (stRawLogInfo.nTripEvent == TRIP_EVENT_END);
	const bool bThisRowTrusted = stRawLogInfo.bRawVldKnown && stRawLogInfo.bRawVld;
	// DRIVE_STATUS=PARKED + 속도 0 이면 좌표 변화는 GPS 튐이다 — 거리로 세지 않고 체류만 연장
	const bool bParkedStill = (stRawLogInfo.nDriveStatus == DRIVE_STATUS_PARKED)
		&& (stRawLogInfo.fSpeed >= 0.0f) && (stRawLogInfo.fSpeed < 1.0f);

	// ── ① 진행 중인 구역 세션 갱신·마감 ───────────────────────────────────────
	for (size_t si = 0; si < pstSession->vtParkRuns.size(); )
	{
		PARK_RUN_SESSION& stRun = pstSession->vtParkRuns[si];

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

		// 체류 종료는 "마지막으로 조건을 만족한 시각" — 디바운스·유예에 쓴 시간을 위반에 넣지 않는다
		CHARGE_INSERT_ROW stRow;
		BuildParkRow(stRun, stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey,
			pstSession->nChargeSeq, stRun.dtLastInZoneTime,
			stRun.dfLastInZoneX, stRun.dfLastInZoneY, "Y", "0", &stRow);
		pvtChargeInserts->push_back(stRow);

		LOGFMTI("[#%02d] parking dwell recorded!device=[%s] trip_id=[%s] seq=[%d] road=[%s] "
			"dwell=[%s]s dist_m=[%s] avg_speed=[%s] trip_ending=[%d]",
			nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID, pstSession->nChargeSeq,
			stRun.szRoadID, stRow.strStaySeconds.c_str(), stRow.strDistM.c_str(),
			stRow.strSpeedKmh.c_str(), static_cast<int>(bTripEnding));

		pstSession->nChargeSeq += 1;
		pstSession->vtParkRuns.erase(pstSession->vtParkRuns.begin() + si);
	}

	if (bTripEnding)
	{
		pstSession->vtParkCands.clear();
		return;
	}

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
			stNew.dtTime = stRawLogInfo.dtGPS;			// 연속의 첫 좌표 — 세션 진입 시각이 된다
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
