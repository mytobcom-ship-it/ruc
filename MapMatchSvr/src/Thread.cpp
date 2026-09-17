/**
 * @file Thread.cpp
 * @brief Thread 클래스 소스 파일
*/
#include "Thread.h"

/**
 * @brief 실행된 Thread
 * @param[in] pParam 부모 클래스 포인터
 * @return nullptr
*/
extern "C" void *threadProc(void *pParam)
{
	CThread *pcThread = reinterpret_cast<CThread *>(pParam);
	pcThread->run();
	pthread_exit(0);
	return nullptr;
}

/**
 * @brief Thread 핸들 값 구하기
 * @return Thread 핸들 값
*/
uint32 Runnable::GetThreadHandle()
{
	return static_cast<uint32>(pthread_self());
}

/**
 * @brief 생성자
 * @param[in] nThreadId 쓰레드 아이디
*/
CThread::CThread(int nThreadId) :
	m_hHandle(0),
	m_nState(ETS_CREATED),
	m_nThreadId(nThreadId),
	m_pcRunnable(nullptr),
	m_context(nullptr)
{
}

/**
 * @brief 생성자
 * @param[in] nThreadId Thread 아이디
 * @param[in] pcRunnable Thread 에서 실행할 Runable 클래스를 상속 받은 클래스
*/
CThread::CThread(int nThreadId, Runnable *pcRunnable) :
	m_hHandle(0),
	m_nState(ETS_CREATED),
	m_nThreadId(nThreadId),
	m_pcRunnable(pcRunnable),
	m_context(nullptr)
{
}

/**
 * @brief 소멸자
*/
CThread::~CThread()
{
	if (m_hHandle != 0)
		pthread_join(m_hHandle, 0);

	if (m_pcRunnable)
	{
		delete m_pcRunnable;
		m_pcRunnable = nullptr;
	}
}

/**
 * @brief 쓰레드 핸들 구하기
 * @return 쓰레드 핸들
*/
uint32 CThread::GetThreadHandle()
{
	return static_cast<uint32>(m_hHandle);
}

/**
 * @brief 쓰레드 아이디 구하기
 * @return 쓰레드 아이디
*/
int CThread::GetThreadId()
{
	return m_nThreadId;
}

/**
 * @brief 쓰레드 생성
 * @param[in] context 쓰레드 전달 데이터
 * @return true(성공), false(실패)
*/
bool CThread::start(void *context)
{
	m_context = context;

	if (pthread_create(&m_hHandle, nullptr, threadProc, this) != 0)
		return false;

	return true;
}

/**
 * @brief Thread 가 실행할 작업 실행
 * @return void
*/
void CThread::run()
{
	if (m_pcRunnable != nullptr)
	{
		m_nState = ETS_RUNNING;
		m_pcRunnable->run(m_nThreadId, m_context);
		m_nState = ETS_STOPPED;
	}
}

/**
 * @brief Thread 실행 종료
 * @return void
*/
void CThread::stop()
{
	if (m_pcRunnable != nullptr)
		m_pcRunnable->stop(m_nThreadId, m_context);
}

/**
 * @brief 쓰레드 sleep 함수
 * @param[in] millis **밀리초** (1000 : 1초)
 * @return void
 * @remark [주석 정정, 2026-09-17 최정우] 종전 주석은 "마이크로 초 (1000000 : 1초)" 라고 적혀
 *   있었으나 사실과 다르다 — 인자를 usleep() 에 넘기기 전에 ×1000 하므로 단위는 밀리초다.
 *   호출부(ThreadPool.cpp WaitForIdle/WaitForActiveIdle/WaitForAllStopped)도 전부
 *   `const int nStepMs = 100; CThread::sleep(nStepMs); nElapsedMs += nStepMs;` 로
 *   밀리초 전제로 쓰고 있다. 주석만 틀렸고 동작은 처음부터 밀리초였다 — 그 주석을 믿고
 *   값을 넣으면 1000배 어긋난다.
*/
void CThread::sleep(long millis)
{
	usleep(millis * 1000);
}

/**
 * @brief 메인 쓰레드에서 쓰레드 분리
 * @return void
*/
void CThread::detach()
{
	// [버그 수정, 2026-09-11 최정우] detach 후에도 m_hHandle 을 그대로 남겨두면, 소멸자의
	//   `if (m_hHandle != 0) pthread_join(...)` 가 이미 detach 된 스레드를 또 join() 하게 된다 —
	//   POSIX 정의되지 않은 동작(join()이 같은 이유로 m_hHandle=0 리셋하는 것과 동일 근거).
	//   CThreadPool 은 생성자에서 start() 직후 곧바로 detach() 하므로(m_bDetatch=true, 운영 기본값)
	//   매 정상 종료마다 재현됐다.
	pthread_detach(m_hHandle);
	m_hHandle = 0;
}

/**
 * @brief 쓰레드 자원 해제
 * @return void
*/
void CThread::join()
{
	pthread_join(m_hHandle, 0);
	m_hHandle = 0;
}

/**
 * @brief 쓰레드 상태 구하기
 * @return 쓰레드 상태
*/
enum ETHREAD_STATE CThread::GetState()
{
	return m_nState;
}

