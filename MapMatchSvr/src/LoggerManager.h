/**
 * @file LoggerManager.h
 * @brief 로그 관리 클래스 헤더 파일
*/
#ifndef __LOGGER_MANAGER_H__
#define __LOGGER_MANAGER_H__

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @class CLoggerManager
 * @brief 로그 관리 클래스
*/
class CLoggerManager
{
public:
	CLoggerManager();
	~CLoggerManager();

	bool Initialize(const string strLogPath, 
		const int nLogKeepRunTime, const int nLogKeepDay);
	void LogDeleteRun(time_t dtNow);

private:
	bool SetRemoveLogFile(time_t dtRmTime, string strLogPath);

private:
	string							m_strLogPath;						// 로그 경로
	int								m_nLogKeepRunTime;					// 로그 삭제 시간 설정
	int								m_nLogKeepDay;						// 로그 보관일
	bool							m_bRun;								// 실행 가능 여부
	time_t							m_dtTime;							// 삭제일 설정 
};

#endif // #ifndef __LOGGER_MANAGER_H__
