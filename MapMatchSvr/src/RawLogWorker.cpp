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
 * @param[in] nMaxAttempt 재시도 최대 횟수 ([database] conn_retry_max, 회)
 * @param[in] nWaitMs 재시도 사이 대기 ([database] conn_retry_wait, ms)
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
 * @return 제거된 세션 수
 * @remark 각 워커가 자기 맵(m_vtTripSessions[nThreadId])만 정리 → 락 불필요(소유권 유지).
 *         run() 배치 처리 직후 호출되어 모니터 스레드와의 동시 접근(데이터 레이스)을 제거한다.
*/
int CRawLogWorker::ExpireTtlSessions(int nThreadId, int nTtlSec)
{
	if (nTtlSec <= 0)
		return 0;

	if (nThreadId < 0 || nThreadId >= static_cast<int>(m_vtTripSessions.size()))
		return 0;

	unordered_map<string, VEHICLE_TRIP_SESSION>& mapSessions =
		m_vtTripSessions[static_cast<size_t>(nThreadId)];

	const time_t dtNow = time(nullptr);
	int nRemoved = 0;

	for (unordered_map<string, VEHICLE_TRIP_SESSION>::iterator it=mapSessions.begin();
			it != mapSessions.end(); )
	{
		if (it->second.dtLastSeen > 0
			&& (dtNow - it->second.dtLastSeen) > static_cast<time_t>(nTtlSec))
		{
			LOGFMTD("[#%02d] session ttl expired!trip_id=[%s] last_seen=[%ld] ttl=[%d]",
				nThreadId, it->first.c_str(),
				static_cast<long>(it->second.dtLastSeen), nTtlSec);
			mapSessions.erase(it++);
			++nRemoved;
		}
		else
			++it;
	}

	if (nRemoved > 0)
	{
		LOGFMTD("[#%02d] session ttl expired removed!count=[%d] ttl_sec=[%d]",
			nThreadId, nRemoved, nTtlSec);
	}

	return nRemoved;
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
 * @brief TRIP_ID 가 DEVICE_KEY 기반 형식인지 검사
 * @param[in] stRawLogInfo 원시 GPS
 * @return true(유효), false(실패)
 * @remark 형식: {DEVICE_KEY}_{YYYYMMDDHH24MISS}
*/
bool CRawLogWorker::IsValidTripIdForDevice(const sRawLogInfo& stRawLogInfo)
{
	if (stRawLogInfo.szDeviceKey[0] == '\0' || stRawLogInfo.szTripID[0] == '\0')
		return false;

	size_t nDeviceKeyLen = strlen(stRawLogInfo.szDeviceKey);
	if (strncmp(stRawLogInfo.szTripID, stRawLogInfo.szDeviceKey, nDeviceKeyLen) != 0)
		return false;

	return (stRawLogInfo.szTripID[nDeviceKeyLen] == '_');
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

	// trip_id 가 DEVICE_KEY_{ts} 형식인지 검사 (2026-07-08 최정우 주석 추가)
	if (!IsValidTripIdForDevice(stRawLogInfo))
	{
		LOGFMTW("[#%02d] reject trip_id mismatch!device=[%s] trip_id=[%s] seq=[%u]",
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
	stSession.nPrevAltitudeM = NO_ALTITUDE;
	stSession.nPrevRoadType = ROAD_TYPE_NORMAL;
	stSession.bHasPrevAlt = false;

	if (bFullReset)
		stSession.bStartWarned = false;
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

	// batch 처리용 DB 커넥션 획득 (#E-1: [database] conn_retry_max/wait 재시도) (2026-07-10 최정우 추가)
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
		if (!ProcessRawLog(nThreadId, (*pvtBatch)[i], &vtUpdates, &stWorkSession, &bTripEnded))
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

		if (!IsRowInUpdates(vtUpdates, (*pvtBatch)[i].szTripID, szGpsSeq))
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
		else
		{
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
	ExpireTtlSessions(nThreadId, m_stConfig.nTtlSec);

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
*/
bool CRawLogWorker::ProcessRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, VEHICLE_TRIP_SESSION *pstSession, bool *pbTripEnded)
{
	if ((pvtUpdates == nullptr) || (pstSession == nullptr) || (pbTripEnded == nullptr))
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

	// GPS 좌표·RAW_VLD 유효성 검사 — SKIP(3). 세션·DB 좌표 미저장 (2026-07-10 최정우 수정)
	if (ShouldSkipGpsInput(nThreadId, stRawLogInfo))
	{
		stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;
		if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
			*pbTripEnded = true;
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
		if (stRawLogInfo.nTripEvent == TRIP_EVENT_END)
			*pbTripEnded = true;

		// 정확도 SKIP — 최근접 있으면 참고용 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리) 저장
		MATCH_LINK_INFO stNear;
		memset(reinterpret_cast<void *>(&stNear), 0, MATCH_LINK_INFO_SIZE);
		stNear.dfIntersectLenSgmt = -1.0;
		CProcessManager& cPM = m_stConfig.pcProcessManager[nThreadId];
		if (cPM.FindNearestSegment(stRawLogInfo, &stNear))
		{
			int nNearLenM = CalcIntersectLenM(stRawLogInfo, stNear.dfMatchX, stNear.dfMatchY);
			return AppendUpdateRow(pvtUpdates, stRawLogInfo, MATCH_STATUS_SKIP, nNearLenM,
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
	else if (m_stConfig.nMatchTimeoutMs > 0)
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

	// ── 단조 진행 가드: 나중 GPS가 진행방향상 "이전(뒤)"에 매칭되면 노이즈로 보고 SKIP·앵커 미갱신 (2026-07-16 최정우 추가) ──
	//   판정: 이동방향(직전 매칭점→현재 GPS) 단위벡터에 "매칭 변위(직전 매칭점→현재 매칭점)"를 투영(m).
	//         투영값이 −MM_BACKWARD_TOL_M 미만이면 진행 반대(역행) → 노이즈로 간주.
	//   U턴/회차는 이동방향 자체가 반대로 바뀌므로 투영이 양수가 되어 자동 허용(오탐 없음).
	//   과금(링크 단위) 순서 역전·중복 방지 목적.
	if (bMatched && pstSession->bHasLastMatch)
	{
		const double dfPi = 3.14159265358979323846;
		double dfMx = 111320.0 * cos(pstSession->dfLastMatchY * dfPi / 180.0);	// m/도(경도)
		double dfMy = 111320.0;													// m/도(위도)
		double dfMoveE = (stRawLogInfo.dfX - pstSession->dfLastMatchX) * dfMx;
		double dfMoveN = (stRawLogInfo.dfY - pstSession->dfLastMatchY) * dfMy;
		double dfMoveLen = sqrt(dfMoveE * dfMoveE + dfMoveN * dfMoveN);
		if (dfMoveLen >= MM_BACKWARD_MIN_MOVE_M)
		{
			double dfMatchE = (stMatchLinkInfo.dfMatchX - pstSession->dfLastMatchX) * dfMx;
			double dfMatchN = (stMatchLinkInfo.dfMatchY - pstSession->dfLastMatchY) * dfMy;
			double dfProgress = (dfMatchE * dfMoveE + dfMatchN * dfMoveN) / dfMoveLen;	// 진행방향 투영(m)
			if (dfProgress < -MM_BACKWARD_TOL_M)
			{
				LOGFMTW("[#%02d] backward-progress skip!device=[%s] trip_id=[%s] seq=[%u] progress=[%.1fm]",
					nThreadId, stRawLogInfo.szDeviceKey, stRawLogInfo.szTripID,
					stRawLogInfo.dwSeqNo, dfProgress);
				nFinalStatus = MATCH_STATUS_SKIP;
				bMatched = false;					// 앵커 미갱신·MATCHED 아님(과금 역전 방지)
			}
		}
	}

	stSession.dwLastGpsSeq = stRawLogInfo.dwSeqNo;

	// ── 매칭 성공(MATCHED) 시에만 세션 앵커 갱신 — SKIP/ERROR 는 직전 성공 앵커 유지 (2026-07-10 최정우 수정) ──
	//   · XY·시각: HEADING/SPEED 보정용 (ALTITUDE_M NULL이어도 갱신)
	//   · 고도: ALTITUDE_M 유효 시에만 nPrevAltitudeM·nPrevRoadType 저장
	//     (직전 매칭 좌표에 Z 없음 — GPS 고도를 앵커로 기억)
	//   · 직전 고도 없이 현재만 있으면 bHasPrevAlt=false → 고도 점수 스킵
	if (bMatched)
	{
		stSession.dfLastMatchX = stMatchLinkInfo.dfMatchX;
		stSession.dfLastMatchY = stMatchLinkInfo.dfMatchY;
		stSession.dtLastMatchGps = stRawLogInfo.dtGPS;
		stSession.bHasLastMatch = true;
		if (stRawLogInfo.nAltitudeM >= 0)
		{
			stSession.nPrevAltitudeM = stRawLogInfo.nAltitudeM;
			stSession.nPrevRoadType = stMatchLinkInfo.nRoadType;
			stSession.bHasPrevAlt = true;
		}
	}

	// INTERSECT_LEN: GPS↔세그먼트 교차점(MATCH_LAT/LON) 하버사인 거리(m) → 정수 반올림
	const bool bHasCoords = (bMatched || bOut);
	int nIntersectLenM = -1;
	if (bHasCoords)
		nIntersectLenM = CalcIntersectLenM(stRawLogInfo,
			stMatchLinkInfo.dfMatchX, stMatchLinkInfo.dfMatchY);

	// DB 저장: MATCHED/SKIP 시 MATCH_LAT/LON·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)·MATCH_LINK_ID. ERROR 는 미저장
	if (!AppendUpdateRow(pvtUpdates, stRawLogInfo, nFinalStatus, nIntersectLenM,
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
			stAltCtx.dfHorizMoveM = dfMoveM;

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
		stAltCtx.dfHorizMoveM = HaversineMeters(stPrev, stCur);
	}

	// ── 고도 앵커 → 연속 맵매칭 컨텍스트 (Begin 미적용) ──
	//   · 전제: bHasPrevAlt (직전 매칭 성공 시 ALTITUDE_M 있었음)
	//   · dfHorizMoveM: 직전 매칭 XY → 현재 GPS XY 하버사인(m) — 경사 판정용
	//   · Δalt = 현재 ALTITUDE_M − nPrevAltitudeM (ProcessManager/ContinueMapMatch에서 사용)
	// · 예) seq10 매칭·고도100m 저장 → seq11 고도106m·같은 고가 → 차이=8 이내 보너스 −3
	if (pstSession->bHasPrevAlt)
	{
		stAltCtx.nPrevAltitudeM = pstSession->nPrevAltitudeM;
		stAltCtx.nPrevRoadType = pstSession->nPrevRoadType;
		stAltCtx.bHasPrevAlt = true;
	}

	// 연속 맵매칭 링크는 "맵매칭 성공(반경 내 MATCHED)" 시에만 세션에 반영한다.
	//   SKIP(정확도/반경 밖)·ERROR 시 직전 성공 링크를 그대로 유지 → 다음 GPS 는 마지막 성공 링크
	//   기준으로 연속 맵매칭을 이어간다. (ProcessRawLog 는 실패 시 로컬 링크를 0으로 리셋하므로
	//   세션 링크에 반영되지 않도록 로컬 복사본으로 호출) (2026-07-10 최정우 수정)
	uint64 qwLinkID = pstSession->qwLinkID;
	bool bMatched = cProcessManager.ProcessRawLog(stAdjusted, qwLinkID, pstMatchLinkInfo,
		stAltCtx.bHasPrevAlt ? &stAltCtx : nullptr);
	if (bMatched)
		pstSession->qwLinkID = qwLinkID;		// 성공 시에만 링크 전진(다음 점 연속 매칭 기준)
	return bMatched;
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
int CRawLogWorker::CalcIntersectLenM(const sRawLogInfo& stRawLogInfo,
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
 * @param[in] nIntersectLenM GPS↔세그먼트 교차점 거리(m, INTERSECT_LEN), -1 이면 미갱신
 * @param[in] pdfMatchLat 매칭 위도 (MATCHED 시), nullptr 이면 미갱신
 * @param[in] pdfMatchLon 매칭 경도 (MATCHED 시), nullptr 이면 미갱신
 * @return true(적재 성공), false(pvtUpdates null·trip_id 무효)
 * @remark invalid trip_id 시 false — run() orphan release 가 PK 없으면 복구 대기
*/
bool CRawLogWorker::AppendUpdateRow(vector<RAW_LOG_UPDATE_ROW> *pvtUpdates,
		const sRawLogInfo& stRawLogInfo, sint16 nStatus, int nIntersectLenM,
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
	if (nIntersectLenM >= 0)
		snprintf(szIntersectLen, sizeof(szIntersectLen), "%d", nIntersectLenM);
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
	//   $4 MATCH_LON, $5 INTERSECT_LEN, $6 MATCH_LINK_ID, $7 MATCH_STATUS) (2026-07-15 최정우 재정렬)
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
