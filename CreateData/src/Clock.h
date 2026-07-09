/**
 * @file Clock.h
 * @brief 수행 시간 측정 클래스 헤더 파일
*/
#ifndef __CLOCK_H__
#define __CLOCK_H__

#include <stdio.h>
#include <time.h>

/**
 * @class CClock
 * @brief 수행 시간 측정 클래스
*/
class CClock
{
public:
	CClock();
	virtual ~CClock();

	void Start();
	void Stop();
	double GetElapsedTime();

private:
	struct timespec				m_tvStart;
	struct timespec				m_tvEnd;
	double						m_dbElapsedTime;
};

#endif	// __CLOCK_H__
