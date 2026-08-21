/**
 * @file TurnInfoLoader.h
 * @brief MOCT TURNINFO.dbf 회전 제한 정보 로더 헤더 파일
*/
#ifndef __TURNINFOLOADER_H__
#define __TURNINFOLOADER_H__

#include <string>
#include <unordered_map>
#include <vector>
#include "TypeDefine.h"
#include "DataFormat.h"

using namespace std;

/**
 * @struct sTurnRule
 * @brief 진입·진출 링크 쌍별 MOCT 회전 규칙
*/
typedef struct sTurnRule
{
	uint16							nTurnType;							// MOCT TURN_TYPE
	uint8							nTurnOper;							// MOCT TURN_OPER (0:전일제, 1:시간제)
} TURN_RULE, *PTURN_RULE;

/**
 * @class CTurnInfoLoader
 * @brief TURNINFO.dbf 로더 (회전 제한 필터용)
*/
class CTurnInfoLoader
{
public:
	CTurnInfoLoader();
	virtual ~CTurnInfoLoader();

	bool Load(const string& strDbfPath);
	bool IsRestricted(const uint64& qwInLinkID, const uint64& qwOutLinkID) const;
	static bool IsProhibitedType(const uint16 nTurnType);
	// 진입 링크에서 TURNINFO 가 "회전 가능"으로 명시한 진출 링크 목록 (허용 유형만)
	//   위상(노드ID) 으로는 이어지지 않지만 원본이 회전 가능하다고 기록한 쌍을
	//   회전 후보로 보완하기 위한 인덱스 (2026-08-22 최정우 추가)
	const vector<uint64>* GetAllowedOutLinks(const uint64& qwInLinkID) const;
	bool GetRule(const uint64& qwInLinkID, const uint64& qwOutLinkID, TURN_RULE& stRule) const;

	inline uint32 GetRecordCount() const { return m_dwRecordCount; }
	inline uint32 GetRestrictedCount() const { return m_dwRestrictedCount; }
	inline uint32 GetTimedCount() const { return m_dwTimedCount; }

private:
	static uint64 MakeKey(const uint64& qwInLinkID, const uint64& qwOutLinkID);
	static uint16 ParseTurnType(const string& strTurnType);
	static uint8 ParseTurnOper(const string& strTurnOper);

private:
	unordered_map<uint64, TURN_RULE>	m_mapTurnRuleList;
	unordered_map<uint64, vector<uint64> >	m_mapAllowedOutList;	// 진입링크 → 허용 진출링크들
	uint32								m_dwRecordCount;
	uint32								m_dwRestrictedCount;		// TURN_TYPE 기준 금지 건수
	uint32								m_dwTimedCount;				// TURN_OPER 시간제 건수
};

#endif //__TURNINFOLOADER_H__
