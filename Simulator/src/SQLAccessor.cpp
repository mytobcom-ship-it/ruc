/**
 * @file SQLAccessor.cpp
 * @brief SQL 문 파일 읽기 클래스 소스 파일
*/
#include "SQLAccessor.h"

/**
 * @brief 생성자
*/
CSQLAccessor::CSQLAccessor()
{
	m_SQLFile = "";
	m_mapSQLEntries.clear();
}

/**
 * @brief 소멸자
*/
CSQLAccessor::~CSQLAccessor()
{
	Uninitialize();
}

/**
 * @brief 초기화
 * @param[in] strSQLFile SQL 파일명
 * @return true(성공), false(실패)
*/
bool CSQLAccessor::Initialize(string strSQLFile)
{
	m_SQLFile = strSQLFile;
	if (access(m_SQLFile.c_str(), F_OK) != 0)
	{
		LOGFMTE("SQL file not found!file=[%s]", m_SQLFile.c_str());
		return false;
	}

	return Load();
}

/**
 * @brief 메모리 반환
 * @return void
*/
void CSQLAccessor::Uninitialize()
{
	m_mapSQLEntries.clear();
	LOGFMTI("SQL accessor uninitialize!");
}

/**
 * @brief 파일 읽기 및 등록
 * @return true(성공), false(실패)
*/
bool CSQLAccessor::Load()
{
	FILE *fp = fopen(m_SQLFile.c_str(), "r");
	char buf[1024], *ptr, *qtr;
	int read = 0;
	string key = "";
	string sql = "";

	if (fp == nullptr)
	{
		LOGFMTE("[%s] SQL file open fail!", m_SQLFile.c_str());
		return false;
	}

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	if (size <= 0)
	{
		fclose(fp);
		LOGFMTE("[%s] SQL file empty!", m_SQLFile.c_str());
		return false;
	}
	fseek(fp, 0, SEEK_SET);

	while (!feof(fp))
	{
		fgets(buf, sizeof(buf)-1, fp);
		read = (int)strlen(buf);

		if (buf[0] == '#') continue;

		for (; read>0 && isspace(buf[read-1]); read--) buf[read-1] = 0x00;
		if (buf[0] == 0x00) continue;

		buf[read] = ' ';
		buf[read+1] = 0x00;

		for (ptr=buf; isspace(*ptr); ptr++);

		if (buf[0] == '[')
		{
			ptr++;
			for (qtr=ptr; *qtr && *qtr != ']'; qtr++);
			*qtr = 0x00;
			key = ptr;
			sql = "";
		}
		else
		{
			for (qtr=ptr; *qtr && *qtr != ';'; qtr++);
			if (*qtr == ';')
			{
				*qtr = 0x00;
				sql += ptr;

				m_mapSQLEntries.insert(pair<string, string>(key, sql));
				sql = "";
			}
			else
				sql += ptr;
		}
	}

	fclose(fp);
	return true;
}

/**
 * @brief 키 값을 이용하여 SQL 문 읽기
 * @param[in] key SQL 문 키 값
 * @return SQL 문
*/
string CSQLAccessor::GetSQL(string key)
{
	map<string, string>::iterator it;

	it = m_mapSQLEntries.find(key);
	if (it == m_mapSQLEntries.end()) return "";
	return it->second;
}
