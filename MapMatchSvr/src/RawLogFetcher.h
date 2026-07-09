/**
 * @file RawLogFetcher.h
 * @brief 원시 GPS 로그 DB 폴링 및 워커 큐 적재 클래스 헤더 파일
*/
#ifndef __RAWLOGFETCHER_H__
#define __RAWLOGFETCHER_H__

#include <stdio.h>
#include <string>
#include <vector>
#include "TypeDefine.h"
#include "MessageType.h"
#include "SingleThread.h"
#include "PostgrePool.h"
#include "ThreadPool.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

#define FETCH_LIMIT_DEFAULT				500
#define FETCH_INTERVAL_DEFAULT			500
#define QUEUE_PAUSE_COUNT_DEFAULT		400
#define QUEUE_MAX_COUNT_DEFAULT			800
#define QUEUE_BUSY_MIN_DEFAULT			2000
#define QUEUE_BUSY_MAX_DEFAULT			10000

/**
 * @class CRawLogFetcher
 * @brief rawgps_select(UPDATE RETURNING) 로 예약(Reserve)·조회 후 워커 큐 적재
 * @remark
 *   - 단일 Feeder 스레드가 DB poll → trip_id batch 분할 → ThreadPool Enqueue
 *   - 큐 backpressure 시 poll 중단, 적응형 sleep 으로 부하 완화
*/
class CRawLogFetcher : public CSingleThread
{
public:
	CRawLogFetcher();
	virtual ~CRawLogFetcher();

	bool Initialize(CPostgrePool *pcPostgrePool, CThreadPool *pcThreadPool,
		const string& strSelectSQL, const string& strUpdateSQL,
		int nWorkerThreads, uint8 nCoordinateType,
		volatile bool *pbRun,
		int nFetchLimit = FETCH_LIMIT_DEFAULT,
		int nFetchInterval = FETCH_INTERVAL_DEFAULT,
		int nQueuePauseCount = QUEUE_PAUSE_COUNT_DEFAULT,
		int nQueueMaxCount = QUEUE_MAX_COUNT_DEFAULT,
		int nQueueBusyMin = QUEUE_BUSY_MIN_DEFAULT,
		int nQueueBusyMax = QUEUE_BUSY_MAX_DEFAULT);

	static bool RunRecover(CPostgrePool *pcPostgrePool,
		const string& strRecoverSQL);

private:
	virtual void run();
	int ComputeFetchSleepMs(int nQueueCount) const;
	bool FetchAndDispatch();
	bool ReserveFetchBatch(PGconn *pcConn, vector<sRawLogInfo> *pvtRawLogInfos);
	void GroupByTripId(const vector<sRawLogInfo>& vtRawLogInfos, vector<RAW_LOG_BATCH> *pvtBatches);
	void DispatchBatches(const vector<RAW_LOG_BATCH>& vtBatches);
	int GetWorkerId(const char *pszDeviceKey) const;
	void ReleaseConnection(PGconn *pcConn);
	bool ParseRow(PGresult *pcResult, int nRow, sRawLogInfo *pstRawLogInfo);
	static bool ExtractRowPk(PGresult *pcResult, int nRow,
		string *pstrDeviceKey, string *pstrGpsDt, string *pstrGpsSeq);
	// parse 실패 행 PROCESSING 해제 — Worker BulkRelease 와 동일 [rawgps_update] $4=0
	bool ReleaseReservedRows(PGconn *pcConn,
		const string *pstrDeviceKeys, const string *pstrGpsDts, const string *pstrGpsSeqs,
		size_t nCount);
	static int GetPgCmdTuples(PGresult *pcResult);
	static bool CheckPgUpdateAffected(PGresult *pcResult, int nExpected, const char *pszLogTag);
	static string BuildPgTextArray(const vector<string>& vtValues);
	static string EscapePgArrayText(const string& strValue);
	static time_t ParseDateTime(const char *pszDateTime);

private:
	CPostgrePool					*m_pcPostgrePool;					// DB connection pool
	CThreadPool						*m_pcThreadPool;					// 워커 ThreadPool (Enqueue 대상)
	string							m_strSelectSQL;						// [rawgps_select] SQL
	string							m_strUpdateSQL;						// [rawgps_update] parse-fail release(0) 공용
	volatile bool					*m_pbRun;							// 서버 실행 플래그
	int								m_nWorkerThreads;					// 워커 스레드 수 (hash % N)
	uint8							m_nCoordinateType;					// GPS 좌표 측지계 (ParseRow)
	int								m_nFetchLimit;						// 1회 조회·예약 최대 건수 (건)
	int								m_nFetchInterval;					// 큐 여유 시 DB 조회 간격 (ms)
	int								m_nQueuePauseCount;					// 큐 batch 수, 이상이면 DB 조회 중단 (건)
	int								m_nQueueMaxCount;					// 큐 더 차면 대기 최대 구간 (건)
	int								m_nQueueBusyMin;					// 큐 혼잡 시 조회 대기 최소 (ms)
	int								m_nQueueBusyMax;					// 큐 혼잡 시 조회 대기 최대 (ms)
};

#endif //__RAWLOGFETCHER_H__
