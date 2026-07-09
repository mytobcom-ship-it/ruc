/**
 * @file TurnInfoLoader.h
 * @brief MOCT TURNINFO.dbf 회전 제한 정보 로더 헤더 파일
*/
#ifndef __TURNINFOLOADER_H__
#define __TURNINFOLOADER_H__

#include <string>
#include <unordered_map>
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
	uint8							nTurnOper;							// MOCT TURN_OPER (0:허용, 1:제한)
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
	bool GetRule(const uint64& qwInLinkID, const uint64& qwOutLinkID, TURN_RULE& stRule) const;

	inline uint32 GetRecordCount() const { return m_dwRecordCount; }
	inline uint32 GetRestrictedCount() const { return m_dwRestrictedCount; }

private:
	static uint64 MakeKey(const uint64& qwInLinkID, const uint64& qwOutLinkID);
	static uint16 ParseTurnType(const string& strTurnType);
	static uint8 ParseTurnOper(const string& strTurnOper);

private:
	unordered_map<uint64, TURN_RULE>	m_mapTurnRuleList;
	uint32								m_dwRecordCount;
	uint32								m_dwRestrictedCount;
};

#endif //__TURNINFOLOADER_H__
