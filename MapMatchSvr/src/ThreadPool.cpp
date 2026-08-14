/**
 * @file ThreadPool.cpp
 * @brief 스레드 풀 클래스 소스 파일
*/
#include "ThreadPool.h"

/**
 * @brief 생성자
*/
CThreadPoolWorker::CThreadPoolWorker() : 
	m_bStopped(false), 
	m_nState(EWS_UNKNOWN)
{
}

/**
 * @brief 작업 쓰레드 실행
 * @param[in] nThreadId 쓰레드 아이디
 * @param[in] context 호출 클래스 포인터
 * @return void
*/
void CThreadPoolWorker::run(int nThreadId, void *context)
{
	CThreadPool *pcThreadPool = reinterpret_cast<CThreadPool *>(context);
	RAW_LOG_BATCH vtRawLog;

	while (!m_bStopped)
	{
		// predicate 검사 + 대기 + Dequeue 를 동일 mutex 로 보호 (lost-wakeup 방지)
		pcThreadPool->m_paMutex[nThreadId].lock();
		while ((pcThreadPool->m_paQueues[nThreadId].Count() == 0) && !m_bStopped)
		{
			m_nState = EWS_WAITING;
			pcThreadPool->m_paCondition[nThreadId].wait(pcThreadPool->m_paMutex[nThreadId]);
			m_nState = EWS_UNKNOWN;
		}

		bool bDequeued = pcThreadPool->m_paQueues[nThreadId].Dequeue(vtRawLog);
		pcThreadPool->m_paMutex[nThreadId].unlock();

		if (bDequeued)
		{
			m_nState = EWS_ACTIVE;
			pcThreadPool->m_pcRunnable->run(nThreadId, &vtRawLog);
			m_nState = EWS_UNKNOWN;
		}
	}

	m_nState = EWS_STOPPED;
}

/**
 * @brief 전체 Thread 정지
 * @param[in] nThreadId 쓰레드 아이디
 * @param[in] context 호출 클래스 포인터
 * @return void
*/
void CThreadPoolWorker::stop(int nThreadId, void *context)
{
	CThreadPool *pcThreadPool = reinterpret_cast<CThreadPool *>(context);

	m_bStopped = true;
	for (int i=0; i<pcThreadPool->m_nMaxThreads; ++i)
	{
		// 반드시 해당 워커의 뮤텍스를 잡은 채로 브로드캐스트할 것 — run()의 대기 루프는 predicate
		// 검사(Count()==0 && !m_bStopped)와 wait() 호출을 같은 뮤텍스로 묶어서 하는데, 여기서 락
		// 없이 브로드캐스트하면 "predicate 검사 통과 후 wait() 호출 직전"의 틈에 신호가 지나가버릴
		// 수 있음 — 그 신호는 대기 등록된 스레드가 없어 유실되고, 뒤늦게 불린 wait()는 더 이상 올
		// 신호가 없어 영영 안 깨어남(missed-wakeup, 셧다운 hang). 뮤텍스를 잡고 브로드캐스트하면
		// worker 는 그 순간 반드시 락 밖(진입 전이거나 wait() 안에서 대기 등록된 상태)에 있으므로
		// 신호를 놓치지 않거나, 다음 predicate 재검사에서 m_bStopped=true 를 직접 관측함
		// (2026-08-14 최정우 수정 — "소스상 문제" 검토 중 발견)
		pcThreadPool->m_paMutex[i].lock();
		pcThreadPool->m_paCondition[i].broadcast();
		pcThreadPool->m_paMutex[i].unlock();
	}
}

/**
 * @brief 생성자
 * @param[in] nMaxThreads 쓰레드 갯수
 * @param[in] pcRunnable 작업용 쓰레드 클래스
*/
CThreadPool::CThreadPool(int nMaxThreads, Runnable *pcRunnable, bool bDetatch) : 
	m_pcRunnable(pcRunnable), 
	m_paQueues(nullptr),
	m_paMutex(nullptr),
	m_paCondition(nullptr),
	m_nMaxThreads(nMaxThreads), 
	m_bDetatch(bDetatch)
{
	if (m_nMaxThreads <= 0)
		return;

	m_paQueues = new CQueue<RAW_LOG_BATCH>[m_nMaxThreads];
	m_paMutex = new CMutex[m_nMaxThreads];
	m_paCondition = new CCondition[m_nMaxThreads];

	for (int i=0; i<m_nMaxThreads; i++)
	{
		ThreadPoolContext context;

		context.worker = new CThreadPoolWorker;
		context.thread = new CThread(i, context.worker);
		m_lstThreadPool.push_back(context);

		context.thread->start(reinterpret_cast<CThreadPool *>(this));
		if (m_bDetatch) context.thread->detach();
	}
}

/**
 * @brief 소멸자
*/
CThreadPool::~CThreadPool()
{
	list<ThreadPoolContext>::iterator it;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		(*it).thread->stop();
		(*it).thread->join();
	}

	for (int i=0; i<30 && GetStoppedThreads()<(int)m_lstThreadPool.size(); i++)
	{
		CThread::sleep(100);
	}

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread)
		{
			delete (*it).thread;
			(*it).thread = nullptr;
		}
	}

	m_lstThreadPool.clear();

	if (m_pcRunnable)
	{
		delete m_pcRunnable;
		m_pcRunnable = nullptr;
	}

	if (m_paQueues)
	{
		delete [] m_paQueues;
		m_paQueues = nullptr;
	}

	if (m_paMutex)
	{
		delete [] m_paMutex;
		m_paMutex = nullptr;
	}

	if (m_paCondition)
	{
		delete [] m_paCondition;
		m_paCondition = nullptr;
	}
}

/**
 * @brief 쓰레드 갯수 구하기
 * @return 쓰레드 갯수
*/
int CThreadPool::GetMaxThreads()
{
	return static_cast<int>(m_lstThreadPool.size());
}

/**
 * @brief 워커 고정 큐에 데이터 넣기 및 해당 Thread 깨우기
 * @param[in] nThreadId 워커(큐) 인덱스
 * @param[in] context 큐 데이터
 * @return void
*/
void CThreadPool::Enqueue(int nThreadId, const RAW_LOG_BATCH &vtRawLog)
{
	if (m_paQueues == nullptr || nThreadId < 0 || nThreadId >= m_nMaxThreads)
		return;

	if (vtRawLog.empty())
		return;

	// Enqueue + 시그널 을 동일 mutex 로 보호 (lost-wakeup 방지)
	m_paMutex[nThreadId].lock();
	m_paQueues[nThreadId].Enqueue(vtRawLog);
	m_paCondition[nThreadId].signal();
	m_paMutex[nThreadId].unlock();
}

/**
 * @brief 큐 데이터 갯수 구하기
 * @param[in] nThreadId 워커(큐) 인덱스 (-1: 전체 합)
 * @return 큐 데이터 갯수
*/
int CThreadPool::GetQueueCount(int nThreadId)
{
	if (m_paQueues == nullptr)
		return 0;

	if (nThreadId >= 0 && nThreadId < m_nMaxThreads)
	{
		m_paMutex[nThreadId].lock();
		int nCount = m_paQueues[nThreadId].Count();
		m_paMutex[nThreadId].unlock();
		return nCount;
	}

	int nTotal = 0;
	for (int i=0; i<m_nMaxThreads; ++i)
	{
		m_paMutex[i].lock();
		nTotal += m_paQueues[i].Count();
		m_paMutex[i].unlock();
	}

	return nTotal;
}

/**
 * @brief 대기 중인 쓰레드 수 얻기
 * @return 대기 중인 쓰레드 수
*/
int CThreadPool::GetWaitingThreads()
{
	list<ThreadPoolContext>::iterator it;
	int nWaitingThreads = 0;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread->GetState() == ETS_RUNNING && 
			(*it).worker->GetState() == EWS_WAITING)
			nWaitingThreads++;
	}

	return nWaitingThreads;
}

/**
 * @brief 작업 중인 쓰레드 수 얻기
 * @return 작업 중인 쓰레드 수
*/
int CThreadPool::GetActiveThreads()
{
	list<ThreadPoolContext>::iterator it;
	int nActiveThreads = 0;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread->GetState() == ETS_RUNNING && 
			(*it).worker->GetState() == EWS_ACTIVE)
			nActiveThreads++;
	}

	return nActiveThreads;
}

/**
 * @brief 정지된 쓰레드 수 얻기
 * @return 정지된 쓰레드 수
*/
int CThreadPool::GetStoppedThreads()
{
	list<ThreadPoolContext>::iterator it;
	int nStoppedThreads = 0;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread->GetState() == ETS_RUNNING && 
			(*it).worker->GetState() == EWS_STOPPED)
			nStoppedThreads++;
	}

	return nStoppedThreads;
}

/**
 * @brief 워커 스레드 종료 요청 (큐 처리 중단)
 * @return void
 * @remark #8 종료: 신규 Dequeue 중단, 진행 중 run() 은 완료 후 종료
*/
void CThreadPool::RequestShutdown()
{
	list<ThreadPoolContext>::iterator it;

	for (it=m_lstThreadPool.begin(); it!=m_lstThreadPool.end(); it++)
	{
		if ((*it).thread != nullptr)
			(*it).thread->stop();
	}
}

/**
 * @brief 활성 워커·큐가 비울 때까지 대기
 * @param[in] nMaxWaitMs 최대 대기 (ms)
 * @return true(유휴), false(타임아웃 시 잔여 작업 있음)
*/
bool CThreadPool::WaitForIdle(int nMaxWaitMs)
{
	if (nMaxWaitMs <= 0)
		return (GetActiveThreads() <= 0 && GetQueueCount() <= 0);

	const int nStepMs = 100;
	int nElapsedMs = 0;

	while (nElapsedMs < nMaxWaitMs)
	{
		if (GetActiveThreads() <= 0 && GetQueueCount() <= 0)
			return true;

		CThread::sleep(nStepMs);
		nElapsedMs += nStepMs;
	}

	return (GetActiveThreads() <= 0 && GetQueueCount() <= 0);
}

/**
 * @brief 진행 중(활성) 워커만 유휴 될 때까지 대기
 * @param[in] nMaxWaitMs 최대 대기 (ms)
 * @return true(활성 없음), false(타임아웃 시 진행 중 batch 잔존)
 * @remark 종료 시 RequestShutdown() 이후 큐는 워커가 Dequeue 하지 않으므로
 *         큐 비움은 WaitForIdle 이 아닌 DrainQueuedBatches 로 처리한다.
*/
bool CThreadPool::WaitForActiveIdle(int nMaxWaitMs)
{
	if (nMaxWaitMs <= 0)
		return (GetActiveThreads() <= 0);

	const int nStepMs = 100;
	int nElapsedMs = 0;

	while (nElapsedMs < nMaxWaitMs)
	{
		if (GetActiveThreads() <= 0)
			return true;

		CThread::sleep(nStepMs);
		nElapsedMs += nStepMs;
	}

	return (GetActiveThreads() <= 0);
}

/**
 * @brief 워커 큐에 남은 batch 전량 추출 (처리·release 용)
 * @param[out] pvtBatches 잔여 batch 목록
 * @return void
 * @remark #8 종료 drain: Dequeue 만 수행, DB release 는 RawLogWorker 가 담당
*/
void CThreadPool::DrainQueuedBatches(vector<RAW_LOG_BATCH> *pvtBatches)
{
	if (pvtBatches == nullptr || m_paQueues == nullptr)
		return;

	for (int i=0; i<m_nMaxThreads; ++i)
	{
		RAW_LOG_BATCH vtBatch;

		m_paMutex[i].lock();
		while (m_paQueues[i].Dequeue(vtBatch))
		{
			if (!vtBatch.empty())
				pvtBatches->push_back(vtBatch);
		}
		m_paMutex[i].unlock();
	}
}
