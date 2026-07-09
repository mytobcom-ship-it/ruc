/**
 * @file IniReader.h
 * @brief ini 파일 읽기 클래스 헤더 파일
*/
#ifndef __INIREADER_H__
#define __INIREADER_H__

#include <stdio.h>
#include <string>
#include <map>
#include <algorithm>
#include "TypeDefine.h"

#define DIR_DELIMITER			'/'							// 디렉토리 및 파일 경로 구분자
#define MAX_LINE_BUFF			512							// ini 파일 한 줄 최대 버퍼

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
	bool GetProfileStr(const string strSection, const string strKey, const string strDefault, string& strValue);
	bool GetProfileInt(const string strSection, const string strKey, const int nDefault, int& nValue);
	bool GetProfileArrayStr(const string strSection, const string strKey, string *strValue, int& nCount);

private:
	bool ReadIniFile();
	bool ReadKeyValue(char *pBuf, char *pKey, char *pVal);

private:
	string						m_strPath;
	string						m_strFile;
	string						m_strFullName;
	FILE						*m_fp;

	map<string, void *>			m_mapSection;
};

#endif	//__INIREADER_H__
