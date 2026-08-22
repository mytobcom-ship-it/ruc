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
// 같은 유형의 과금구역이 한 도로를 공유할 수 있다(인접 구역의 경계 링크, 장구간 안의 단구간 등).
//   예전엔 유형당 세션이 1개뿐이라 겹친 구역 중 하나만 잡히고 나머지는 조용히 사라졌다.
//   구역별로 세션을 따로 들고 있으면 동시에 여러 구역을 진행할 수 있다 (2026-08-23 최정우 추가)
typedef struct sZoneRunSession
{
	char							szRoadID[20+1];						// 진행 중인 구역 road_id
	time_t							dtEntryTime;						// 진입 시각
	double							dfEntryX;							// 진입 시점 매칭 위치 경도 — from_lon
	double							dfEntryY;							// 진입 시점 매칭 위치 위도 — from_lat
	double							dfAccumDistM;						// 진입 이후 누적 이동거리(m)
	double							dfLastX;							// 직전 매칭 위치 경도 — to_lon
	double							dfLastY;							// 직전 매칭 위치 위도 — to_lat
	uint64							qwLastLinkID;						// 구역 안에서 마지막으로 매칭된 링크 — 이탈 보정용
	time_t							dtExitCandidateTime;				// "무존" 최초 감지 시각 — 재진입 유예용(면제도로만 사용)

	sZoneRunSession() :
		dtEntryTime(0), dfEntryX(0.0), dfEntryY(0.0), dfAccumDistM(0.0),
		dfLastX(0.0), dfLastY(0.0), qwLastLinkID(0), dtExitCandidateTime(0)
	{
		szRoadID[0] = '\0';
	}
} ZONE_RUN_SESSION;

// 주정차는 구역별로 디바운스·유예·체류 스냅샷을 따로 들어야 해서 전용 구조체를 쓴다
//   (2026-08-23 최정우 추가 — 폴리곤이 겹쳐 설정될 수 있어 동시 진행 지원)
typedef struct sParkRunSession
{
	char							szRoadID[20+1];						// 진행 중인 구역 road_id
	time_t							dtEntryTime;						// 세션 시작 시각 — occur_dt(진입 시각)
	double							dfEntryX;							// 시작 raw GPS 경도 — from_lon
	double							dfEntryY;
	double							dfAccumDistM;						// 누적 이동거리(m)
	double							dfLastX;							// 직전 raw GPS 경도(하버사인 기준점)
	double							dfLastY;
	int								nExitTicks;							// 이탈 연속 감지 횟수(park_exitcnt 디바운스)
	time_t							dtExitCandidateTime;				// "무존" 최초 감지 시각(park_regrace)
	time_t							dtLastInZoneTime;					// 마지막으로 조건을 만족한 시각 — 체류 종료 기준
	double							dfLastInZoneX;
	double							dfLastInZoneY;
	time_t							dtLastConfirmedTime;				// 마지막 raw_vld=true 확인 시각 — park_ttl 기준
	double							dfLastConfirmedX;
	double							dfLastConfirmedY;

	sParkRunSession() :
		dtEntryTime(0), dfEntryX(0.0), dfEntryY(0.0), dfAccumDistM(0.0), dfLastX(0.0), dfLastY(0.0),
		nExitTicks(0), dtExitCandidateTime(0), dtLastInZoneTime(0), dfLastInZoneX(0.0),
		dfLastInZoneY(0.0), dtLastConfirmedTime(0), dfLastConfirmedX(0.0), dfLastConfirmedY(0.0)
	{ szRoadID[0] = '\0'; }
} PARK_RUN_SESSION;

// 세션 개시 전 "연속 충족" 카운터 — 구역별로 따로 센다 (2026-08-23 최정우 추가)
typedef struct sParkCandidate
{
	char							szRoadID[20+1];
	int								nTicks;								// 연속 충족 횟수
	time_t							dtTime;								// 연속의 첫 좌표 시각
	double							dfX;								// 연속의 첫 좌표
	double							dfY;

	sParkCandidate() : nTicks(0), dtTime(0), dfX(0.0), dfY(0.0) { szRoadID[0] = '\0'; }
} PARK_CANDIDATE;

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
	//   정상 재부과됨(리셋 안 하면 재통과가 부과 누락됨) (2026-08-12 최정우 추가). vector 인 이유:
	//   같은 link_id 에 개방형(M) 게이트가 2개 이상 있는 경우 전부 독립적으로 진입 추적해야
	//   함(2026-08-13 최정우 수정 — 원래 char[20+1] 단일 필드는 이 상황을 처리 못 함)
	vector<string>					vtActiveGateIds;						// 현재 진입해 있는 게이트 TOLLGATE_ID 목록, 없으면 빈 vector
	int								nChargeSeq;							// 이 trip 의 다음 PRIM_CHARGEHAND.trip_seq(1부터, 신규 trip 시작 시 리셋)

	// 폐쇄형 게이트 트랙 — 입구(I) 게이트 통과 후 출구(O) 게이트 통과 전까지 상태 유지.
	//   개방형(엣지 감지)과 달리 진입~진출 사이 "구간에 머무는 상태"를 실제로 들고 있어야 함 (2026-08-12 최정우 추가)
	bool							bInClosedRoad;						// true=입구 통과, 출구 대기 중
	char							szEntryTollgateId[20+1];				// 입구 게이트 TOLLGATE_ID
	char							szClosedRoadId[20+1];					// 진입한 폐쇄형 구역 road_id — 짝이 맞는 출구만 인정
	double							dfEntryFromLat;						// 입구 게이트 링크의 from_node 위도(입구 시점 캡처)
	double							dfEntryFromLon;						// 입구 게이트 링크의 from_node 경도
	time_t							dtEntryTime;							// 입구 통과 시각 — occur_dt 로 사용
	// "방금 출구 처리한 링크/구역으로 즉시 재진입 방지" 가드는 더 이상 세션에 안 둠 — 세션에 두면
	//   트립이 끝날 때까지 안 풀려서 같은 구역 재통과(진짜 재진입)까지 막아버리는 버그였음. 실제로
	//   막아야 하는 범위는 "출구 처리 직후 같은 tick 안에서 바로 이어지는 입구 후보 검사"뿐이라
	//   ProcessClosedRoadCharge()/ProcessSpeedZoneCharge() 함수 호출 범위(지역변수)로 충분함
	//   (2026-08-20 최정우 수정 — qwClosedRoadJustExitedLinkID/szClosedRoadJustExitedRoadId 제거)

	// 구간단속 트랙 — 폐쇄형과 별도 독립 상태(같은 도로 위에 겹쳐 동시에 진행 가능하므로 공유 불가) (2026-08-12 최정우 추가)
	bool							bInSpeedZone;							// true=입구 통과, 출구 대기 중
	char							szSpeedZoneRoadId[20+1];				// 진입한 구간단속 구역 road_id
	time_t							dtSpeedEntryTime;						// 입구 통과 시각 — 평균속도·occur_dt 계산용
	char							szSpeedEntryTollgateId[20+1];			// 입구 게이트 TOLLGATE_ID — from_id/to_id·
											//   entry/exit_tollgate_id 게이트ID 기반으로 변경(2026-08-20
											//   최정우 수정, 사용자 지시 — 기존엔 구역 road_id만 쓰고
											//   게이트ID 자체는 안 남겼음)
	// qwSpeedZoneJustExitedLinkID/szSpeedZoneJustExitedRoadId 도 동일 이유로 제거(2026-08-20 최정우 수정) — 위 주석 참고

	// 주정차 트랙 — 게이트/구간단속과 별도 독립 상태(폐쇄형 고속도로 위 정차 등 동시 진행 가능).
	//   맵매칭 전 raw GPS 기준(다른 3종은 매칭 링크 기준)이라 세션 판단 재료도 raw 좌표·raw 속도.
	//   구역판정(위치)+SPEED_KMH(서행 구분)+체류시간만으로 판정 — DRIVE_STATUS 는 안 씀(엔진 on
	//   상태 정차 위반을 놓칠 위험), 위치반경 기반 별도 정지판정도 안 씀(SPEED_KMH로 충분,
	//   [[project_parking_match_pseudocode]] 2026-08-13 개정 참고) (2026-08-13 최정우 추가)
	// 주정차 — 구역별 세션·후보 목록 (2026-08-23 최정우 수정)
	vector<PARK_RUN_SESSION>		vtParkRuns;
	vector<PARK_CANDIDATE>			vtParkCands;

	// 비과금도로 트랙(ROAD_KIND=5) — 게이트가 없어 매칭 링크→구역 역인덱스로 진입/이탈 판정.
	//   정상 진행/정상 이탈 시에는 아무것도 INSERT 안 함(어차피 비과금이라 기록할 요금이 없음) —
	//   TTL 만료로 세션이 강제 마감될 때만 예외적으로 1건 기록(사용자 지시, 2026-08-13 추가)
	// 면제도로 트랙(ROAD_KIND=5) — 게이트 없이 매칭 링크→구역 역인덱스로 진입/이탈 판정
	//   (2026-08-13 최초 추가, 2026-08-14 세 차례 재설계 — "모든 미등록 링크" 방식 → zone 기반 복귀 +
	//   charge_type="0" 통합 → 다시 charge_type="5" 고유값 + from_id/to_id를 링크ID에서 zone의
	//   road_id로 원복(사용자 재지시). 진입~이탈 매칭 위치가 from/to_lat·lon)
	// 면제도로 — 구역별 세션 목록 (2026-08-23 최정우 수정, 일반도로와 동일 구조)
	vector<ZONE_RUN_SESSION>		vtExemptRuns;
																				//   원래 구역으로 복귀하면 취소, 초과하면 확정 마감(재진입 유예, 2026-08-14 최정우 추가)

	// 일반도로 트랙(ROAD_KIND=0, NODE_STEP) — 비과금도로와 동일하게 게이트 없는 LINE 구조라
	//   매칭 링크→구역 역인덱스로 진입/이탈 판정. 단, 비과금도로와 달리 실제 과금 대상이라
	//   정상 이탈·트립종료 시에도 Y/0 으로 매번 1건 INSERT(사용자 지시, 2026-08-14 추가 —
	//   RL-Z00002 등 지정 구역 진입~이탈 누적거리 기준, 다른 유형과 겹쳐도 무조건 별도 부과)
	// 일반도로(NODE_STEP) — 구역별 세션 목록. 겹쳐 설정된 구역을 동시에 진행한다 (2026-08-23 최정우 수정)
	vector<ZONE_RUN_SESSION>		vtNodeStepRuns;

	// ── 1틱 지연 커밋 버퍼 — 반대편(짝) 링크 1틱 오매칭 보정용 (2026-08-21 최정우 추가) ──
	//   RunMapMatch 로 정상 매칭(bMatched && !bUntrustedMatch)된 행을 곧바로 과금 처리·DB
	//   반영하지 않고 1건 보류했다가, 바로 다음 GPS의 확정 링크가 "역행의심으로 튀기 전" 링크와
	//   같으면(=1틱만 반대편 짝 링크로 튀었다가 즉시 복귀) GPS 노이즈로 판단해 SKIP(미과금)으로
	//   보정한다. 맵매칭 엔진 자체의 연속매칭 앵커(qwLinkID/dfLastMatchLinkPos 등, RunMapMatch
	//   내부에서 실시간 갱신)는 전혀 건드리지 않음 — 오직 "과금 함수 호출 + rawgps_update 반영
	//   타이밍"만 1틱 늦춘다. 세션(디바이스)에 붙어있어 DB fetch 배치 경계를 넘어서도 유지됨.
	bool							bHasPendingCommit;
	RAW_LOG_INFO					stPendingRawLogInfo;					// 보류 행 원본 GPS 입력
	MATCH_LINK_INFO					stPendingMatchLinkInfo;					// 보류 행 매칭 결과
	sint16							nPendingFinalStatus;					// 보류 행의 확정 MATCH_STATUS(보정 전)
	int								nPendingIntersectLen;					// 보류 행 INTERSECT_LEN
	bool							bPendingHasCoords;						// 보류 행 MATCH_LAT/LON 저장 여부
	uint64							qwLastConfirmedLinkID;					// 마지막으로 "신뢰 가능(과금 반영)"하게 커밋된 링크 ID(0=없음) — 보정판단 기준
	// 보류 행 처리 시점의 과금용 "직전 매칭 위치·시각" 스냅샷 — dfLastMatchX/Y 등은 RunMapMatch 가
	//   매 행마다 실시간으로 최신값으로 전진시키므로, 보류 행을 나중에 commit할 때는 그 당시(보류
	//   시점) 값을 써야 이동거리·속도가 정확함(그렇지 않으면 이미 몇 틱 지난 최신 위치를 "직전
	//   위치"로 오인해 이동거리가 잘못 계산됨)
	double							dfPendingPrevMatchX;
	double							dfPendingPrevMatchY;
	time_t							dtPendingPrevMatchGps;
	bool							bPendingHadLastMatch;

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
		bInSpeedZone(false),									// (2026-08-12 최정우 추가)
		dtSpeedEntryTime(0),									// (2026-08-12 최정우 추가)
		bHasPendingCommit(false),								// (2026-08-21 최정우 추가)
		nPendingFinalStatus(MATCH_STATUS_PENDING),				// (2026-08-21 최정우 추가)
		nPendingIntersectLen(-1),								// (2026-08-21 최정우 추가)
		bPendingHasCoords(false),								// (2026-08-21 최정우 추가)
		qwLastConfirmedLinkID(0),								// (2026-08-21 최정우 추가)
		dfPendingPrevMatchX(0.0),								// (2026-08-21 최정우 추가)
		dfPendingPrevMatchY(0.0),								// (2026-08-21 최정우 추가)
		dtPendingPrevMatchGps(0),								// (2026-08-21 최정우 추가)
		bPendingHadLastMatch(false)								// (2026-08-21 최정우 추가)
	{
		szTripId[0] = '\0';									// (2026-07-08 최정우 추가)
		// vtActiveGateIds 는 vector 라 기본 생성자가 이미 빈 상태로 초기화함 (2026-08-13 최정우 수정)
		szEntryTollgateId[0] = '\0';							// (2026-08-12 최정우 추가)
		szClosedRoadId[0] = '\0';							// (2026-08-12 최정우 추가)
		szSpeedZoneRoadId[0] = '\0';							// (2026-08-12 최정우 추가)
		szSpeedEntryTollgateId[0] = '\0';						// (2026-08-20 최정우 추가)
		memset(reinterpret_cast<void *>(&stPendingRawLogInfo), 0, RAW_LOG_INFO_SIZE);		// (2026-08-21 최정우 추가)
		memset(reinterpret_cast<void *>(&stPendingMatchLinkInfo), 0, MATCH_LINK_INFO_SIZE);	// (2026-08-21 최정우 추가)
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
 * @brief [charge_insert] bulk INSERT 1행 파라미터 — 개방형·폐쇄형·구간단속·주정차 4유형 공용 (2026-08-13 최정우 수정)
 * @remark PRIM_CHARGEHAND 컬럼 순서(query.sql [charge_insert] 와 반드시 일치): trip_id, device_key,
 *   trip_seq, charge_type, charge_unit, link_id, from_id, to_id, from_lat, from_lon, to_lat, to_lon,
 *   zone_id, zone_name, dist_m, speed_kmh, speed_limit_kmh, occur_dt, trip_start_dt, tollgate_id,
 *   entry_tollgate_id, exit_tollgate_id, reg_dt, upd_dt, charge_yn, charge_status, stay_seconds, trip_end_dt
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
	string							strStaySeconds;						// 체류시간(초) — 주정차 전용(컬럼 코멘트: "체류 시간(초). 주정차 위반 판단"),
																		//   다른 3종은 빈 값(DB 기본 0) (2026-08-13 최정우 추가)
	string							strTripEndDt;						// 주정차 TTL 만료 강제마감 전용 — 더 이상 GPS 수신 불가로 판단한 시각.
																		//   그 외는 빈 값(NULL 유지, 실제 TRIP_EVENT=2 시 [trip_end] UPDATE가 채움) (2026-08-13 최정우 추가)
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
	CDataLoader						*pcDataLoader;							// 형상정보(LINK_INFO.qwOppositeLinkID 조회) — 반대편 짝 링크 1틱 오매칭 보정용 (2026-08-21 최정우 추가)
	string							strUpdateSQL;						// [rawgps_update] 완료(1/3/4) 및 release(0) 공용
	string							strChargeInsertSQL;						// [charge_insert] 개방형 게이트 통과 bulk INSERT (비어있으면 비활성) (2026-08-12 최정우 수정)
	string							strTripEndUpdateSQL;						// [trip_end] 트립 종료 시 trip_end_dt UPDATE (비어있으면 비활성) (2026-08-12 최정우 추가)
	string							strAbnormalTripEndSQL;						// [trip_abend] TTL 만료 시 미확정 레코드 마감 UPDATE, 4유형 공용 (비어있으면 비활성) (2026-08-13 최정우 추가, 2026-08-13 수정 — 개방형 한정 해제)
	int								nWorkerThreads;
	int								nTtlSec;							// trip_id 세션 유지 시간 (초, 0=비활성)
	int								nMatchTimeoutMs;					// 1 GPS 맵매칭 처리 임계 (ms, 초과 시 ERROR 격리, 0=비활성)
	int								nRetryMax;							// release→PENDING 재시도 상한. 초과 시 ERROR(4) 고정. 0=무제한
	int								nConnRetryMax;						// [database] retrymax — 풀 연결 핸들 확보 재시도 최대 횟수 (회, 2026-07-10 최정우 추가)
	int								nConnRetryWait;						// [database] retrywait — 재시도 사이 대기 (ms, 2026-07-10 최정우 추가)
	int								nRadiusSkip;						// config radius_skip — ACCURACY_M 초과 시 SKIP (m). 0=비활성 (2026-07-08 최정우)
	int								nHeadingMaxDist;					// (단위: m) 연속매칭 heading 계산 이동거리 상한. 초과 시 heading 미사용, 0=비활성 ([mapmatch] distance) (2026-07-15 최정우 추가)
	double							dfSpeedFactor;					// config speed_factor — 이동거리 환산속도/SPEED_KMH 배율 상한. 0=비활성 (2026-07-20 최정우 추가)
	int								nSpeedMargin;					// config speed_margin (km/h) — 노이즈 허용 여유분 (2026-07-20 최정우 추가)
	int								nReverseConfirm;					// config reverse_confirm — 연속 역행 확정 포인트 수 (2026-07-21 최정우 추가)
	int								nParkBuf;							// config park_buf — 구역판정 버퍼 상한(m) (2026-08-13 최정우 추가)
	int								nParkExitCnt;						// config park_exitcnt — 구역 이탈 확정 연속 GPS 건수(디바운스) (2026-08-13 최정우 추가)
	int								nParkSpeedMax;						// config park_speedmax — 주정차 판정 속도 상한(km/h) (2026-08-22 최정우 추가)
	int								nParkEntryCnt;						// config park_entrycnt — 세션 개시 연속 GPS 건수 (2026-08-22 최정우 추가)
	int								nParkRegraceSec;					// config park_regrace — 재진입 유예시간(초) (2026-08-14 최정우 추가)
	int								nParkTtlSec;						// config park_ttl — 마지막 신뢰 확인 후 강제 마감까지의 시간(초) (2026-08-19 최정우 추가)
	int								nExemptRegraceSec;					// config exempt_regrace — 재진입 유예시간(초) (2026-08-14 최정우 추가)
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

	// #6: dtLastSeen 경과 세션 제거 (모니터 주기 호출). pcConn 은 TTL 만료 시점에 열려 있는 주정차
	//   세션을 즉시 위반 INSERT 하는 데 씀(2026-08-13 최정우 추가)
	int ExpireTtlSessions(int nThreadId, int nTtlSec, PGconn *pcConn);
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
	//   2026-08-13 재작성: 한 링크에 같은 방향 게이트 2개 이상/gate_div='B' 겸용 게이트/서로 다른
	//   구역의 출구·입구가 같은 링크를 공유하는 경우까지 처리 — CollectGateCandidates() 참고
	void ProcessClosedRoadCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// 구간단속 입/출구 게이트 판정 — road_kind='3'. 평균속도(구역 실거리÷경과시간) 계산,
	//   charge_yn/charge_status 는 기본 Y/0(다른 유형과 동일, 2026-08-13 최정우 수정 — 원래는
	//   항상 N/4 고정이었음) (2026-08-12 최정우 추가). 2026-08-13 재작성 — 폐쇄형과 동일한
	//   멀티게이트/공유링크/gate_div='B' 대응 적용
	void ProcessSpeedZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// 주정차 세션 시작(최초 진입 및 재진입 유예 초과 후 동틱 재진입 공용) — 필드 설정만 분리
	//   (2026-08-14 최정우 추가, 재진입 유예 도입과 함께 중복 제거용으로 분리)
	// 주정차(POLY) 판정 — 맵매칭 전 raw GPS·raw 속도 기준(다른 3종과 달리 매칭 결과 안 씀).
	//   구역판정(위치, ACCURACY_M 적응형 버퍼)+SPEED_KMH(서행 컷오프)+체류시간으로 판정, 구역
	//   이탈은 park_exitcnt 회 연속 확인 후에만 확정(디바운스) — RunMapMatch 호출 "전" 실행 (2026-08-13 최정우 추가)
	//   2026-08-14 재진입 유예 추가: 디바운스 통과(=진짜 이탈 후보) 후에도 다른 구역이 아니라 "무존"
	//   이면 park_regrace 초 동안 즉시 확정하지 않고 대기 — 그 안에 같은 구역으로 복귀하면 병합,
	//   초과하면 확정 마감. 확정 마감 시점에 이미 다른 구역 위라면 유예 없이 곧바로 그 구역으로
	//   새 세션 시작(경계 전환 병합, BeginParkingZoneSession 재사용)
	//   2026-08-22 확장 — 규칙 2(매칭 좌표도 폴리곤 내)·규칙 4(매칭 좌표가 폴리곤 밖이면 즉시 해제)를
	//   위해 매칭 결과를 함께 받는다. bMatchTrusted=false 면 매칭 좌표를 보지 않고 원시 좌표만으로 판정.
	void ProcessParkingCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtChargeInserts,
		bool bMatchTrusted = false, double dfMatchX = 0.0, double dfMatchY = 0.0);
	// TTL 만료로 세션이 지워지기 직전, 아직 열려있는 주정차 세션이 체류 임계 이상이면 위반 1건
	// 적재 — trip END를 놓쳤든 단말이 전송을 멈췄든 서버는 원인을 구분 못하므로 "계속 정차 중"으로
	// 간주(사용자 지시, 2026-08-13 추가)
	// 주정차 과금 1행 생성 — 정상 마감·TTL·강제마감 공용 (2026-08-23 최정우 추가)
	void BuildParkRow(const PARK_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, double dfEndX, double dfEndY,
		const char *pszChargeYn, const char *pszChargeStatus, CHARGE_INSERT_ROW *pstRow);
	void AppendExpiredParkingCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut);
	// park_ttl — 세션(디바이스)은 살아있는데 주정차 세션만 마지막 신뢰 확인 후 오래 방치된 경우
	//   좌표 확인 없이 강제 마감 — park_* 필드만 리셋하고 세션 자체는 유지 (2026-08-19 최정우 추가)
	void AppendStaleParkingCharge(int nThreadId, const string& strDeviceKey,
		VEHICLE_TRIP_SESSION *pstSession, vector<CHARGE_INSERT_ROW> *pvtOut);
	// 면제도로 세션 시작(최초 진입 및 재진입 유예 초과 후 동틱 재진입 공용) — 필드 설정만 분리
	//   (2026-08-14 최정우 추가, 재진입 유예 도입과 함께 중복 제거용으로 분리)
	// 면제도로(ROAD_KIND=5) 진입/이탈 판정 — 게이트 없이 매칭 링크→구역 역인덱스로 판정.
	//   출력은 charge_type="5"(비과금도로 고유값) + charge_yn='N'/charge_status='4' 고정(사용자
	//   지시, 2026-08-14 — zone 기반 판정으로 복귀, charge_type/from_id/to_id 모두 최종 원복 상태).
	//   2026-08-14 재진입 유예 추가: "무존" 상태여도 exempt_regrace 초 동안 즉시 확정하지 않고
	//   대기 — 그 안에 같은 구역으로 복귀하면 병합, 초과하면 확정 마감. 확정 마감 시점에 이미 다른
	//   구역 위라면 유예 없이 곧바로 그 구역으로 새 세션 시작(경계 전환 병합, BeginExemptZoneSession
	//   재사용). 진행 중 TTL 만료로 세션이 강제 마감되는 경우는 AppendExpiredExemptZoneCharge() 가 별도 처리
	void ProcessExemptZoneCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// TTL 만료로 세션이 지워지기 직전, 아직 열려있는 면제도로 세션이면 N/4 로 1건 기록
	// (다른 3종의 N/3(AUDIT)과 달리 면제도로는 애초에 과금 대상이 아니므로 N/4(SKIP)가 맞다는
	//   사용자 판단 계승, 2026-08-14)
	// 면제도로 과금 1행 생성 — 정상 이탈과 TTL 만료 공용 (2026-08-23 최정우 추가)
	void BuildExemptRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, CHARGE_INSERT_ROW *pstRow);
	void AppendExpiredExemptZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut);
	// 일반도로(ROAD_KIND=0, NODE_STEP) 진입/이탈 판정 — 비과금도로와 동일 구조(게이트 없이 매칭
	//   링크→구역 역인덱스)지만 실제 과금 대상이라 이탈·트립종료 시 항상 Y/0 으로 1건 기록
	//   (사용자 지시, 2026-08-14 추가)
	void ProcessNodeStepCharge(int nThreadId, const sRawLogInfo& stRawLogInfo,
		const MATCH_LINK_INFO& stMatchLinkInfo, VEHICLE_TRIP_SESSION *pstSession,
		vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// 보류(pending) 중인 1틱 지연 행을 확정(commit) — 반대편 짝 링크 1틱 오매칭이면 SKIP(미과금)으로
	//   보정 후, 과금 함수 호출(직전 매칭 위치·시각은 보류 시점 스냅샷으로 잠깐 바꿔치기 후 원복) +
	//   rawgps_update 큐잉까지 수행. bHasNextLinkID/qwNextLinkID=보정판단용 "다음" 확정 링크,
	//   없으면 false/0(보정 시도 안 함, 계산된 값 그대로 커밋) (2026-08-21 최정우 추가)
	void CommitPendingRow(int nThreadId, VEHICLE_TRIP_SESSION *pstSession,
		bool bHasNextLinkID, uint64 qwNextLinkID,
		vector<RAW_LOG_UPDATE_ROW> *pvtUpdates, vector<CHARGE_INSERT_ROW> *pvtChargeInserts);
	// TTL 만료로 세션이 지워지기 직전, 아직 열려있는 일반도로 세션이면 N/3(AUDIT)로 1건 기록 —
	//   실제 과금 대상이라 폐쇄형/구간단속/주정차와 동일하게 AUDIT(3), 비과금도로의 SKIP(4)과는 다름
	//   (사용자 지시 패턴 계승, 2026-08-14 추가)
	// 일반도로 과금 1행 생성 — 정상 이탈과 TTL 만료가 같은 형식을 쓰도록 공용화 (2026-08-23 최정우 추가)
	void BuildNodeStepRow(const ZONE_RUN_SESSION& stRun, const string& strTripId,
		const string& strDeviceKey, int nChargeSeq, time_t dtEnd, CHARGE_INSERT_ROW *pstRow);
	void AppendExpiredNodeStepCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, vector<CHARGE_INSERT_ROW> *pvtOut);
	// 세션이 지워지기(TTL) 또는 정리되기(트립 정상종료, TRIP_EVENT=2) 직전, 아직 입구만 통과하고
	//   출구를 못 찾은 폐쇄형 세션이면 N/3(AUDIT)로 1건 기록 — 출구를 못 봐서 dist_m/speed_kmh/
	//   to_id/to_lat·lon은 비워둠(모르는 값을 지어내지 않음, NULLIF(...,'')로 DB에 NULL 저장됨),
	//   확실히 아는 것(입구 게이트·구역·진입시각)만 기록 (2026-08-14 최정우 추가 — 기존에 이 함수
	//   자체가 없어 진행 중 세션이 TTL 시 통째로 유실되던 문제 해결).
	//   dtEndTime — 마감 기준 시각(trip_end_dt/stay_seconds 계산용): TTL 경로는 세션의 마지막
	//   처리 시각(dtLastSeen, wall-clock), 트립 정상종료 경로는 그 tick의 GPS 시각(dtGPS) — 호출
	//   측이 상황에 맞는 값을 넘겨줌(2026-08-20 최정우 수정 — 트립 정상종료 시에도 호출되도록 확장)
	void AppendExpiredClosedRoadCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut);
	// 세션이 지워지기(TTL) 또는 정리되기(트립 정상종료) 직전, 아직 입구만 통과하고 출구를 못 찾은
	//   구간단속 세션이면 N/3(AUDIT)로 1건 기록 — 폐쇄형과 동일 이유로 dist_m/speed_kmh/to_lat·lon은
	//   비워둠 (2026-08-14 최정우 추가, 2026-08-20 최정우 수정 — dtEndTime 파라미터화, 근거는
	//   AppendExpiredClosedRoadCharge() 주석 참고)
	void AppendExpiredSpeedZoneCharge(int nThreadId, const string& strDeviceKey,
		const VEHICLE_TRIP_SESSION& stSession, time_t dtEndTime, vector<CHARGE_INSERT_ROW> *pvtOut);
	bool BulkInsertCharges(PGconn *pcConn, const vector<CHARGE_INSERT_ROW>& vtCharges);
	// 트립 종료 시 그 trip_id 의 PRIM_CHARGEHAND 전 행에 trip_end_dt 반영 (2026-08-12 최정우 추가)
	bool UpdateTripEndDt(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows);
	// TTL 만료(비정상 종료) 시 그 trip_id 의 4유형 전부 중 아직 TRIP_END_DT 없는 행을
	//   N/3(AUDIT) + TRIP_END_DT(마지막 확인 시각)으로 마감(사용자 지시, 2026-08-13 추가,
	//   2026-08-13 수정 — 개방형 한정 해제, status 4→3 정정)
	bool UpdateAbnormalTripEnd(PGconn *pcConn, const vector<TRIP_END_UPDATE_ROW>& vtRows);
	static string FormatDateTime14(time_t dtValue);
	// TRIP_ID({6자리 숫자}_{YYYYMMDDHH24MISS}) 에서 시각 부분만 추출 — TRIP_ID 안의 첫 '_'
	//   위치를 직접 찾아 그 다음부터 반환(DEVICE_KEY 길이에 의존하지 않음). 형식이 안 맞으면
	//   nullptr (2026-08-19 최정우 추가 — 기존엔 DEVICE_KEY 길이만큼 건너뛰는 방식이었는데,
	//   TRIP_ID 포맷이 {DEVICE_KEY}_{시각}에서 {CAR_SEQ_NO 6자리}_{시각}로 바뀌면서 길이가
	//   달라져 trip_start_dt 가 엉뚱한 위치부터 잘리는 버그가 있었음, 실측 확인됨)
	static const char* ExtractTripStartDt(const char *szTripId);
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
