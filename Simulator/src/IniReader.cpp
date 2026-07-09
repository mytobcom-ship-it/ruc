/**
 * @file IniReader.cpp
 * @brief ini 파일 읽기 클래스 소스 파일
*/
#include "IniReader.h"

/**
 * @brief 생성자
*/
CIniReader::CIniReader()
	: m_strPath(""), m_strFile(""), m_fp(nullptr)
{
	m_mapSection.clear();
}

/**
 * @brief 생성자
 * @param[in] strPath 경로
 * @param[in] strFile 파일명
*/
CIniReader::CIniReader(const string strPath, const string strFile)
	: m_strPath(strPath), m_strFile(strFile), m_fp(nullptr)
{
	m_strFullName = m_strPath + DIR_DELIMITER + m_strFile;
	m_mapSection.clear();
}

/**
 * @brief 생성자
 * @param[in] strFile 경로 포함 파일명
*/
CIniReader::CIniReader(const string strFile)
	: m_strFullName(strFile), m_fp(nullptr)
{
	string::size_type nIndex = strFile.rfind(DIR_DELIMITER);

	if (nIndex == string::npos)
	{
		m_strPath = "";
		m_strFile = strFile;
	}
	else
	{
		m_strPath = strFile.substr(0, nIndex);
		m_strFile = strFile.substr(nIndex, strFile.length());
	}

	m_mapSection.clear();
}

/**
 * @brief 소멸자
*/
CIniReader::~CIniReader()
{
	map<string, void *>::iterator it;
	for (it = m_mapSection.begin(); it != m_mapSection.end(); ++it)
	{
		multimap<string, string> *mapKey = (multimap<string, string> *)it->second;
		mapKey->clear();
		delete mapKey;
	}

	m_mapSection.clear();
}

/**
 * @brief ini 파일 읽기
 * @return true, false
*/
bool CIniReader::Open()
{
	// ini 파일 열기 (2026-07-08 최정우 주석 추가)
	m_fp = fopen(m_strFullName.c_str(), "r");
	if (!m_fp)
	{
		perror("ini file open fail!");
		return false;
	}

	// ini 파일 줄 단위 파싱 (2026-07-08 최정우 주석 추가)
	if (!ReadIniFile())
	{
		perror("ini file read fail!");
		// ini 파일 핸들 닫기 (2026-07-08 최정우 주석 추가)
		fclose(m_fp);
		return false;
	}

	// ini 파일 핸들 닫기 (2026-07-08 최정우 주석 추가)
	fclose(m_fp);

	return true;
}

/**
 * @brief ini 파일 줄 단위로 읽기
 * @return true, false
*/
bool CIniReader::ReadIniFile()
{
	char szLine[MAX_LINE_BUFF];
	char szToken[MAX_LINE_BUFF];
	char szValue[MAX_LINE_BUFF];
	char *pPos, *pToken;

	multimap<string, string> *pCurrentSection = nullptr;

	while (fgets(szLine, MAX_LINE_BUFF, m_fp))
	{
		// ignore whitespace
		pPos = szLine;
		while (isspace(*pPos)) pPos++;

		// Check Comment/Section/Key
		if ((*pPos == '\0') || (*pPos == '\r') || (*pPos == '\n') || (*pPos == '#') || (*pPos == ';'))
			continue;
		else if (*pPos == '[')								// Section
		{
			pPos++;
			pToken = szToken;
			while (*pPos && *pPos != ']')
				*pToken++ = *pPos++;
			*pToken = '\0';

			pCurrentSection = new (std::nothrow)multimap<string, string>;
			if (pCurrentSection == nullptr)
				return false;

			string strToken(szToken);
			transform(strToken.begin(), strToken.end(), strToken.begin(), ::toupper);
			m_mapSection.insert(pair<string, void *>(strToken, pCurrentSection));
		}
		else												// Key
		{
			if (pCurrentSection == nullptr) continue;

			// key=value 한 줄을 Key/Value 로 분리 (2026-07-08 최정우 주석 추가)
			if (!ReadKeyValue(pPos, szToken, szValue)) continue;

			string strToken(szToken);
			transform(strToken.begin(), strToken.end(), strToken.begin(), ::toupper);
			pCurrentSection->insert(pair<string, string>(strToken, szValue));
		}
	}

	return true;
}

/**
 * @brief ini 파일에서 Key/Value 로 분리
 * @param[in] pBuf ini 파일에서 읽은 key/value 한쌍
 * @param[out] pKey Key 값
 * @param[out] pVal Value 값
 * @return true, false
*/
bool CIniReader::ReadKeyValue(char *pBuf, char *pKey, char *pVal)
{
	char *pPos;

	pPos = pKey;
	while ((*pBuf != '\0') && (*pBuf != '='))
		*pPos++ = *pBuf++;

	// = 가 없는 경우
	if (*pBuf == '\0') return false;

	// Key 우측 공백 무시
	*pPos = '\0';
	pPos--;
	while ((pPos >= pKey) && 
		(*pPos == ' ' || *pPos == '\t' || *pPos == '\r' || *pPos == '\n'))
	{
		*pPos = '\0';
		pPos--;
	}

	// Value
	pPos = pVal;
	pBuf++;
	while ((*pBuf == ' ') || (*pBuf == '\t'))
		pBuf++;

	while (*pBuf != '\0')
		*pPos++ = *pBuf++;

	// Value 우측 공백 무시
	*pPos = '\0';
	pPos--;
	while ((pPos >= pVal) && 
		(*pPos == ' ' || *pPos == '\t' || *pPos == '\r' || *pPos == '\n'))
	{
		*pPos = '\0';
		pPos--;
	}

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 문자열 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] strDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] strValue Section, Key 에 해당하는 문자열 값
 * @return true, false
*/
bool CIniReader::GetProfileStr(const string strSection, const string strKey, const string strDefault, string& strValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	// Section 맵에서 섹션 조회 (2026-07-08 최정우 주석 추가)
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		strValue = strDefault;
		return false;
	}

	multimap<string, string> *mapKey = (multimap<string, string> *)itSection->second;
	if (mapKey == nullptr) return false;

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		strValue = strDefault;
		return false;
	}

	strValue = itKey->second;

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 숫자형 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] nDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] nValue Section, Key 에 해당하는 숫자형 값
 * @return true, false
*/
bool CIniReader::GetProfileInt(const string strSection, const string strKey, const int nDefault, int& nValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	// Section 맵에서 섹션 조회 (2026-07-08 최정우 주석 추가)
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		nValue = nDefault;
		return false;
	}

	multimap<string, string> *mapKey = (multimap<string, string> *)itSection->second;
	if (mapKey == nullptr) return false;

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		nValue = nDefault;
		return false;
	}

	nValue = atoi(itKey->second.c_str());

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 값 목록
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[out] strValue Section, Key 에 해당하는 값 목록
 * @param[in,out] nCount Section, Key 에 해당하는 값 목록 갯수
 * @return true, false
*/
bool CIniReader::GetProfileArrayStr(const string strSection, const string strKey, string *strValue, int& nCount)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	// Section 맵에서 섹션 조회 (2026-07-08 최정우 주석 추가)
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		nCount = 0;
		return false;
	}

	multimap<string, string>::iterator itKey;
	pair<multimap<string, string>::iterator, multimap<string, string>::iterator> range;
	multimap<string, string> *mapKey = (multimap<string, string> *)itSection->second;
	if (mapKey == nullptr) return false;

	int nCnt = 0;
	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	range = mapKey->equal_range(strTmp);
	for (itKey = range.first; itKey != range.second; ++itKey)
	{
		if (nCnt >= nCount) break;

		strValue[nCnt] = itKey->second;
		nCnt++;
	}
	nCount = nCnt;

	return true;
}
