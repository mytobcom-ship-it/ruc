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
	// [버그 수정, 2026-09-10 최정우] CLOCK_REALTIME 은 시스템 벽시계라 NTP 보정 등으로 도중에
	// 앞/뒤로 점프할 수 있다 — 유일한 사용처(RawLogWorker.cpp cMatchClock, 1 GPS 맵매칭 처리
	// 시간 측정 후 임계값 초과 시 ERROR 격리)는 실제 CPU 작업 시간만 재면 되므로, 외부 시각
	// 조정에 영향받지 않는 CLOCK_MONOTONIC 으로 바꾼다(최소 재현으로, NTP 5초 점프 시 실제
	// 2ms 작업이 5002ms로 계산돼 정상 매칭이 타임아웃 오탐으로 버려짐을 확인). m_tvStart/
	// m_tvEnd 는 GetElapsedTime() 의 차이 계산에만 쓰이고 외부에 절대시각으로 노출되지 않아
	// (grep 확인) 안전하게 바꿀 수 있다.
 	clock_gettime(CLOCK_MONOTONIC, &m_tvStart);
 	clock_gettime(CLOCK_MONOTONIC, &m_tvEnd);
	m_dbElapsedTime = 0.0f;
 }

/**
 * @brief 수행 시간 측정 종료
 * @return void
*/
void CClock::Stop()
{
 	clock_gettime(CLOCK_MONOTONIC, &m_tvEnd);
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
