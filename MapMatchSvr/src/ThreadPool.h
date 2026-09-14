/**
 * @file ThreadPool.h
 * @brief 스레드 풀 관리용 클래스 헤더 파일
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
#include "log4z.h"

using namespace std;
using namespace zsummer::log4z;

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
	// #8: 워커 종료 요청 후 유휴 대기·큐 잔여 batch 추출
	void RequestShutdown();
	bool WaitForIdle(int nMaxWaitMs);
	bool WaitForActiveIdle(int nMaxWaitMs);
	// [버그 수정, 2026-09-11 최정우] 소멸자(delete) 전에 호출 — 전체 워커가 실제로 run() 을 빠져나가
	//   EWS_STOPPED 에 도달했는지 확인용. false 반환 시 호출측(CServer)은 ThreadPool 뿐 아니라
	//   그 하위에서 참조되는 RawLogWorker/ProcessManager/DataLoader/ChargeDataLoader/PostgrePool
	//   delete 도 함께 건너뛰어야 한다(아직 실행 중인 detach 워커의 use-after-free 방지).
	bool WaitForAllStopped(int nMaxWaitMs);
	void DrainQueuedBatches(vector<RAW_LOG_BATCH> *pvtBatches);

private:
	Runnable						*m_pcRunnable;						// 작업 쓰레드 클래스
	CQueue<RAW_LOG_BATCH>			*m_paQueues;						// 워커별 고정 큐
	CMutex							*m_paMutex;							// 워커별 대기 mutex
	CCondition						*m_paCondition;						// 워커별 시그널

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
