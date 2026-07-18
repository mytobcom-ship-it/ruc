/**
 * @file RawLogWorker.h
 * @brief 원시 GPS batch 맵매칭·DB 결과 갱신 워커
*/
#ifndef __RAWLOGWORKER_H__
#define __RAWLOGWORKER_H__

#include <string>
#include <vector>
#include <unordered_map>
#include "TypeDefine.h"
#include "MessageType.h"
#include "Thread.h"
#include "PostgrePool.h"
#include "ProcessManager.h"

using namespace std;

/**
 * @struct sVehicleTripSession
 * @brief trip_id 단위 운행 세션 (연속 맵매칭·TTL 유지용)
 * @remark TRIP_ID 는 수집서버가 START 시 적재한다. 세션 맵 키 = TRIP_ID.
*/
typedef struct sVehicleTripSession
{
	uint64							qwLinkID;							// 직전 맵매칭 링크 ID (연속 맵매칭)
	time_t							dtLastSeen;							// 마지막 처리 시각 (TTL sweep용)
	uint32							dwLastGpsSeq;						// 마지막 처리 GPS_SEQ (역전·리셋 감지)
	bool							bStartWarned;						// START(0) 누락 경고 1회용
	double							dfLastMatchX;						// 직전 매칭 성공 X(경도, WGS84) — HEADING/SPEED 계산 기준 (2026-07-08 최정우 추가)
	double							dfLastMatchY;						// 직전 매칭 성공 Y(위도, WGS84) (2026-07-08 최정우 추가)
	time_t							dtLastMatchGps;						// 직전 매칭 성공 GPS 수신시각 — 속도 계산용 (2026-07-08 최정우 추가)
	bool							bHasLastMatch;						// 직전 매칭 좌표 보유 여부 (2026-07-08 최정우 추가)
	sint16							nPrevAltitudeM;						// 직전 매칭 성공 GPS 고도(m) — 연속 고도 앵커. NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE (고가/지하/교량 등)
	bool							bHasPrevAlt;						// 직전 고도 앵커 보유 — true일 때만 연속 맵매칭 고도 점수 적용
	char							szTripId[60+1];						// 현재 세션의 TRIP_ID — 신규 trip 감지(END/START 누락 대비) (2026-07-08 최정우 추가)

	sVehicleTripSession() :
		qwLinkID(0),
		dtLastSeen(0),
		dwLastGpsSeq(0),
		bStartWarned(false),
		dfLastMatchX(0.0),									// (2026-07-08 최정우 추가)
		dfLastMatchY(0.0),									// (2026-07-08 최정우 추가)
		dtLastMatchGps(0),									// (2026-07-08 최정우 추가)
		bHasLastMatch(false),								// (2026-07-08 최정우 추가)
		nPrevAltitudeM(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		bHasPrevAlt(false)
	{
		szTripId[0] = '\0';									// (2026-07-08 최정우 추가)
	}
} VEHICLE_TRIP_SESSION, *PVEHICLE_TRIP_SESSION;

/**
 * @struct sRawLogUpdateRow
 * @brief rawgps_update 1행 파라미터 (배치 종료 시 일괄 UPDATE)
 * @remark [rawgps_update] $3=match_status:
 *   - 1(MATCHED) / 3(SKIP) / 4(ERROR) : 맵매칭 정상 완료
 *   - 0(PENDING) : bulk 실패 시 예약 해제(release). $4~$6 은 '' 로 MATCH_*·INTERSECT_LEN 미갱신
*/
typedef struct sRawLogUpdateRow
{
	string							strTripId;
	string							strGpsSeq;
	string							strMatchStatus;
	string							strIntersectLen;					// INTERSECT_LEN: GPS↔세그먼트 교차점 거리(m)
	string							strMatchLat;
	string							strMatchLon;
	string							strMatchLinkId;						// 맵매칭 링크 ID (MATCH_LINK_ID) (2026-07-15 최정우 추가)
} RAW_LOG_UPDATE_ROW, *PRAW_LOG_UPDATE_ROW;

/**
 * @struct sRawLogWorkerConfig
 * @brief 워커 공유 설정
*/
typedef struct sRawLogWorkerConfig
{
	CPostgrePool					*pcPostgrePool;
	CProcessManager					*pcProcessManager;
	string							strUpdateSQL;						// [rawgps_update] 완료(1/3/4) 및 release(0) 공용
	string							strChargeInsertSQL;						// [charge_insert] #10 보류 — 재설계 후 INSERT
	int								nWorkerThreads;
	int								nTtlSec;							// trip_id 세션 유지 시간 (초, 0=비활성)
	int								nMatchTimeoutMs;					// 1 GPS 맵매칭 처리 임계 (ms, 초과 시 ERROR 격리, 0=비활성)
	int								nRetryMax;							// release→PENDING 재시도 상한. 초과 시 ERROR(4) 고정. 0=무제한
	int								nConnRetryMax;						// [database] conn_retry_max — 풀 연결 핸들 확보 재시도 최대 횟수 (회, 2026-07-10 최정우 추가)
	int								nConnRetryWait;						// [database] conn_retry_wait — 재시도 사이 대기 (ms, 2026-07-10 최정우 추가)
	int								nRadiusSkip;						// config radius_skip — ACCURACY_M 초과 시 SKIP (m). 0=비활성 (2026-07-08 최정우)
	int								nHeadingMaxDist;					// (단위: m) 연속매칭 heading 계산 이동거리 상한. 초과 시 heading 미사용, 0=비활성 ([mapmatch] distance) (2026-07-15 최정우 추가)
	// int								nRadiusSkipM;						// (구) config radius_skip_m (2026-07-08 최정우)
	// int								nAccuracySkip;						// (구) config accuracy_skip (2026-07-08 최정우)
} RAWLOG_WORKER_CONFIG, *PRAWLOG_WORKER_CONFIG;

/**
 * @class CRawLogWorker
 * @brief ThreadPool Runnable – trip_id batch 처리
*/
class CRawLogWorker : public virtual Runnable
{
public:
	CRawLogWorker();
	virtual ~CRawLogWorker();

	void SetConfig(const RAWLOG_WORKER_CONFIG& stConfig);

	// #6: dtLastSeen 경과 세션 제거 (모니터 주기 호출)
	int ExpireTtlSessions(int nThreadId, int nTtlSec);
	// #7/#8: 예약(batch) PROCESSING→PENDING release
	bool ReleaseReservedBatch(PGconn *pcConn, const RAW_LOG_BATCH& vtBatch, int nThreadId);

	virtual void run(int nThreadId, void *context);
	virtual void stop(int nThreadId, void *context);

private:
	// pstSession: 배치 임시 세션(in-memory). bulk 성공 후에만 m_vtTripSessions 에 반영
	bool ProcessRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, VEHICLE_TRIP_SESSION *pstSession, bool *pbTripEnded);
	bool RunMapMatch(int nThreadId, const sRawLogInfo& stRawLogInfo, VEHICLE_TRIP_SESSION *pstSession,
		MATCH_LINK_INFO *pstMatchLinkInfo);
	static bool AppendUpdateRow(vector<RAW_LOG_UPDATE_ROW> *pvtUpdates,
		const sRawLogInfo& stRawLogInfo, sint16 nStatus, int nIntersectLenM = -1,
		const double *pdfMatchLat = nullptr, const double *pdfMatchLon = nullptr,
		uint64 qwMatchLinkId = 0);
	bool BulkUpdateRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates);
	// bulk update 실패 시 동일 rawgps_update 로 PROCESSING(2)→PENDING(0) 예약 해제
	bool BulkReleaseRawLogs(PGconn *pcConn, const vector<RAW_LOG_UPDATE_ROW>& vtUpdates);
	// 반환 전 미완료 트랜잭션 ROLLBACK 가드 (향후 명시적 트랜잭션 대비)
	void ReleaseConnection(PGconn *pcConn);
	static bool AppendReleaseRowFromRawLog(vector<RAW_LOG_UPDATE_ROW> *pvtRelease,
		const sRawLogInfo& stRawLogInfo);
	static bool IsRowInUpdates(const vector<RAW_LOG_UPDATE_ROW>& vtUpdates,
		const string& strTripId, const string& strGpsSeq);
	static int GetPgCmdTuples(PGresult *pcResult);
	static bool CheckPgUpdateAffected(PGresult *pcResult, int nExpected, const char *pszLogTag);
	static string BuildPgTextArray(const vector<string>& vtValues);
	static string EscapePgArrayText(const string& strValue);
	static bool ValidateRawLog(int nThreadId, const sRawLogInfo& stRawLogInfo, sint16 *pnRejectStatus);
	static bool ShouldSkipGpsInput(int nThreadId, const sRawLogInfo& stRawLogInfo);
	static bool IsValidTripIdForDevice(const sRawLogInfo& stRawLogInfo);
	static bool IsValidTripEvent(sint16 nTripEvent);
	static bool NeedsBeginReset(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, bool *pbFullReset);
	static void ResetTripSessionForBegin(VEHICLE_TRIP_SESSION& stSession, bool bFullReset);
	// 하버사인: WGS84 경위도(도) 두 점 사이 지표거리(m) (2026-07-08 최정우 추가)
	static double HaversineMeters(const POINT& stA, const POINT& stB);
	// INTERSECT_LEN: GPS↔세그먼트 교차점(MATCH_LAT/LON) 하버사인 거리(m) 반올림
	static int CalcIntersectLenM(const sRawLogInfo& stRawLogInfo, double dfMatchLon, double dfMatchLat);

private:
	RAWLOG_WORKER_CONFIG				m_stConfig;
	vector<unordered_map<string, VEHICLE_TRIP_SESSION> > m_vtTripSessions;
	CGISUtil							m_cGISUtil;							// 방위각(GetDirAngleDegree) 계산용, stateless (2026-07-08 최정우 추가)
};

#endif //__RAWLOGWORKER_H__
