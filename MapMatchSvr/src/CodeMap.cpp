/**
 * @file CodeMap.cpp
 * @brief 코드 테이블별 값 구하기 클래스 소스 파일
*/
#include "CodeMap.h"

/**
 * @brief 생성자
*/
CCodeMap::CCodeMap()
{
}

/**
 * @brief 소멸자
*/
CCodeMap::~CCodeMap()
{
}

/**
 * @brief 메시지에 해당하는 코드
 * @param[in] pstCodeEntry 코드 테이블 구조체
 * @param[in] nEntryCount 코드 등록 갯수
 * @param[in] pszValue 코드 해당 값
 * @return 코드
*/
int CCodeMap::GetCode(PCODE_ENTRY pstCodeEntry, int nEntryCount, char *pszValue)
{
	for (int i=0; i<nEntryCount; ++i)
	{
		if (strcmp(pstCodeEntry[i].pszValue, pszValue) == 0)
			return pstCodeEntry[i].nCode;
	}

	return INVALID_CODE;
}

/**
 * @brief 코드에 해당하는 메시지
 * @param[in] pstCodeEntry 코드 테이블 구조체
 * @param[in] nEntryCount 코드 등록 갯수
 * @param[in] nCode 코드
 * @return 메시지
*/
const char *CCodeMap::GetValue(PCODE_ENTRY pstCodeEntry, int nEntryCount, int nCode)
{
	for (int i=0; i<nEntryCount; ++i)
	{
		if (pstCodeEntry[i].nCode == nCode)
			return pstCodeEntry[i].pszValue;
	}

	return nullptr;
}
