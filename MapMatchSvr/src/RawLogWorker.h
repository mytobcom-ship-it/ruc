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
#include "ChargeDataLoader.h"

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
	sint16							nPrevAltitude;						// 직전 매칭 성공 GPS 고도(m) — 연속 고도 앵커. NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE (고가/지하/교량 등)
	bool							bHasPrevAlt;						// 직전 고도 앵커 보유 — true일 때만 연속 맵매칭 고도 점수 적용
	double							dfLastMatchLinkPos;					// 직전 매칭 위치 — 링크 시작점부터 거리(m), 역행 페널티용 (2026-07-20 최정우 추가)
	bool							bHasPrevLinkPos;					// dfLastMatchLinkPos 보유 여부 (2026-07-20 최정우 추가)
	int								nReverseStreak;						// 연속 역행(bReverseSuspect) 포인트 수 — reverse_confirm 미만이면 SKIP·앵커 고정 (2026-07-21 최정우 추가)
	bool							bLastPointOk;						// 직전 처리 포인트가 정상 매칭(앵커 갱신)이었는지 — false 면 이상속도 검사 신뢰 못함 (2026-07-21 최정우 추가)

	char							szTripId[60+1];						// 현재 세션의 TRIP_ID — 신규 trip 감지(END/START 누락 대비) (2026-07-08 최정우 추가)

	// 개방형 게이트 트랙 — "마지막 통과 게이트"가 아니라 "현재 게이트 링크 위에 있는가"(엣지 감지) 의미.
	//   매칭 링크가 게이트 링크가 아니게 되는 순간 반드시 빈 값으로 리셋해야 동일 게이트 재통과 시
	//   정상 재부과됨(리셋 안 하면 재통과가 부과 누락됨) (2026-08-12 최정우 추가)
	char							szActiveGateId[20+1];					// 현재 진입해 있는 게이트 TOLLGATE_ID, 없으면 빈 문자열
	int								nChargeSeq;							// 이 trip 의 다음 PRIM_CHARGEHAND.trip_seq(1부터, 신규 trip 시작 시 리셋)

	// 폐쇄형 게이트 트랙 — 입구(I) 게이트 통과 후 출구(O) 게이트 통과 전까지 상태 유지.
	//   개방형(엣지 감지)과 달리 진입~진출 사이 "구간에 머무는 상태"를 실제로 들고 있어야 함 (2026-08-12 최정우 추가)
	bool							bInClosedRoad;						// true=입구 통과, 출구 대기 중
	char							szEntryTollgateId[20+1];				// 입구 게이트 TOLLGATE_ID
	char							szClosedRoadId[20+1];					// 진입한 폐쇄형 구역 road_id — 짝이 맞는 출구만 인정
	double							dfEntryFromLat;						// 입구 게이트 링크의 from_node 위도(입구 시점 캡처)
	double							dfEntryFromLon;						// 입구 게이트 링크의 from_node 경도
	time_t							dtEntryTime;							// 입구 통과 시각 — occur_dt 로 사용
	uint64							qwClosedRoadJustExitedLinkID;			// 방금 출구 처리한 link_id — 같은 링크에서 즉시 재진입 방지(2026-08-12 최정우 추가)

	// 구간단속 트랙 — 폐쇄형과 별도 독립 상태(같은 도로 위에 겹쳐 동시에 진행 가능하므로 공유 불가) (2026-08-12 최정우 추가)
	bool							bInSpeedZone;							// true=입구 통과, 출구 대기 중
	char							szSpeedZoneRoadId[20+1];				// 진입한 구간단속 구역 road_id
	time_t							dtSpeedEntryTime;						// 입구 통과 시각 — 평균속도·occur_dt 계산용
	uint64							qwSpeedZoneJustExitedLinkID;			// 방금 출구 처리한 link_id — 즉시 재진입 방지

	sVehicleTripSession() :
		qwLinkID(0),
		dtLastSeen(0),
		dwLastGpsSeq(0),
		bStartWarned(false),
		dfLastMatchX(0.0),									// (2026-07-08 최정우 추가)
		dfLastMatchY(0.0),									// (2026-07-08 최정우 추가)
		dtLastMatchGps(0),									// (2026-07-08 최정우 추가)
		bHasLastMatch(false),								// (2026-07-08 최정우 추가)
		nPrevAltitude(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		bHasPrevAlt(false),
		dfLastMatchLinkPos(0.0),							// (2026-07-20 최정우 추가)
		bHasPrevLinkPos(false),								// (2026-07-20 최정우 추가)
		nReverseStreak(0),									// (2026-07-21 최정우 추가)
		bLastPointOk(true),									// (2026-07-21 최정우 추가)
		nChargeSeq(1),										// (2026-08-12 최정우 추가)
		bInClosedRoad(false),								// (2026-08-12 최정우 추가)
		dfEntryFromLat(0.0),									// (2026-08-12 최정우 추가)
		dfEntryFromLon(0.0),									// (2026-08-12 최정우 추가)
		dtEntryTime(0),										// (2026-08-12 최정우 추가)
		qwClosedRoadJustExitedLinkID(0),						// (2026-08-12 최정우 추가)
		bInSpeedZone(false),									// (2026-08-12 최정우 추가)
		dtSpeedEntryTime(0),									// (2026-08-12 최정우 추가)
		qwSpeedZoneJustExitedLinkID(0)							// (2026-08-12 최정우 추가)
	{
		szTripId[0] = '\0';									// (2026-07-08 최정우 추가)
		szActiveGateId[0] = '\0';							// (2026-08-12 최정우 추가)
		szEntryTollgateId[0] = '\0';							// (2026-08-12 최정우 추가)
		szClosedRoadId[0] = '\0';							// (2026-08-12 최정우 추가)
		szSpeedZoneRoadId[0] = '\0';							// (2026-08-12 최정우 추가)
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
 * @struct sChargeInsertRow
 * @brief [charge_insert] bulk INSERT 1행 파라미터 — 개방형(OPEN_ROAD)·폐쇄형(CLOSED_ROAD) 공용 (2026-08-12 최정우 수정)
 * @remark PRIM_CHARGEHAND 컬럼 순서(query.sql [charge_insert] 와 반드시 일치): trip_id, device_key,
 *   trip_seq, charge_type, charge_unit, link_id, from_id, to_id, from_lat, from_lon, to_lat, to_lon,
 *   zone_id, zone_name, dist_m, speed_kmh, speed_limit_kmh, occur_dt, trip_start_dt, tollgate_id,
 *   entry_tollgate_id, exit_tollgate_id, reg_dt, upd_dt
*/
typedef struct sChargeInsertRow
{
	string							strTripId;
	string							strDeviceKey;
	string							strChargeSeq;						// PRIM_CHARGEHAND.trip_seq
	string							strChargeType;						// 1=OPEN_ROAD, 2=CLOSED_ROAD
	string							strChargeUnit;						// 0=NODE(개방형), 1=LINK(폐쇄형)
	string							strLinkId;
	string							strFromId;							// 개방형=게이트ID, 폐쇄형=입구게이트ID
	string							strToId;							// 개방형=게이트ID, 폐쇄형=출구게이트ID
	string							strFromLat;
	string							strFromLon;
	string							strToLat;
	string							strToLon;
	string							strZoneId;							// base_roadlink.road_id (없으면 빈 문자열)
	string							strZoneName;						// base_roadlink.road_nm (없으면 빈 문자열)
	string							strDistM;							// 폐쇄형: 입구~출구 누적거리(m). 개방형은 빈 값(DB 기본 0) (2026-08-12 최정우 추가)
	string							strSpeedKmh;						// 순간속도 — 직전 매칭 위치·시각 있을 때만 계산, 없으면 빈 값 (2026-08-12 최정우 추가)
	string							strSpeedLimitKmh;					// 매칭 링크 제한속도(MATCH_LINK_INFO.nMaxSpeed) (2026-08-12 최정우 추가)
	string							strOccurDt;							// YYYYMMDDHH24MISS
	string							strTripStartDt;						// YYYYMMDDHH24MISS (trip_id 에서 추출)
	string							strTollgateId;						// 개방형·폐쇄형 모두 실측상 빈 값
	string							strEntryTollgateId;					// 폐쇄형 전용: 입구게이트ID. 개방형은 빈 값 (2026-08-12 최정우 추가)
	string							strExitTollgateId;					// 폐쇄형 전용: 출구게이트ID. 개방형은 빈 값 (2026-08-12 최정우 추가)
	string							strRegDt;							// YYYYMMDDHH24MISS — upd_dt 와 동일 값 (2026-08-12 최정우 추가)
	string							strUpdDt;							// strRegDt 와 항상 동일 (2026-08-12 최정우 추가)
	string							strChargeYn;						// 빈 값=DB 기본(Y). 폐쇄형 입/출구 게이트 이상 시 "N" 명시 (2026-08-12 최정우 추가)
	string							strChargeStatus;					// 빈 값=DB 기본(0=PENDING). 폐쇄형 입/출구 게이트 이상 시 "4"(SKIP) 명시 (2026-08-12 최정우 추가)
} CHARGE_INSERT_ROW, *PCHARGE_INSERT_ROW;

/**
 * @struct sTripEndUpdateRow
 * @brief [trip_end] bulk UPDATE 1행 — 트립 종료 시 그 trip_id 의 PRIM_CHARGEHAND 전 행에
 *        trip_end_dt 반영 (2026-08-12 최정우 추가)
 * @remark 과금 INSERT 는 게이트 통과 "즉시"(트립 종료 전) 발생하므로 trip_end_dt 는 그 시점에
 *   알 수 없음 — 트립이 실제로 끝나는 시점(TRIP_EVENT=2)에 별도 UPDATE 로 채운다.
 *   실측(59.11.91.162)은 INSERT 자체를 트립종료 시점에 하는 방식이라 reg_dt=upd_dt=trip_end_dt가
 *   항상 같았지만, 이 구현은 즉시 INSERT 방식을 유지하고 trip_end_dt 만 나중에 UPDATE 하므로
 *   reg_dt(최초 INSERT 시각)와 upd_dt(이 UPDATE 시각)가 달라질 수 있음 — 의도된 차이.
*/
typedef struct sTripEndUpdateRow
{
	string							strTripId;
	string							strTripEndDt;						// YYYYMMDDHH24MISS — END GPS 의 실제 수신 시각
	string							strUpdDt;							// 이 UPDATE 실행 시각
} TRIP_END_UPDATE_ROW, *PTRIP_END_UPDATE_ROW;

/**
 * @struct sRawLogWorkerConfig
 * @brief 워커 공유 설정
*/
typedef struct sRawLogWorkerConfig
{
	CPostgrePool					*pcPostgrePool;
	CProcessManager					*pcProcessManager;
	CChargeDataLoader					*pcChargeDataLoader;					// 게이트·구역 캐시 — 개방형 과금 판정용(nullptr=과금 비활성) (2026-08-12 최정우 추가)
	string							strUpdateSQL;						// [rawgps_update] 완료(1/3/4) 및 release(0) 공용
	string							strChargeInsertSQL;						// [charge_insert] 개방형 게이트 통과 bulk INSERT (비어있으면 비활성) (2026-08-12 최정우 수정)
	string							strTripEndUpdateSQL;						// [trip_end] 트립 종료 시 trip_end_dt UPDATE (비어있으면 비활성) (2026-08-12 최정우 추가)
	int								nWorkerThreads;
	int								nTtlSec;							// trip_id 세션 유지 시간 (초, 0=비활성)
	int								nMatchTimeoutMs;					// 1 GPS 맵매칭 처리 임계 (ms, 초과 시 ERROR 격리, 0=비활성)
	int								nRetryMax;							// release→PENDING 재시도 상한. 초과 시 ERROR(4) 고정. 0=무제한
	int								nConnRetryMax;						// [database] conn_retry_max — 풀 연결 핸들 확보 재시도 최대 횟수 (회, 2026-07-10 최정우 추가)
	int								nConnRetryWait;						// [database] conn_retry_wait — 재시도 사이 대기 (ms, 2026-07-10 최정우 추가)
	int								nRadiusSkip;						// config radius_skip — ACCURACY_M 초과 시 SKIP (m). 0=비활성 (2026-07-08 최정우)
	int								nHeadingMaxDist;					// (단위: m) 연속매칭 heading 계산 이동거리 상한. 초과 시 heading 미사용, 0=비활성 ([mapmatch] distance) (2026-07-15 최정우 추가)
	double							dfSpeedFactor;					// config speed_factor — 이동거리 환산속도/SPEED_KMH 배율 상한. 0=비활성 (2026-07-20 최정우 추가)
	int								nSpeedMargin;					// config speed_margin (km/h) — 노이즈 허용 여유분 (2026-07-20 최정우 추가)
	int								nReverseConfirm;					// config reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
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
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		vector<TRIP_END_UPDATE_ROW> *pvtTripEndUpdates,
		VEHICLE_TRIP_SESSION *pstSession, bool *pbTripEnded);
	bool RunMapMatch(int nThreadId, const sRawLogInfo& stRawLogInfo, VEHICLE_TRIP_SESSION *pstSession,
		MATCH_LINK_INFO *pstMatchLinkInfo);
	// 개방형 게이트 통과 판정 — 엣지 감지(진입 시 1건 적재), 링크가 게이트 아니게 되면 세션 리셋 (2026-08-12 최정우 추가)
	void ProcessOpenGateCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// 폐쇄형 입/출구 게이트 판정 — 입구(I) 통과 시 구간 진입 상태로 전환, 출구(O) 통과 시 누적거리와
	//   함께 1건 적재. road_kind='2'(폐쇄형)인 구역의 게이트만 처리(구간단속 road_kind='3' 등 제외) (2026-08-12 최정우 추가)
	void ProcessClosedRoadCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// 구간단속 입/출구 게이트 판정 — road_kind='3'. 평균속도(구역 실거리÷경과시간) 계산,
	//   charge_yn/charge_status 는 항상 N/4 고정(통행료 파이프라인 비대상 — 실측 확인) (2026-08-12 최정우 추가)
	void ProcessSpeedZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	bool BulkInsertCharges(PGconn *pcConn, const vector<CHARGE_INSERT_ROW>& vtCharges);
	// 트립 종료 시 그 trip_id 의 PRIM_CHARGEHAND 전 행에 trip_end_dt 반영 (2026-08-12 최정우 추가)
	bool UpdateTripEndDt(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows);
	static string FormatDateTime14(time_t dtValue);
	static bool AppendUpdateRow(vector<RAW_LOG_UPDATE_ROW> *pvtUpdates,
		const sRawLogInfo& stRawLogInfo, sint16 nStatus, int nIntersectLen = -1,
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
	// 이동거리 환산속도 vs SPEED_KMH 정합성 검사 — 이상치 GPS SKIP 판정 (2026-07-20 최정우 추가)
	bool ShouldSkipImplausibleSpeed(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, int *pnImpliedSpeedKmh);
	static bool IsValidTripIdForDevice(const sRawLogInfo& stRawLogInfo);
	static bool IsValidTripEvent(sint16 nTripEvent);
	static bool NeedsBeginReset(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const VEHICLE_TRIP_SESSION& stSession, bool *pbFullReset);
	static void ResetTripSessionForBegin(VEHICLE_TRIP_SESSION& stSession, bool bFullReset);
	// 하버사인: WGS84 경위도(도) 두 점 사이 지표거리(m) (2026-07-08 최정우 추가)
	static double HaversineMeters(const POINT& stA, const POINT& stB);
	// INTERSECT_LEN: GPS↔세그먼트 교차점(MATCH_LAT/LON) 하버사인 거리(m) 반올림
	static int CalcIntersectLen(const sRawLogInfo& stRawLogInfo, double dfMatchLon, double dfMatchLat);

private:
	RAWLOG_WORKER_CONFIG				m_stConfig;
	vector<unordered_map<string, VEHICLE_TRIP_SESSION> > m_vtTripSessions;
	CGISUtil							m_cGISUtil;							// 방위각(GetDirAngleDegree) 계산용, stateless (2026-07-08 최정우 추가)
};

#endif //__RAWLOGWORKER_H__
