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
		string strHost, int nPort = 5432,  int nMinConnect = 3, int nMaxconnect = 5, int nTimeOut = 60);
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
};

#endif	//__POSTGREPOOL_H__
