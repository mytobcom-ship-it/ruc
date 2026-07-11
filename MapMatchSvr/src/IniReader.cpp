/**
 * @file IniReader.cpp
 * @brief ini 파일 읽기 클래스 소스 파일
 */
#include "IniReader.h"
#include "DataDefine.h"

/**
 * @brief 생성자
*/
CIniReader::CIniReader() : 
	m_strPath(""), 
	m_strFile(""), 
	m_fp(nullptr)
{
	m_mapSection.clear();
}

/**
 * @brief 생성자
 * @param[in] strPath 경로
 * @param[in] strFile 파일명
*/
CIniReader::CIniReader(const string strPath, const string strFile) : 
	m_strPath(strPath), 
	m_strFile(strFile), 
	m_fp(nullptr)
{
	m_strFullName = m_strPath + DIR_DELIMITER + m_strFile;
	m_mapSection.clear();
}

/**
 * @brief 생성자
 * @param[in] strFile 경로 포함 파일명
*/
CIniReader::CIniReader(const string strFile) : 
	m_strFullName(strFile), 
	m_fp(nullptr)
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
	}							// if (nIndex == string::npos)

	m_mapSection.clear();
}

/**
 * @brief 소멸자
*/
CIniReader::~CIniReader()
{
	map<string, void *>::iterator it;
	for (it=m_mapSection.begin(); it!=m_mapSection.end(); ++it)
	{
		multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(it->second);
		mapKey->clear();
		if (mapKey != nullptr) delete mapKey;
		mapKey = nullptr;
	}							// for (it=m_mapSection.begin(); it!=m_mapSection.end(); ++it)

	m_mapSection.clear();
}

/**
 * @brief ini 파일 읽기
 * @return true, false
*/
bool CIniReader::Open()
{
	// 파일명이 없거나 최대 길이 초과시 예외 처리 추가 (2025-12-04 최정우 추가)
	if ((m_strFullName.empty()) || 
		(m_strFullName.length() > static_cast<std::size_t>(MAX_PATH)))
	{
		LOGFMTE("file path is invalid!path=[%s]", m_strFullName.c_str());
		return false;
	}							// if ((m_strFullName.empty()) || 
								//	   (m_strFullName.length() > static_cast<std::size_t>(PATH_MAX)))

	// 파일이 존재하는지 확인 처리 (2025-12-04 최정우 추가)
	if (access(m_strFullName.c_str(), F_OK) != 0)
	{
		LOGFMTE("file is not found!path=[%s]", m_strFullName.c_str());
		return false;
	}							// if (access(m_strFullName.c_str(), F_OK) != 0)

	m_fp = fopen(m_strFullName.c_str(), "r");
	if (!m_fp)
	{
		perror("ini file open failed!\n");
		return false;
	}							// if (!m_fp)

	if (!ReadIniFile())
	{
		perror("ini file read failed!\n");
		fclose(m_fp);
		return false;
	}							// if (!ReadIniFile())

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
	char *pszPos, *pszToken;

	// 초기화 (2025-12-04 최정우 추가)
	memset(szLine, 0, static_cast<size_t>(MAX_LINE_BUFF));
	memset(szToken, 0, static_cast<size_t>(MAX_LINE_BUFF));
	memset(szValue, 0, static_cast<size_t>(MAX_LINE_BUFF));

	multimap<string, string> *pmapCurrentSection = nullptr;

	while (fgets(szLine, static_cast<int>(MAX_LINE_BUFF), m_fp))
	{
		// ignore whitespace
		pszPos = szLine;

		// 문자열 종료가 아닌지 확인 코드 추가 (2025-12-04 최정우 보완)
		while ((*pszPos != '\0') && (isspace(static_cast<unsigned char>(*pszPos))))
			pszPos++;

		// Check Comment/Section/Key
		if ((*pszPos == '\0') || (*pszPos == '\r') || (*pszPos == '\n') || (*pszPos == '#') || (*pszPos == ';'))
			continue;
		else if (*pszPos == '[')										// Section
		{
			pszPos++;
			pszToken = szToken;
			while (*pszPos && *pszPos != ']')
				*pszToken++ = *pszPos++;
			*pszToken = '\0';

			pmapCurrentSection = new (std::nothrow)multimap<string, string>;
			if (pmapCurrentSection == nullptr)
				return false;

			string strToken(szToken);
			transform(strToken.begin(), strToken.end(), strToken.begin(), ::toupper);
			m_mapSection.insert(pair<string, void *>(strToken, pmapCurrentSection));
		}
		else															// Key
		{
			if (pmapCurrentSection == nullptr) continue;

			if (!ReadKeyValue(pszPos, szToken, szValue)) continue;

			string strToken(szToken);
			transform(strToken.begin(), strToken.end(), strToken.begin(), ::toupper);
			pmapCurrentSection->insert(pair<string, string>(strToken, szValue));
		}						// if ((*pszPos == '\0') || (*pszPos == '\r') || (*pszPos == '\n') || (*pszPos == '#') || (*pszPos == ';'))
	}							// while (fgets(szLine, static_cast<int>(MAX_LINE_BUFF), m_fp))

	return true;
}

/**
 * @brief ini 파일에서 Key/Value 로 분리
 * @param[in] pszBuf ini 파일에서 읽은 key/value 한쌍
 * @param[out] pszKey Key 값
 * @param[out] pszVal Value 값
 * @return true, false
 */
bool CIniReader::ReadKeyValue(char *pszBuf, char *pszKey, char *pszVal)
{
	char *pszPos;

	pszPos = pszKey;
	while ((*pszBuf != '\0') && (*pszBuf != '='))
		*pszPos++ = *pszBuf++;

	// = 가 없는 경우
	if (*pszBuf == '\0') return false;

	// Key 우측 공백 무시
	*pszPos = '\0';
	pszPos--;
	while ((pszPos >= pszKey) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))
	{
		*pszPos = '\0';
		pszPos--;
	}							// while ((pszPos >= pszKey) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))

	// Value
	pszPos = pszVal;
	pszBuf++;
	while ((*pszBuf == ' ') || (*pszBuf == '\t'))
		pszBuf++;

	while (*pszBuf != '\0')
		*pszPos++ = *pszBuf++;

	// Value 우측 공백 무시
	*pszPos = '\0';
	pszPos--;
	while ((pszPos >= pszVal) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))
	{
		*pszPos = '\0';
		pszPos--;
	}							// while ((pszPos >= pszVal) && ((*pszPos == ' ') || (*pszPos == '\t') || (*pszPos == '\r') || (*pszPos == '\n')))

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
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		strValue = strDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
	if (mapKey == nullptr) return false;

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		strValue = strDefault;
		return false;
	}							// if (itKey == mapKey->end())

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
bool CIniReader::GetProfileInt(const string strSection, const string strKey, const int nDefault, int &nValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		nValue = nDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
	if (mapKey == nullptr) return false;

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		nValue = nDefault;
		return false;
	}							// if (itKey == mapKey->end())

	// 숫자열인지 검사
	if (!m_cUtil.Isdigit(itKey->second))
	{
		nValue = nDefault;
		return false;
	}							// if (!m_cUtil.Isdigit(itKey->second))

	// 예외 처리 추가 (2025-12-04 최정우 추가)
	try
	{
		std::size_t ztPos = 0;
		nValue = std::stoi(itKey->second, &ztPos);

		// 값 전체를 int 형으로 변환하지 못한 경우 (2025-12-04 최정우 추가)
		if (ztPos != itKey->second.length())
		{
			nValue = nDefault;
			return false;
		}						// if (ztPos != itKey->second.length())
	}
	catch (const std::exception &e)
	{
		nValue = nDefault;
		return false;
	}							// try

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 실수형 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] fDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] fValue Section, Key 에 해당하는 숫자형 값
 * @return true, false
 * @remark 2024-01-04 최정우 추가
 */
bool CIniReader::GetProfileFloat(const string strSection, const string strKey, const float fDefault, float &fValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		fValue = fDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
	if (mapKey == nullptr) return false;

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		fValue = fDefault;
		return false;
	}							// if (itKey == mapKey->end())

	// 실수인지 검사 (2024-01-19 최정우 추가)
	if (!m_cUtil.Isdecimal(itKey->second))
	{
		fValue = fDefault;
		return false;
	}							// if (!m_cUtil.Isdecimal(itKey->second))

	// 예외 처리 추가 (2025-12-04 최정우 추가)
	try
	{
		std::size_t ztPos = 0;
		fValue = static_cast<float>(std::stod(itKey->second, &ztPos));

		// 값 전체를 float 형으로 변환하지 못한 경우 (2025-12-04 최정우 추가)
		if (ztPos != itKey->second.length())
		{
			fValue = fDefault;
			return false;
		}						// if (ztPos != itKey->second.length())
	}
	catch (const std::exception &e)
	{
		fValue = fDefault;
		return false;
	}							// try

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 실수형 값
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[in] dfDefault Section, Key 에 해당하는 값이 없을 경우 기본 값
 * @param[out] dfValue Section, Key 에 해당하는 숫자형 값
 * @return true, false
 * @remark 2024-01-04 최정우 추가
 */
bool CIniReader::GetProfileDouble(const string strSection, const string strKey, const double dfDefault, double &dfValue)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		dfValue = dfDefault;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
	if (mapKey == nullptr) return false;

	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	multimap<string, string>::iterator itKey = mapKey->find(strTmp);
	if (itKey == mapKey->end())
	{
		dfValue = dfDefault;
		return false;
	}							// if (itKey == mapKey->end())

	// 실수인지 검사 (2024-01-19 최정우 추가)
	if (!m_cUtil.Isdecimal(itKey->second))
	{
		dfValue = dfDefault;
		return false;
	}							// if (!m_cUtil.Isdecimal(itKey->second))

	// 예외 처리 추가 (2025-12-04 최정우 추가)
	try
	{
		std::size_t ztPos = 0;
		dfValue = std::stod(itKey->second, &ztPos);

		// 값 전체를 double 형으로 변환하지 못한 경우 (2025-12-04 최정우 추가)
		if (ztPos != itKey->second.length())
		{
			dfValue = dfDefault;
			return false;
		}						// if (ztPos != itKey->second.length())
	}
	catch (const std::exception &e)
	{
		dfValue = dfDefault;
		return false;
	}							// try

	return true;
}

/**
 * @brief ini 파일에서 Section, Key 에 해당하는 값 목록
 * @param[in] strSection Section 명
 * @param[in] strKey Key Key 명
 * @param[out] pstrValue Section, Key 에 해당하는 값 목록
 * @param[out] nCount Section, Key 에 해당하는 값 목록 갯수
 * @return true, false
 */
bool CIniReader::GetProfileArrayStr(const string strSection, const string strKey, string *pstrValue, int& nCount)
{
	string strTmp = strSection;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	map<string, void *>::iterator itSection = m_mapSection.find(strTmp);
	if (itSection == m_mapSection.end())
	{
		nCount = 0;
		return false;
	}							// if (itSection == m_mapSection.end())

	multimap<string, string>::iterator itKey;
	pair<multimap<string, string>::iterator, multimap<string, string>::iterator> range;
	multimap<string, string> *mapKey = reinterpret_cast<multimap<string, string> *>(itSection->second);
	if (mapKey == nullptr) return false;

	int nCnt = 0;
	strTmp = strKey;
	transform(strTmp.begin(), strTmp.end(), strTmp.begin(), ::toupper);
	range = mapKey->equal_range(strTmp);
	for (itKey=range.first; itKey!=range.second; ++itKey)
	{
		if (nCnt >= nCount) break;

		pstrValue[nCnt] = itKey->second;
		nCnt++;
	}							// for (itKey=range.first; itKey!=range.second; ++itKey)
	nCount = nCnt;

	return true;
}
