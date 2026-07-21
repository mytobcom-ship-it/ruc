/**
 * @file DataDefine.h
 * @brief 각 데이터 정의 헤더 파일
*/
#ifndef __DATADEFINE_H__
#define __DATADEFINE_H__

#include <string.h>
#include "TypeDefine.h"

#define UNUSE_LOG_KEEP				-1									// 로그 관리 사용하지 않음
#define MAX_PATH					255									// 경로 최대 길이

// 초기화 코드
#define NO_ANGLE					-1									// 방위각 정보 없음
#define NO_RADIUS					-1									// 검색 반경 입력값 없음
#define NO_SPEED					-1									// 제한속도 입력값없음
#define NO_ACCURACY					-1									// 수평 오차(ACCURACY_M) 미적용
#define NO_ALTITUDE					-1									// 고도(ALTITUDE_M) 미적용
#define NO_PERIOD					-1									// GPS 입력 주기(초) 없음

/**
 * @enum eLinkRoadType
 * @brief ROAD_TYPE — LINK_INFO.nRoadType (uint8, DBF 숫자 그대로)
 * @remark MOCT_LINK.ROAD_TYPE: 0=일반, 1=교량, 2=터널, 3=고가, 4=지하
*/
enum eLinkRoadType : uint8
{
	ROAD_TYPE_NORMAL				= 0,								// 000 일반도로
	ROAD_TYPE_BRIDGE				= 1,								// 001 교량
	ROAD_TYPE_TUNNEL				= 2,								// 002 터널
	ROAD_TYPE_ELEVATED				= 3,								// 003 고가차도
	ROAD_TYPE_UNDERGROUND			= 4									// 004 지하차도
};

/**
 * @enum LinkConnectCode
 * @brief CONNECT — LINK_INFO 의 nConnect (uint8, DBF 숫자 그대로)
 * @remark LINK_INFO 의 nConnect 0=아님, 1=연결로. 101~108=구 링크 등급별 연결로(레거시)
*/
enum eLinkConnectCode : uint8
{
	LINK_CONNECT_NONE				= 0,								// 연결로 아님
	LINK_CONNECT_YES				= 1,								// MOCT 연결로
	LINK_CONNECT_EXPRESS			= 101,								// 고속도로 연결로 (레거시)
	LINK_CONNECT_URBAN				= 102,								// 도시고속도로 연결로 (레거시)
	LINK_CONNECT_NATIONAL			= 103,								// 일반국도 연결로 (레거시)
	LINK_CONNECT_METRO				= 104,								// 특별·광역시도 연결로 (레거시)
	LINK_CONNECT_NATIONAL_LOCAL		= 105,								// 국가지원지방도 연결로 (레거시)
	LINK_CONNECT_LOCAL				= 106,								// 지방도 연결로 (레거시)
	LINK_CONNECT_CITY				= 107,								// 시·군도 연결로 (레거시)
	LINK_CONNECT_ETC				= 108								// 기타도로 연결로 (레거시)
};

/**
 * @enum eLinkRoadRank
 * @brief ROAD_RANK — LINK_INFO 의 nRoadRank (uint8, DBF 숫자 그대로)
 * @remark 101~108=도로 등급. 0=미설정(생성자 초기값, MOCT 컬럼 값 아님)
*/
enum eLinkRoadRank : uint8
{
	LINK_ROAD_RANK_NONE				= 0,								// 미설정 (초기값)
	LINK_ROAD_RANK_EXPRESS			= 101,								// 101 고속도로
	LINK_ROAD_RANK_URBAN			= 102,								// 102 도시고속화도로
	LINK_ROAD_RANK_NATIONAL			= 103,								// 103 일반국도
	LINK_ROAD_RANK_METRO			= 104,								// 104 특별·광역시도
	LINK_ROAD_RANK_NATIONAL_LOCAL	= 105,								// 105 국가지원지방도
	LINK_ROAD_RANK_LOCAL			= 106,								// 106 지방도
	LINK_ROAD_RANK_CITY				= 107,								// 107 시·군도
	LINK_ROAD_RANK_ETC				= 108								// 108 기타
};

/**
 * @enum eLinkNodeType
 * @brief NODE_TYPE — MOCT_NODE.NODE_TYPE → LINK_INFO.nStNodeType/nEdNodeType (uint8)
 * @remark 0=미설정(초기값). 101~107=MOCT 노드 유형
*/
enum eLinkNodeType : uint8
{
	NODE_TYPE_NONE					= 0,								// 미설정 (초기값)
	NODE_TYPE_CROSSROAD				= 101,								// 교차로
	NODE_TYPE_JC					= 102,								// JC (분기점)
	NODE_TYPE_SA					= 103,								// SA/휴게소
	NODE_TYPE_IC					= 104,								// IC (나들목)
	NODE_TYPE_TG					= 105,								// TG (톨게이트)
	NODE_TYPE_ETC					= 106,								// 기타
	NODE_TYPE_CITY					= 107								// 시·군도 노드 등
};

/**
 * @struct sAltitudeScoreConfig
 * @brief 연속 맵매칭 고도(ALTITUDE_M) 보조 점수 설정 — config [mapmatch] alt_gap/alt_penalty/alt_weight/alt_slope
 * @remark Begin 맵매칭 미적용. alt_weight=0 이면 전체 비활성.
 *
 * 고도 보조 비용(음수=유리·양수=불리)은 기존 dfCost(INTERSECT_LEN+방위각)에 가산.
 *   |Δalt| ≤ alt_gap:
 *     · 같은 ROAD_TYPE  → −alt_penalty (alt_penalty 가 양수인 설정 기준 — 부호는 그대로 뒤집어 적용)
 *     · 호환 ROAD_TYPE  → 0  (고가↔교량)
 *     · 불일치          → +alt_penalty
 *   |Δalt| > alt_gap:
 *     · alt_weight × (|Δalt| − alt_gap) + 방향 패널티
 *     · 상승(Δ>차이) 시 지하 후보 +alt_penalty / 하강(Δ<−차이) 시 고가·교량 후보 +alt_penalty
 *   |Δalt|/수평거리 > alt_slope → 고도 신호 무시(0)
 *
 * 예) 차이=8, alt_penalty=10, 가중치=0.5 / 직전·현재 100m·106m, 같은 고가
 *     → Δ=6 ≤ 8 → 고도 보조 −10m (후보 우선)
*/
typedef struct sAltitudeScoreConfig
{
	sint16							nGap;								// config alt_gap — 직전 매칭 고도와 허용 차이(m)
	sint16							nAltPenalty;						// config alt_penalty — 양수=ROAD_TYPE 불일치 비용 가산·음수=같은 ROAD_TYPE 비용 감산(m)
	double							dfWeight;							// config alt_weight — 차이 초과 시 고도차 가중. 0=비활성
	double							dfSlope;							// config alt_slope — |Δ고도|/수평거리 상한. 초과 시 고도 무시

	sAltitudeScoreConfig() :
		nGap(8),
		nAltPenalty(10),
		dfWeight(0.5),
		dfSlope(0.12)
	{}
} ALTITUDE_SCORE_CONFIG, *PALTITUDE_SCORE_CONFIG;

// 에러 코드
#define NO_ERROR					0									// 오류 없음
#define INVALID_COORDTYPE			1									// 측지계 코드 오류
#define INVALID_COORDINATE			2									// 좌표 오류
#define INVALID_ANGLE				3									// 방위각 오류
#define INVALID_ROADTYPE			4									// 도로등급 오류
#define INVALID_SEARCHRADIUS		5									// 검색 반경 오류
#define INVALID_LINKID				6									// LinkID 오류
#define INVALID_RESULTCOUNT			7									// 응답 개수 오류
#define INVALID_SEARCHSTEP			8									// 탐색 단계 오류
#define NOT_FOUND_GRIDINFO			9									// GRID 정보 검색 실패
#define NOT_FOUND_LINKID			10									// LinkID 검색 실패
#define MAP_MATCH_FAIL				11									// 맵매칭 실패

// 맵매칭 소프트 비용(방위각 가중) 파라미터 — ±45° 하드컷 대체 (2026-07-08 최정우 추가)
#define MM_DIR_WEIGHT				1.0									// 방위각 1도당 비용(m 환산) 가중치(w_a). 거리(m)+w_a·|방위각차|
#define MM_DIR_MAX_DEG				120									// 방위각 차 하드 상한(초과 후보 배제, 역방향 오매칭 방지)

// 방위각 비용의 최대 상쇄 한도(m) — 이보다 더 가까운 후보는 방위각이 아무리 잘 맞아도 역전 불가 (2026-07-18 최정우 추가)
//   목적: dfCost=거리+w_a·|각도차| 합산 시, 각도차가 커도(하드컷 120° 이내) 비용이 무한정 커져 실거리가
//   훨씬 먼 후보가 선택되는 것을 방지. 예) 5m/100°(cap 미적용 시 cost 105) vs 40m/5° 후보에서,
//   cap=15 적용 시 5m 후보 cost=20 으로 40m 후보(cost 45)를 이김 → 근접 우위가 보존됨.
#define MM_DIR_MAX_PENALTY		15.0								// (단위: m) 방위각 비용 상한
#define MM_SPEED_LOW_KMH			5									// 이하: 저속 → 방위각 불신(w_a=0)
#define MM_SPEED_HIGH_KMH			20									// 이상: 방위각 가중치 최대(w_a=MM_DIR_WEIGHT)

// HEADING/SPEED 가 DB NULL 일 때 직전 매칭좌표로 계산하기 위한 임계 (2026-07-08 최정우 추가)
#define MM_CALC_MAX_GAP_SEC			10									// 직전 매칭점과의 시간간격(초) 초과 시 계산 불신 → 미적용
#define MM_CALC_MIN_DIST			2.0									// (단위: m) 이동거리 미만이면 방위각 노이즈 → 방위각 계산 미적용

// 정식 매칭 실패(반경 밖)·정확도 SKIP 시 최근접 세그먼트 진단 탐색 반경(m) (2026-07-10 최정우 추가)
//   좌표·INTERSECT_LEN(GPS↔세그먼트 교차점 거리)를 참고용으로 남기기 위한 최대 탐색 반경.
#define MM_DIAG_RADIUS				250									// (단위: m) 반경 밖 최근접 후보 탐색 반경(진단용, 방위각 무시)

// bReverseSuspect(연속역행 스트릭용 신호) 판정 시, 이보다 작은 위치 후퇴는 부동소수점 오차로
//   보고 무시 — margin 과 무관하게 항상 적용되는 최소 임계 (2026-07-21 최정우 추가)
#define MM_REVERSE_SUSPECT_EPS		0.1									// (단위: m) 역행 의심 판정 최소 후퇴거리

// 연속 실패 후 Begin 재검색 시, 직전 성공 링크와 "연결(회전 가능)되지 않은" 후보에 주는 비용 페널티(m 상당) (2026-07-15 최정우 추가)
//   목적: 회전·수렴 구간에서 나란한 도로로 튀는 오매칭 억제. 소프트 페널티라 명백히 더 가까운 도로는 그대로 선택됨.
#define MM_CONNECT_PENALTY			30.0								// (단위: m) 직전 성공 링크와 미연결 후보 cost 가산

// 연속 맵매칭 depth0 최적 후보가 링크 경계(시작/끝)에 스냅(클램프)됐는지 판정 허용오차(m) (2026-07-15 최정우 추가)
//   경계 클램프면 차량이 링크 끝을 지난 것 → 연결 다음 링크에 더 나은 내부 수선발이 있을 수 있어 depth 확장
#define MM_CLAMP_EPS				1.0									// (단위: m) 링크 경계 클램프 판정 허용오차

// 최종 확정 후보가 여전히 경계 클램프(depth 확장해도 더 나은 내부 수선발을 못 찾음) 이면서,
//   GPS↔매칭점 거리(INTERSECT_LEN)가 이 값을 넘으면 "신뢰도 낮은 매칭"으로 SKIP 처리 — 여러 GPS_SEQ가
//   같은 꺾임점으로 뭉개져(클램프) MATCH_LAT/LON 이 정지한 것처럼 보이는 오탐(예: 주정차 오판) 방지용
//   (2026-07-21 최정우 추가 — 클램프 저신뢰 SKIP)
#define MM_CLAMP_SKIP_LEN			10.0								// (단위: m) 클램프+거리 초과 시 SKIP 판정 임계

// 같은 링크 위 역행이 "확실한 노이즈"(heading 정방향 확인 + 오차 작음 + 후보 1개)로 판정되면,
//   매칭 좌표가 뒤로 밀린 것처럼 보이지 않게 직전 위치보다 이 거리(m)만큼 앞으로 보정한다.
//   링크 끝(END 노드)을 넘어서면 END 노드 좌표로 클램프 (2026-07-22 최정우 추가)
#define MM_NOISE_FORWARD_NUDGE_M	1.0									// (단위: m) 확실한 노이즈 보정 시 전진량

// (D) 장시간 공백 시 세션 앵커 폐기 → 초기(Begin) 재획득 임계 (2026-07-15 최정우 추가)
//   직전 "매칭 성공" 이후 경과 시간이 이 값을 넘으면 연속성 신뢰 불가 → 세션 리셋
#define MM_SESSION_RESET_GAP_SEC	30									// 직전 매칭 후 gap(초) 초과 시 세션 리셋

// (B) 연속 맵매칭 공백 적응: 직전 매칭점→현재 이동거리 클수록 탐색 depth(maxstep) 확대 (2026-07-15 최정우 추가)
//   차량이 여러 링크 전진했을 수 있어, 이동거리 MM_STEP_EXTEND_DIST 마다 depth +1 (최대 +MM_STEP_EXTEND_MAX)
#define MM_STEP_EXTEND_DIST			50.0								// (단위: m) depth +1 당 이동거리
#define MM_STEP_EXTEND_MAX			3									// 공백 적응 depth 최대 추가량

/**
 * @enum eCoordinateType
 * @brief 측지계 코드
*/
enum eCoordinateType
{
	EPSG3857						= 1,								// 구글지도/빙지도/야후지도/OSM 등에서 사용중인 좌표계
	WGS84GEO,															// 경위도
	KATECH,																// TM128(Transverse Mercator; 횡메카토르) 한국 표준
	BESSELGEO															// GCS Bessel 1841 타원체를 사용
};

/**
 * @struct sMatchEntry
 * @brief 초기 맵매칭 결과
 * @remark
 *	- nRoadRank : LINK_ROAD_RANK_* — MOCT_LINK.ROAD_RANK (101~108)
 *	- nConnect : LINK_CONNECT_* — MOCT 0/1, 구 링크 101~108 (MOCT_LINK.CONNECT → nConnect)
 *	- nRoadType : ROAD_TYPE_* — MOCT_LINK.ROAD_TYPE (0~4)
 *	- nStNodeType/nEdNodeType : NODE_TYPE_* — MOCT_NODE.NODE_TYPE (101~107)
*/
typedef struct sMatchEntry
{
	double							dfMatchX;							// 매핑 X 좌표
	double							dfMatchY;							// 매핑 Y 좌표
	double							dfSgmtMatchLen;						// 세그먼트 시작점부터 교차점까지 거리(m)
	double							dfIntersectLenSgmt;					// GPS 좌표와 세그먼트 교차점까지 거리(m) — DB INTERSECT_LEN
	double							dfCost;								// 소프트 비용 = INTERSECT_LEN(m) + w_a·|방위각차| (링크 선택 기준) (2026-07-08 최정우 추가)
	double							dfAngleCost;						// 방위각 비용(m) — match trace formula용
	double							dfAltAdj;							// 고도 보조 비용(m) — Continue 만, match trace formula용
	double							dfReversePenalty;					// 역행 페널티(m) — match trace formula용 (2026-07-20 최정우 추가)
	bool							bReverseSuspect;					// 위치 역행 + heading 도 역방향 일치 — margin 과 무관, 연속역행(reverse_confirm) 판정 전용 신호 (2026-07-21 최정우 추가)
	bool							bSgmtClamped;						// 세그먼트 끝점(꺾임점) 스냅 — 클램프 저신뢰 SKIP 판정용 (2026-07-21 최정우 추가)
	bool							bHasHeading;						// heading 값 존재 여부 — 같은 링크 역행 판정 시 노이즈/판단불가 구분용 (2026-07-22 최정우 추가)
	bool							bAmbiguousReverse;					// 같은 링크 역행인데 heading 없음/애매해 노이즈 단정 불가 — SKIP 처리용 (2026-07-22 최정우 추가)
	sint16							nDirAngleDiff;						// 주행방향 각도 차이
	uint64							qwLinkID;							// 링크 ID
	uint16							wLenFromLink;						// 링크의 시작점에서 부터 매핑된 세그먼트 시작점까지 거리
	uint8							nMaxSpeed;							// 제한 속도
	double							dfLen;								// 링크 길이
	uint8							nRoadRank;							// 도로 종별[3]
	uint8							nConnect;							// 연결로 코드[3]
	uint8							nRoadType;							// 도로 유형[3]
	uint8							nLanes;								// 차선 정보
	char							szRoadName[46];						// 도로명
	uint64							qwStNodeID;							// 시작 노드 ID
	double							dfStNodeX;							// 시작 노드 X
	double							dfStNodeY;							// 시작 노드 Y
	uint8							nStNodeType;						// 시작 노드 속성
	uint64							qwEdNodeID;							// 종료 노드 ID
	double							dfEdNodeX;							// 종료 노드 X
	double							dfEdNodeY;							// 종료 노드 Y
	uint8							nEdNodeType;						// 종료 노드 속성

	sMatchEntry() : 
		dfMatchX(0.0), 
		dfMatchY(0.0), 
		dfSgmtMatchLen(-1.0), 
		dfIntersectLenSgmt(-1.0), 
		dfCost(-1.0), 
		dfAngleCost(0.0), 
		dfAltAdj(0.0),
		dfReversePenalty(0.0),
		bReverseSuspect(false),
		bSgmtClamped(false),
		bHasHeading(false),
		bAmbiguousReverse(false),
		nDirAngleDiff(0),
		qwLinkID(0),
		wLenFromLink(0),
		nMaxSpeed(0), 
		dfLen(0.0), 
		nRoadRank(LINK_ROAD_RANK_NONE), 
		nConnect(LINK_CONNECT_NONE), 
		nRoadType(ROAD_TYPE_NORMAL), 
		nLanes(0), 
		qwStNodeID(0), 
		dfStNodeX(0.0), 
		dfStNodeY(0.0), 
		nStNodeType(0), 
		qwEdNodeID(0), 
		dfEdNodeX(0.0), 
		dfEdNodeY(0.0), 
		nEdNodeType(0)
	{
		memset(reinterpret_cast<void *>(szRoadName), 0, sizeof(szRoadName));
	}

	bool operator<(const struct sMatchEntry& data) const
	{
		// 2026-07-08 최정우 주석 처리
		//return dfIntersectLenSgmt < data.dfIntersectLenSgmt;

		// 소프트 비용(거리 + 방위각 가중) 최소 우선.
		// 비용 동일 시 INTERSECT_LEN(GPS↔세그먼트 교차점 거리)로 tie-break (2026-07-08 최정우 수정)
		if (dfCost != data.dfCost)
			return dfCost < data.dfCost;
		return dfIntersectLenSgmt < data.dfIntersectLenSgmt;
	}
} MATCH_ENTRY, *PMATCH_ENTRY;

#define MATCH_ENTRY_SIZE												sizeof(MATCH_ENTRY)

#endif //__DATADEFINE_H__
