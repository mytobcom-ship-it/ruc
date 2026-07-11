/**
 * @file LoggerManager.cpp
 * @brief 로그 관리 클래스 소스 파일
*/
#include "LoggerManager.h"

/**
 * @brief 생성자
*/
CLoggerManager::CLoggerManager() : 
	m_nLogKeepRunTime(0), 
	m_nLogKeepDay(0), 
	m_bRun(true)
{
	m_strLogPath.clear();
	m_dtTime = time(nullptr);
}

/**
 * @brief 소멸자
*/
CLoggerManager::~CLoggerManager()
{
}

/**
 * @brief 초기화
 * @param[in] strLogPath 로그 경로
 * @param[in] nLogKeepRunTime 실행 시간
 * @param[in] nLogKeepDay 로그 보관일 (단위 : 날자)
 * @return true(성공), false(실패)
*/
bool CLoggerManager::Initialize(const string strLogPath, 
	const int nLogKeepRunTime, const int nLogKeepDay)
{
	m_strLogPath = strLogPath;
	m_nLogKeepRunTime = (nLogKeepRunTime >= 0) ? nLogKeepRunTime : UNUSE_LOG_KEEP;
	m_nLogKeepDay = nLogKeepDay;
	m_bRun = true;
	m_dtTime = time(nullptr);

	// log path is exist check
	if (m_strLogPath.empty())
	{
		LOGFMTE("log path is empty!path=[%s]", m_strLogPath.c_str());
		return false;
	}

	// log path is exist check
	// 로그 디렉터리 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(m_strLogPath.c_str(), F_OK) != 0)
	{
		LOGFMTE("log path is not exist!path=[%s]", m_strLogPath.c_str());
		return false;
	}

	// runtime check
	if (m_nLogKeepRunTime != UNUSE_LOG_KEEP)
	{
		if ((m_nLogKeepRunTime < 0) || (m_nLogKeepRunTime > 23))
		{
			LOGFMTE("runtime is invalid!runtime=[%d]", m_nLogKeepRunTime);
			return false;
		}

		// log 보관 일수 확인
		if (m_nLogKeepDay <= 0)
		{
			LOGFMTE("log keep day is invalid!day=[%d]", m_nLogKeepDay);
			return false;
		}
	}

	return true;
}

/**
 * @brief 로그 삭제 실행
 * @param[in] dtNow 현재 시각 (초)
 * @return void
*/
void CLoggerManager::LogDeleteRun(time_t dtNow)
{
	struct tm stTm;

	// 실행 안함
	if (m_nLogKeepRunTime <= UNUSE_LOG_KEEP) return;

	// 30초 경과 이전 이면 ...
	if ((dtNow - m_dtTime) <= 30) return;

	m_dtTime = dtNow;
	// 현재 시각을 struct tm 으로 변환 (실행 시각 판별) (2026-07-08 최정우 주석 추가)
	localtime_r(&m_dtTime, &stTm);
	if (stTm.tm_hour != m_nLogKeepRunTime)
	{
		m_bRun = true;
		return;
	}

	// 실행 시간 1 번만 실행 플래그
	if (!m_bRun) return;

	if ((m_bRun) && (stTm.tm_hour == m_nLogKeepRunTime))
	{
		time_t dtRmTime = m_dtTime - (m_nLogKeepDay * 60 * 60 * 24);
		m_bRun = false;
		// 보관일 초과 .log 파일 재귀 삭제 (2026-07-08 최정우 주석 추가)
		SetRemoveLogFile(dtRmTime, m_strLogPath);
	}
}

/**
 * @brief 로그 경로내 파일 목록
 * @param[in] dtRmTime 로그 삭제 파일 시간 (초)
 * @param[in] strLogPath 로그 경로
 * @return true(성공), false(실패)
*/
bool CLoggerManager::SetRemoveLogFile(time_t dtRmTime, string strLogPath)
{
	struct stat stStatInfo;

	// 대상 로그 경로 접근 가능 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(strLogPath.c_str(), F_OK) != 0)
	{
		LOGFMTW("file or directory is not exist!path=[%s]", strLogPath.c_str());
		return false;
	}

	// init
	memset(reinterpret_cast<void *>(&stStatInfo), 0, sizeof(struct stat));

	// 로그 경로가 디렉터리인지 lstat 확인 (2026-07-08 최정우 주석 추가)
	lstat(strLogPath.c_str(), &stStatInfo);
	if (!S_ISDIR(stStatInfo.st_mode))
	{
		LOGFMTW("directory is not found!path=[%s]", strLogPath.c_str());
		return false;
	}

	DIR *pDir = nullptr;
	struct dirent *pstEntry = nullptr;

	// 로그 디렉터리 열거 시작 (2026-07-08 최정우 주석 추가)
	if ((pDir = opendir(strLogPath.c_str())) == nullptr)
	{
		LOGFMTW("directory open failed!path=[%s]", strLogPath.c_str());
		return false;
	}

	while ((pstEntry = readdir(pDir)) != nullptr)
	{
		// init
		memset(reinterpret_cast<void *>(&stStatInfo), 0, sizeof(struct stat));

		string strFilePath = strLogPath + "/" + pstEntry->d_name;
		while (strFilePath.find("//") != string::npos)
			strFilePath.replace(strFilePath.find("//"), 2, "/");

		stat(strFilePath.c_str(), &stStatInfo);
		if (S_ISDIR(stStatInfo.st_mode))						// 디렉토리이면 ...
		{
			if ((strcmp(pstEntry->d_name, "..") == 0) || 
				(strcmp(pstEntry->d_name, ".") == 0))
				continue;

			string strSubDir = strLogPath + "/" + pstEntry->d_name;
			// 하위 디렉터리 재귀 탐색 (2026-07-08 최정우 주석 추가)
			SetRemoveLogFile(dtRmTime, strSubDir);
		}
		else													// 파일이면 ...
		{
			string strExten = ".log";
			if (strFilePath.rfind(strExten) != string::npos)
			{
				if (stStatInfo.st_mtim.tv_sec < dtRmTime)
				{
					// 보관 기한 초과 .log 파일 삭제 (2026-07-08 최정우 주석 추가)
					if (remove(strFilePath.c_str()) != 0)
						LOGFMTW("[%s] log file remove failed!file=[%d : %s]", strFilePath.c_str(), errno, strerror(errno));
					else
						LOGFMTI("[%s] log file remove success!", strFilePath.c_str());
				}
			}
		}
	}

	// 디렉터리 핸들 닫기 (2026-07-08 최정우 주석 추가)
	closedir(pDir);

	return true;
}