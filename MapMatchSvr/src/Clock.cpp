/**
 * @file Clock.cpp
 * @brief 수행 시간 측정 클래스 소스 파일
*/
#include "Clock.h"

/**
 * @brief 생성자
*/
CClock::CClock()
	: m_dbElapsedTime(0.0)
{
}

/**
 * @brief 소멸자
*/
CClock::~CClock()
{
}

/**
 * @brief 수행 시간 측정 시작
 * @return void
*/
 void CClock::Start()
 {
 	clock_gettime(CLOCK_REALTIME, &m_tvStart);
 	clock_gettime(CLOCK_REALTIME, &m_tvEnd);
	m_dbElapsedTime = 0.0f;
 }

/**
 * @brief 수행 시간 측정 종료
 * @return void
*/
void CClock::Stop()
{
 	clock_gettime(CLOCK_REALTIME, &m_tvEnd);
}

/**
 * @brief 수행 시간 계산 및 반환
 * @return 수행 시간
*/
double CClock::GetElapsedTime()
{
	struct timespec tvDiffTime;

	tvDiffTime.tv_sec = m_tvEnd.tv_sec - m_tvStart.tv_sec;
	tvDiffTime.tv_nsec = m_tvEnd.tv_nsec - m_tvStart.tv_nsec;

	if (tvDiffTime.tv_nsec < 0)
	{
		tvDiffTime.tv_sec -= 1;
		tvDiffTime.tv_nsec += 1000000000.0;
	}

	m_dbElapsedTime = static_cast<double>(tvDiffTime.tv_sec) + static_cast<double>(tvDiffTime.tv_nsec) / 1000000000.0;

	return m_dbElapsedTime;
}
