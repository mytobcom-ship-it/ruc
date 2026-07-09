/**
 * @file Queue.h
 * @brief 큐 클래스 헤더 및 소스 (template) 파일
 * @remark
 *   이 큐는 자체 동기화를 하지 않습니다 (외부 락 전제).
 *   동시 접근이 필요한 경우 호출 측에서 mutex 로 보호해야 합니다.
 *   (예: ThreadPool 의 워커별 m_paMutex 로 보호하여 사용)
*/
#ifndef __QUEUE_H__
#define __QUEUE_H__

#include <deque>

using namespace std;

/**
 * @class CQueue
 * @brief 큐 클래스 (비동기화 / 외부 락 전제)
*/
template< class T, typename TQueue = deque<T> >
class CQueue
{
public:
	/**
	 * @brief 생성자
	*/
	CQueue() {}

	/**
	 * @brief 소멸자
	*/
	~CQueue()
	{
		Clear();
	}

	/**
	 * @brief 큐에 데이터 넣기
	 * @param[in] element 큐 데이터
	 * @return void
	*/
	void Enqueue(const T& element)
	{
		m_listQueue.push_back(element);
	}

	/**
	 * @brief 큐에서 데이터 가져오기
	 * @param[out] element 큐 데이터
	 * @return true, false
	*/
	bool Dequeue(T& element)
	{
		if (m_listQueue.empty())
			return false;

		element = m_listQueue.front();
		m_listQueue.pop_front();

		return true;
	}

	/**
	 * @brief 큐에서 데이터 가져오기
	 * @param[out] element 큐 데이터
	 * @return true, false
	 * @remark 데이터를 큐에서 삭제하지 않음
	*/
	bool Peek(T& element)
	{
		if (m_listQueue.empty())
			return false;

		element = m_listQueue.front();

		return true;
	}

	/**
	 * @brief 큐의 모든 데이터 삭제
	 * @return void
	*/
	void Clear()
	{
		m_listQueue.clear();
	}

	/**
	 * @brief 큐의 데이터 개수 구하기
	 * @return 큐의 데이터 개수
	*/
	int Count()
	{
		return static_cast<int>(m_listQueue.size());
	}

	/**
	 * @brief 큐가 비어 있는지 여부
	 * @return true, false
	*/
	bool IsEmpty()
	{
		return m_listQueue.empty();
	}

private:
	TQueue						m_listQueue;
};

#endif	//__QUEUE_H__
