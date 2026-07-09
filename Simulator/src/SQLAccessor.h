/**
 * @file SQLAccessor.h
 * @brief SQL 문 파일 읽기 클래스 헤더 파일
*/
#ifndef __SQLACCESSOR_H__
#define __SQLACCESSOR_H__

#include <stdio.h>
#include <unistd.h>
#include <string>
#include <map>
#include "TypeDefine.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @class CSQLAccessor
 * @brief SQL 읽기 클래스
*/
class CSQLAccessor
{
public:
	CSQLAccessor();
	~CSQLAccessor();

	bool Initialize(string strSQLFile);
	void Uninitialize();

	string GetSQL(string key);

private:
	bool Load();

private:
	string m_SQLFile;
	map<string, string> m_mapSQLEntries;
};

#endif //__SQLACCESSOR_H__
