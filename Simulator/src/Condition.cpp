/**
 * @file Condition.cpp
 * @brief Thread 접근 제어용 클래스 소스 파일
*/
#include "Condition.h"

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
