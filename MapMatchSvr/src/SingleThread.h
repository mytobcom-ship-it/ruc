/**
 * @file SingleThread.h
 * @brief 싱글 쓰레드 클래스 헤더 파일
*/
#ifndef __SINGLETHREAD_H__
#define __SINGLETHREAD_H__

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <signal.h>
#include <pthread.h>
#include <string>
#include <stdexcept>
#include "TypeDefine.h"

using namespace std;

/**
 * @enum STHREAD_STATE
 * @brief 쓰레드 상태
*/
enum STHREAD_STATE
{
	ESS_STOPED						= -1,								// 쓰레드 멈춤
	ESS_INITIAL,														// 쓰레드 초기화
	ESS_RUNNING,														// 쓰레드 실행 중
};

/**
 * @class CSingleThread
 * @brief 싱글 쓰레드 클래스
*/
class CSingleThread
{
public:
	CSingleThread();
	CSingleThread(const string& name);
	virtual ~CSingleThread();
	void start();
	void join();
	void join(unsigned long time);
	void detach();
	void interrupt();
	bool IsInterrupted();
	bool IsAlive();
	const string& getName();
	void setName(const string& name);

private:
	void Initialize(const string& name);
	static void *threadHandler(void *pParam);
	static void interruptHandler(int sig);
	virtual void run() = 0;

private:
	int								m_nState;
	string							m_name;
	pthread_t						m_thread;
	pthread_mutex_t					m_mutex;
	pthread_cond_t					m_cond;
	bool							m_bJoinning;
	bool							m_bIsInterrupted;
	static pthread_attr_t			m_attr;
	static long						m_nId;
};

/**
 * @class InterruptedException : public exception
 * @brief 인터럽트 클래스
*/
class InterruptedException : public exception
{
public:
	explicit InterruptedException(const string& message)
		: m_message(message)
	{}

	virtual ~InterruptedException() throw()
	{}

	virtual const char *what() const throw()
	{
		return m_message.c_str();
	}

private:
	string							m_message;
};

/**
 * @class IllegalThreadStateException : public exception
 * @brief 쓰레드 예외 처리 상태
*/
class IllegalThreadStateException : public exception
{
public:
	explicit IllegalThreadStateException(const string& message)
		: m_message(message)
	{}

	virtual ~IllegalThreadStateException() throw()
	{}

	virtual const char *what() const throw()
	{
		return m_message.c_str();
	}

private:
	string							m_message;
};

#endif	//__SINGLETHREAD_H__
