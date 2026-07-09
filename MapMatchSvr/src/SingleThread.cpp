/**
 * @file SingleThread.cpp
 * @brief 싱글 쓰레드 클래스 소스 파일
*/
#include "SingleThread.h"

long CSingleThread::m_nId = 0;
pthread_attr_t CSingleThread::m_attr;

/**
 * @brief 쓰레드 핸들러
 * @param[in] pParam 쓰레드 전달 포인터
 * @return nullptr 
*/
void *CSingleThread::threadHandler(void *pParam)
{
	signal(SIGUSR1, interruptHandler);
	CSingleThread *pcThread = reinterpret_cast<CSingleThread *>(pParam);
	try
	{
		pcThread->run();
	}
	catch (InterruptedException& e)
	{
		pcThread->m_bIsInterrupted = true;
	}

	pcThread->m_nId = -1;
	if (pcThread->m_bJoinning)
	{
		pthread_mutex_lock(&pcThread->m_mutex);
		pthread_cond_broadcast(&pcThread->m_cond);
		pthread_mutex_unlock(&pcThread->m_mutex);
	}

	return nullptr;
}

/**
 * @brief 시그널 핸들러
 * @param[in] sig 시그널
 * @return void
*/
void CSingleThread::interruptHandler(int sig)
{
	throw InterruptedException("thread interrupted!");
}

/**
 * @brief 생성자
*/
CSingleThread::CSingleThread()
{
	char name[32];
	sprintf(name, "Thread%ld", m_nId);
	Initialize(name);
}

/**
 * @brief 생성자
 * @param[in] name 쓰레드 이름
*/
CSingleThread::CSingleThread(const string& name)
{
	Initialize(name);
}

/**
 * @brief 소멸자
*/
CSingleThread::~CSingleThread()
{
	pthread_attr_destroy(&m_attr);
	pthread_mutex_destroy(&m_mutex);
	pthread_cond_destroy(&m_cond);
}

/**
 * @brief 초기화
 * @param[in] name 쓰레드 이름
 * @return void
*/
void CSingleThread::Initialize(const string& name)
{
	m_name = name;
	m_bJoinning = false;
	m_bIsInterrupted = false;
	pthread_mutex_init(&m_mutex, nullptr);
	pthread_cond_init(&m_cond, nullptr);
	m_nState = static_cast<int>(ESS_INITIAL);

	pthread_mutex_lock(&m_mutex);
	if (m_nId == 0)
	{
		pthread_attr_init(&m_attr);
		pthread_attr_setdetachstate(&m_attr, PTHREAD_CREATE_DETACHED);
	}
	m_nId++;
	pthread_mutex_unlock(&m_mutex);
}

/**
 * @brief 쓰레드 시작
 * @return void
*/
void CSingleThread::start()
{
	if (m_nState == static_cast<int>(ESS_RUNNING))
	{
		throw IllegalThreadStateException("thread already started!");
	}
	else if (m_nState == static_cast<int>(ESS_INITIAL))
	{
		m_nState = static_cast<int>(ESS_RUNNING);
		if (pthread_create(&m_thread, &m_attr, threadHandler, this) != 0)
			return;
	}
	else if (m_nState == static_cast<int>(ESS_STOPED))
	{
		throw IllegalThreadStateException("thread has been started!");
	}
}

/**
 * @brief 쓰레드 종료
 * @return void
*/
void CSingleThread::join()
{
	if (m_nState == static_cast<int>(ESS_RUNNING))
	{
		pthread_mutex_lock(&m_mutex);
		m_bJoinning = true;
		pthread_cond_wait(&m_cond, &m_mutex);
		m_bJoinning = false;
		pthread_mutex_unlock(&m_mutex);
	}
}

/**
 * @brief 입력시간 후 쓰레드 종료
 * @param[in] time 밀리세컨드
 * @return void
*/
void CSingleThread::join(unsigned long time)
{
	if (m_nState == static_cast<int>(ESS_RUNNING))
	{
		struct timeval now;
		struct timespec timeout;

		gettimeofday(&now, nullptr);
		ldiv_t t = ldiv(time * 1000000, 1000000000);
		timeout.tv_sec = now.tv_sec + t.quot;
		timeout.tv_nsec = now.tv_usec * 1000 + t.rem;
		
		pthread_mutex_lock(&m_mutex);
		m_bJoinning = true;
		pthread_cond_timedwait(&m_cond, &m_mutex, &timeout);
		m_bJoinning = false;
		pthread_mutex_unlock(&m_mutex);
	}
}

/**
 * @brief 인터럽트 쓰레드
 * @return void
*/
void CSingleThread::interrupt()
{
	pthread_kill(m_thread, SIGUSR1);
}

/**
 * @brief 인터럽트 여부
 * @return true, false
*/
bool CSingleThread::IsInterrupted()
{
	return m_bIsInterrupted;
}

/**
 * @brief 쓰레드가 실행 중인지 여부
 * @return true, false
*/
bool CSingleThread::IsAlive()
{
	return (m_nState == static_cast<int>(ESS_RUNNING)) ? true : false;
}

/**
 * @brief 쓰레드 이름 구하기
 * @return 쓰레드 이름
*/
const string& CSingleThread::getName()
{
	return m_name;
}

/**
 * @brief 쓰레드 이름 설정
 * @param[in] name 쓰레드 이름
 * @return void
*/
void CSingleThread::setName(const string& name)
{
	m_name = name;
}
