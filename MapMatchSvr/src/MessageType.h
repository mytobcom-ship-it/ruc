/**
 * @file MessageType.h
 * @brief http 수신 메시지 정의 헤더 파일
*/
#ifndef __MESSAGETYPE_H__
#define __MESSAGETYPE_H__

#include <time.h>
#include <vector>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataDefine.h"

using namespace std;

#define TRIP_EVENT_START				0									// 출발
#define TRIP_EVENT_NONE					1									// 주행 중 (기본값)
#define TRIP_EVENT_END					2									// 도착

#define DRIVE_STATUS_ON_ROAD			0									// 주행 중 (기본값)
#define DRIVE_STATUS_IDLE				1									// 일시 정지
#define DRIVE_STATUS_PARKED				2									// 정차·주차
#define DRIVE_STATUS_TUNNELING			3									// GPS 음영 구간

#define MATCH_STATUS_PENDING			0									// 대기 (기본값)
#define MATCH_STATUS_MATCHED			1									// 맵매칭 완료
#define MATCH_STATUS_PROCESSING			2									// 처리 중
#define MATCH_STATUS_SKIP				3									// 제외
#define MATCH_STATUS_ERROR				4									// 오류

/**
 * @struct sRawLogInfo
 * @brief GPS 로그 정보
*/
typedef struct sRawLogInfo
{
	char							szDeviceID[56+1];					// 디바이스 ID (DEVICE_KEY_YYYYMMDD_운행순번4자리)
	char							szDeviceKey[47+1];					// 디바이스 키 (모바일 앱 인증 키)
	char							szTripID[60+1];						// 운행 ID (수집서버 적재: {DEVICE_KEY}_{YYYYMMDDHH24MISS})
	sint16							nTripEvent;							// TRIP_EVENT SMALLINT (0:START, 1:NONE, 2:END)
	sint16							nDriveStatus;						// DRIVE_STATUS SMALLINT (0:ON_ROAD, 1:IDLE, 2:PARKED, 3:TUNNELING)
	uint32							dwSeqNo;							// 순번
	bool							bGpsLatNull;						// GPS_LAT NULL 여부
	bool							bGpsLonNull;						// GPS_LON NULL 여부
	bool							bRawVldKnown;						// RAW_VLD 컬럼 조회 여부
	bool							bRawVld;							// RAW_VLD TRUE (bRawVldKnown 일 때만 유효)
	double							dfX;								// X 좌표
	double							dfY;								// Y 좌표
	float							fSpeed;								// 순간 속도 (km/h)
	time_t							dtGPS;								// GPS 수신 시간
	time_t							dtRecv;								// 수집서버 수신 시간 (서버 수신 시간)
	sint16							nAngle;								// 방위각 (0~359, -1:사용안함)
	sint16							nAccuracyM;							// 수평 오차(m). NO_ACCURACY=미적용
	sint16							nAltitudeM;							// 고도(m). NO_ALTITUDE=미적용
	uint8							nRoadRank;							// (미사용, 0 유지) 구 도로등급 힌트 필드
	uint8							nCoordinateType;					// 측지계 코드 (1:EPSG3857, 2:WGS84GEO, 3:KATECH, 4:BESSELGEO)
	
	sRawLogInfo() :
		nTripEvent(TRIP_EVENT_NONE),
		nDriveStatus(DRIVE_STATUS_ON_ROAD),
		dwSeqNo(0),
		bGpsLatNull(false),
		bGpsLonNull(false),
		bRawVldKnown(false),
		bRawVld(false),
		dfX(0.0), 
		dfY(0.0), 
		fSpeed(0.0f), 
		dtGPS(0), 
		dtRecv(0), 
		nAngle(-1), 
		nAccuracyM(NO_ACCURACY),
		nAltitudeM(NO_ALTITUDE),
		nRoadRank(0), 
		nCoordinateType(WGS84GEO)
	{
		memset(reinterpret_cast<void *>(szDeviceID), 0, sizeof(szDeviceID));
		memset(reinterpret_cast<void *>(szDeviceKey), 0, sizeof(szDeviceKey));
		memset(reinterpret_cast<void *>(szTripID), 0, sizeof(szTripID));
	}
} RAW_LOG_INFO, *PRAW_LOG_INFO;

#define RAW_LOG_INFO_SIZE												sizeof(RAW_LOG_INFO)

/**
 * @typedef RAW_LOG_BATCH
 * @brief 동일 운행(trip_id) 단위 원시 GPS 로그 묶음 (gps_dt·gps_seq 순)
*/
typedef vector<sRawLogInfo> RAW_LOG_BATCH;

/**
 * @struct sAltMatchCtx
 * @brief 연속 맵매칭 고도 보조 점수용 세션 컨텍스트 (trip_id 세션 → ProcessManager 전달)
 * @remark
 *   직전 매칭 성공 시 RawLogWorker가 GPS ALTITUDE_M·ROAD_TYPE을 기억.
 *   Δalt = 현재 nAltitudeM − nPrevAltitude, dfHorizMove로 경사(alt_slope) 검사.
*/
typedef struct sAltMatchCtx
{
	sint16							nPrevAltitude;						// 직전 매칭 성공 시 GPS 고도(m). NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE
	bool							bHasPrevAlt;						// 직전 고도·도로유형 보유 여부
	double							dfHorizMove;						// 직전 매칭점→현재 GPS 수평거리(m)
	double							dfPrevLinkPos;						// 직전 매칭 위치 — 링크 시작점부터 거리(m), 역행 페널티용 (2026-07-20 최정우 추가)
	bool							bHasPrevLinkPos;					// dfPrevLinkPos 보유 여부 (2026-07-20 최정우 추가)
	// dfPrevMatchX/Y 는 "신뢰 가능한" 매칭에서만 갱신되는데(bHasLastMatch), bHasPrevLinkPos 는
	//   매칭 성공이면 무조건 갱신된다. 두 조건이 어긋나 dfPrevMatchX 가 0 인 채로 노이즈 보정
	//   기준점으로 쓰이면 매칭 좌표가 (0,0) 근처로 찍힌다 — 실측 확인(000370_20260819093236
	//   seq26~ , 13,200km 오매칭). 그래서 보유 여부를 따로 들고 다닌다 (2026-08-23 최정우 추가)
	bool							bHasPrevMatchPos;					// dfPrevMatchX/Y 유효 여부
	double							dfPrevMatchX;						// 직전(신뢰 가능) 매칭 성공 X(경도, WGS84) — 같은 링크 노이즈 보정 기준점 (2026-07-22 최정우 추가)
	double							dfPrevMatchY;						// 직전(신뢰 가능) 매칭 성공 Y(위도, WGS84) (2026-07-22 최정우 추가)
	// 원시 GPS 좌표·방향(heading)이 직전 tick과 완전히 동일한지 — true 면 같은 링크 노이즈 보정
	//   (1m 강제전진, MM_NOISE_FORWARD_NUDGE_M)을 적용하지 않고 세그먼트 매칭이 실제로 계산한
	//   좌표를 그대로 쓴다. 좌표·방향이 똑같다는 건 차량이 실제로 움직이지 않았다는 뜻이라,
	//   강제전진을 걸면 정지 중에도 위치가 계속 앞으로 밀리는 누적 드리프트가 생긴다
	//   (사용자 지시, 2026-09-02 최정우 추가)
	bool							bSameRawAndHeadingAsPrev;
	// 직전 확정 매칭 GPS 시각 → 현재 GPS 시각 경과초 — dfHorizMove 와 짝을 이뤄, depth 탐색
	//   재구성 경로의 절대 물리속도(km/h) 타당성 판정에 사용. 계산 불가(직전 매칭 없음·역전 등)
	//   시 음수 유지 (2026-09-04 최정우 추가)
	double							dfGapSec;

	sAltMatchCtx() :
		nPrevAltitude(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		bHasPrevAlt(false),
		dfHorizMove(0.0),
		dfPrevLinkPos(0.0),
		bHasPrevLinkPos(false),
		bHasPrevMatchPos(false),
		dfPrevMatchX(0.0),
		dfPrevMatchY(0.0),
		bSameRawAndHeadingAsPrev(false),
		dfGapSec(-1.0)
	{}
} ALT_MATCH_CTX, *PALT_MATCH_CTX;

/**
 * @struct sMapMatchInput
 * @brief 맵 매칭 입력 정보 (CMapMatch 시작/Continue 호출용)
*/
typedef struct sMapMatchInput
{
	uint8							nCoordinateType;					// 측지계 코드 (1:EPSG3857, 2:WGS84GEO, 3:KATECH, 4:BESSELGEO)
	sint16							nRadius;							// 맵매칭 유효거리
	double 							dfX;								// X 좌표
	double 							dfY;								// Y 좌표
	sint16							nAngle;								// 방위각 (0~359, -1:사용안함)
	sint16							nSpeed;								// 순간속도 (km/h, -1:없음) — 방위각 가중치 적응용 (2026-07-08 최정우 추가)
	uint64							qwLinkID;							// 현재 주행중인 링크 ID (연속 측위)
	uint8							nRoadRank;							// (미사용, 0 유지) 구 도로등급 힌트 필드
	sint16							nSearchStep;						// 연속 측위시 탐색할 단계 (연속 측위, 기본:0, 0~최대검색단계)
	sint16							nAltitudeM;							// 현재 GPS 고도(m). NO_ALTITUDE=없음
	sint16							nPrevAltitude;						// 직전 매칭 GPS 고도(m). NO_ALTITUDE=없음
	uint8							nPrevRoadType;						// 직전 성공 링크 ROAD_TYPE
	sint16							nDriveStatus;						// DRIVE_STATUS (터널 시 고도 무시)
	double							dfHorizMove;						// 직전 매칭점→현재 GPS 수평거리(m)
	bool							bUseAltScore;						// 연속 맵매칭 고도 보조 점수 적용 여부
	uint64							qwBiasLinkID;						// 연속실패 Begin 재검색용: 직전 성공 링크(연결성 편향, 0=미적용) (2026-07-15 최정우 추가)
	double							dfPrevLinkPos;						// 직전 매칭 위치 — 링크 시작점부터 거리(m), 역행 페널티용 (2026-07-20 최정우 추가)
	bool							bHasPrevLinkPos;					// dfPrevLinkPos 보유 여부 (2026-07-20 최정우 추가)
	// dfPrevMatchX/Y 는 "신뢰 가능한" 매칭에서만 갱신되는데(bHasLastMatch), bHasPrevLinkPos 는
	//   매칭 성공이면 무조건 갱신된다. 두 조건이 어긋나 dfPrevMatchX 가 0 인 채로 노이즈 보정
	//   기준점으로 쓰이면 매칭 좌표가 (0,0) 근처로 찍힌다 — 실측 확인(000370_20260819093236
	//   seq26~ , 13,200km 오매칭). 그래서 보유 여부를 따로 들고 다닌다 (2026-08-23 최정우 추가)
	bool							bHasPrevMatchPos;					// dfPrevMatchX/Y 유효 여부
	double							dfPrevMatchX;						// 직전(신뢰 가능) 매칭 성공 X(경도, WGS84) — 같은 링크 노이즈 보정 기준점 (2026-07-22 최정우 추가)
	double							dfPrevMatchY;						// 직전(신뢰 가능) 매칭 성공 Y(위도, WGS84) (2026-07-22 최정우 추가)
	bool							bSameRawAndHeadingAsPrev;			// 원시좌표·방향이 직전 tick과 완전히 동일 — true 면 노이즈
																		//   보정(1m 강제전진) 미적용 (2026-09-02 최정우 추가)
	double							dfGapSec;							// 직전 확정 매칭 GPS 시각→현재 GPS 시각 경과초(ALT_MATCH_CTX.dfGapSec
																		//   전달용) — 재구성 경로 절대속도 타당성 판정. 음수=계산 불가 (2026-09-04 최정우 추가)

	sMapMatchInput() :
		nCoordinateType(WGS84GEO),
		nRadius(NO_RADIUS),
		dfX(0.0),
		dfY(0.0),
		nAngle(NO_ANGLE),
		nSpeed(NO_SPEED),
		qwLinkID(0),
		nRoadRank(0),
		nSearchStep(2),
		nAltitudeM(NO_ALTITUDE),
		nPrevAltitude(NO_ALTITUDE),
		nPrevRoadType(ROAD_TYPE_NORMAL),
		nDriveStatus(DRIVE_STATUS_ON_ROAD),
		dfHorizMove(0.0),
		bUseAltScore(false),
		qwBiasLinkID(0),
		dfPrevLinkPos(0.0),
		bHasPrevLinkPos(false),
		bHasPrevMatchPos(false),
		dfPrevMatchX(0.0),
		dfPrevMatchY(0.0),
		bSameRawAndHeadingAsPrev(false),
		dfGapSec(-1.0)
	{}
} MAP_MATCH_INPUT, *PMAP_MATCH_INPUT;

// MATCH_LINK_INFO.aqwPathLinkIDs 칸 수 (2026-08-20 최정우 추가)
#define MATCH_LINK_INFO_MAX_PATH		8

/**
 * @struct sMatchLinkInfo
 * @brief 매칭 링크 정보
 * @remark
 * 	- nConnect : 0:연결로 아님, 1:연결로 (MOCT). 101~108:구 링크 등급별 연결로
 * 	- nRoadType : 000:일반, 001:교량, 002:터널, 003:고가, 004:지하 (MOCT_LINK.ROAD_TYPE)
 * 	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sMatchLinkInfo
{
	uint16							wErrorCode;							// 에러 코드
	char							szErrorMsg[48];						// 에러 메시지
	double							dfMatchX;							// 매핑 X 좌표
	double							dfMatchY;							// 매핑 Y 좌표
	double							dfSgmtMatchLen;						// 세그먼트 시작 좌표에서 매핑 좌표까지의 거리(m)
	double                          dfIntersectLenSgmt;					// GPS 좌표와 세그먼트 교차점까지 거리(m) — DB INTERSECT_LEN
	sint16							nDirAngleDiff;						// 매핑 각도 차이
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점부터 세그먼트 시작점까지 거리 (m)
	uint8							nMaxSpeed;							// 제한 속도 (km/h)
	double							dfLen;								// 링크 길이 (m)
	uint8							nRoadRank;							// 도로 종별[3]
	uint8							nConnect;							// 연결 코드[3]
	uint8							nRoadType;							// 도로 유형[3]
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	double							dfStNodeX;							// 시작 노드 X
	double							dfStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성[3]
	uint64							qwEdNodeID;							// 종료 노드 ID
	double							dfEdNodeX;							// 종료 노드 X
	double							dfEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성[3]
	bool							bOutOfRadius;						// 반경 밖·진단반경 초과 최근접 — SKIP 참고용 좌표·거리 유효, 정식 MATCHED 아님 (2026-07-10 최정우 수정)
	bool							bReverseSuspect;					// 위치 역행 + heading 도 역방향 일치 — 연속역행(reverse_confirm) 스트릭 판정 전용 (2026-07-21 최정우 추가)
	bool							bClampLowConf;						// 경계 클램프 + INTERSECT_LEN 초과 — 신뢰도 낮은 매칭 SKIP 처리용 (2026-07-21 최정우 추가)
	bool							bAmbiguousReverse;					// 같은 링크 역행인데 heading 없음/애매해 노이즈 단정 불가 — SKIP 처리용 (2026-07-22 최정우 추가)
	bool							bContinueFallback;					// 직전 확정 위치와의 연결성(위상 그래프)이 검증되지 않고 순수 근접 거리만으로 채택된 결과 —
																		//   ① Continue 완전 실패 후 Begin(반경 최근접) 폴백(ProcessManager::AttemptMatch), 또는
																		//   ② Continue 성공했으나 방위각 비용 초과로 병행 Begin 재평가가 덮어씀(MapMatch.cpp
																		//   bBeginOverrode) 두 경우 공용. 이동거리 타당성(SKIP) 판정 대상 한정용 (2026-09-04 최정우 추가)
	bool							bImplausiblePath;					// depth 탐색 재구성 경로 길이가 경과시간(dfGapSec) 대비 물리적으로 불가능한 절대속도
																		//   (MM_PATH_ABS_MAX_KMH 초과, MapMatch.cpp)로 나온 경우 — 최종 링크 자체를 SKIP 판정용으로
																		//   표시 (2026-09-04 최정우 추가)
	bool							bImplausibleDirection;				// depth 탐색 재구성 경로(직전 신뢰 위치→이번 확정 위치)의 전체 진행방향이 현재 heading
																		//   과 크게 어긋난 경우 — 시간·거리는 타당해도 실제 주행 동작(회전 등)으로 보기 어려움
																		//   (MM_DIR_MAX_DEG 재사용, MapMatch.cpp). bImplausiblePath 와 동일하게 SKIP 판정용
																		//   (2026-09-04 최정우 추가)
	bool							bImplausibleSpeedLimit;			// depth 탐색 재구성 경로 구간(hop)별 등록 제한속도 기준 최소 소요시간 합이 실제
																		//   경과시간(dfGapSec)보다 큰 경우 — 평균속도(bImplausiblePath)는 상한 이내여도
																		//   경로 중 유독 느린 구간(좁은 길 등) 하나만으로 물리적으로 불가능함을 잡아낸다
																		//   (MM_PATH_SPEEDLIMIT_MARGIN 재사용, MapMatch.cpp). bImplausiblePath 와 동일하게
																		//   SKIP 판정용 (2026-09-04 최정우 추가)
	// 직전 확정 링크 다음부터 이번 확정 링크까지 실제 경유한 링크 ID 목록(최소 1개=qwLinkID) —
	//   게이트/구역 판정이 qwLinkID 하나만이 아니라 경유 링크 전부를 확인하게 함. 이 구조체가
	//   다른 곳에서 memset(0, MATCH_LINK_INFO_SIZE) 로 리셋되는 관례라 vector 대신 고정 배열
	//   사용(POD 유지) — nSearchStep 상한(maxstep+MM_STEP_EXTEND_MAX, 실무상 5 이하)보다
	//   충분히 큰 8칸 (2026-08-20 최정우 추가)
	uint64							aqwPathLinkIDs[MATCH_LINK_INFO_MAX_PATH];
	uint8							nPathLinkCount;						// aqwPathLinkIDs 유효 개수(0=경로 정보 없음 — qwLinkID 만 사용)
} MATCH_LINK_INFO, *PMATCH_LINK_INFO;

#define MATCH_LINK_INFO_SIZE											sizeof(MATCH_LINK_INFO)

#endif //__MESSAGETYPE_H__
