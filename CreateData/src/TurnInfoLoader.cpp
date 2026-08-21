/**
 * @file TurnInfoLoader.cpp
 * @brief MOCT TURNINFO.dbf 회전 제한 정보 로더 소스 파일
*/
#include "TurnInfoLoader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "log4z.h"

using namespace zsummer::log4z;

namespace {

static uint16 ReadLE16U(const unsigned char *p)
{
	return static_cast<uint16>(static_cast<uint16>(p[0]) | (static_cast<uint16>(p[1]) << 8));
}

static uint32 ReadLE32U(const unsigned char *p)
{
	return (static_cast<uint32>(p[3]) << 24) | (static_cast<uint32>(p[2]) << 16) |
		(static_cast<uint32>(p[1]) << 8) | static_cast<uint32>(p[0]);
}

static string TrimField(const char *p, size_t nLen)
{
	size_t nStart = 0;
	size_t nEnd = nLen;
	while (nStart < nEnd && (p[nStart] == ' ' || p[nStart] == '\0' || p[nStart] == '\t'))
		++nStart;
	while (nEnd > nStart && (p[nEnd - 1] == ' ' || p[nEnd - 1] == '\0' || p[nEnd - 1] == '\t'))
		--nEnd;
	return string(p + nStart, nEnd - nStart);
}

static uint64 ParseLinkID(const string& strValue)
{
	if (strValue.empty())
		return 0;
	return static_cast<uint64>(strtoull(strValue.c_str(), nullptr, 10));
}

} // namespace

/**
 * @brief 생성자
*/
CTurnInfoLoader::CTurnInfoLoader() :
	m_dwRecordCount(0),
	m_dwRestrictedCount(0),
	m_dwTimedCount(0)
{
}

/**
 * @brief 소멸자
*/
CTurnInfoLoader::~CTurnInfoLoader()
{
	m_mapTurnRuleList.clear();
	m_mapAllowedOutList.clear();
}

/**
 * @brief 진입·진출 링크 쌍 키 생성
*/
uint64 CTurnInfoLoader::MakeKey(const uint64& qwInLinkID, const uint64& qwOutLinkID)
{
	return qwInLinkID * 10000000000ULL + qwOutLinkID;
}

/**
 * @brief MOCT TURN_TYPE 문자열 → 숫자 코드
*/
uint16 CTurnInfoLoader::ParseTurnType(const string& strTurnType)
{
	if (strTurnType.empty())
		return TURN_TYPE_UNKNOWN;
	return static_cast<uint16>(strtoul(strTurnType.c_str(), nullptr, 10));
}

/**
 * @brief MOCT TURN_OPER 문자열 → 숫자 코드
*/
uint8 CTurnInfoLoader::ParseTurnOper(const string& strTurnOper)
{
	if (strTurnOper.empty())
		return TURN_OPER_ALLDAY;
	return static_cast<uint8>(strtoul(strTurnOper.c_str(), nullptr, 10));
}

/**
 * @brief TURNINFO.dbf 로드
 * @param[in] strDbfPath TURNINFO.dbf 경로
 * @return true(성공), false(실패)
*/
bool CTurnInfoLoader::Load(const string& strDbfPath)
{
	m_mapTurnRuleList.clear();
	m_mapAllowedOutList.clear();
	m_dwRecordCount = 0;
	m_dwRestrictedCount = 0;
	m_dwTimedCount = 0;

	FILE *fp = fopen(strDbfPath.c_str(), "rb");
	if (fp == nullptr)
	{
		LOGFMTE("turninfo dbf open failed!file=[%s]", strDbfPath.c_str());
		return false;
	}

	unsigned char szHeader[32];
	if (fread(szHeader, 1, 32, fp) != 32)
	{
		fclose(fp);
		LOGFMTE("turninfo dbf header read failed!file=[%s]", strDbfPath.c_str());
		return false;
	}

	uint32 dwRecordCount = ReadLE32U(szHeader + 4);
	uint16 wHeaderLen = ReadLE16U(szHeader + 8);
	uint16 wRecordSize = ReadLE16U(szHeader + 10);

	struct DbfFieldDef
	{
		string strName;
		uint8 nLength;
	};

	vector<DbfFieldDef> vtFields;
	fseek(fp, 32, SEEK_SET);
	while (true)
	{
		unsigned char szField[32];
		if (fread(szField, 1, 32, fp) != 32)
			break;
		if (szField[0] == 0x0D)
			break;

		DbfFieldDef stField;
		stField.strName = TrimField(reinterpret_cast<char *>(szField), 11);
		stField.nLength = szField[16];
		vtFields.push_back(stField);
	}

	int nStLinkIdx = -1;
	int nEdLinkIdx = -1;
	int nTurnTypeIdx = -1;
	int nTurnOperIdx = -1;
	for (size_t i=0; i<vtFields.size(); ++i)
	{
		if (vtFields[i].strName == "ST_LINK") nStLinkIdx = static_cast<int>(i);
		else if (vtFields[i].strName == "ED_LINK") nEdLinkIdx = static_cast<int>(i);
		else if (vtFields[i].strName == "TURN_TYPE") nTurnTypeIdx = static_cast<int>(i);
		else if (vtFields[i].strName == "TURN_OPER") nTurnOperIdx = static_cast<int>(i);
	}

	if (nStLinkIdx < 0 || nEdLinkIdx < 0 || nTurnTypeIdx < 0 || nTurnOperIdx < 0)
	{
		fclose(fp);
		LOGFMTE("turninfo dbf required field missing!file=[%s]", strDbfPath.c_str());
		return false;
	}

	vector<char> vtRecord(wRecordSize);
	fseek(fp, wHeaderLen, SEEK_SET);
	for (uint32 i=0; i<dwRecordCount; ++i)
	{
		if (fread(&vtRecord[0], 1, wRecordSize, fp) != wRecordSize)
			break;
		if (vtRecord[0] == '*')
			continue;

		size_t nPos = 1;
		vector<string> vtValues;
		for (size_t f=0; f<vtFields.size(); ++f)
		{
			vtValues.push_back(TrimField(&vtRecord[nPos], vtFields[f].nLength));
			nPos += vtFields[f].nLength;
		}

		uint64 qwInLinkID = ParseLinkID(vtValues[static_cast<size_t>(nStLinkIdx)]);
		uint64 qwOutLinkID = ParseLinkID(vtValues[static_cast<size_t>(nEdLinkIdx)]);
		if (qwInLinkID == 0 || qwOutLinkID == 0)
			continue;

		TURN_RULE stRule;
		stRule.nTurnType = ParseTurnType(vtValues[static_cast<size_t>(nTurnTypeIdx)]);
		stRule.nTurnOper = ParseTurnOper(vtValues[static_cast<size_t>(nTurnOperIdx)]);

		uint64 qwKey = MakeKey(qwInLinkID, qwOutLinkID);
		m_mapTurnRuleList[qwKey] = stRule;
		// 허용 유형이면 진입링크별 목록에도 넣어 둔다 — BinaryMaker 가 위상으로 이어지지
		//   않는 회전(교차로가 여러 노드로 나뉜 경우)을 보완할 때 쓴다 (2026-08-22 최정우 추가)
		if (!IsProhibitedType(stRule.nTurnType))
			m_mapAllowedOutList[qwInLinkID].push_back(qwOutLinkID);
		++m_dwRecordCount;
		// 금지 집계는 TURN_TYPE 기준 — TURN_OPER 는 전일제/시간제 운영 구분일 뿐이다
		//   (2026-08-22 최정우 수정 — 기존엔 TURN_OPER==1 을 세어 전국 13건만 잡혔다)
		if (IsProhibitedType(stRule.nTurnType))
			++m_dwRestrictedCount;
		if (stRule.nTurnOper == TURN_OPER_TIMED)
			++m_dwTimedCount;
	}

	fclose(fp);

	LOGFMTI("turninfo dbf loaded!file=[%s], records=[%u], prohibited=[%u], timed=[%u]",
		strDbfPath.c_str(), m_dwRecordCount, m_dwRestrictedCount, m_dwTimedCount);
	return (m_dwRecordCount > 0);
}

/**
 * @brief MOCT TURN_TYPE 이 금지 유형인가
 * @param[in] nTurnType MOCT TURN_TYPE (011 은 11 로 저장됨)
 * @return true(금지), false(허용 유형 또는 미등록)
 * @remark
 * 	금지: 003 회전금지, 101 좌회전금지, 102 직진금지, 103 우회전금지
 * 	허용: 001 비보호회전, 002 버스만회전, 011 U-TURN, 012 P-TURN
 * 	(2026-08-22 최정우 추가)
*/
bool CTurnInfoLoader::IsProhibitedType(const uint16 nTurnType)
{
	switch (nTurnType)
	{
	case TURN_TYPE_NOTURN:			// 003 회전금지
	case TURN_TYPE_NOLEFT:			// 101 좌회전금지
	case TURN_TYPE_NOSTRAIGHT:		// 102 직진금지
	case TURN_TYPE_NORIGHT:			// 103 우회전금지
		return true;
	default:
		return false;
	}
}

/**
 * @brief 회전 제한(금지) 여부
 * @remark
 * 	MOCT 규격상 금지 여부는 TURN_TYPE 이 담는다 — 003 회전금지, 101 좌회전금지,
 * 	102 직진금지, 103 우회전금지. 나머지(001 비보호회전, 002 버스만회전, 011 U-TURN,
 * 	012 P-TURN)는 허용 유형이므로 제외하지 않는다.
 *
 * 	TURN_OPER 는 전일제(0)/시간제(1) 운영 구분일 뿐 허용·금지 플래그가 아니므로
 * 	제외 판정에 쓰지 않는다. 시간제 금지도 시간대를 알 수 없는 동안은 상시 금지로
 * 	취급한다(TURNINFO.dbf 의 REMARK 가 전량 공백이라 운영 시간대 복원 불가).
 *
 * 	2026-08-22 최정우 수정 — 기존 구현은 TURN_OPER==1 만 제한으로 봐서 전국 44,218건 중
 * 	13건만 제외하고 금지회전 16,372건이 전부 통과하고 있었다. 규격서(ITS 표준 노드·링크
 * 	구축·운영지침 24쪽)와 전국 회전각 실측으로 해석을 확정했다.
*/
bool CTurnInfoLoader::IsRestricted(const uint64& qwInLinkID, const uint64& qwOutLinkID) const
{
	unordered_map<uint64, TURN_RULE>::const_iterator it =
		m_mapTurnRuleList.find(MakeKey(qwInLinkID, qwOutLinkID));
	if (it == m_mapTurnRuleList.end())
		return false;

	return IsProhibitedType(it->second.nTurnType);
}


/**
 * @brief 회전 규칙 조회
*/
bool CTurnInfoLoader::GetRule(const uint64& qwInLinkID, const uint64& qwOutLinkID, TURN_RULE& stRule) const
{
	unordered_map<uint64, TURN_RULE>::const_iterator it =
		m_mapTurnRuleList.find(MakeKey(qwInLinkID, qwOutLinkID));
	if (it == m_mapTurnRuleList.end())
		return false;
	stRule = it->second;
	return true;
}

/**
 * @brief 진입 링크에서 TURNINFO 가 회전 가능으로 명시한 진출 링크 목록
 * @param[in] qwInLinkID 진입 링크 ID
 * @return 진출 링크 ID 벡터 (없으면 nullptr)
 * @remark
 * 	금지 유형(003/101/102/103)은 제외돼 있다. (2026-08-22 최정우 추가)
*/
const vector<uint64>* CTurnInfoLoader::GetAllowedOutLinks(const uint64& qwInLinkID) const
{
	unordered_map<uint64, vector<uint64> >::const_iterator it = m_mapAllowedOutList.find(qwInLinkID);
	if (it == m_mapAllowedOutList.end())
		return nullptr;
	return &(it->second);
}
