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
	m_nState(ETS_CREATED), 
	m_nThreadId(nThreadId)
{
}

/**
 * @brief 생성자
 * @param[in] nThreadId Thread 아이디
 * @param[in] pcRunnable Thread 에서 실행할 Runable 클래스를 상속 받은 클래스
*/
CThread::CThread(int nThreadId, Runnable *pcRunnable) : 
	m_nState(ETS_CREATED), 
	m_nThreadId(nThreadId), 
	m_pcRunnable(pcRunnable)
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
 * @return true, false
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
 * @param[in] millis 마이크로 초 (1000000 : 1초)
 * @return void
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
	pthread_detach(m_hHandle);
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

/**
 * @brief 입력 값 증가
 * @param[in] val 정수형 입력 값
 * @return 증가된 값
 * @remark 디버깅용
*/
long CThread::InterlockedIncrement(volatile long *val)
{
	return ++(*val);
}

/**
 * @brief 입력 값 감소
 * @param[in] val 정수형 입력 값
 * @return 감소된 값
 * @remark 디버깅용
*/
long CThread::InterlockedDecrement(volatile long *val)
{
	return --(*val); // unsafe
}
