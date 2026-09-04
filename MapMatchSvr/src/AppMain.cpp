/**
 * @file AppMain.cpp
 * @brief main 함수 소스 파일
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <string>
#include "Config.h"
#include "ConfigDefaults.h"
#include "log4z.h"
#include "IniReader.h"
#include "Util.h"
#include "Server.h"
#include "SingleInstanceLock.h"

using namespace zsummer::log4z;
using namespace std;

// 중복 기동 방지용 PID 파일 경로 — run_svr.sh 가 이미 같은 경로를 관례로 쓰고 있어(BIN_DIR 기준
//   상대경로, cd 로 이미 실행 디렉터리로 이동한 뒤 기동) 그대로 재사용한다. run_svr.sh 자체의
//   중복 감지(readlink /proc/$pid/exe = MM_BIN 비교)는 make install 로 바이너리가 교체되면
//   exe 경로가 "(deleted)"로 나와 실패하는 걸로 이미 확인됨(2026-08-21) — 셸 스크립트가 아니라
//   프로세스 자신이 직접 파일 잠금(flock)을 거는 방식이라야 바이너리 교체·비정상 종료(kill -9)에도
//   항상 정확하다(잠금은 OS가 fd 를 들고 있는 프로세스가 살아있는 동안만 유효, 프로세스가 죽으면
//   fd 가 자동으로 닫히며 즉시 풀림 — PID 파일 존재 여부만으로 판단하는 것보다 훨씬 견고)
//   (2026-09-04 최정우 추가, 사용자 지시)
static const char *SINGLE_INSTANCE_PID_FILE = "./MapMatchSvr.pid";

// 잠금 fd — 파일 스코프 static(함수 로컬 아님). AppMain.cpp(main) 과 Server.cpp(1초 타이머)
//   양쪽에서 같은 잠금 상태를 봐야 해서(IsSingleInstanceLockIntact 가 재점유 시 이 값을
//   갱신) 함수 안이 아니라 번역 단위 스코프에 둔다. 절대 닫지 않음 — 프로세스 종료 시 OS 가
//   자동 회수 (2026-09-04 최정우 추가)
static int s_nLockFd = -1;

/**
 * @brief 중복 기동 방지 — 실행 디렉터리의 PID 파일에 배타적 flock 을 시도해 이미 살아있는
 *   인스턴스가 있으면 즉시 실패시킨다. 로거 기동 전(config.ini 조차 읽기 전)에 가장 먼저
 *   호출해야 무거운 초기화(DB 접속·지도 데이터 로딩 등)를 시작하기 전에 빠르게 실패한다.
 * @return true(잠금 획득 성공, 유일한 인스턴스), false(이미 다른 인스턴스가 실행 중이거나 오류)
 * @remark 성공 시 연 fd 를 프로세스 생명 주기 내내 열어둔 채로 반환한다 — fd 를 닫으면 그
 *   순간 잠금이 풀려버리므로 프로세스가 살아있는 동안은 절대 close() 하면 안 된다. 정상/비정상
 *   종료 모두 OS 가 프로세스 종료 시 자동으로 fd 를 닫아 잠금을 해제하므로 별도 해제 코드는
 *   필요 없다 (2026-09-04 최정우 추가)
*/
bool AcquireSingleInstanceLock()
{
	s_nLockFd = open(SINGLE_INSTANCE_PID_FILE, O_CREAT | O_RDWR, 0644);
	if (s_nLockFd < 0)
	{
		fprintf(stderr, "pid file open failed!file=[%s] errno=[%d:%s]\n",
			SINGLE_INSTANCE_PID_FILE, errno, strerror(errno));
		return false;
	}

	if (flock(s_nLockFd, LOCK_EX | LOCK_NB) != 0)
	{
		char szPrevPid[32] = {0,};
		ssize_t nRead = read(s_nLockFd, szPrevPid, sizeof(szPrevPid) - 1);
		if (nRead > 0)
		{
			// 개행 등 뒤쪽 공백류 제거 — 그대로 두면 에러 메시지에 줄바꿈이 섞여 출력됨
			while ((nRead > 0) && ((szPrevPid[nRead - 1] == '\n') || (szPrevPid[nRead - 1] == '\r')))
				--nRead;
			szPrevPid[nRead] = '\0';
		}
		fprintf(stderr, "another MapMatchSvr instance is already running!pid_file=[%s] pid=[%s]\n",
			SINGLE_INSTANCE_PID_FILE, (szPrevPid[0] != '\0') ? szPrevPid : "?");
		close(s_nLockFd);
		s_nLockFd = -1;
		return false;
	}

	// 잠금 획득 성공 — 파일 내용을 이번 프로세스 PID 로 갱신(참고/디버깅용, run_svr.sh 도 이미
	//   동일 파일에 같은 값을 써왔으므로 형식 호환)
	if (ftruncate(s_nLockFd, 0) != 0) { /* 참고용 표시일 뿐, 실패해도 잠금 자체는 유효 */ }
	char szPid[32];
	int nPidLen = snprintf(szPid, sizeof(szPid), "%d\n", static_cast<int>(getpid()));
	lseek(s_nLockFd, 0, SEEK_SET);
	if (write(s_nLockFd, szPid, static_cast<size_t>(nPidLen)) < 0) { /* 참고용 표시일 뿐 */ }

	return true;
}

/**
 * @brief 잠금 중인 PID 파일이 여전히 그 경로에 그대로 존재하는지(삭제·교체되지 않았는지)
 *   fstat(연 fd)·stat(현재 경로) 의 device+inode 를 비교해 확인한다.
 * @remark flock 은 파일 "경로"가 아니라 fd 가 가리키는 inode 에 걸리므로, 이 프로세스가 잠근
 *   뒤 외부에서(rm 등) 그 경로의 파일이 삭제되면 — 우리 fd 는 여전히(이제는 이름 없는) 옛
 *   inode 를 유효하게 잠그고 있지만, 그 경로에 O_CREAT 로 새로 열리는 다음 프로세스는 완전히
 *   다른 새 inode 를 잠그게 돼 서로 충돌하지 않고 둘 다 성공해버린다 — 중복 기동 방지가
 *   뚫리는 것을 실측으로 재현·확인(2026-09-04, rm 직후 두 번째 인스턴스가 정상 기동돼 기존
 *   인스턴스와 함께 같은 DB 를 동시에 처리하는 것까지 확인). 이 함수는 그 구멍을 완전히
 *   막지는 못하지만(재발견까지의 짧은 시간차 동안은 여전히 취약) 최대 1회 호출 주기(Server.cpp
 *   1초 타이머)만큼으로 창을 좁히고, 발생 시 강한 경고 로그를 남겨 운영자가 "PID 파일이 왜
 *   없어졌는지"를 바로 알아챌 수 있게 한다 (2026-09-04 최정우 추가, 사용자 지시)
 * @return true(정상), false(경로가 사라져 있었음 — 즉시 재점유 시도함)
*/
bool IsSingleInstanceLockIntact()
{
	if (s_nLockFd < 0)
		return false;

	struct stat stFdStat, stPathStat;
	if ((fstat(s_nLockFd, &stFdStat) == 0)
		&& (stat(SINGLE_INSTANCE_PID_FILE, &stPathStat) == 0)
		&& (stFdStat.st_dev == stPathStat.st_dev)
		&& (stFdStat.st_ino == stPathStat.st_ino))
	{
		return true;			// 경로가 여전히 우리가 잠근 그 inode 를 가리킴 — 정상
	}

	// 경로가 사라졌거나(삭제) 다른 파일로 바뀜 — 같은 경로에 즉시 새로 만들어 재점유한다.
	//   원래 fd(s_nLockFd)의 잠금은 그대로 유효하므로 잃는 게 없다 — 이건 "다음 중복 기동
	//   시도가 잠글 대상"을 다시 만들어주는 보강일 뿐
	int nNewFd = open(SINGLE_INSTANCE_PID_FILE, O_CREAT | O_RDWR, 0644);
	if (nNewFd >= 0)
	{
		flock(nNewFd, LOCK_EX | LOCK_NB);
		char szPid[32];
		int nPidLen = snprintf(szPid, sizeof(szPid), "%d\n", static_cast<int>(getpid()));
		if (write(nNewFd, szPid, static_cast<size_t>(nPidLen)) < 0) { /* 참고용 표시일 뿐 */ }
		close(s_nLockFd);
		s_nLockFd = nNewFd;
	}

	return false;
}

/**
 * @brief 초기화 함수
 * @param[in] config_file 환경설정 파일 경로
 * @param[in,out] pstConfig 프로세스 구동 환경 설정 값
 * @return true(성공), false(실패)
*/
bool Initialize(string config_file, PCONFIG pstConfig)
{
	if (access(config_file.c_str(), F_OK) != 0)
	{
		perror("config.ini file not found!\n");
		return false;
	}

	CIniReader cIniReader(config_file.c_str());
	if (!cIniReader.Open())
	{
		perror("config file is not found!\n");
		return false;
	}

	// [log] (2026-07-11 최정우 주석 추가)
	// [log] path (단위: 경로) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("log", "path", CFG_DEF_PATH, pstConfig->strLogPath);
	if (pstConfig->strLogPath.empty())
	{
		perror("log path is empty!\n");
		return false;
	}

	// [log] level (단위: 레벨) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "level", CFG_DEF_LEVEL, pstConfig->nLogLevel);
	switch (pstConfig->nLogLevel)
	{
	case 0: pstConfig->nLogLevel = LOG_LEVEL_TRACE; break;
	case 1: pstConfig->nLogLevel = LOG_LEVEL_DEBUG; break;
	case 2: pstConfig->nLogLevel = LOG_LEVEL_INFO; break;
	case 3: pstConfig->nLogLevel = LOG_LEVEL_WARN; break;
	case 4: pstConfig->nLogLevel = LOG_LEVEL_ERROR; break;
	default: pstConfig->nLogLevel = LOG_LEVEL_INFO; break;
	}

	// [log] runtime (단위: 시) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "runtime", CFG_DEF_RUNTIME, pstConfig->nLogKeepRunTime);
	if (pstConfig->nLogKeepRunTime > 23)
	{
		perror("log keep runtime is invalid!\n");
		return false;
	}
	if (pstConfig->nLogKeepRunTime < 0)
		pstConfig->nLogKeepRunTime = UNUSE_LOG_KEEP;

	// [log] keepday (단위: 일) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("log", "keepday", CFG_DEF_KEEPDAY, pstConfig->nLogKeepDay);
	if (pstConfig->nLogKeepRunTime > UNUSE_LOG_KEEP && pstConfig->nLogKeepDay <= 0)
	{
		perror("log keep day is invalid!\n");
		return false;
	}

	// [database] (2026-07-11 최정우 주석 추가)
	// [database] host (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "host", "", pstConfig->strDBHost);
	if (pstConfig->strDBHost.empty())
	{
		perror("db host is empty!\n");
		return false;
	}

	// [database] port (단위: 포트) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "port", CFG_DEF_PORT, pstConfig->nDBPort);
	if ((pstConfig->nDBPort <= 0) || (pstConfig->nDBPort > 65535))
	{
		perror("db port is invalid!\n");
		return false;
	}

	// [database] name (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "name", "", pstConfig->strDBName);
	if (pstConfig->strDBName.empty())
	{
		perror("db name is empty!\n");
		return false;
	}

	// [database] userid (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "userid", "", pstConfig->strDBUserID);
	if (pstConfig->strDBUserID.empty())
	{
		perror("db user id is empty!\n");
		return false;
	}

	// [database] password (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("database", "password", "", pstConfig->strDBPasswd);
	if (pstConfig->strDBPasswd.empty())
	{
		perror("db user password is empty!\n");
		return false;
	}

	// [database] minconnect (단위: 최소 연결) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "minconnect", CFG_DEF_MINCONNECT, pstConfig->nDBMinConnect);
	// [database] maxconnect (0=자동) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("database", "maxconnect", CFG_DEF_MAXCONNECT, pstConfig->nDBMaxConnect);
	// [database] retrymax (단위: 최대 재시도) (2026-07-11 최정우 주석 추가, 2026-08-21 최정우 수정 — key명 conn_retry_max→retrymax)
	cIniReader.GetProfileInt("database", "retrymax", CFG_DEF_CONN_RETRY_MAX, pstConfig->nConnRetryMax);
	// [database] retrywait (단위: ms) (2026-07-11 최정우 주석 추가, 2026-08-21 최정우 수정 — key명 conn_retry_wait→retrywait)
	cIniReader.GetProfileInt("database", "retrywait", CFG_DEF_CONN_RETRY_WAIT, pstConfig->nConnRetryWait);
	if (pstConfig->nConnRetryMax < 1)
		pstConfig->nConnRetryMax = CFG_DEF_CONN_RETRY_MAX;
	if (pstConfig->nConnRetryWait < 0)
		pstConfig->nConnRetryWait = 0;

	// [query] (2026-07-11 최정우 주석 추가)
	// [query] file (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("query", "file", "", pstConfig->strSQLFile);
	if (pstConfig->strSQLFile.empty())
	{
		perror("sql file is empty!\n");
		return false;
	}

	// [sql] (2026-07-11 최정우 주석 추가)
	// [sql] rawlog_recover (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_recover", "", pstConfig->strRawLogRecoverSession);
	if (pstConfig->strRawLogRecoverSession.empty())
	{
		perror("gps data recover sql session is empty!\n");
		return false;
	}

	// [sql] rawlog_select (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_select", "", pstConfig->strRawLogSelectSession);
	if (pstConfig->strRawLogSelectSession.empty())
	{
		perror("gps data select sql session is empty!\n");
		return false;
	}

	// [sql] rawlog_update (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "rawlog_update", "", pstConfig->strRawLogUpdateSession);
	if (pstConfig->strRawLogUpdateSession.empty())
	{
		perror("gps data update sql session is empty!\n");
		return false;
	}

	// [sql] charge_insert (선택) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("sql", "charge_insert", "", pstConfig->strChargeInsertSession);
	// [sql] gate_select (선택, 비어 있으면 CChargeDataLoader 게이트 캐시 비활성) (2026-08-12 최정우 추가)
	cIniReader.GetProfileStr("sql", "gate_select", "", pstConfig->strGateSelectSession);
	// [sql] zone_select (선택, 비어 있으면 CChargeDataLoader 구역 캐시 비활성) (2026-08-12 최정우 추가)
	cIniReader.GetProfileStr("sql", "zone_select", "", pstConfig->strZoneSelectSession);
	// [sql] parkfine_select (선택, 비어 있으면 주정차 체류시간 임계 비활성) (2026-08-24 최정우 추가)
	cIniReader.GetProfileStr("sql", "parkfine_select", "", pstConfig->strParkFineSelectSession);
	// [sql] trip_end (선택, 비어 있으면 trip_end_dt UPDATE 비활성) (2026-08-12 최정우 추가)
	cIniReader.GetProfileStr("sql", "trip_end", "", pstConfig->strTripEndUpdateSession);
	// [sql] trip_abend (선택, 비어 있으면 비활성) (2026-08-13 최정우 추가, 2026-08-21 최정우 수정 — key명 abnormal_trip_end→trip_abend)
	cIniReader.GetProfileStr("sql", "trip_abend", "", pstConfig->strAbnormalTripEndSession);
	// [sql] trip_seqoff/trip_seqfin (선택, 비어 있으면 TRIP_SEQ 재부여 비활성) (2026-09-03 최정우 추가)
	cIniReader.GetProfileStr("sql", "trip_seqoff", "", pstConfig->strTripSeqOffSession);
	cIniReader.GetProfileStr("sql", "trip_seqfin", "", pstConfig->strTripSeqFinSession);
	// [sql] server_status (선택, 비어 있으면 서버 상태 하트비트 비활성) (2026-08-20 최정우 추가)
	cIniReader.GetProfileStr("sql", "server_status", "", pstConfig->strServerStatusSession);
	// [sql] stale_recover (선택, 비어 있으면 좀비 PROCESSING 운영 중 회수 비활성) (2026-08-29 최정우 추가)
	cIniReader.GetProfileStr("sql", "stale_recover", "", pstConfig->strStaleRecoverSession);
	// [server] id — PROC_SERVERSTATUS.SERVER_ID (2026-08-20 최정우 추가)
	cIniReader.GetProfileStr("server", "id", CFG_DEF_SERVER_ID, pstConfig->strServerId);
	// [server] status_interval (단위: sec, 0=비활성) (2026-08-20 최정우 추가)
	cIniReader.GetProfileInt("server", "status_interval", CFG_DEF_STATUS_INTVL, pstConfig->nServerStatusIntervalSec);
	if (pstConfig->nServerStatusIntervalSec < 0)
		pstConfig->nServerStatusIntervalSec = CFG_DEF_STATUS_INTVL;
	// [charge] gate_reload (단위: sec, 0=재조회 없음) (2026-08-12 최정우 추가)
	cIniReader.GetProfileInt("charge", "gate_reload", CFG_DEF_GATE_RELOAD, pstConfig->nGateReloadSec);
	if (pstConfig->nGateReloadSec < 0)
		pstConfig->nGateReloadSec = CFG_DEF_GATE_RELOAD;
	// [server] stale_sec (단위: sec, 0=비활성) (2026-08-29 최정우 추가)
	cIniReader.GetProfileInt("server", "stale_sec", CFG_DEF_STALE_SEC, pstConfig->nStaleSec);
	if (pstConfig->nStaleSec < 0)
		pstConfig->nStaleSec = CFG_DEF_STALE_SEC;
	// [charge] park_pad (단위: m) — 구역판정 시 폴리곤 바깥으로 확장 허용하는 최대 거리(ACCURACY_M 캡) (2026-08-13 최정우 추가, 2026-09-03 park_buf 에서 개명)
	cIniReader.GetProfileInt("charge", "park_pad", CFG_DEF_PARK_PAD, pstConfig->nParkPad);
	// [charge] park_accmax (단위: m) — 주정차 판정 좌표 정확도 상한, 0=비활성 (2026-08-23 최정우 추가)
	cIniReader.GetProfileInt("charge", "park_accmax", CFG_DEF_PARK_ACCMAX, pstConfig->nParkAccMax);
	// [mapmatch] ignore_rawvld — RAW_VLD 무시 전량 매칭(검증용, 기본 0) (2026-08-23 최정우 추가)
	cIniReader.GetProfileInt("mapmatch", "ignore_rawvld", CFG_DEF_IGNORE_RAWVLD, pstConfig->nIgnoreRawVld);
	if (pstConfig->nParkPad < 0)
		pstConfig->nParkPad = CFG_DEF_PARK_PAD;
	// [charge] park_exitcnt — 구역 이탈 확정 연속 GPS 건수(디바운스) (2026-08-13 최정우 추가)
	cIniReader.GetProfileInt("charge", "park_exitcnt", CFG_DEF_PARK_EXITCNT, pstConfig->nParkExitCnt);

	// [charge] node_exitcnt — 일반도로(NODE_STEP) 이탈 확정 연속 GPS 건수(디바운스). 순간
	//   오매칭 1틱으로 세션이 쪼개지는 것 방지 (2026-08-24 최정우 추가)
	cIniReader.GetProfileInt("charge", "node_exitcnt", CFG_DEF_NODE_EXITCNT, pstConfig->nNodeExitCnt);
	if (pstConfig->nNodeExitCnt < 1)
		pstConfig->nNodeExitCnt = CFG_DEF_NODE_EXITCNT;

	// [charge] park_speedmax (단위: km/h) — 이 속도 이하일 때만 주정차 판정. 실측(21트립) 결과
	//   정차 구간 최대속도 0~5km/h, 통과 구간 12~40km/h 로 완전히 분리됨 (2026-08-22 최정우 추가)
	cIniReader.GetProfileInt("charge", "park_speedmax", CFG_DEF_PARK_SPEEDMAX, pstConfig->nParkSpeedMax);

	// [charge] park_entrycnt — 조건을 연속으로 이만큼 충족해야 세션 개시. 1~2점(0~3초)짜리는
	//   체류시간 산출이 불가능하고 GPS 튐과 구분되지 않아 제외 (2026-08-22 최정우 추가)
	cIniReader.GetProfileInt("charge", "park_entrycnt", CFG_DEF_PARK_ENTRYCNT, pstConfig->nParkEntryCnt);
	if (pstConfig->nParkExitCnt < 1)
		pstConfig->nParkExitCnt = CFG_DEF_PARK_EXITCNT;
	// [charge] park_regrace (단위: sec) — 재진입 유예시간 (2026-08-14 최정우 추가)
	cIniReader.GetProfileInt("charge", "park_regrace", CFG_DEF_PARK_REGRACE, pstConfig->nParkRegraceSec);
	if (pstConfig->nParkRegraceSec < 0)
		pstConfig->nParkRegraceSec = CFG_DEF_PARK_REGRACE;
	// [charge] park_ttl (단위: sec) — 마지막 신뢰(RAW_VLD=true) 확인 후 좌표 없이 강제 마감까지의
	//   시간(0=비활성) (2026-08-19 최정우 추가)
	cIniReader.GetProfileInt("charge", "park_ttl", CFG_DEF_PARK_TTL, pstConfig->nParkTtlSec);
	if (pstConfig->nParkTtlSec < 0)
		pstConfig->nParkTtlSec = CFG_DEF_PARK_TTL;
	// [charge] exempt_regrace (단위: sec) — 재진입 유예시간 (2026-08-14 최정우 추가)
	cIniReader.GetProfileInt("charge", "exempt_regrace", CFG_DEF_EXEMPT_REGRACE, pstConfig->nExemptRegraceSec);
	if (pstConfig->nExemptRegraceSec < 0)
		pstConfig->nExemptRegraceSec = CFG_DEF_EXEMPT_REGRACE;

	// [feeder] (2026-07-11 최정우 주석 추가)
	// [feeder] limit (단위: 건수) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "limit", CFG_DEF_LIMIT, pstConfig->nFetchLimit);
	// [feeder] fetch_interval (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("feeder", "fetch_interval", CFG_DEF_FETCH_INTVL, pstConfig->nFetchInterval);
	// [feeder] queue_pause (단위: 건수) (2026-07-11 최정우 주석 추가, 2026-08-21 최정우 수정 — key명 queue_pause_count→q_pause→queue_pause)
	cIniReader.GetProfileInt("feeder", "queue_pause", CFG_DEF_Q_PAUSE_CNT, pstConfig->nQueuePauseCount);
	// [feeder] queue_max (단위: 건수) (2026-07-11 최정우 주석 추가, 2026-08-21 최정우 수정 — key명 queue_max_count→q_max→queue_max)
	cIniReader.GetProfileInt("feeder", "queue_max", CFG_DEF_Q_MAX_CNT, pstConfig->nQueueMaxCount);
	// [feeder] queue_busymin (단위: ms) (2026-07-11 최정우 주석 추가, 2026-08-21 최정우 수정 — key명 queue_busy_min→q_busymin→queue_busymin)
	cIniReader.GetProfileInt("feeder", "queue_busymin", CFG_DEF_Q_BUSY_MIN, pstConfig->nQueueBusyMin);
	// [feeder] queue_busymax (단위: ms) (2026-07-11 최정우 주석 추가, 2026-08-21 최정우 수정 — key명 queue_busy_max→q_busymax→queue_busymax)
	cIniReader.GetProfileInt("feeder", "queue_busymax", CFG_DEF_Q_BUSY_MAX, pstConfig->nQueueBusyMax);
	if (pstConfig->nFetchLimit <= 0)
		pstConfig->nFetchLimit = CFG_DEF_LIMIT;
	if (pstConfig->nFetchInterval < 0)
		pstConfig->nFetchInterval = CFG_DEF_FETCH_INTVL;
	if (pstConfig->nQueuePauseCount <= 0)
		pstConfig->nQueuePauseCount = CFG_DEF_Q_PAUSE_CNT;
	if (pstConfig->nQueueMaxCount < pstConfig->nQueuePauseCount)
		pstConfig->nQueueMaxCount = pstConfig->nQueuePauseCount;
	if (pstConfig->nQueueBusyMin < pstConfig->nFetchInterval)
		pstConfig->nQueueBusyMin = pstConfig->nFetchInterval;
	if (pstConfig->nQueueBusyMax < pstConfig->nQueueBusyMin)
		pstConfig->nQueueBusyMax = pstConfig->nQueueBusyMin;

	// [worker] (2026-07-11 최정우 주석 추가)
	// [worker] ttl_sec (단위: sec) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("worker", "ttl_sec", CFG_DEF_TTL, pstConfig->nTtlSec);
	// [worker] shutdown_wait (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("worker", "shutdown_wait", CFG_DEF_SHUTDOWN_WAIT, pstConfig->nShutdownWait);
	// [worker] retry_max (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("worker", "retry_max", CFG_DEF_RETRY_MAX, pstConfig->nRetryMax);
	if (pstConfig->nRetryMax < 0)
		pstConfig->nRetryMax = 0;
	if (pstConfig->nTtlSec < 0)
		pstConfig->nTtlSec = 0;
	if (pstConfig->nShutdownWait < 0)
		pstConfig->nShutdownWait = 0;

	// [threads] (2026-07-11 최정우 주석 추가)
	// [threads] count (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("threads", "count", CFG_DEF_COUNT, pstConfig->nThreads);
	if (pstConfig->nThreads <= 0)
	{
		perror("thread count is invalid!\n");
		return false;
	}

	// maxconnect 자동 보정 — threads.count 참조 (2026-07-11 최정우 주석 추가)
	if (pstConfig->nDBMaxConnect <= 0)
		pstConfig->nDBMaxConnect = pstConfig->nThreads + 2;
	if (pstConfig->nDBMaxConnect < (pstConfig->nThreads + 1))
		pstConfig->nDBMaxConnect = pstConfig->nThreads + 1;
	if (pstConfig->nDBMinConnect < 1)
		pstConfig->nDBMinConnect = 1;
	if (pstConfig->nDBMinConnect > pstConfig->nDBMaxConnect)
		pstConfig->nDBMinConnect = pstConfig->nDBMaxConnect;

	// [data] (2026-07-11 최정우 주석 추가)
	// [data] file (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileStr("data", "file", "", pstConfig->strDataFile);
	if (pstConfig->strDataFile.empty())
	{
		perror("data binary file is empty!\n");
		return false;
	}

	// [mapmatch] (2026-07-11 최정우 주석 추가)
	// [mapmatch] geodetic (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "geodetic", CFG_DEF_GEODETIC, pstConfig->nGeodetic);
	if ((pstConfig->nGeodetic <= 0) || (pstConfig->nGeodetic > 4))
		pstConfig->nGeodetic = CFG_DEF_GEODETIC;

	// [mapmatch] radius (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius", CFG_DEF_RADIUS, pstConfig->nRadius);
	if ((pstConfig->nRadius < 0) || (pstConfig->nRadius > 250))
		pstConfig->nRadius = CFG_DEF_RADIUS;

	// [mapmatch] radius_scale (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileDouble("mapmatch", "radius_scale", CFG_DEF_RADIUS_SCALE, pstConfig->dfRadiusScale);
	if (pstConfig->dfRadiusScale <= 0.0)
		pstConfig->dfRadiusScale = CFG_DEF_RADIUS_SCALE;

	// [mapmatch] radius_min (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius_min", CFG_DEF_RADIUS_MIN, pstConfig->nRadiusMin);
	// [mapmatch] radius_max (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius_max", CFG_DEF_RADIUS, pstConfig->nRadiusMax);
	if (pstConfig->nRadiusMin <= 0)
		pstConfig->nRadiusMin = CFG_DEF_RADIUS_MIN;
	if (pstConfig->nRadiusMax < pstConfig->nRadiusMin)
		pstConfig->nRadiusMax = pstConfig->nRadius;

	// [mapmatch] radius_skip (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "radius_skip", CFG_DEF_RADIUS_SKIP, pstConfig->nRadiusSkip);
	if (pstConfig->nRadiusSkip < 0)
		pstConfig->nRadiusSkip = CFG_DEF_RADIUS_SKIP;

	// [mapmatch] alt_gap (단위: m) (2026-07-21 최정우 수정 — altitude_gap 이름 변경)
	cIniReader.GetProfileInt("mapmatch", "alt_gap", CFG_DEF_ALT_GAP, pstConfig->nAltGap);
	// [mapmatch] alt_penalty (양수=페널티·음수=보너스) (2026-07-21 최정우 수정 — altitude_bonus/altitude_penalty 통합)
	cIniReader.GetProfileInt("mapmatch", "alt_penalty", CFG_DEF_ALT_PENALTY, pstConfig->nAltPenalty);

	// [mapmatch] alt_weight (2026-07-21 최정우 수정 — altitude_weight 이름 변경)
	cIniReader.GetProfileDouble("mapmatch", "alt_weight", CFG_DEF_ALT_WEIGHT, pstConfig->dfAltWeight);
	if (pstConfig->dfAltWeight < 0.0)
		pstConfig->dfAltWeight = CFG_DEF_ALT_WEIGHT;

	// [mapmatch] alt_slope (2026-07-21 최정우 수정 — altitude_slope 이름 변경)
	cIniReader.GetProfileDouble("mapmatch", "alt_slope", CFG_DEF_ALT_SLOPE, pstConfig->dfAltSlope);
	if (pstConfig->dfAltSlope < 0.0)
		pstConfig->dfAltSlope = CFG_DEF_ALT_SLOPE;

	// [mapmatch] reverse_confirm — 연속 역행 확정 포인트 수. 미만이면 노이즈로 보고 SKIP·앵커 고정 (2026-07-21 최정우 추가)
	cIniReader.GetProfileInt("mapmatch", "reverse_confirm", CFG_DEF_REVERSE_CONFIRM, pstConfig->nReverseConfirm);
	if (pstConfig->nReverseConfirm <= 0)
		pstConfig->nReverseConfirm = CFG_DEF_REVERSE_CONFIRM;

	// [mapmatch] opp_streakmax — 왕복분리 반대편 링크 연속 오매칭 허용 틱 수(저속 구간 방위각 미반영
	//   + 반대편 링크 근접이 겹칠 때 대비). 이 안에 원래 링크로 복귀하면 그동안의 틱을 SKIP 재기록
	//   (2026-08-24 최정우 추가)
	cIniReader.GetProfileInt("mapmatch", "opp_streakmax", CFG_DEF_OPP_STREAKMAX, pstConfig->nOppStreakMax);
	if (pstConfig->nOppStreakMax <= 0)
		pstConfig->nOppStreakMax = CFG_DEF_OPP_STREAKMAX;

	// [mapmatch] speed_factor/margin — 이동거리 환산속도와 SPEED_KMH 정합성 SKIP 판정 (2026-07-20 최정우 추가)
	cIniReader.GetProfileDouble("mapmatch", "speed_factor", CFG_DEF_SPEED_FACTOR, pstConfig->dfSpeedFactor);
	if (pstConfig->dfSpeedFactor < 0.0)
		pstConfig->dfSpeedFactor = CFG_DEF_SPEED_FACTOR;
	cIniReader.GetProfileInt("mapmatch", "speed_margin", CFG_DEF_SPEED_MARGIN, pstConfig->nSpeedMargin);
	if (pstConfig->nSpeedMargin < 0)
		pstConfig->nSpeedMargin = CFG_DEF_SPEED_MARGIN;

	if (pstConfig->nAltGap < 0)
		pstConfig->nAltGap = 0;
	// nAltPenalty 는 부호 자체가 의미(양수=페널티·음수=보너스)이므로 하한 clamp 없음 (2026-07-21 최정우 수정)

	// [mapmatch] maxstep (필수 >0) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "maxstep", 0, pstConfig->nMaxStep);

	// [mapmatch] hoppenalty — 후보가 직전 링크에서 depth(hop) 를 건너뛴 만큼 가산할 비용(m).
	//   0=비활성. 값이 클수록 "가깝지만 멀리 돌아가야 하는" 후보가 배제된다 (2026-08-22 최정우 추가)
	cIniReader.GetProfileDouble("mapmatch", "hoppenalty", MM_HOP_PENALTY, pstConfig->dfHopPenalty);
	// [mapmatch] hoppenalty_lenratio — 짧은 링크에서 벌점을 깎는 비율, 0=비활성 (2026-08-23 최정우 추가)
	cIniReader.GetProfileDouble("mapmatch", "hoppenalty_lenratio", MM_HOP_LEN_RATIO, pstConfig->dfHopLenRatio);
	if (pstConfig->dfHopPenalty < 0.0)
		pstConfig->dfHopPenalty = 0.0;
	if (pstConfig->nMaxStep <= 0)
	{
		perror("map match is max step is invalid!\n");
		return false;
	}

	// [mapmatch] distance (단위: m) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "distance", CFG_DEF_DISTANCE, pstConfig->nDistance);
	if (pstConfig->nDistance <= 0)
		pstConfig->nDistance = CFG_DEF_DISTANCE;

	// [mapmatch] timeout (단위: ms) (2026-07-11 최정우 주석 추가)
	cIniReader.GetProfileInt("mapmatch", "timeout", CFG_DEF_TIMEOUT, pstConfig->nMatchTimeout);
	if (pstConfig->nMatchTimeout < 0)
		pstConfig->nMatchTimeout = 0;

	return true;
}

/**
 * @brief 기동 시 로딩된 환경설정(config.ini) 값을 config.ini 섹션 단위로 묶어 로그로 남긴다.
 *   로거가 기동한 뒤(ILog4zManager::start() 이후)에만 호출 가능 — Initialize() 안에서는 로거가
 *   아직 없어 perror 만 쓸 수 있다. 선택 항목(빈 문자열이면 해당 기능 비활성)은 "(비활성)"으로
 *   표시해 실제로 어떤 기능이 켜져 있는지 한눈에 보이게 한다 (2026-09-04 최정우 추가, 사용자 지시)
 * @param[in] stConfig 로딩된 환경설정 값
 * @return void
*/
static void LogStartupConfig(const CONFIG& stConfig)
{
	auto OptStr = [](const string& s) -> const char *
	{
		return s.empty() ? "(비활성)" : s.c_str();
	};

	LOGFMTI("===================================================================");
	LOGFMTI("MapMatchSvr 기동 — config.ini 로딩 완료");
	LOGFMTI("-------------------------------------------------------------------");
	LOGFMTI("[log] path=[%s] level=[%d] keep_runtime=[%d]h keep_day=[%d]d",
		stConfig.strLogPath.c_str(), stConfig.nLogLevel, stConfig.nLogKeepRunTime, stConfig.nLogKeepDay);
	LOGFMTI("[database] host=[%s] port=[%d] name=[%s] userid=[%s] minconnect=[%d] "
		"maxconnect=[%d] retrymax=[%d] retrywait=[%d]ms",
		stConfig.strDBHost.c_str(), stConfig.nDBPort, stConfig.strDBName.c_str(),
		stConfig.strDBUserID.c_str(), stConfig.nDBMinConnect, stConfig.nDBMaxConnect,
		stConfig.nConnRetryMax, stConfig.nConnRetryWait);
	LOGFMTI("[query] file=[%s]", stConfig.strSQLFile.c_str());
	LOGFMTI("[sql] rawlog_recover/select/update=[%s]/[%s]/[%s]",
		OptStr(stConfig.strRawLogRecoverSession), OptStr(stConfig.strRawLogSelectSession),
		OptStr(stConfig.strRawLogUpdateSession));
	LOGFMTI("[sql] charge_insert=[%s] gate_select=[%s] zone_select=[%s] parkfine_select=[%s]",
		OptStr(stConfig.strChargeInsertSession), OptStr(stConfig.strGateSelectSession),
		OptStr(stConfig.strZoneSelectSession), OptStr(stConfig.strParkFineSelectSession));
	LOGFMTI("[sql] trip_end=[%s] trip_abend=[%s] trip_seqoff/fin=[%s]/[%s]",
		OptStr(stConfig.strTripEndUpdateSession), OptStr(stConfig.strAbnormalTripEndSession),
		OptStr(stConfig.strTripSeqOffSession), OptStr(stConfig.strTripSeqFinSession));
	LOGFMTI("[sql] server_status=[%s] stale_recover=[%s]",
		OptStr(stConfig.strServerStatusSession), OptStr(stConfig.strStaleRecoverSession));
	LOGFMTI("[server] id=[%s] status_interval=[%d]s stale_sec=[%d]s",
		stConfig.strServerId.c_str(), stConfig.nServerStatusIntervalSec, stConfig.nStaleSec);
	LOGFMTI("[charge] gate_reload=[%d]s park_pad=[%d]m park_accmax=[%d]m park_speedmax=[%d]km/h",
		stConfig.nGateReloadSec, stConfig.nParkPad, stConfig.nParkAccMax, stConfig.nParkSpeedMax);
	LOGFMTI("[charge] park_entrycnt=[%d] park_exitcnt=[%d] node_exitcnt=[%d] "
		"park_regrace=[%d]s park_ttl=[%d]s exempt_regrace=[%d]s",
		stConfig.nParkEntryCnt, stConfig.nParkExitCnt, stConfig.nNodeExitCnt,
		stConfig.nParkRegraceSec, stConfig.nParkTtlSec, stConfig.nExemptRegraceSec);
	LOGFMTI("[feeder] limit=[%d] fetch_interval=[%d]ms queue_pause/max=[%d]/[%d] "
		"queue_busymin/max=[%d]/[%d]ms",
		stConfig.nFetchLimit, stConfig.nFetchInterval, stConfig.nQueuePauseCount,
		stConfig.nQueueMaxCount, stConfig.nQueueBusyMin, stConfig.nQueueBusyMax);
	LOGFMTI("[worker] ttl_sec=[%d]s shutdown_wait=[%d]ms retry_max=[%d]",
		stConfig.nTtlSec, stConfig.nShutdownWait, stConfig.nRetryMax);
	LOGFMTI("[threads] count=[%d]", stConfig.nThreads);
	LOGFMTI("[data] file=[%s]", stConfig.strDataFile.c_str());
	LOGFMTI("[mapmatch] geodetic=[%d] radius=[%d](min[%d]~max[%d],scale[%.2f]) radius_skip=[%d]m "
		"distance=[%d]m timeout=[%d]ms maxstep=[%d]",
		stConfig.nGeodetic, stConfig.nRadius, stConfig.nRadiusMin, stConfig.nRadiusMax,
		stConfig.dfRadiusScale, stConfig.nRadiusSkip, stConfig.nDistance, stConfig.nMatchTimeout,
		stConfig.nMaxStep);
	LOGFMTI("[mapmatch] alt_gap=[%d]m alt_penalty=[%d] alt_weight=[%.2f] alt_slope=[%.2f]",
		stConfig.nAltGap, stConfig.nAltPenalty, stConfig.dfAltWeight, stConfig.dfAltSlope);
	LOGFMTI("[mapmatch] hoppenalty=[%.2f] hoppenalty_lenratio=[%.2f] reverse_confirm=[%d] "
		"opp_streakmax=[%d] speed_factor=[%.2f] speed_margin=[%d]km/h",
		stConfig.dfHopPenalty, stConfig.dfHopLenRatio, stConfig.nReverseConfirm,
		stConfig.nOppStreakMax, stConfig.dfSpeedFactor, stConfig.nSpeedMargin);
	LOGFMTI("===================================================================");
}

/**
 * @brief main 함수
 * @return -1, 0
*/
int main()
{
	// 중복 기동 방지 — config.ini 조차 읽기 전, 가장 먼저 확인해 이미 살아있는 인스턴스가
	//   있으면 무거운 초기화(DB 접속 등)를 시작하기도 전에 즉시 종료한다 (2026-09-04 최정우 추가)
	if (!AcquireSingleInstanceLock())
		exit(1);

	CUtil cUtil;
	CONFIG stConfig;
	string config_file = "./config.ini";

	// 환경설정 파일 읽기
	if (!Initialize(config_file, &stConfig))
		exit(0);

	// log 경로
	if (stConfig.strLogPath.empty())
	{
		perror("log path is empty!\n");
		exit(0);
	}

	// 데이터 바이너리 절대 경로 (실행 디렉터리 기준)
	char szPath[MAX_PATH];
	memset(reinterpret_cast<void *>(szPath), 0, MAX_PATH);
	// 실행 디렉터리 절대 경로 획득 (2026-07-08 최정우 주석 추가)
	if (getcwd(szPath, MAX_PATH) == nullptr)
	{
		perror("directory is not found!\n");
		exit(0);
	}
	if (stConfig.strDataFile.empty() || stConfig.strDataFile[0] == '/')
		; // already absolute or empty handled above
	else
		stConfig.strDataFile = string(szPath) + "/" + stConfig.strDataFile;
	
	ILog4zManager::getRef().setLoggerPath(LOG4Z_MAIN_LOGGER_ID, stConfig.strLogPath.c_str());
	ILog4zManager::getRef().setLoggerLevel(LOG4Z_MAIN_LOGGER_ID, stConfig.nLogLevel);
	ILog4zManager::getRef().setLoggerOutFile(LOG4Z_MAIN_LOGGER_ID, true);
	// 콘솔 표시(ANSI 컬러) 비활성화 — 실제 로그는 이미 파일로 남는데(setLoggerOutFile),
	//   콘솔 출력은 nohup/setsid 로 기동 시 터미널이 아니라 *_launcher.log 로 그대로
	//   리다이렉트되어, 색상 이스케이프 코드([0m[32m 등)가 텍스트로 찍히는 원인이었다
	//   (2026-07-21 최정우 추가)
	ILog4zManager::getRef().setLoggerDisplay(LOG4Z_MAIN_LOGGER_ID, false);
	// log4z 로거 기동 (2026-07-08 최정우 주석 추가)
	if (!ILog4zManager::getRef().start())
	{
		perror("log open fail!\n");
		exit(0);
	}

	// 로거가 막 기동한 시점 — 이제부터 남기는 로그가 이번 프로세스 기동의 첫 로그가 된다.
	//   이후 CServer::Initialize() 가 DB 접속·지도 데이터 적재·과금 캐시 로딩 등을 순서대로
	//   진행하며 각 단계 로그를 남기므로, 그 앞에 로딩된 환경설정 값부터 먼저 남겨 전체 기동
	//   과정을 로그만으로 순서대로 따라갈 수 있게 한다 (2026-09-04 최정우 추가, 사용자 지시)
	LOGFMTI("single instance lock acquired!pid_file=[%s] pid=[%d]",
		SINGLE_INSTANCE_PID_FILE, static_cast<int>(getpid()));
	LogStartupConfig(stConfig);

	CServer *pcServer = new (std::nothrow)CServer;
	if (pcServer == nullptr)
	{
		LOGFMTE("server is null");
		exit(0);
	}

	try
	{
		try
		{
			if (pcServer->Initialize(stConfig))
			{
				pcServer->start();
				pcServer->join();
			}
		}
		catch (exception& e)
		{
			pcServer->Uninitialize();
			LOGFMTE("error=[%s]", e.what());
		}
	}
	catch (IllegalThreadStateException& e)
	{
		pcServer->Uninitialize();
		LOGFMTE("error=[%s]", e.what());
	}

	pcServer->Uninitialize();
	delete pcServer;
	pcServer = nullptr;
	// log4z 로거 종료 (2026-07-08 최정우 주석 추가)
	ILog4zManager::getRef().stop();

	return 0;
}
