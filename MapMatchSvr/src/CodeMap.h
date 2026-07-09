/**
 * @file CodeMap.h
 * @brief 코드 테이블별 값 구하기 클래스 헤더 파일
*/
#ifndef __CODEMAP_H__
#define __CODEMAP_H__

#include <stdio.h>
#include <string.h>
#include "TypeDefine.h"

/**
 * @struct sCodeEntry
 * @brief 코드 테이블
*/
typedef struct sCodeEntry
{
	int								nCode;						// 코드
	const char						*pszValue;					// 코드 값
} CODE_ENTRY, *PCODE_ENTRY;

#define NOE(x)													(int)(sizeof(x) / sizeof(CODE_ENTRY))
#define INVALID_CODE											-1

/**
 * @class CCodeMap
 * @brief 코드 테이블별 값 구하기 클래스
*/
class CCodeMap
{
public:
	CCodeMap();
	virtual ~CCodeMap();

	int GetCode(PCODE_ENTRY pstCodeEntry, int nEntryCount, char *pszValue);
	const char *GetValue(PCODE_ENTRY pstCodeEntry, int nEntryCount, int nCode);
};

#endif	//__CODEMAP_H__
