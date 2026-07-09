/**
 * @file Thread.h
 * @brief Thread 클래스 헤더 파일
*/
#ifndef __THREAD_H__
#define __THREAD_H__

#include <unistd.h>
#include <pthread.h>
#include "TypeDefine.h"

/**
 * @class Runnable
 * @brief 작업용 쓰레드에서 상속 받을 인터페이스 클래스
*/
class Runnable
{
public:
	Runnable() {}
	virtual ~Runnable() {}
	virtual void run(int nThreadId, void *context = nullptr) = 0;
	virtual void stop(int nThreadId, void *context = nullptr) {}
	static uint32 GetThreadHandle();
};

/**
 * @enum ETHREAD_STATE
 * @brief Thread 상태
*/
enum ETHREAD_STATE
{
	ETS_CREATED						= 0,								// Thread 가 생성된 상태
	ETS_RUNNING,														// Thread 가 실행중인 상태
	ETS_STOPPED															// Thread 가 멈춘 상태
};

/**
 * @class CThread
 * @brief 쓰레드 클래스
*/
class CThread
{
public:
	CThread(int nThreadId = 0);
	CThread(int nThreadId, Runnable *pcRunnable);
	virtual ~CThread();
	uint32 GetThreadHandle();
	int GetThreadId();
	bool start(void *context = nullptr);
	virtual void run();
	virtual void stop();
	static void sleep(long millis);
	void detach();
	void join();
	enum ETHREAD_STATE GetState();
	static long InterlockedIncrement(volatile long *val);
	static long InterlockedDecrement(volatile long *val);

protected:
	pthread_t					m_hHandle;
	enum ETHREAD_STATE			m_nState;
	int							m_nThreadId;
	Runnable					*m_pcRunnable;
	void						*m_context;
};

#endif	//__THREAD_H__
