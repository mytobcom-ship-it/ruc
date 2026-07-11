/**
 * @file Mutex.h
 * @brief Mutex 헤더 클래스 헤더 파일
*/
#ifndef __MUTEX_H__
#define __MUTEX_H__

#include <stdio.h>
#include <pthread.h>

/**
 * @class CMutex
 * @brief mutex 관리용 클래스
*/
class CMutex
{
public:
	CMutex();
	~CMutex();

	void lock();
	void unlock();

public:
	pthread_mutex_t				m_mutex;
};

#endif //__MUTEX_H__
