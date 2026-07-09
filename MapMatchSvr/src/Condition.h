/**
 * @file Condition.h
 * @brief Thread 접근 제어용 클래스 헤더 파일
*/
#ifndef __CONDITION_H__
#define __CONDITION_H__

#include <pthread.h>
#include "TypeDefine.h"
#include "Mutex.h"

/**
 * @class CCondition
 * @brief 이벤트 관리용 클래스
*/
class CCondition
{
public:
	CCondition();
	~CCondition();
	void broadcast();
	void signal();
	void wait(CMutex& mutex);
	bool waitTimed(CMutex& mutex, int nWaitMs);

protected:
	pthread_cond_t				m_condition;
};

#endif	//__CONDITION_H__
