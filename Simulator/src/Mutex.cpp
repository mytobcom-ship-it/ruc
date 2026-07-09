/**
 * @file Mutex.cpp
 * @brief Mutex 클래스 소스 파일
*/
#include "Mutex.h"

/**
 * @brief 생성자
*/
CMutex::CMutex()
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m_mutex, &attr);
	pthread_mutexattr_destroy(&attr);
}

/**
 * @brief 소멸자
*/
CMutex::~CMutex()
{
	pthread_mutex_destroy(&m_mutex);
}

/**
 * @brief Mutex 잠금
 * @return void
*/
void CMutex::lock()
{
	pthread_mutex_lock(&m_mutex);
}

/**
 * @brief Mutex 해제
 * @return void
*/
void CMutex::unlock()
{
	pthread_mutex_unlock(&m_mutex);
}
