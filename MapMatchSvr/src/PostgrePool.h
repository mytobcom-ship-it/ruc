/**
 * @file PostgrePool.h
 * @brief Postgre DB 커넥션 풀 클래스 헤더 파일
*/
#ifndef __POSTGREPOOL_H__
#define __POSTGREPOOL_H__

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <deque>
#include <libpq-fe.h>
#include "TypeDefine.h"
#include "Mutex.h"
#include "Condition.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @class CPostgrePool
 * @brief Postgre DB 연결 관리 클래스
*/
class CPostgrePool
{
public:
	CPostgrePool();
	virtual ~CPostgrePool();

	bool InitializePool(string strUserID, string strPassword, string strDataBase, 
		string strHost, int nPort = 5432, int nMinConnect = 3, int nMaxConnect = 5, int nTimeOut = 60);
	void UninitializePool();
	PGconn *getConnection();
	void releaseConnection(PGconn *pcHandle);
	int getAvailableConnections();
	int getActiveConnections();
	int getPooledConnections();

private:
	PGconn *createConnection();
	void freeConnection(PGconn *pcHandle);
	bool pingConnection(PGconn *pcHandle);
	void keepPoolAlive();
	static void *KeepAliveThread(void *arg);

private:
	CMutex							m_cMutex;
	CCondition						m_cCondition;
	deque<PGconn *>					m_dqQueue;

private:
	string							m_strUserID;
	string							m_strPassword;
	string							m_strDataBase;
	string							m_strHost;
	string							m_strPort;
	int								m_nMinConnect;
	int								m_nMaxConnect;
	int								m_nTimeOut;

	pthread_t						m_hThread;
	int								m_nPooledConnections;
	bool							m_bIsValid;
	// [2026-09-21 최정우 추가] UninitializePool() 중복 실행 가드. m_bIsValid 로는 대신할 수 없다 —
	//   그 값은 정리 **도중에도** false 이기 때문에(회수 루프 전에 먼저 내린다) "이미 정리를
	//   마쳤는가" 를 구분하지 못한다. CServer::m_bUninitialized 와 같은 패턴이다.
	bool							m_bUninitialized;
};

#endif	//__POSTGREPOOL_H__
