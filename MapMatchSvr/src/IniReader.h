/**
 * @file IniReader.h
 * @brief ini 파일 읽기 클래스 헤더 파일
 */
#ifndef __INI_READER_H__
#define __INI_READER_H__

#include <stdio.h>
#include <string>
#include <map>
#include <algorithm>
#include <stdexcept>													// 2025-12-04 최정우 헤더 추가
#include "TypeDefine.h"
#include "Util.h"

/**
 * @brief 디렉토리 및 파일 경로 구분자
 * @remark 2022-03-22 최정우 추가
 */
#define DIR_DELIMITER																'/'

/** 
 * @brief ini 파일 한 줄 최대 버퍼
 * @remark 2022-03-22 최정우 추가
 */
#define MAX_LINE_BUFF																512

using namespace std;

/**
 * @class CIniReader
 * @brief ini 파일 읽기 클래스
 */
class CIniReader
{
public:
	CIniReader();
	CIniReader(const string strPath, const string strFile);
	CIniReader(const string strFile);
	virtual ~CIniReader();

	bool Open();
	bool GetProfileStr(const string strSection, const string strKey, const string strDefault, string &strValue);
	bool GetProfileInt(const string strSection, const string strKey, const int nDefault, int &nValue);
	bool GetProfileFloat(const string strSection, const string strKey, const float fDefault, float &fValue);
	bool GetProfileDouble(const string strSection, const string strKey, const double dfDefault, double &dfValue);
	bool GetProfileArrayStr(const string strSection, const string strKey, string *pstrValue, int &nCount);

private:
	bool ReadIniFile();
	bool ReadKeyValue(char *pszBuf, char *pszKey, char *pszVal);

private:
	CUtil							m_cUtil;

private:
	string							m_strPath;
	string							m_strFile;
	string							m_strFullName;
	FILE							*m_fp;
	map<string, void *>				m_mapSection;
};

#endif	//__INI_READER_H__