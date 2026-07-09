/**
 * @file PostgrePool.cpp
 * @brief Postgre DB 연결 관리 클래스 소스 파일
*/
#include "PostgrePool.h"

/**
 * @brief DB 연결 확인 쓰레드
 * @param[in] arg CPostgrePool 클래스 포인터
 * @return void
*/
void *CPostgrePool::KeepAliveThread(void *arg)
{
	CPostgrePool *pcPostgrePool = reinterpret_cast<CPostgrePool *>(arg);
	if (!pcPostgrePool) return nullptr;

	pcPostgrePool->keepPoolAlive();
	return nullptr;
}

/**
 * @brief 생성자
*/
CPostgrePool::CPostgrePool()
{
	m_dqQueue.clear();
	m_nPooledConnections = 0;
	m_bIsValid = false;
}

/**
 * @brief 소멸자
*/
CPostgrePool::~CPostgrePool()
{
	UninitializePool();
}

/**
 * @brief DB 연결 및 초기화
 * @param[in] strUserID 접속 아이디
 * @param[in] strPassword 접속 비밀번호
 * @param[in] strDataBase 데이터베이스
 * @param[in] strHost 접속 아이피
 * @param[in] nPort 접속 포트
 * @param[in] nMinConnect 최소 접속 수
 * @param[in] nMaxConnect 최대 접속 수
 * @param[in] nTimeOut 데이터베이스 연결 검사 (초)
 * @return true, false
*/
bool CPostgrePool::InitializePool(string strUserID, string strPassword, string strDataBase, 
		string strHost, int nPort, int nMinConnect, int nMaxConnect, int nTimeOut)
{
	m_strUserID = strUserID;
	m_strPassword = strPassword;
	m_strDataBase = strDataBase;
	m_strHost = strHost;
	m_strPort = std::to_string(nPort);
	m_nMinConnect = nMinConnect;
	m_nMaxConnect = nMaxConnect;
	m_nTimeOut = nTimeOut;

	if (m_nMinConnect < 1) m_nMinConnect = 1;
	if (m_nMaxConnect < m_nMinConnect) m_nMaxConnect = m_nMinConnect;
	if (m_nTimeOut <= 0) m_nTimeOut = 60;

	for (int i=0; i<m_nMinConnect; ++i)
	{
		if (createConnection() == nullptr)
		{
			UninitializePool();
			return false;
		}
	}

	if (pthread_create(&m_hThread, nullptr, KeepAliveThread, reinterpret_cast<void *>(this)) < 0)
	{
		UninitializePool();
		return false;
	}

	m_bIsValid = true;
	return true;
}

/**
 * @brief DB 연결 종료 및 메모리 반환
 * @return void
*/
void CPostgrePool::UninitializePool()
{
	int left = m_nPooledConnections;

	if (m_bIsValid)
	{
		m_cMutex.lock();
		pthread_cancel(m_hThread);
		if (pthread_join(m_hThread, nullptr) < 0)
			LOGFMTE("Can not join pthread for PostgreSQL Connection Pool!");
		m_cMutex.unlock();
	}

	while (left > 0)
	{
		m_cMutex.lock();
		while (static_cast<int>(m_dqQueue.size()) > 0)
		{
			freeConnection(m_dqQueue.front());
			m_dqQueue.pop_front();
		}
		left = m_nPooledConnections;
		m_cMutex.unlock();
	}

	m_dqQueue.clear();
	m_bIsValid = false;
	LOGFMTI("pogstgre connection pool is uninitialize!");
}

/**
 * @brief DB 연결
 * @return DB 연결 핸들 값, nullptr 
*/
PGconn *CPostgrePool::createConnection()
{
	PGconn *pcHandle = PQsetdbLogin(m_strHost.c_str(), m_strPort.c_str(), nullptr, nullptr, m_strDataBase.c_str(), m_strUserID.c_str(), m_strPassword.c_str());

	if ((PQstatus(pcHandle) != CONNECTION_OK) && (PQsetnonblocking(pcHandle, 1) != 0))
	{
		LOGFMTE("DB connection fail!error=[%s]", PQerrorMessage(pcHandle));
		return nullptr;
	}

	m_dqQueue.push_back(pcHandle);
	m_nPooledConnections++;
	return pcHandle;
}

/**
 * @brief DB 연결 종료
 * @param[in] pcHandle DB 연결 핸들 값
 * @return void
*/
void CPostgrePool::freeConnection(PGconn *pcHandle)
{
	PQfinish(pcHandle);
	pcHandle = nullptr;
	if (m_nPooledConnections > 0) m_nPooledConnections--;
}

/**
 * @brief DB 연결 세션 얻기
 * @return DB 연결 핸들 값
*/
PGconn *CPostgrePool::getConnection()
{
	PGconn *pcHandle = nullptr;

	m_cMutex.lock();
	if (!m_bIsValid)
	{
		m_cMutex.unlock();
		LOGFMTE("db connection pool is invalid!");
		return nullptr;
	}

	while (((static_cast<int>(m_dqQueue.size())) == 0) && 
		(m_nPooledConnections >= m_nMaxConnect))
	{
		m_cCondition.wait(m_cMutex);
	}

	if (static_cast<int>(m_dqQueue.size()) > 0)
		pcHandle = m_dqQueue.front();
	else if (m_nPooledConnections < m_nMaxConnect)
	{
		pcHandle = createConnection();
		if (pcHandle == nullptr)
		{
			m_cMutex.unlock();
			return nullptr;
		}
	}

	m_dqQueue.pop_front();
	m_cMutex.unlock();
	return pcHandle;
}

/**
 * @brief DB 연결 세션 반환
 * @param[in] pcHandle DB 연결 핸들값
 * @return void
*/
void CPostgrePool::releaseConnection(PGconn *pcHandle)
{
	if (!pcHandle)
	{
		m_cMutex.lock();
		if (m_nPooledConnections > 0) m_nPooledConnections--;
		m_cMutex.unlock();
		return;
	}

	m_cMutex.lock();
	m_dqQueue.push_back(pcHandle);
	m_cCondition.signal();
	m_cMutex.unlock();
}

/**
 * @brief 사용 가능한 연결 세션 수 구하기
 * @return 사용 가능한 연결 세션 수
*/
int CPostgrePool::getAvailableConnections()
{
	m_cMutex.lock();
	int nCount = m_nMaxConnect - m_nPooledConnections;
	if (nCount < 0) nCount = 0;
	m_cMutex.unlock();

	return nCount;
}

/**
 * @brief 사용 중인 연결 세션 수 구하기
 * @return 사용 중인 연결 세션 수
*/
int CPostgrePool::getActiveConnections()
{
	m_cMutex.lock();
	int nCount = m_nPooledConnections - static_cast<int>(m_dqQueue.size());
	if (nCount < 0) nCount = 0;
	m_cMutex.unlock();

	return nCount;
}

/**
 * @brief 사용 가능한 연결 세션 수 구하기
 * @return 사용 가능한 연결 세션 수
*/
int CPostgrePool::getPooledConnections()
{
	m_cMutex.lock();
	int nCount = m_nPooledConnections;
	m_cMutex.unlock();

	return nCount;
}

/**
 * @brief DB 연결 확인
 * @return void
*/
void CPostgrePool::keepPoolAlive()
{
	sleep(m_nTimeOut);
	while (true)
	{
		m_cMutex.lock();

		while (static_cast<int>(m_dqQueue.size()) > m_nMinConnect)
		{
			freeConnection(m_dqQueue.front());
			m_dqQueue.pop_front();
		}

		for (deque<PGconn *>::iterator it=m_dqQueue.begin(); it!=m_dqQueue.end(); )
		{
			if (!pingConnection(*it))
				it = m_dqQueue.erase(it);
			else ++it;
		}

		while (m_nPooledConnections < m_nMinConnect)
		{
			if (createConnection() == nullptr)
				break;
		}

		m_cMutex.unlock();
		sleep(m_nTimeOut);
	}
}

/**
 * @brief DB 연결 상태 확인
 * @param[in] pcHandle DB 연결 핸들 값
 * @return true, false
*/
bool CPostgrePool::pingConnection(PGconn *pcHandle)
{
	string strSQL = "select 0";

	PGresult *res = PQexec(pcHandle, strSQL.c_str());
	if (!res)
	{
		LOGFMTE("postgre command failed!error=[%s]", PQerrorMessage(pcHandle));
		return false;
	}

	if ((PQresultStatus(res) == PGRES_TUPLES_OK) && PQntuples(res))
	{
		PQgetvalue(res, 0, 0);
		PQclear(res);
	}
	else
	{
		PQclear(res);
		LOGFMTE("postgre health check failed!error=[%s]", PQerrorMessage(pcHandle));
		return false;
	}

	return true;
}
