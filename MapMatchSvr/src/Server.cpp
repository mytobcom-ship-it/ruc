/**
 * @file Server.cpp
 * @brief 서버 클래스 소스 파일
*/
#include "Server.h"
#include "RawLogWorker.h"
#include <signal.h>

static CServer *g_pcServerInstance = nullptr;
// SIGINT/SIGTERM 수신 플래그 — 핸들러는 이 값만 세팅(async-시그널-safe) (2026-07-10 최정우 추가)
static volatile sig_atomic_t g_nShutdownRequested = 0;

/**
 * @brief SIGINT/SIGTERM 수신 시 서버 run 루프 종료
 * @param[in] nSignal 시그널 번호
 * @return void
 */
static void ServerSignalHandler(int nSignal)
{
	(void)nSignal;

	// 2026-07-10 최정우 주석 처리: 시그널 컨텍스트에서 뮤텍스/조건변수/pthread_kill 호출은
	// async-시그널-safe 위반. 특히 인터럽트()가 run 스레드에 SIGUSR1→예외를 던져
	//   pthread_cond_timedwait(m_cRunCondition) 를 강제 언와인드 → 조건변수 내부 ref 오염 →
	//   종료 시 pthread_cond_destroy 무한 대기(hang) 유발.
	//if (g_pcServerInstance != nullptr)
	//{
	//	g_pcServerInstance->RequestShutdown();
	//g_pcServerInstance->인터럽트();
	//}
	// 플래그만 세팅. run 루프가 이를 관찰하여 정상 경로로 종료 (2026-07-10 최정우 수정)
	g_nShutdownRequested = 1;
}

/**
 * @brief 로그 관리 검사 쓰레드
 * @param[in] context 호출 클래스 포인터
 * @return nullptr
*/
extern "C" void *TimerThread(void *context)
{
	CServer *pcServer = reinterpret_cast<CServer *>(context);
	if (pcServer == nullptr)
		return nullptr;

	CUtil cUtil;

	time_t dtPre = time(nullptr);
	time_t dtNow = time(nullptr);
	while (pcServer->IsRun())
	{
		dtNow = time(nullptr);
		if (dtPre != dtNow)
		{
			// 매 초마다 로그 관리 시간 검사 및 처리
			pcServer->ProcessPeriodSec(dtNow);
		}

		dtPre = dtNow;
		// 타이머 쓰레드 10ms 슬립 (2026-07-08 최정우 주석 추가)
		cUtil.Sleep(0, 10000);
	}

	return nullptr;
}

/**
 * @brief 작업용 스레드 클래스 (CRawLogWorker 래퍼)
*/
class CWorkerManager : public virtual Runnable
{
public:
	CWorkerManager() : m_pcRawLogWorker(nullptr) {}

	void SetWorker(CRawLogWorker *pcRawLogWorker)
	{
		m_pcRawLogWorker = pcRawLogWorker;
	}

	virtual void run(int nThreadId, void *context)
	{
		if (m_pcRawLogWorker != nullptr)
		{
			// ThreadPool Runnable → RawLogWorker 배치 처리 위임 (2026-07-08 최정우 주석 추가)
			m_pcRawLogWorker->run(nThreadId, context);
		}
	}

private:
	CRawLogWorker					*m_pcRawLogWorker;
};

/**
 * @brief 생성자
*/
CServer::CServer() : 
	m_pcPostgrePool(nullptr),
	m_pcSQLAccessor(nullptr), 
	m_pcLoggerManager(nullptr), 
	m_pcDataLoader(nullptr),
	m_pcChargeDataLoader(nullptr),
	m_pcThreadPool(nullptr),
	m_pcProcessManager(nullptr), 
	m_pcRawLogFetcher(nullptr),
	m_pcRawLogWorker(nullptr),
	m_bRun(false), 
	m_bUninitialized(false),
	m_nWorkerThread(0), 
	m_hTimerThread(0),
	m_nDBMinConnect(CFG_DEF_MINCONNECT),
	m_nDBMaxConnect(5),
	m_nGateReloadSec(CFG_DEF_GATE_RELOAD),
	m_dtLastGateReload(0),
	m_nParkBuf(CFG_DEF_PARK_BUF),
	m_nParkAccMax(CFG_DEF_PARK_ACCMAX),
	m_nParkExitCnt(CFG_DEF_PARK_EXITCNT),
	m_nParkRegraceSec(CFG_DEF_PARK_REGRACE),
	m_nParkTtlSec(CFG_DEF_PARK_TTL),
	m_nExemptRegraceSec(CFG_DEF_EXEMPT_REGRACE),
	m_strServerId(CFG_DEF_SERVER_ID),
	m_nServerStatusIntervalSec(CFG_DEF_STATUS_INTVL),
	m_dtLastServerStatusUpdate(0),
	m_qwPrevCpuTotal(0),
	m_qwPrevCpuIdle(0),
	m_dfLastCpuPct(0.0),
	m_bCpuSampleValid(false),
	m_nFetchLimit(CFG_DEF_LIMIT),
	m_nFetchInterval(CFG_DEF_FETCH_INTVL),
	m_nQueuePauseCount(CFG_DEF_Q_PAUSE_CNT),
	m_nQueueMaxCount(CFG_DEF_Q_MAX_CNT),
	m_nQueueBusyMin(CFG_DEF_Q_BUSY_MIN),
	m_nQueueBusyMax(CFG_DEF_Q_BUSY_MAX),
	m_nTtlSec(CFG_DEF_TTL),
	m_nShutdownWait(CFG_DEF_SHUTDOWN_WAIT),
	m_nRetryMax(CFG_DEF_RETRY_MAX),
	m_nCoordinateType(0),
	m_nRadius(0),
	m_nMaxStep(0),
	m_dwMaxDistance(0),
	m_nMatchTimeout(0),
	m_dfRadiusScale(CFG_DEF_RADIUS_SCALE),
	m_nRadiusMin(CFG_DEF_RADIUS_MIN),
	m_nRadiusMax(CFG_DEF_RADIUS),
	m_nRadiusSkip(CFG_DEF_RADIUS_SKIP),
	m_nAltGap(CFG_DEF_ALT_GAP),
	m_nAltPenalty(CFG_DEF_ALT_PENALTY),
	m_dfAltWeight(CFG_DEF_ALT_WEIGHT),
	m_dfAltSlope(CFG_DEF_ALT_SLOPE),
	m_nReverseConfirm(CFG_DEF_REVERSE_CONFIRM),
	m_dfSpeedFactor(CFG_DEF_SPEED_FACTOR),
	m_nSpeedMargin(CFG_DEF_SPEED_MARGIN),
	m_dtLastMonitorLog(0),
	m_nLastQueueCount(0),
	m_bQueueWarnActive(false),
	m_bAllBusyWarnActive(false)
{
}

/**
 * @brief 소멸자
*/
CServer::~CServer()
{
	// 소멸 시 서버·리소스 정리 (2026-07-08 최정우 주석 추가)
	Uninitialize();
}

/**
 * @brief 서버 초기화
 * @param[in] stConfig 환경 설정
 * @return true(성공), false(실패)
*/
bool CServer::Initialize(const CONFIG& stConfig)
{
	m_strLogPath = stConfig.strLogPath;
	m_nLogLevel = stConfig.nLogLevel;
	m_nLogKeepRunTime = stConfig.nLogKeepRunTime;
	m_nLogKeepDay = stConfig.nLogKeepDay;
	m_nWorkerThread = stConfig.nThreads;
	m_strDBHost = stConfig.strDBHost;
	m_nDBPort = stConfig.nDBPort;
	m_strDBName = stConfig.strDBName;
	m_strDBUserID = stConfig.strDBUserID;
	m_strDBPasswd = stConfig.strDBPasswd;
	m_nDBMinConnect = stConfig.nDBMinConnect;
	m_nDBMaxConnect = stConfig.nDBMaxConnect;
	m_nThreads = stConfig.nThreads;
	m_strDataFile = stConfig.strDataFile;
	m_nCoordinateType = static_cast<uint8>(stConfig.nGeodetic);
	m_nRadius = static_cast<sint16>(stConfig.nRadius);
	m_nMaxStep = static_cast<uint16>(stConfig.nMaxStep);
	m_dfHopPenalty = stConfig.dfHopPenalty;					// (2026-08-22 최정우 추가)
	m_dwMaxDistance = static_cast<uint32>(stConfig.nDistance);
	m_nMatchTimeout = stConfig.nMatchTimeout;
	m_dfRadiusScale = stConfig.dfRadiusScale;
	m_nRadiusMin = stConfig.nRadiusMin;
	m_nRadiusMax = stConfig.nRadiusMax;
	m_nRadiusSkip = stConfig.nRadiusSkip;
	m_nAltGap = stConfig.nAltGap;
	m_nAltPenalty = stConfig.nAltPenalty;
	m_dfAltWeight = stConfig.dfAltWeight;
	m_dfAltSlope = stConfig.dfAltSlope;
	m_nReverseConfirm = stConfig.nReverseConfirm;
	m_dfSpeedFactor = stConfig.dfSpeedFactor;
	m_nSpeedMargin = stConfig.nSpeedMargin;
	m_nFetchLimit = stConfig.nFetchLimit;
	m_nFetchInterval = stConfig.nFetchInterval;
	m_nQueuePauseCount = stConfig.nQueuePauseCount;
	m_nQueueMaxCount = stConfig.nQueueMaxCount;
	m_nQueueBusyMin = stConfig.nQueueBusyMin;
	m_nQueueBusyMax = stConfig.nQueueBusyMax;
	m_nTtlSec = stConfig.nTtlSec;
	m_nShutdownWait = stConfig.nShutdownWait;
	m_nRetryMax = stConfig.nRetryMax;
	m_nGateReloadSec = stConfig.nGateReloadSec;					// (2026-08-12 최정우 추가)
	m_nParkBuf = stConfig.nParkBuf;								// (2026-08-13 최정우 추가)
	m_nParkAccMax = stConfig.nParkAccMax;						// (2026-08-23 최정우 추가)
	m_nParkExitCnt = stConfig.nParkExitCnt;						// (2026-08-13 최정우 추가)
	m_nParkSpeedMax = stConfig.nParkSpeedMax;					// (2026-08-22 최정우 추가)
	m_nParkEntryCnt = stConfig.nParkEntryCnt;					// (2026-08-22 최정우 추가)
	m_nParkRegraceSec = stConfig.nParkRegraceSec;					// (2026-08-14 최정우 추가)
	m_nParkTtlSec = stConfig.nParkTtlSec;							// (2026-08-19 최정우 추가)
	m_nExemptRegraceSec = stConfig.nExemptRegraceSec;				// (2026-08-14 최정우 추가)
	m_strServerId = stConfig.strServerId;							// (2026-08-20 최정우 추가)
	m_nServerStatusIntervalSec = stConfig.nServerStatusIntervalSec;	// (2026-08-20 최정우 추가)

	LOGFMTI("please wait ....");

	// SQL 쿼리문 읽기
	m_pcSQLAccessor = new (std::nothrow)CSQLAccessor;
	if (m_pcSQLAccessor == nullptr)
	{
		LOGFMTE("sql read class start fail!");
		// SQL 접근자 할당 실패 시 부분 초기화 롤백 (2026-07-08 최정우 주석 추가)
		Uninitialize();
		return false;
	}

	// log 보관 관리 클래스
	m_pcLoggerManager = new (std::nothrow)CLoggerManager;
	if (!m_pcLoggerManager)
	{
		LOGFMTE("logger manager class memory allocate failed!");
		// 로거 매니저 할당 실패 시 부분 초기화 롤백 (2026-07-08 최정우 주석 추가)
		Uninitialize();
		return false;
	}

	// 로그 보관·삭제 스케줄 초기화 (2026-07-08 최정우 주석 추가)
	if (!m_pcLoggerManager->Initialize(m_strLogPath, m_nLogKeepRunTime, m_nLogKeepDay))
	{
		LOGFMTE("log manager initialize failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("logger manager initialize success!");

	// SQL load
	m_strSQLFile = stConfig.strSQLFile;
	// SQL 파일에서 쿼리문 로드 (2026-07-08 최정우 주석 추가)
	if (!m_pcSQLAccessor->Initialize(m_strSQLFile))
	{
		LOGFMTE("sql query read fail!");
		Uninitialize();
		return false;
	}

	// PROCESSING 복구 SQL (기동 시 1회)
	// 세션명으로 PROCESSING→PENDING 복구 SQL 조회 (2026-07-08 최정우 주석 추가)
	m_strRawLogRecoverSQL = m_pcSQLAccessor->GetSQL(stConfig.strRawLogRecoverSession);
	if (m_strRawLogRecoverSQL.empty())
	{
		LOGFMTE("raw gps recover query is empty!");
		Uninitialize();
		return false;
	}

	// 조회·예약 SQL (UPDATE RETURNING)
	// 세션명으로 rawgps_select SQL 조회 (2026-07-08 최정우 주석 추가)
	m_strRawLogSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strRawLogSelectSession);
	if (m_strRawLogSelectSQL.empty())
	{
		LOGFMTE("raw gps data select query is empty!");
		Uninitialize();
		return false;
	}

	// 결과 갱신 SQL
	// 세션명으로 rawgps_update SQL 조회 (2026-07-08 최정우 주석 추가)
	m_strRawLogUpdateSQL = m_pcSQLAccessor->GetSQL(stConfig.strRawLogUpdateSession);
	if (m_strRawLogUpdateSQL.empty())
	{
		LOGFMTE("raw gps data update query is empty!");
		Uninitialize();
		return false;
	}

	// 과금 대상 INSERT SQL (세션 미지정·SQL 없으면 스킵)
	if (!stConfig.strChargeInsertSession.empty())
	{
		m_strChargeInsertSQL = m_pcSQLAccessor->GetSQL(stConfig.strChargeInsertSession);
		if (m_strChargeInsertSQL.empty())
			LOGFMTW("charge_insert session=[%s] sql is empty — charge disabled",
				stConfig.strChargeInsertSession.c_str());
	}
	else
	{
		LOGFMTW("charge_insert session not configured — charge disabled");
	}

	// 트립 종료 시 trip_end_dt UPDATE SQL (세션 미지정·SQL 없으면 비활성) (2026-08-12 최정우 추가)
	if (!stConfig.strTripEndUpdateSession.empty())
	{
		m_strTripEndUpdateSQL = m_pcSQLAccessor->GetSQL(stConfig.strTripEndUpdateSession);
		if (m_strTripEndUpdateSQL.empty())
			LOGFMTW("trip_end session=[%s] sql is empty — trip_end_dt update disabled",
				stConfig.strTripEndUpdateSession.c_str());
	}
	else
	{
		LOGFMTW("trip_end session not configured — trip_end_dt update disabled");
	}

	// TTL 만료(비정상 종료) 시 개방형 미확정 레코드 마감 UPDATE SQL (2026-08-13 최정우 추가)
	if (!stConfig.strAbnormalTripEndSession.empty())
	{
		m_strAbnormalTripEndSQL = m_pcSQLAccessor->GetSQL(stConfig.strAbnormalTripEndSession);
		if (m_strAbnormalTripEndSQL.empty())
			LOGFMTW("trip_abend session=[%s] sql is empty — abnormal trip end update disabled",
				stConfig.strAbnormalTripEndSession.c_str());
	}
	else
	{
		LOGFMTW("trip_abend session not configured — abnormal trip end update disabled");
	}

	// 서버 상태(CPU/메모리) 하트비트 UPDATE SQL (세션 미지정·SQL 없으면 비활성) (2026-08-20 최정우 추가)
	if (!stConfig.strServerStatusSession.empty())
	{
		m_strServerStatusSQL = m_pcSQLAccessor->GetSQL(stConfig.strServerStatusSession);
		if (m_strServerStatusSQL.empty())
			LOGFMTW("server_status session=[%s] sql is empty — server status heartbeat disabled",
				stConfig.strServerStatusSession.c_str());
	}
	else
	{
		LOGFMTW("server_status session not configured — server status heartbeat disabled");
	}

	// 과금 게이트 전량 조회 SQL (세션 미지정·SQL 없으면 CChargeDataLoader 게이트 캐시 비활성) (2026-08-12 최정우 추가)
	if (!stConfig.strGateSelectSession.empty())
	{
		m_strGateSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strGateSelectSession);
		if (m_strGateSelectSQL.empty())
			LOGFMTW("gate_select session=[%s] sql is empty — gate cache disabled",
				stConfig.strGateSelectSession.c_str());
	}
	else
	{
		LOGFMTW("gate_select session not configured — gate cache disabled");
	}

	// 과금 구역 전량 조회 SQL (세션 미지정·SQL 없으면 CChargeDataLoader 구역 캐시 비활성) (2026-08-12 최정우 추가)
	if (!stConfig.strZoneSelectSession.empty())
	{
		m_strZoneSelectSQL = m_pcSQLAccessor->GetSQL(stConfig.strZoneSelectSession);
		if (m_strZoneSelectSQL.empty())
			LOGFMTW("zone_select session=[%s] sql is empty — zone cache disabled",
				stConfig.strZoneSelectSession.c_str());
	}
	else
	{
		LOGFMTW("zone_select session not configured — zone cache disabled");
	}

	LOGFMTI("sql accessor initialize success!");

	if (m_pcSQLAccessor != nullptr)
	{
		delete m_pcSQLAccessor;
		m_pcSQLAccessor = nullptr;
	}
	LOGFMTI("sql accessor uninitialize success!");

	// PostgreSQL DB 커넥션 풀
	m_pcPostgrePool = new (std::nothrow)CPostgrePool;
	if (m_pcPostgrePool == nullptr)
	{
		LOGFMTE("db connection pool memory allocate failed!");
		Uninitialize();
		return false;
	}
	// DB 커넥션 풀 min/max 연결 초기화 (2026-07-08 최정우 주석 추가)
	if (!m_pcPostgrePool->InitializePool(m_strDBUserID, m_strDBPasswd, m_strDBName, m_strDBHost,
			m_nDBPort, m_nDBMinConnect, m_nDBMaxConnect))
	{
		LOGFMTE("db connection pool initialize failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("db connection pool initialize success!min=[%d] max=[%d]",
		m_nDBMinConnect, m_nDBMaxConnect);

	// 기반 데이터 메모리 클래스
	m_pcDataLoader = new (std::nothrow)CDataLoader;
	if (m_pcDataLoader == nullptr)
	{
		LOGFMTE("data loader memory allocate failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("data loader memory allocate success!");

	// 맵매칭 바이너리 데이터 로더 경로·maxstep 설정 (2026-07-08 최정우 주석 추가)
	m_pcDataLoader->SetHopPenalty(m_dfHopPenalty);				// (2026-08-22 최정우 추가)
	m_pcDataLoader->Initialize(m_strDataFile, m_nMaxStep);
	// link.psf 등 기반 데이터 메모리 적재 (2026-07-08 최정우 주석 추가)
	if (!m_pcDataLoader->SetDataUpdate())
	{
		LOGFMTE("binary data load failed!file=[%s]", m_strDataFile.c_str());
		Uninitialize();
		return false;
	}
	// 적재된 그리드·링크 통계 콘솔 출력 (2026-07-08 최정우 주석 추가)
	m_pcDataLoader->SetDataInfoDisplay();

	// 과금 게이트 데이터 클래스 — gate_select SQL 미설정 시 비활성(치명적 실패 아님) (2026-08-12 최정우 추가)
	if (!m_strGateSelectSQL.empty())
	{
		m_pcChargeDataLoader = new (std::nothrow)CChargeDataLoader;
		if (m_pcChargeDataLoader == nullptr)
		{
			LOGFMTE("charge data loader memory allocate failed!");
			Uninitialize();
			return false;
		}

		if (!m_pcChargeDataLoader->Initialize(m_pcPostgrePool, m_strGateSelectSQL, m_strZoneSelectSQL))
		{
			LOGFMTE("charge data loader initialize failed!");
			Uninitialize();
			return false;
		}

		// 기동 시 1회 게이트 캐시 로드 — 실패해도 서버 기동은 계속(과금 판정만 비활성) (2026-08-12 최정우 추가)
		if (!m_pcChargeDataLoader->LoadGates())
			LOGFMTW("initial gate cache load failed — charge gate matching disabled until next reload");
		else
			LOGFMTI("gate cache initial load success!count=[%zu]", m_pcChargeDataLoader->GetGateCount());

		// 기동 시 1회 구역 캐시 로드 — 실패해도 서버 기동은 계속(zone_name 조회만 비활성) (2026-08-12 최정우 추가)
		if (!m_pcChargeDataLoader->LoadZones())
			LOGFMTW("initial zone cache load failed — zone name lookup disabled until next reload");
		else
			LOGFMTI("zone cache initial load success!count=[%zu]", m_pcChargeDataLoader->GetZoneCount());

		m_dtLastGateReload = time(nullptr);
	}

	m_pcProcessManager = new (std::nothrow)CProcessManager[m_nWorkerThread];
	if (m_pcProcessManager == nullptr)
	{
		LOGFMTE("map match process manager memory allocate failed!");
		Uninitialize();
		return false;
	}

	for (int i=0; i<m_nWorkerThread; ++i)
	{
		// config alt_* → 스레드별 ProcessManager·ContinueMapMatch 고도 보조 점수
		ALTITUDE_SCORE_CONFIG stAltitudeConfig;
		stAltitudeConfig.nGap = static_cast<sint16>(m_nAltGap);
		stAltitudeConfig.nAltPenalty = static_cast<sint16>(m_nAltPenalty);
		stAltitudeConfig.dfWeight = m_dfAltWeight;
		stAltitudeConfig.dfSlope = m_dfAltSlope;

		if (!m_pcProcessManager[i].Initialize(i, m_pcDataLoader,
				m_nCoordinateType, m_nRadius, m_dwMaxDistance,
				m_dfRadiusScale, static_cast<sint16>(m_nRadiusMin),
				static_cast<sint16>(m_nRadiusMax), stAltitudeConfig))
		{
			LOGFMTE("process manager[%d] initialize failed!", i);
			Uninitialize();
			return false;
		}
	}
	LOGFMTI("map match process manager initialize success!count=[%d]", m_nWorkerThread);

	m_pcRawLogWorker = new (std::nothrow)CRawLogWorker;
	if (m_pcRawLogWorker == nullptr)
	{
		LOGFMTE("raw log worker memory allocate failed!");
		Uninitialize();
		return false;
	}

	RAWLOG_WORKER_CONFIG stWorkerConfig;
	stWorkerConfig.pcPostgrePool = m_pcPostgrePool;
	stWorkerConfig.pcProcessManager = m_pcProcessManager;
	stWorkerConfig.pcChargeDataLoader = m_pcChargeDataLoader;			// nullptr 이면 개방형 과금 판정 비활성 (2026-08-12 최정우 추가)
	stWorkerConfig.pcDataLoader = m_pcDataLoader;						// LINK_INFO.qwOppositeLinkID 조회용 (2026-08-21 최정우 추가)
	stWorkerConfig.strUpdateSQL = m_strRawLogUpdateSQL;
	stWorkerConfig.strChargeInsertSQL = m_strChargeInsertSQL;
	stWorkerConfig.strTripEndUpdateSQL = m_strTripEndUpdateSQL;			// (2026-08-12 최정우 추가)
	stWorkerConfig.strAbnormalTripEndSQL = m_strAbnormalTripEndSQL;		// (2026-08-13 최정우 추가)
	stWorkerConfig.nWorkerThreads = m_nWorkerThread;
	stWorkerConfig.nTtlSec = m_nTtlSec;
	stWorkerConfig.nMatchTimeoutMs = m_nMatchTimeout;
	stWorkerConfig.nRetryMax = m_nRetryMax;
	stWorkerConfig.nConnRetryMax = stConfig.nConnRetryMax;
	stWorkerConfig.nConnRetryWait = stConfig.nConnRetryWait;
	stWorkerConfig.nRadiusSkip = m_nRadiusSkip;
	stWorkerConfig.dfSpeedFactor = m_dfSpeedFactor;			// (2026-07-20 최정우 추가)
	stWorkerConfig.nSpeedMargin = m_nSpeedMargin;				// (2026-07-20 최정우 추가)
	// 연속 역행 스트릭 판정 (2026-07-21 최정우 수정 — dip 판정 대체)
	stWorkerConfig.nReverseConfirm = m_nReverseConfirm;
	stWorkerConfig.nHeadingMaxDist = static_cast<int>(m_dwMaxDistance);	// [mapmatch] distance → live heading 거리 상한 (2026-07-15 최정우 추가)
	stWorkerConfig.nParkBuf = m_nParkBuf;						// (2026-08-13 최정우 추가)
	stWorkerConfig.nParkAccMax = m_nParkAccMax;					// (2026-08-23 최정우 추가)
	stWorkerConfig.nParkExitCnt = m_nParkExitCnt;				// (2026-08-13 최정우 추가)
	stWorkerConfig.nParkSpeedMax = m_nParkSpeedMax;				// (2026-08-22 최정우 추가)
	stWorkerConfig.nParkEntryCnt = m_nParkEntryCnt;				// (2026-08-22 최정우 추가)
	stWorkerConfig.nParkRegraceSec = m_nParkRegraceSec;			// (2026-08-14 최정우 추가)
	stWorkerConfig.nParkTtlSec = m_nParkTtlSec;					// (2026-08-19 최정우 추가)
	stWorkerConfig.nExemptRegraceSec = m_nExemptRegraceSec;		// (2026-08-14 최정우 추가)
	// 워커에 DB pool·ProcessManager·SQL·TTL·conn_retry 등 공유 설정 전달 (2026-07-10 최정우 추가)
	m_pcRawLogWorker->SetConfig(stWorkerConfig);

	CWorkerManager *pcWorkerManager = new (std::nothrow)CWorkerManager;
	if (pcWorkerManager == nullptr)
	{
		LOGFMTE("worker manager memory allocate failed!");
		Uninitialize();
		return false;
	}
	// Runnable 래퍼에 RawLogWorker 인스턴스 연결 (2026-07-08 최정우 주석 추가)
	pcWorkerManager->SetWorker(m_pcRawLogWorker);

	m_pcThreadPool = new (std::nothrow)CThreadPool(m_nWorkerThread, pcWorkerManager);
	if (m_pcThreadPool == nullptr)
	{
		LOGFMTE("worker 스레드 풀 memory allocate failed!");
		delete pcWorkerManager;
		Uninitialize();
		return false;
	}
	LOGFMTI("worker 스레드 풀 memory allocate success!count=[%d]", m_nWorkerThread);

	m_pcRawLogFetcher = new (std::nothrow)CRawLogFetcher;
	if (m_pcRawLogFetcher == nullptr)
	{
		LOGFMTE("raw log fetcher memory allocate failed!");
		Uninitialize();
		return false;
	}

	// Feeder에 DB pool·ThreadPool·폴링/backpressure 파라미터 설정 (2026-07-08 최정우 주석 추가)
	if (!m_pcRawLogFetcher->Initialize(m_pcPostgrePool, m_pcThreadPool,
			m_strRawLogSelectSQL, m_strRawLogUpdateSQL,
			m_nWorkerThread, m_nCoordinateType, &m_bRun,
			m_nFetchLimit, m_nFetchInterval,
			m_nQueuePauseCount, m_nQueueMaxCount,
			m_nQueueBusyMin, m_nQueueBusyMax))
	{
		LOGFMTE("raw log fetcher initialize failed!");
		Uninitialize();
		return false;
	}

	m_bRun = true;

	// 1초 주기 로그 보관 검사 타이머 쓰레드 기동 — 반드시 m_bRun=true 이후에 생성할 것
	//   (2026-08-14 최정우 수정 — 기존엔 초기화 극초반(로거 초기화 직후)에 생성해서, DB pool·
	//   대용량 도로망 데이터 로딩·워커풀 구성 등 시간이 걸리는 나머지 초기화 도중 새 스레드가
	//   while(IsRun()) 최초 진입 체크에서 m_bRun=false 를 관측하고 즉시 종료해버리는 레이스가
	//   있었음 — 그 결과 로그 보관정리도 gate_reload/zone 주기 재조회도 사실상 한 번도 안 도는
	//   상태였음("소스상 문제" 검토 중 발견). 스레드 생성을 m_bRun=true 확정 뒤로 옮겨 원천 차단)
	if (pthread_create(&m_hTimerThread, nullptr, TimerThread, reinterpret_cast<void *>(this)) != 0)
	{
		LOGFMTE("timer thread create failed!");
		Uninitialize();
		return false;
	}
	LOGFMTI("timer thread create success!");

	m_dtLastMonitorLog = time(nullptr);
	g_pcServerInstance = this;
	// SIGINT/SIGTERM 수신 시 우아한 종료 핸들러 등록 (2026-07-08 최정우 주석 추가)
	signal(SIGINT, ServerSignalHandler);
	signal(SIGTERM, ServerSignalHandler);

	// 좀비 PROCESSING 복구는 기동 필수 안전망.
	// 실패 시 재시도 후에도 실패면 기동 중단(fail-fast)
	bool bRecovered = false;
	for (int nAttempt=1; nAttempt<=RECOVER_RETRY_MAX; ++nAttempt)
	{
		// 기동 시 PROCESSING→PENDING 좀비 행 일괄 복구 (2026-07-08 최정우 주석 추가)
		if (CRawLogFetcher::RunRecover(m_pcPostgrePool, m_strRawLogRecoverSQL))
		{
			bRecovered = true;
			break;
		}

		LOGFMTW("raw log recover failed! attempt=[%d/%d]", nAttempt, RECOVER_RETRY_MAX);
		if (nAttempt < RECOVER_RETRY_MAX)
		{
			// 복구 재시도 전 대기 (단위: ms) (2026-07-11 최정우 수정)
			usleep(RECOVER_RETRY_INTERVAL * 1000);
		}
	}

	if (!bRecovered)
	{
		LOGFMTE("raw log recover failed after [%d] attempts! abort startup!", RECOVER_RETRY_MAX);
		Uninitialize();
		return false;
	}

	// RawLogFetcher 폴링 쓰레드 기동 (2026-07-08 최정우 주석 추가)
	m_pcRawLogFetcher->start();
	LOGFMTI("raw log fetcher start!limit=[%d] fetch_interval=[%d]ms "
		"queue_pause=[%d] queue_max=[%d] queue_busymin=[%d]ms queue_busymax=[%d]ms "
		"ttl_sec=[%d]s shutdown_wait=[%d]ms",
		m_nFetchLimit, m_nFetchInterval,
		m_nQueuePauseCount, m_nQueueMaxCount,
		m_nQueueBusyMin, m_nQueueBusyMax,
		m_nTtlSec, m_nShutdownWait);

	return true;
}

/**
 * @brief 메모리 반환
 * @return void
*/
void CServer::Uninitialize()
{
	// 중복 호출 가드: main 명시 호출(AppMain) + 소멸자 호출 이중 실행 방지 (2026-07-10 최정우 추가)
	if (m_bUninitialized)
		return;
	m_bUninitialized = true;

	// run 루프·Fetcher·Worker 종료 플래그 설정 (2026-07-08 최정우 주석 추가)
	RequestShutdown();

	if (m_pcRawLogFetcher != nullptr)
	{
		// Feeder 스레드 인터럽트 후 join (2026-07-08 최정우 주석 추가)
		m_pcRawLogFetcher->interrupt();
		m_pcRawLogFetcher->join();
		delete m_pcRawLogFetcher;
		m_pcRawLogFetcher = nullptr;
	}
	LOGFMTI("raw log fetcher uninitialize!");

	// #8: 워커 종료 — 진행 중 batch 완료 대기 후 큐 잔여는 PENDING release
	if (m_pcThreadPool != nullptr)
	{
		// ThreadPool 워커 종료 요청: 신규 Dequeue 중단 (2026-07-08 최정우 주석 추가)
		m_pcThreadPool->RequestShutdown();

		if (m_nShutdownWait > 0)
		{
			// 진행 중(활성) batch 만 대기. 큐 잔여는 워커가 더 이상 꺼내지 않음 (2026-07-11 수정)
			bool bActiveIdle = m_pcThreadPool->WaitForActiveIdle(m_nShutdownWait);
			if (!bActiveIdle)
			{
				LOGFMTW("shutdown active drain timeout!active=[%d] queue=[%d] drain_ms=[%d]",
					m_pcThreadPool->GetActiveThreads(),
					m_pcThreadPool->GetQueueCount(),
					m_nShutdownWait);
			}
		}

		int nQueuedBatches = m_pcThreadPool->GetQueueCount();
		if (nQueuedBatches > 0)
		{
			LOGFMTI("shutdown: queued batches=[%d] releasing PROCESSING→PENDING (not matching)",
				nQueuedBatches);
		}

		// 큐 잔여 batch PROCESSING→PENDING release (#8) (2026-07-08 최정우 주석 추가)
		DrainPendingBatchesAndRelease();

		delete m_pcThreadPool;
		m_pcThreadPool = nullptr;
	}
	LOGFMTI("worker 스레드 풀 uninitialize!");

	if (m_pcRawLogWorker != nullptr)
	{
		delete m_pcRawLogWorker;
		m_pcRawLogWorker = nullptr;
	}

	if (m_pcSQLAccessor != nullptr)
	{
		delete m_pcSQLAccessor;
		m_pcSQLAccessor = nullptr;
	}

	// timer thread join
	if (m_hTimerThread)
	{
		long int nStatus;
		// 로그 보관 타이머 쓰레드 종료 대기 (2026-07-08 최정우 주석 추가)
		pthread_join(m_hTimerThread, reinterpret_cast<void **>(&nStatus));
		m_hTimerThread = 0;
	}
	LOGFMTI("timer thread join success!");

	if (m_pcDataLoader != nullptr)
	{
		// 맵매칭 바이너리 데이터 메모리 해제 (2026-07-08 최정우 주석 추가)
		m_pcDataLoader->Uninitialize();
		delete m_pcDataLoader;
		m_pcDataLoader = nullptr;
	}
	LOGFMTI("data loader uninitialize!");

	if (m_pcChargeDataLoader != nullptr)
	{
		// 과금 게이트 캐시 메모리 해제 (2026-08-12 최정우 추가)
		m_pcChargeDataLoader->Uninitialize();
		delete m_pcChargeDataLoader;
		m_pcChargeDataLoader = nullptr;
	}
	LOGFMTI("charge data loader uninitialize!");

	if (m_pcPostgrePool != nullptr)
	{
		// DB 커넥션 풀 연결 전부 종료 (2026-07-08 최정우 주석 추가)
		m_pcPostgrePool->UninitializePool();
		delete m_pcPostgrePool;
		m_pcPostgrePool = nullptr;
	}
	LOGFMTI("db connection pool uninitialize!");

	// logger manager thread stop
	if (m_pcLoggerManager) delete m_pcLoggerManager;
	m_pcLoggerManager = nullptr;

	// GPS 정보 맵 매칭 처리 클래스
	if (m_pcProcessManager != nullptr) delete [] m_pcProcessManager;
	m_pcProcessManager = nullptr;
	LOGFMTI("map match process manager uninitialize!");

	if (g_pcServerInstance == this)
		g_pcServerInstance = nullptr;

	LOGFMTI("server uninitialize!");
}

/**
 * @brief 서버 run 루프 종료 요청
 * @return void
 */
void CServer::RequestShutdown()
{
	m_cRunMutex.lock();
	m_bRun = false;
	m_cRunCondition.broadcast();
	m_cRunMutex.unlock();
}

/**
 * @brief 종료 시 워커 큐 잔여 batch 예약 해제 (#8)
 * @return true(release 시도 완료), false(pool/worker/conn 없음)
*/
bool CServer::DrainPendingBatchesAndRelease()
{
	if (m_pcThreadPool == nullptr || m_pcRawLogWorker == nullptr || m_pcPostgrePool == nullptr)
		return false;

	vector<RAW_LOG_BATCH> vtPending;
	// 종료 시 ThreadPool 큐에 남은 batch 목록 추출 (2026-07-08 최정우 주석 추가)
	m_pcThreadPool->DrainQueuedBatches(&vtPending);

	if (vtPending.empty())
		return true;

	// drain용 DB 커넥션 풀에서 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("shutdown drain: db connection is null!pending_batches=[%d]",
			static_cast<int>(vtPending.size()));
		return false;
	}

	int nReleased = 0;
	int nFailed = 0;

	for (size_t i=0; i<vtPending.size(); ++i)
	{
		// 미처리 batch PROCESSING→PENDING 일괄 release (2026-07-08 최정우 주석 추가)
		if (m_pcRawLogWorker->ReleaseReservedBatch(pcConn, vtPending[i], -1))
			++nReleased;
		else
			++nFailed;
	}

	// drain용 DB 커넥션 풀 반환 (2026-07-08 최정우 주석 추가)
	m_pcPostgrePool->releaseConnection(pcConn);

	LOGFMTW("shutdown drain release done!batches=[%d] ok=[%d] fail=[%d]",
		static_cast<int>(vtPending.size()), nReleased, nFailed);

	return (nFailed == 0);
}

/**
 * @brief run 루프 대기 (종료 시그널 또는 타임아웃)
 * @return void
 */
void CServer::WaitForNextCycle()
{
	m_cRunMutex.lock();
	if ((m_bRun) && (!IsInterrupted()))
	{
		// run 루프 주기 대기 또는 종료 시그널 수신 (단위: ms) (2026-07-11 최정우 수정)
		m_cRunCondition.waitTimed(m_cRunMutex, SERVER_RUN_WAIT);
	}
	m_cRunMutex.unlock();
}

/**
 * @brief 워커·큐·DB 풀 상태 모니터링 (주기 DEBUG, 임계치 WARN)
 * @param[in] dtNow 현재 시각 (초)
 * @return void
 */
void CServer::LogMonitorStatus(time_t dtNow)
{
	if (m_pcThreadPool == nullptr)
		return;

	int nQueue = m_pcThreadPool->GetQueueCount();
	int nActive = m_pcThreadPool->GetActiveThreads();
	int nWaiting = m_pcThreadPool->GetWaitingThreads();

	LOGFMTD("monitor queue=[%d] active=[%d/%d] waiting=[%d]",
		nQueue, nActive, m_nWorkerThread, nWaiting);

	if (m_pcPostgrePool != nullptr)
	{
		LOGFMTD("monitor db pooled=[%d] active=[%d] available=[%d]",
			m_pcPostgrePool->getPooledConnections(),
			m_pcPostgrePool->getActiveConnections(),
			m_pcPostgrePool->getAvailableConnections());
	}

	if (nQueue >= m_nQueuePauseCount)
	{
		if (!m_bQueueWarnActive)
		{
			LOGFMTW("monitor queue backlog threshold!queue=[%d] threshold=[%d]",
				nQueue, m_nQueuePauseCount);
			m_bQueueWarnActive = true;
		}
	}
	else if (m_bQueueWarnActive)
	{
		LOGFMTI("monitor queue backlog recovered!queue=[%d]", nQueue);
		m_bQueueWarnActive = false;
	}

	bool bAllBusy = (m_nWorkerThread > 0) && (nActive >= m_nWorkerThread) && (nQueue > 0);
	if (bAllBusy)
	{
		if (!m_bAllBusyWarnActive)
		{
			LOGFMTW("monitor all workers busy with pending queue!queue=[%d] active=[%d]",
				nQueue, nActive);
			m_bAllBusyWarnActive = true;
		}
	}
	else if (m_bAllBusyWarnActive)
	{
		LOGFMTI("monitor worker load normalized!queue=[%d] active=[%d]", nQueue, nActive);
		m_bAllBusyWarnActive = false;
	}

	m_nLastQueueCount = nQueue;
	(void)dtNow;
}

/**
 * @brief Thread 실행
 * @return void
*/
void CServer::run()
{
	while (m_bRun && !IsInterrupted())
	{
		// SIGINT/SIGTERM 플래그 관찰 시 정상 스레드 컨텍스트에서 종료 요청 후 루프 탈출 (2026-07-10 최정우 추가)
		// → run() 정상 return → threadHandler 가 m_cond 브로드캐스트 → main join() 정상 복귀
		//   → pthread_cond_timedwait 강제 언와인드 없음 → 조건변수 오염/destroy hang 방지
		if (g_nShutdownRequested)
		{
			RequestShutdown();
			break;
		}

		time_t dtNow = time(nullptr);
		// 모니터 로그 주기 (단위: sec) (2026-07-11 최정우 주석 추가)
		if ((dtNow - m_dtLastMonitorLog) >= SERVER_MONITOR_INTERVAL)
		{
			// 워커·큐·DB 풀 상태 주기 모니터링 (2026-07-08 최정우 주석 추가)
			LogMonitorStatus(dtNow);
			m_dtLastMonitorLog = dtNow;
		}

		// 종료 또는 타임아웃까지 run 루프 대기 (2026-07-08 최정우 주석 추가)
		WaitForNextCycle();
	}
}

/**
 * @brief 주기적 1 초마다 실행
 * @param[in] dtNow 현재 시각 (초)
 * @return void
*/
void CServer::ProcessPeriodSec(time_t dtNow)
{
	// 설정 시각·보관일 기준 만료 로그 파일 삭제 (2026-07-08 최정우 주석 추가)
	m_pcLoggerManager->LogDeleteRun(dtNow);

	// 게이트·구역 캐시 주기 재조회 — gate_reload=0 이면 비활성. 기존 타이머 스레드(1초 주기) 재사용,
	// 별도 스레드 없음. 워커 처리량에 영향 없도록 best-effort — 실패해도 이전 캐시 유지 (2026-08-12 최정우 추가)
	if ((m_pcChargeDataLoader != nullptr) && (m_nGateReloadSec > 0) &&
		((dtNow - m_dtLastGateReload) >= m_nGateReloadSec))
	{
		if (!m_pcChargeDataLoader->LoadGates())
			LOGFMTW("gate cache reload failed — keeping previous cache");
		else
			LOGFMTI("gate cache reload success!count=[%zu]", m_pcChargeDataLoader->GetGateCount());

		if (!m_pcChargeDataLoader->LoadZones())
			LOGFMTW("zone cache reload failed — keeping previous cache");
		else
			LOGFMTI("zone cache reload success!count=[%zu]", m_pcChargeDataLoader->GetZoneCount());

		m_dtLastGateReload = dtNow;
	}

	// CPU 사용률 샘플 — 매초 누적(직전 /proc/stat 대비 델타), 하트비트 반영 시점엔 이 최신값을
	//   그대로 사용한다(하트비트 순간에만 샘플링하면 그 찰나의 값이라 대표성이 떨어짐) (2026-08-20 최정우 추가)
	UpdateCpuSample();

	// 서버 상태(CPU/메모리) 하트비트 — status_interval=0 이거나 SQL 미설정이면 비활성.
	//   best-effort(실패해도 다음 주기에 재시도, 워커 처리 방해 금지) (2026-08-20 최정우 추가)
	if ((m_nServerStatusIntervalSec > 0) && (!m_strServerStatusSQL.empty()) &&
		((dtNow - m_dtLastServerStatusUpdate) >= m_nServerStatusIntervalSec))
	{
		UpdateServerStatus();
		m_dtLastServerStatusUpdate = dtNow;
	}
}

/**
 * @brief CPU 사용률 샘플링 — /proc/stat 델타 기반, 매초 호출
 * @return void
 * @remark
 *   "cpu  user nice system idle iowait irq softirq steal ..." 누적 tick 값을 읽어 직전
 *   샘플과의 차이로 사용률(%)을 계산한다. 최초 1회는 델타를 낼 이전 값이 없어 계산을
 *   건너뛴다(m_bCpuSampleValid=false) (2026-08-20 최정우 추가)
*/
void CServer::UpdateCpuSample()
{
	FILE *fp = fopen("/proc/stat", "r");
	if (fp == nullptr)
		return;

	char szLine[256];
	memset(szLine, 0, sizeof(szLine));
	bool bRead = (fgets(szLine, sizeof(szLine), fp) != nullptr);
	fclose(fp);
	if (!bRead)
		return;

	unsigned long long qwUser=0, qwNice=0, qwSystem=0, qwIdle=0, qwIowait=0, qwIrq=0, qwSoftirq=0, qwSteal=0;
	int nMatched = sscanf(szLine, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		&qwUser, &qwNice, &qwSystem, &qwIdle, &qwIowait, &qwIrq, &qwSoftirq, &qwSteal);
	if (nMatched < 4)						// 최소 user/nice/system/idle 은 있어야 함
		return;

	unsigned long long qwIdleAll = qwIdle + qwIowait;
	unsigned long long qwTotal = qwUser + qwNice + qwSystem + qwIdleAll + qwIrq + qwSoftirq + qwSteal;

	if (m_bCpuSampleValid && (qwTotal > m_qwPrevCpuTotal))
	{
		unsigned long long qwTotalDiff = qwTotal - m_qwPrevCpuTotal;
		unsigned long long qwIdleDiff = (qwIdleAll >= m_qwPrevCpuIdle) ? (qwIdleAll - m_qwPrevCpuIdle) : 0;
		m_dfLastCpuPct = 100.0 * static_cast<double>(qwTotalDiff - qwIdleDiff) / static_cast<double>(qwTotalDiff);
	}

	m_qwPrevCpuTotal = qwTotal;
	m_qwPrevCpuIdle = qwIdleAll;
	m_bCpuSampleValid = true;
}

/**
 * @brief 메모리 사용률·용량 조회 — /proc/meminfo 기준
 * @param[out] nUsedMB 사용 메모리(MB) — MemTotal-MemAvailable
 * @param[out] nTotalMB 전체 메모리(MB)
 * @param[out] dfMemPct 사용률(%)
 * @return true(성공), false(읽기 실패)
*/
bool CServer::GetMemInfo(int& nUsedMB, int& nTotalMB, double& dfMemPct)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	if (fp == nullptr)
		return false;

	long lMemTotalKB = -1, lMemAvailableKB = -1;
	char szLine[256];
	while (fgets(szLine, sizeof(szLine), fp) != nullptr)
	{
		if (strncmp(szLine, "MemTotal:", 9) == 0)
			sscanf(szLine + 9, "%ld", &lMemTotalKB);
		else if (strncmp(szLine, "MemAvailable:", 13) == 0)
			sscanf(szLine + 13, "%ld", &lMemAvailableKB);

		if ((lMemTotalKB >= 0) && (lMemAvailableKB >= 0))
			break;
	}
	fclose(fp);

	if ((lMemTotalKB <= 0) || (lMemAvailableKB < 0))
		return false;

	nTotalMB = static_cast<int>(lMemTotalKB / 1024);
	long lUsedKB = lMemTotalKB - lMemAvailableKB;
	if (lUsedKB < 0) lUsedKB = 0;
	nUsedMB = static_cast<int>(lUsedKB / 1024);
	dfMemPct = 100.0 * static_cast<double>(lUsedKB) / static_cast<double>(lMemTotalKB);
	return true;
}

/**
 * @brief 서버 상태(CPU/메모리) 하트비트 — PROC_SERVERSTATUS 1행 UPDATE
 * @return void
 * @remark best-effort — 연결/쿼리 실패해도 에러 로그만 남기고 다음 주기에 재시도 (2026-08-20 최정우 추가)
*/
void CServer::UpdateServerStatus()
{
	if (m_strServerStatusSQL.empty() || (m_pcPostgrePool == nullptr))
		return;

	int nUsedMB = 0, nTotalMB = 0;
	double dfMemPct = 0.0;
	if (!GetMemInfo(nUsedMB, nTotalMB, dfMemPct))
	{
		LOGFMTW("server status update skipped — meminfo read failed");
		return;
	}

	char szCpuPct[32], szMemPct[32], szUsedMB[16], szTotalMB[16];
	snprintf(szCpuPct, sizeof(szCpuPct), "%.1f", m_dfLastCpuPct);
	snprintf(szMemPct, sizeof(szMemPct), "%.1f", dfMemPct);
	snprintf(szUsedMB, sizeof(szUsedMB), "%d", nUsedMB);
	snprintf(szTotalMB, sizeof(szTotalMB), "%d", nTotalMB);

	PGconn *pcConn = m_pcPostgrePool->getConnection();
	if (pcConn == nullptr)
	{
		LOGFMTE("server status update — get connection failed!");
		return;
	}

	const char *pszParams[5] = { m_strServerId.c_str(), szCpuPct, szMemPct, szUsedMB, szTotalMB };
	PGresult *pcResult = PQexecParams(pcConn, m_strServerStatusSQL.c_str(),
		5, nullptr, pszParams, nullptr, nullptr, 0);

	if ((pcResult == nullptr) || (PQresultStatus(pcResult) != PGRES_COMMAND_OK))
	{
		LOGFMTW("server status update failed!error=[%s]", PQerrorMessage(pcConn));
	}
	else
	{
		LOGFMTI("server status update success!id=[%s] cpu=[%s%%] mem=[%s%%] used=[%sMB] total=[%sMB]",
			m_strServerId.c_str(), szCpuPct, szMemPct, szUsedMB, szTotalMB);
	}

	if (pcResult != nullptr)
		PQclear(pcResult);
	m_pcPostgrePool->releaseConnection(pcConn);
}