/**
 * @file ChargeDataLoader.h
 * @brief 과금 게이트 정보 로딩 클래스 헤더 파일
*/
#ifndef __CHARGEDATALOADER_H__
#define __CHARGEDATALOADER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "TypeDefine.h"
#include "DataFormat.h"
#include "PostgrePool.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

#pragma pack(push, 1)

/**
 * @struct sGateInfo
 * @brief base_tollgate 1행에 대응하는 게이트 정보
 * @remark
 *   - dwSearchLon/dwSearchLat : CCoordConvert::WGS84ToSearchCoord 로 미리 변환해둔 내부 스케일 좌표
 *     (LINK_INFO.dwStNodeX/Y 와 동일한 사전변환 방식) — GetGateNearby 매 호출마다 재변환하지 않기 위함
 *     (2026-08-12 최정우 추가)
*/
typedef struct sGateInfo
{
	char							szTollgateID[20+1];					// 게이트 ID (예: TG00009)
	char							szRoadID[20+1];							// 소속 구역 ID (base_roadlink.road_id)
	char							szGateDiv[1+1];							// 게이트 구분 M/I/O/B
	double							dfLon;									// 경도(WGS84, 원본 도)
	double							dfLat;									// 위도(WGS84, 원본 도)
	uint32							dwSearchLon;							// 경도(내부 스케일, WGS84ToSearchCoord 결과)
	uint32							dwSearchLat;							// 위도(내부 스케일, WGS84ToSearchCoord 결과)
	uint64							qwLinkID;								// 매칭 링크 ID (0=없음)

	sGateInfo() :
		dfLon(0.0),
		dfLat(0.0),
		dwSearchLon(0),
		dwSearchLat(0),
		qwLinkID(0)
	{
		memset(szTollgateID, 0, sizeof(szTollgateID));
		memset(szRoadID, 0, sizeof(szRoadID));
		memset(szGateDiv, 0, sizeof(szGateDiv));
	}
} GATE_INFO, *PGATE_INFO;

#define GATE_INFO_SIZE					sizeof(GATE_INFO)
// vector — 폐쇄형/구간단속 진입·출구 게이트가 같은 link_id 를 공유하는 실사례가 있어(TG00007/08,
//   TG00012/13 확인됨) link_id 당 게이트 1개로 가정한 단순 map 은 later 로 덮여써서 데이터 유실됨.
//   반드시 vector 로 전부 보관하고, gate_div 로 걸러서 원하는 쪽을 골라야 함 (2026-08-12 최정우 수정)
typedef unordered_map<uint64, vector<GATE_INFO>>	mapGateInfo;

#pragma pack(pop)

// ZONE_INFO는 string 멤버(가변길이 jsonb 원문)가 있어 GATE_INFO와 달리 1바이트 패킹 대상이
// 아님 — GATE_INFO/LINK_INFO 계열은 바이너리(psf) 레이아웃과 맞출 필요가 있어 패킹했지만,
// ZONE_INFO는 DB 조회 결과를 필드별로 채우는 구조라 애초에 패킹이 불필요함 (2026-08-12 최정우 추가)
/**
 * @struct sZoneInfo
 * @brief base_roadlink 1행에 대응하는 구역 정보 (2026-08-12 최정우 추가)
 * @remark
 *   - 개방형은 road_nm(zone_name)만 당장 사용. link_ids/coords는 원본 jsonb 텍스트 그대로
 *     보관만 해두고, 실제 파싱(멤버십 조회·PointInPolygon)은 폐쇄형/구간단속/주정차 구역
 *     진입·이탈 판정 구현 시점에 처리 — 이 코드베이스에 JSON 파서가 아직 없어 지금은 미파싱
*/
typedef struct sZoneInfo
{
	char							szRoadID[20+1];							// 구역 ID (base_roadlink.road_id)
	char							szRoadKind[2+1];						// 유형 0~4(일반/개방/폐쇄/구간단속/주정차)
	char							szRoadNm[100+1];						// 구역명 — 개방형 zone_name 으로 사용
	char							szGeomType[4+1];						// LINE/POLY
	double							dfSpeedLimitKmh;						// 제한속도(구간단속용, 해당없음=0)
	char							szUseYN[1+1];
	string							strLinkIdsJson;							// link_ids 원본(jsonb 텍스트, 미파싱 TODO)
	string							strCoordsJson;							// coords 원본(jsonb 텍스트, 미파싱 TODO)

	sZoneInfo() :
		dfSpeedLimitKmh(0.0)
	{
		memset(szRoadID, 0, sizeof(szRoadID));
		memset(szRoadKind, 0, sizeof(szRoadKind));
		memset(szRoadNm, 0, sizeof(szRoadNm));
		memset(szGeomType, 0, sizeof(szGeomType));
		memset(szUseYN, 0, sizeof(szUseYN));
	}
} ZONE_INFO, *PZONE_INFO;

#define ZONE_INFO_SIZE					sizeof(ZONE_INFO)
typedef unordered_map<string, ZONE_INFO>	mapZoneInfo;

/**
 * @class CChargeDataLoader
 * @brief base_tollgate 전량을 메모리로 로드하고 조회를 제공하는 클래스
 * @remark
 *   - CDataLoader(link.psf 로딩)와 동일하게 "로드 1회 → 이후 인메모리 조회" 철학을 따름
 *   - link_id 우선 O(1) 조회, 실패 시 좌표거리(gate_radius) 폴백 — 게이트 판정(MatchGate)에서 사용
 *   - LoadGates()를 기동 시 1회 호출(현재), 또는 주기 재조회 스레드에서 반복 호출(config.ini
 *     [charge] gate_reload, 향후) 하는 두 경우 모두 대응 — 매번 새 맵을 만들어 std::swap 으로
 *     교체해 조회 스레드와의 락 경합을 최소화(doc/README.txt §F 패턴) (2026-08-12 최정우 추가)
*/
class CChargeDataLoader
{
public:
	CChargeDataLoader();
	virtual ~CChargeDataLoader();

	bool Initialize(CPostgrePool *pcPostgrePool, const string& strGateSelectSQL,
		const string& strZoneSelectSQL);
	void Uninitialize();
	bool LoadGates();
	bool LoadZones();

	PGATE_INFO GetGateByLinkId(const uint64 qwLinkID, const char cGateDiv = 0);
	PGATE_INFO GetGateNearby(const double dfLat, const double dfLon, const double dfGateRadiusM);
	PGATE_INFO GetGateByRoadId(const string& strRoadID, const char cGateDiv);
	PZONE_INFO GetZoneByRoadId(const string& strRoadID);

	inline const bool IsLoad() const { return m_bLoad; }
	size_t GetGateCount() const;
	inline const size_t GetZoneCount() const { return m_mapZoneInfo.size(); }

private:
	CPostgrePool					*m_pcPostgrePool;						// 커넥션 풀(비소유 — Server 가 생성한 것 공유)
	string							m_strGateSelectSQL;						// base_tollgate 전량 조회 SQL(query.sql 세션 로드 결과)
	string							m_strZoneSelectSQL;						// base_roadlink 전량 조회 SQL (2026-08-12 최정우 추가)
	mapGateInfo						m_mapGateInfo;							// link_id → 게이트 정보
	mapZoneInfo						m_mapZoneInfo;							// road_id → 구역 정보 (2026-08-12 최정우 추가)
	bool							m_bLoad;
};

#endif //__CHARGEDATALOADER_H__
