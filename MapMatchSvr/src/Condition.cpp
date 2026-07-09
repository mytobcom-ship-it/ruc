/**
 * @file Condition.cpp
 * @brief Thread 접근 제어용 클래스 소스 파일
*/
#include "Condition.h"
#include <errno.h>
#include <time.h>

/**
 * @brief 생성자
*/
CCondition::CCondition()
{
	pthread_cond_init(&m_condition, nullptr);
}

/**
 * @brief 소멸자
*/
CCondition::~CCondition()
{
	pthread_cond_destroy(&m_condition);
}

/**
 * @brief 대기 중인 모든 Thread 깨우기
 * @return void
*/
void CCondition::broadcast()
{
	pthread_cond_broadcast(&m_condition);
}

/**
 * @brief 대기 중인 하나의 Thread 깨우기
 * @return void
*/
void CCondition::signal()
{
	pthread_cond_signal(&m_condition);
}

/**
 * @brief 동작 중인 Thread 잠시 중단
 * @param[in] mutex 동기화용 Mutex
 * @return void
*/
void CCondition::wait(CMutex& mutex)
{
	pthread_cond_wait(&m_condition, &mutex.m_mutex);
}

/**
 * @brief 동작 중인 Thread 를 최대 nWaitMs 밀리초 대기 (signal 시 즉시 반환)
 * @param[in] mutex 동기화용 Mutex (호출 전 lock 상태)
 * @param[in] nWaitMs 최대 대기 시간 (ms)
 * @return true(signal), false(timeout)
 */
bool CCondition::waitTimed(CMutex& mutex, int nWaitMs)
{
	if (nWaitMs <= 0)
		return false;

	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return false;

	ts.tv_sec += nWaitMs / 1000;
	ts.tv_nsec += static_cast<long>(nWaitMs % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L)
	{
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}

	int nRet = pthread_cond_timedwait(&m_condition, &mutex.m_mutex, &ts);
	return (nRet == 0);
}
