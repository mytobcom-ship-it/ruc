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
	m_dwRestrictedCount(0)
{
}

/**
 * @brief 소멸자
*/
CTurnInfoLoader::~CTurnInfoLoader()
{
	m_mapTurnRuleList.clear();
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
		return TURN_OPER_ALLOW;
	return static_cast<uint8>(strtoul(strTurnOper.c_str(), nullptr, 10));
}

/**
 * @brief TURNINFO.dbf 로드
 * @param[in] strDbfPath TURNINFO.dbf 경로
 * @return true, false
*/
bool CTurnInfoLoader::Load(const string& strDbfPath)
{
	m_mapTurnRuleList.clear();
	m_dwRecordCount = 0;
	m_dwRestrictedCount = 0;

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
		++m_dwRecordCount;
		if (stRule.nTurnOper == TURN_OPER_RESTRICT)
			++m_dwRestrictedCount;
	}

	fclose(fp);

	LOGFMTI("turninfo dbf loaded!file=[%s], records=[%u], restricted=[%u]",
		strDbfPath.c_str(), m_dwRecordCount, m_dwRestrictedCount);
	return (m_dwRecordCount > 0);
}

/**
 * @brief 회전 제한(금지) 여부
*/
bool CTurnInfoLoader::IsRestricted(const uint64& qwInLinkID, const uint64& qwOutLinkID) const
{
	unordered_map<uint64, TURN_RULE>::const_iterator it =
		m_mapTurnRuleList.find(MakeKey(qwInLinkID, qwOutLinkID));
	if (it == m_mapTurnRuleList.end())
		return false;
	return (it->second.nTurnOper == TURN_OPER_RESTRICT);
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
