/**
 * @file ThreadPool.h
 * @brief Thread Pool 관리용 클래스 헤더 파일
*/
#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <stdio.h>
#include <list>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "MessageType.h"
#include "Queue.h"
#include "Mutex.h"
#include "Condition.h"
#include "Thread.h"

using namespace std;

/**
 * @enum EWORKER_STATE
 * @brief 작업 Thread 상태
*/
enum EWORKER_STATE
{
	EWS_UNKNOWN						= 0,								// 작업 Thread 가 알수 없는 상태
	EWS_WAITING,														// 작업 Thread 가 대기 중인 상태
	EWS_ACTIVE,															// 작업 Thread 가 활성화된 상태
	EWS_STOPPED															// 작업 Thread 가 멈춘 상태
};

/**
 * @class CThreadPoolWorker : public virtual Runnable
 * @brief 작업용 쓰레드 클래스
*/
class CThreadPoolWorker : public virtual Runnable
{
public:
	CThreadPoolWorker();
	virtual void run(int nThreadId, void *context);
	virtual void stop(int nThreadId, void *context);
	enum EWORKER_STATE GetState() { return m_nState; }

private:
	volatile bool					m_bStopped;
	enum EWORKER_STATE				m_nState;
};

/**
 * @class CThreadPool
 * @brief 쓰레드 관리 클래스
*/
class CThreadPool
{
public:
	CThreadPool(int nMaxThreads, Runnable *pcRunnable, bool bDetatch = true);
	virtual ~CThreadPool();
	int GetMaxThreads();
	void Enqueue(int nThreadId, const RAW_LOG_BATCH &vtRawLog);
	int GetQueueCount(int nThreadId = -1);
	int GetWaitingThreads();
	int GetActiveThreads();
	int GetStoppedThreads();
	// #8: 워커 종료 요청 후 idle 대기·큐 잔여 batch 추출
	void RequestShutdown();
	bool WaitForIdle(int nMaxWaitMs);
	void DrainQueuedBatches(vector<RAW_LOG_BATCH> *pvtBatches);

private:
	Runnable						*m_pcRunnable;						// 작업 쓰레드 클래스
	CQueue<RAW_LOG_BATCH>			*m_paQueues;						// 워커별 고정 큐
	CMutex							*m_paMutex;							// 워커별 대기 mutex
	CCondition						*m_paCondition;						// 워커별 signal

private:
	friend class 					CThreadPoolWorker;

	struct ThreadPoolContext
	{
		CThreadPoolWorker  *worker;
		CThread *thread;
	};

	list<ThreadPoolContext>			m_lstThreadPool;					// 쓰레드 목록
	int								m_nMaxThreads;						// 쓰레드 개수
	bool							m_bDetatch;							// detach 여부
};

#endif //__THREADPOOL_H__
