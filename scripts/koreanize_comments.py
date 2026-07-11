#!/usr/bin/env python3
"""
Normalize C/C++ source files to UTF-8 (no BOM) and translate English comment text to Korean.

Comments only: // lines, /* */ blocks, Doxygen tags (@brief, @param, @return, @remark).
Code identifiers, CFG_* constants, paths, SQL session names, and technical tokens are preserved.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

# ── encoding candidates (order matters) ───────────────────────────────────────
ENCODINGS = ("utf-8-sig", "utf-8", "cp949", "euc-kr", "latin-1")

EXCLUDE_DIRS = {"web", "venv", "node_modules", ".git", "build", "cmake-build-debug"}

# Technical tokens / identifiers — never translate
PROTECTED_TOKENS = {
    "true", "false", "nullptr", "NULL", "void", "int", "bool", "char", "double",
    "float", "long", "short", "unsigned", "signed", "const", "static", "virtual",
    "class", "struct", "enum", "namespace", "return", "if", "else", "for", "while",
    "do", "switch", "case", "break", "continue", "sizeof", "typedef", "template",
    "public", "private", "protected", "friend", "inline", "extern", "volatile",
    "SIGTERM", "SIGINT", "pthread", "PQexec", "PGconn", "ROLLBACK", "COMMIT",
    "UPDATE", "INSERT", "SELECT", "RETURNING", "PROCESSING", "PENDING", "ERROR",
    "MATCHED", "SKIP", "START", "Enqueue", "Dequeue", "Runnable", "ParseRow",
    "BulkRelease", "BulkUpdate", "async-signal-safe", "lost-wakeup", "backpressure",
    "hash", "device_key", "trip_id", "GPS", "SQL", "DB", "INI", "WKT", "EPSG",
    "WGS84", "BESSELGEO", "GRS80GEO", "KATECH", "BESSELTM", "GRS80TM", "EPSG3857",
    "GRS80UTMK", "GRS80TM2010", "MOCT", "TURNINFO", "ROAD_TYPE", "NO_SPEED",
    "NO_ANGLE", "NO_ACCURACY", "NO_ALTITUDE", "ACCURACY_M", "ALTITUDE_M", "HEADING",
    "SPEED_KMH", "GPS_DT", "RECV_DT", "GPS_SEQ", "TRIP_ID", "RAW_VLD", "PRIM_RAWGPS",
    "ThreadPool", "Thread", "Feeder", "Worker", "Server", "AppMain", "RawLogFetcher",
    "RawLogWorker", "ProcessManager", "ContinueMapMatch", "BeginMapMatch", "MapMatch",
    "Begin", "Continue", "trace", "query", "feeder", "database", "threads", "mapmatch",
    "whitespace", "ignore", "keepday",
    "PostgrePool", "PostgreSQL", "log4z", "LOG4Z", "LOGI", "LOGD", "LOGW", "LOGFMTE",
    "LOGFMTI", "LOGFMTW", "IniReader", "DataLoader", "ShapeFile", "ShapeLoader",
    "BinaryMaker", "TurnInfoLoader", "CoordConvert", "GISUtil", "GeoUtil",
    "RouteProvider", "SimServer", "Vehicle", "mutex", "predicate", "detach",
    "Reserve", "release", "drain", "poll", "sleep", "ms", "sec", "tick", "bbox",
    "SRID", "link_id", "t_node", "max_spd", "wkt", "boolean", "bigint", "text",
    "KST", "Liang-Barsky", "Haversine", "init", "run", "cond", "scale", "config",
    "radius", "altitude", "timeout", "distance", "geodetic", "accuracy", "heading",
    "speed", "batch", "pool", "host", "port", "file", "path", "level", "logger",
    "screen", "output", "enable", "disable", "attribute", "runtime", "history",
    "status", "started", "process", "config", "default", "filter", "display",
    "limit", "size", "unit", "byte", "suffix", "reserve", "count", "thread",
    "safe", "push", "find", "start", "stop", "create", "overwrite", "singleton",
    "macro", "format", "stream", "input", "log", "main", "invalid", "touch",
    "example", "support", "version", "windows", "debug", "ANSI", "OEM", "console",
    "conversion", "immediately", "queue", "up", "when", "recommended", "auto",
    "exit", "statistics", "optimze", "from", "BESSELGEO", "GRS80GEO", "WGS84GEO",
    "KATECH", "BESSELTM", "GRS80TM", "EPSG3857", "GRS80UTMK", "namespace",
    "already", "absolute", "empty", "handled", "above", "check", "load", "manager",
    "timer", "join", "memory", "allocate", "initialize", "uninitialize", "success",
    "failed", "fail", "open", "read", "write", "close", "rename", "delete",
    "complete", "elapsed", "time", "null", "handle", "offset", "vertex", "node",
    "link", "grid", "shape", "shapefile", "dbf", "shp", "header", "record",
    "field", "required", "missing", "truncated", "adjusted", "invalid", "code",
    "type", "load", "turninfo", "restricted", "geometry", "binary", "temp",
    "exist", "rename", "errno", "data", "create", "derived", "endpoints", "set",
}

# Longest-first phrase replacements (case-sensitive where noted)
PHRASE_REPLACEMENTS: List[Tuple[str, str]] = [
    ("PostgreSQL connection pool", "PostgreSQL DB 커넥션 풀"),
    ("Postgre Connection Pool", "Postgre DB 커넥션 풀"),
    ("Postgre Connection Pool 클래스 헤더 파일", "Postgre DB 커넥션 풀 클래스 헤더 파일"),
    ("DB connection pool", "DB 커넥션 풀"),
    ("db connection pool", "DB 커넥션 풀"),
    ("connection pool", "커넥션 풀"),
    ("Thread Pool 클래스", "스레드 풀 클래스"),
    ("Thread Pool 관리용 클래스 헤더 파일", "스레드 풀 관리용 클래스 헤더 파일"),
    ("Thread Pool", "스레드 풀"),
    ("thread pool", "스레드 풀"),
    ("graceful shutdown", "우아한 종료"),
    ("shutdown active drain", "종료 시 active drain"),
    ("shutdown drain", "종료 drain"),
    ("shutdown signal", "종료 시그널"),
    ("shutdown 시", "종료 시"),
    ("shutdown 또는", "종료 또는"),
    ("shutdown", "종료"),
    ("header file", "헤더 파일"),
    ("source file", "소스 파일"),
    ("memory allocate", "메모리 할당"),
    ("constructor", "생성자"),
    ("destructor", "소멸자"),
    ("log manager run check", "로그 매니저 실행 확인"),
    ("log 보관 관리 클래스", "로그 보관 관리 클래스"),
    ("timer thread join", "타이머 스레드 join"),
    ("logger manager thread stop", "로거 매니저 스레드 중지"),
    ("GPS 정보 맵 매칭 처리 클래스", "GPS 정보 맵매칭 처리 클래스"),
    ("SQL load", "SQL 로드"),
    ("SQL 파일", "SQL 파일"),
    ("SQL 쿼리문 읽기", "SQL 쿼리문 읽기"),
    ("SQL 접근자", "SQL 접근자"),
    ("SQL 파일 읽기", "SQL 파일 읽기"),
    ("Uninitialize 중복 실행 가드", "Uninitialize 중복 실행 가드"),
    ("connection 반환 전 미완료 트랜잭션 ROLLBACK", "커넥션 반환 전 미완료 트랜잭션 ROLLBACK"),
    ("DB pool", "DB 풀"),
    ("DB connection", "DB 커넥션"),
    ("DB host", "DB 호스트"),
    ("DB port", "DB 포트"),
    ("DB INSERT", "DB INSERT"),
    ("DB 커넥션", "DB 커넥션"),
    ("main 함수", "main 함수"),
    ("main logger", "메인 로거"),
    ("전체 Thread 정지", "전체 스레드 정지"),
    ("Thread 실행", "스레드 실행"),
    ("작업 Thread", "작업 스레드"),
    ("작업용 쓰레드", "작업용 스레드"),
    ("쓰레드 관리", "스레드 관리"),
    ("쓰레드 목록", "스레드 목록"),
    ("쓰레드 개수", "스레드 개수"),
    ("쓰레드 아이디", "스레드 ID"),
    ("쓰레드 갯수", "스레드 개수"),
    ("Feeder 쓰레드", "Feeder 스레드"),
    ("DB 연결 확인 쓰레드", "DB 연결 확인 스레드"),
    ("mutex 관리용 클래스", "mutex 관리용 클래스"),
    ("Mutex 헤더 클래스 헤더 파일", "Mutex 헤더 클래스"),
    ("좌표 변한 클래스", "좌표 변환 클래스"),
    ("// from BESSELGEO", "// BESSELGEO 에서 변환"),
    ("// from GRS80GEO", "// GRS80GEO 에서 변환"),
    ("// from WGS84GEO", "// WGS84GEO 에서 변환"),
    ("// from KATECH", "// KATECH 에서 변환"),
    ("// from BESSELTM", "// BESSELTM 에서 변환"),
    ("// from GRS80TM", "// GRS80TM 에서 변환"),
    ("// from GRS80TM2010", "// GRS80TM2010 에서 변환 (Korea 2000 / Central Belt 2010, EPSG:5186, FN=600000)"),
    ("// from EPSG3857", "// EPSG3857 에서 변환"),
    ("// from GRS80UTMK", "// GRS80UTMK 에서 변환"),
    ("// init", "// 초기화"),
    ("// already absolute or empty handled above", "// 이미 절대 경로이거나 위에서 처리된 빈 값"),
    ("// namespace", "// namespace"),
    ("// log 경로", "// 로그 경로"),
    ("// parse 실패 행 PROCESSING 해제", "// parse 실패 행 PROCESSING 해제"),
    ("// Node shape 의 Vertex", "// Node shape 의 Vertex"),
    ("// shapefile 형상 정보 로딩", "// shapefile 형상 정보 로딩"),
    ("// GRID·링크·회전 정보 생성", "// GRID·링크·회전 정보 생성"),
    ("// MOCT TURNINFO 회전 제한(금지) 쌍 제외", "// MOCT TURNINFO 회전 제한(금지) 쌍 제외"),
    ("// GRID 별 세그먼트", "// GRID 별 세그먼트"),
    ("// GRID ID", "// GRID ID"),
    ("// GRID 경계", "// GRID 경계"),
    ("// GRID 모서리", "// GRID 모서리"),
    ("// GRID 세그먼트", "// GRID 세그먼트"),
    ("// GRID 번호", "// GRID 번호"),
    ("// GRID 크기", "// GRID 크기"),
    ("// GRID 왼쪽", "// GRID 왼쪽"),
    ("// ini file open fail!", "// ini 파일 열기 실패!"),
    ("// ini file read fail!", "// ini 파일 읽기 실패!"),
    ("// config.ini file not found!", "// config.ini 파일을 찾을 수 없음!"),
    ("// config file open failed!", "// 설정 파일 열기 실패!"),
    ("// geometry binary file is empty!", "// geometry 바이너리 파일이 비어 있음!"),
    # log4z phrases
    ("DO NOT TOUCH", "수정 금지"),
    ("logger ID type", "로거 ID 타입"),
    ("the invalid logger id", "유효하지 않은 로거 ID"),
    ("the main logger id", "메인 로거 ID"),
    ("can use this id to set the main logger's attribute", "메인 로거 속성 설정에 이 ID 사용 가능"),
    ("the main logger name", "메인 로거 이름"),
    ("check VC VERSION", "VC 버전 확인"),
    ("format micro cannot support VC6 or VS2003, please use stream input log, like LOGI, LOGD, LOG_DEBUG, LOG_STREAM", "format micro는 VC6/VS2003 미지원 — LOGI, LOGD, LOG_DEBUG, LOG_STREAM 등 스트림 입력 로그 사용"),
    ("LOG Level", "로그 레벨"),
    ("the max logger count", "최대 로거 개수"),
    ("the max log content length", "최대 로그 내용 길이"),
    ("the max stl container depth", "최대 STL 컨테이너 깊이"),
    ("the log queue length limit size", "로그 큐 길이 제한"),
    ("all logger synchronous output or not", "모든 로거 동기 출력 여부"),
    ("all logger synchronous display to the windows debug output", "모든 로거를 Windows 디버그 출력에 동기 표시"),
    ("default logger output file", "기본 로거 출력 파일"),
    ("default log filter level", "기본 로그 필터 레벨"),
    ("default logger display", "기본 로거 화면 표시"),
    ("default logger output to file", "기본 로거 파일 출력"),
    ("default logger month dir used status", "기본 로거 월별 디렉터리 사용 여부"),
    ("default logger output file limit size, unit M byte", "기본 로거 출력 파일 크기 제한(M바이트)"),
    ("default logger show suffix (file name and line number)", "기본 로거 접미사 표시(파일명·행번호)"),
    ("support ANSI->OEM console conversion on Windows", "Windows ANSI→OEM 콘솔 변환 지원"),
    ("default logger force reserve log file count", "기본 로거 보관 로그 파일 강제 개수"),
    ("log4z class", "log4z 클래스"),
    ("Log4z Singleton", "Log4z 싱글톤"),
    ("Config or overwrite configure", "설정 로드 또는 덮어쓰기"),
    ("Needs to be called before ILog4zManager::Start,, OR Do not call", "ILog4zManager::Start 이전 호출 필요, 아니면 호출하지 말 것"),
    ("Create or overwrite logger", "로거 생성 또는 덮어쓰기"),
    ("Start Log Thread. This method can only be called once by one process", "로그 스레드 시작(프로세스당 1회만 호출 가능)"),
    ("Default the method will be calling at process exit auto", "기본적으로 프로세스 종료 시 자동 호출"),
    ("Default no need to call and no recommended", "기본적으로 호출 불필요·비권장"),
    ("Find logger. thread safe", "로거 조회(스레드 안전)"),
    ("Push log, thread safe", "로그 적재(스레드 안전)"),
    ("set logger's attribute, thread safe", "로거 속성 설정(스레드 안전)"),
    ("immediately when enable, and queue up when disable", "활성화 시 즉시, 비활성화 시 큐 적재"),
    ("Update logger's attribute from config file, thread safe", "설정 파일에서 로거 속성 갱신(스레드 안전)"),
    ("Log4z status statistics, thread safe", "Log4z 상태 통계(스레드 안전)"),
    ("base macro", "기본 매크로"),
    ("fast macro", "빠른 매크로"),
    ("super macro", "슈퍼 매크로"),
    ("format input log", "포맷 입력 로그"),
    ("format string", "포맷 문자열"),
    ("optimze from std::stringstream to Log4zStream", "std::stringstream → Log4zStream 최적화"),
    ("path for log file", "로그 파일 경로"),
    ("one logger one name", "로거당 이름 1개"),
    ("filter level", "필터 레벨"),
    ("display to screen", "화면 표시"),
    ("output to file", "파일 출력"),
    ("limit file's size, unit Million byte", "파일 크기 제한(백만 바이트)"),
    ("enable/disable the log's suffix.(file name:line number)", "로그 접미사(파일명:행번호) 활성/비활성"),
    ("log file reserve time. unit is time second", "로그 파일 보관 시간(초)"),
    ("file create time", "파일 생성 시각"),
    ("file create day time", "파일 생성 일자"),
    ("rolling file index", "롤링 파일 인덱스"),
    ("current file length", "현재 파일 길이"),
    ("file handle", "파일 핸들"),
    ("thread status", "스레드 상태"),
    ("wait thread started", "스레드 시작 대기"),
    ("hot change name or path for one logger", "단일 로거 이름/경로 핫 변경"),
    ("the process info", "프로세스 정보"),
    ("config file name", "설정 파일명"),
    ("logger id manager, [logger name]:[logger id]", "로거 ID 관리자 [로거명]:[로거ID]"),
    ("the last used id of _loggers", "_loggers 마지막 사용 ID"),
    ("write file", "파일 쓰기"),
    ("output to file", "파일 출력"),
    ("limit file size", "파일 크기 제한"),
    ("display log in file line", "파일·행번호 로그 표시"),
    ("load config file error", "설정 파일 로드 오류"),
    ("configure error: too many calls to Config", "Config 호출 과다 오류"),
    ("the old config file", "이전 설정 파일"),
    ("the new config file", "새 설정 파일"),
    ("the new config file is null", "새 설정 파일이 null"),
    ("read configure and create with overwriting", "설정 읽기 및 덮어쓰기 생성"),
    ("create with overwriting", "덮어쓰기로 생성"),
    ("trim utf8 file bom", "UTF-8 파일 BOM 제거"),
    (" 读取配置文件并覆写", "설정 파일 읽기 및 덮어쓰기"),
    (" 覆写式创建", "덮어쓰기 방식 생성"),
    (" 查找ID", "ID 조회"),
    ("logger key", "로거 키"),
    ("attribute", "속성"),
    ("runtime info", "런타임 정보"),
    ("history", "이력"),
    ("UTILITY", "유틸리티"),
    ("Log4zFileHandler", "Log4zFileHandler"),
    ("LockHelper", "LockHelper"),
    ("AutoLock", "AutoLock"),
    ("SemHelper", "SemHelper"),
    ("ThreadHelper", "ThreadHelper"),
    ("LogData", "LogData"),
    ("LoggerInfo", "LoggerInfo"),
    ("LogerManager", "LogerManager"),
]

# Word-level fallback (applied to remaining pure-alpha tokens in comments)
WORD_MAP = {
    "constructor": "생성자",
    "destructor": "소멸자",
    "initialize": "초기화",
    "uninitialize": "해제",
    "connection": "커넥션",
    "thread": "스레드",
    "pool": "풀",
    "shutdown": "종료",
    "header": "헤더",
    "source": "소스",
    "file": "파일",
    "path": "경로",
    "manager": "매니저",
    "memory": "메모리",
    "allocate": "할당",
    "success": "성공",
    "failed": "실패",
    "failure": "실패",
    "error": "오류",
    "check": "확인",
    "load": "로드",
    "open": "열기",
    "close": "닫기",
    "read": "읽기",
    "write": "쓰기",
    "create": "생성",
    "delete": "삭제",
    "rename": "이름변경",
    "complete": "완료",
    "start": "시작",
    "stop": "중지",
    "wait": "대기",
    "timeout": "타임아웃",
    "signal": "시그널",
    "handler": "핸들러",
    "register": "등록",
    "graceful": "우아한",
    "drain": "drain",
    "active": "활성",
    "idle": "유휴",
    "queue": "큐",
    "batch": "배치",
    "worker": "워커",
    "feeder": "Feeder",
    "server": "서버",
    "client": "클라이언트",
    "config": "설정",
    "default": "기본",
    "minimum": "최소",
    "maximum": "최대",
    "count": "개수",
    "number": "번호",
    "size": "크기",
    "limit": "제한",
    "interval": "간격",
    "retry": "재시도",
    "attempt": "시도",
    "release": "해제",
    "reserve": "예약",
    "recover": "복구",
    "parse": "파싱",
    "convert": "변환",
    "format": "포맷",
    "display": "표시",
    "output": "출력",
    "input": "입력",
    "filter": "필터",
    "level": "레벨",
    "enable": "활성화",
    "disable": "비활성화",
    "status": "상태",
    "state": "상태",
    "unknown": "알 수 없음",
    "waiting": "대기 중",
    "stopped": "중지됨",
    "running": "실행 중",
    "valid": "유효",
    "invalid": "무효",
    "empty": "비어 있음",
    "null": "null",
    "handle": "핸들",
    "pointer": "포인터",
    "context": "컨텍스트",
    "parameter": "파라미터",
    "argument": "인자",
    "return": "반환",
    "value": "값",
    "type": "타입",
    "class": "클래스",
    "object": "객체",
    "instance": "인스턴스",
    "list": "목록",
    "map": "맵",
    "array": "배열",
    "string": "문자열",
    "buffer": "버퍼",
    "data": "데이터",
    "binary": "바이너리",
    "geometry": "형상",
    "shape": "형상",
    "grid": "GRID",
    "segment": "세그먼트",
    "link": "링크",
    "node": "노드",
    "vertex": "정점",
    "route": "경로",
    "vehicle": "차량",
    "simulator": "시뮬레이터",
    "coordinate": "좌표",
    "latitude": "위도",
    "longitude": "경도",
    "distance": "거리",
    "angle": "각도",
    "heading": "방위각",
    "speed": "속도",
    "altitude": "고도",
    "accuracy": "정확도",
    "radius": "반경",
    "weight": "가중치",
    "bonus": "보너스",
    "penalty": "페널티",
    "gap": "차이",
    "slope": "기울기",
    "scale": "스케일",
    "minimum": "최소",
    "maximum": "최대",
    "upper": "상한",
    "lower": "하한",
    "bound": "경계",
    "boundary": "경계",
    "corner": "모서리",
    "edge": "변",
    "intersection": "교차",
    "clip": "클리핑",
    "cliping": "클리핑",
    "clipping": "클리핑",
    "loading": "로딩",
    "saving": "저장",
    "store": "저장",
    "stored": "저장됨",
    "temporary": "임시",
    "temp": "임시",
    "exist": "존재",
    "missing": "누락",
    "required": "필수",
    "optional": "선택",
    "restricted": "제한",
    "forbidden": "금지",
    "turn": "회전",
    "rotation": "회전",
    "estimate": "추정",
    "detect": "감지",
    "detected": "감지됨",
    "auto": "자동",
    "manual": "수동",
    "adaptive": "적응형",
    "periodic": "주기",
    "cycle": "주기",
    "interval": "간격",
    "tick": "tick",
    "second": "초",
    "minute": "분",
    "hour": "시",
    "day": "일",
    "time": "시각",
    "date": "날짜",
    "timestamp": "타임스탬프",
    "local": "로컬",
    "global": "전역",
    "shared": "공유",
    "static": "정적",
    "dynamic": "동적",
    "safe": "안전",
    "unsafe": "비안전",
    "lock": "잠금",
    "unlock": "잠금해제",
    "mutex": "mutex",
    "condition": "조건",
    "broadcast": "브로드캐스트",
    "predicate": "predicate",
    "prevent": "방지",
    "protection": "보호",
    "guard": "가드",
    "overflow": "오버플로",
    "underflow": "언더플로",
    "bound": "경계",
    "range": "범위",
    "offset": "오프셋",
    "index": "인덱스",
    "identifier": "식별자",
    "name": "이름",
    "key": "키",
    "field": "필드",
    "column": "컬럼",
    "row": "행",
    "table": "테이블",
    "record": "레코드",
    "result": "결과",
    "query": "쿼리",
    "session": "세션",
    "transaction": "트랜잭션",
    "commit": "커밋",
    "rollback": "롤백",
    "bind": "바인딩",
    "binding": "바인딩",
    "execute": "실행",
    "execution": "실행",
    "affect": "영향",
    "affected": "영향받은",
    "expect": "기대",
    "expected": "기대값",
    "matched": "일치함",
    "mismatch": "불일치",
    "compare": "비교",
    "comparison": "비교",
    "verify": "검증",
    "validation": "검증",
    "validate": "검증",
    "extract": "추출",
    "escape": "이스케이프",
    "literal": "리터럴",
    "element": "원소",
    "component": "구성요소",
    "module": "모듈",
    "utility": "유틸리티",
    "helper": "헬퍼",
    "wrapper": "래퍼",
    "delegate": "위임",
    "dispatch": "디스패치",
    "enqueue": "Enqueue",
    "dequeue": "Dequeue",
    "interrupt": "인터럽트",
    "observe": "관찰",
    "monitor": "모니터",
    "logging": "로깅",
    "logger": "로거",
    "trace": "추적",
    "debug": "디버그",
    "info": "정보",
    "warn": "경고",
    "warning": "경고",
    "fatal": "치명",
    "critical": "치명",
    "notice": "알림",
    "statistics": "통계",
    "metric": "지표",
    "performance": "성능",
    "elapsed": "경과",
    "duration": "지속",
    "throughput": "처리량",
    "latency": "지연",
    "overhead": "오버헤드",
    "backpressure": "backpressure",
    "pressure": "압력",
    "congestion": "혼잡",
    "busy": "혼잡",
    "pause": "일시중지",
    "resume": "재개",
    "suspend": "일시중단",
    "resume": "재개",
    "fork": "포크",
    "join": "join",
    "detach": "detach",
    "attach": "attach",
    "spawn": "생성",
    "kill": "종료",
    "terminate": "종료",
    "exit": "종료",
    "abort": "중단",
    "cancel": "취소",
    "pending": "대기",
    "processing": "처리 중",
    "processed": "처리됨",
    "finished": "완료",
    "done": "완료",
    "skip": "건너뜀",
    "ignore": "무시",
    "omit": "생략",
    "include": "포함",
    "exclude": "제외",
    "only": "만",
    "once": "1회",
    "always": "항상",
    "never": "절대",
    "sometimes": "가끔",
    "usually": "보통",
    "typically": "일반적으로",
    "generally": "일반적으로",
    "specifically": "구체적으로",
    "especially": "특히",
    "approximately": "대략",
    "about": "약",
    "around": "약",
    "between": "사이",
    "within": "이내",
    "without": "없이",
    "before": "이전",
    "after": "이후",
    "during": "동안",
    "while": "동안",
    "until": "까지",
    "since": "이후",
    "when": "시",
    "where": "위치",
    "which": "which",
    "that": "that",
    "this": "이",
    "these": "이들",
    "those": "그들",
    "with": "와",
    "without": "없이",
    "from": "에서",
    "into": "로",
    "onto": "위에",
    "over": "위",
    "under": "아래",
    "above": "위",
    "below": "아래",
    "inside": "내부",
    "outside": "외부",
    "internal": "내부",
    "external": "외부",
    "public": "공개",
    "private": "비공개",
    "protected": "보호",
    "global": "전역",
    "local": "로컬",
    "main": "메인",
    "primary": "주",
    "secondary": "보조",
    "auxiliary": "보조",
    "backup": "백업",
    "fallback": "폴백",
    "default": "기본",
    "custom": "사용자",
    "user": "사용자",
    "admin": "관리자",
    "system": "시스템",
    "application": "애플리케이션",
    "service": "서비스",
    "daemon": "데몬",
    "process": "프로세스",
    "program": "프로그램",
    "library": "라이브러리",
    "framework": "프레임워크",
    "platform": "플랫폼",
    "environment": "환경",
    "configuration": "설정",
    "setting": "설정",
    "option": "옵션",
    "flag": "플래그",
    "switch": "스위치",
    "mode": "모드",
    "state": "상태",
    "phase": "단계",
    "step": "단계",
    "stage": "단계",
    "phase": "단계",
    "operation": "동작",
    "action": "동작",
    "behavior": "동작",
    "function": "함수",
    "method": "메서드",
    "routine": "루틴",
    "procedure": "절차",
    "algorithm": "알고리즘",
    "logic": "로직",
    "rule": "규칙",
    "policy": "정책",
    "strategy": "전략",
    "pattern": "패턴",
    "template": "템플릿",
    "model": "모델",
    "schema": "스키마",
    "structure": "구조",
    "format": "포맷",
    "layout": "레이아웃",
    "design": "설계",
    "implementation": "구현",
    "interface": "인터페이스",
    "abstract": "추상",
    "concrete": "구체",
    "base": "기본",
    "derived": "파생",
    "parent": "부모",
    "child": "자식",
    "root": "루트",
    "leaf": "리프",
    "head": "헤드",
    "tail": "테일",
    "front": "앞",
    "back": "뒤",
    "top": "상단",
    "bottom": "하단",
    "left": "왼쪽",
    "right": "오른쪽",
    "center": "중앙",
    "middle": "중간",
    "start": "시작",
    "end": "끝",
    "begin": "시작",
    "finish": "종료",
    "initial": "초기",
    "final": "최종",
    "first": "첫",
    "last": "마지막",
    "next": "다음",
    "previous": "이전",
    "current": "현재",
    "new": "새",
    "old": "구",
    "original": "원본",
    "copy": "복사",
    "clone": "복제",
    "duplicate": "중복",
    "unique": "고유",
    "same": "동일",
    "different": "다른",
    "equal": "같음",
    "equivalent": "동등",
    "similar": "유사",
    "related": "관련",
    "associated": "연관",
    "linked": "연결",
    "connected": "연결",
    "disconnected": "연결해제",
    "attached": "부착",
    "detached": "분리",
    "bound": "바인딩",
    "unbound": "언바인딩",
    "free": "해제",
    "busy": "사용 중",
    "available": "사용 가능",
    "unavailable": "사용 불가",
    "ready": "준비",
    "not": "아님",
    "yes": "예",
    "no": "아니오",
    "ok": "OK",
    "fail": "실패",
    "pass": "통과",
    "true": "true",
    "false": "false",
}

LICENSE_MARKERS = (
    "MIT license",
    "Permission is hereby granted",
    "THE SOFTWARE IS PROVIDED",
    "Log4z is licensed",
    "COPYRIGHT",
    "end of COPYRIGHT",
    "UPDATES LOG",
    "VERSION 0.1.0",
    "contact me:",
    "tencent qq group",
)

DOXYGEN_TAG_RE = re.compile(
    r"^(\s*(?:/\*\*?\s*)?\*?\s*)"
    r"(@(?:brief|param(?:\[[^\]]*\])?|return|remark|file|class|enum))\s*"
    r"(.*)$"
)

PARAM_NAME_RE = re.compile(
    r"^(\[[^\]]*\]\s+)?([A-Za-z_][\w]*)\s+(.*)$"
)

BRACKET_RE = re.compile(r"\[[^\]]+\]")

IDENT_RE = re.compile(
    r"\b(?:"
    r"CFG_[A-Z0-9_]+|"
    r"m_[a-zA-Z]\w*|"
    r"[A-Z][a-z]+(?:[A-Z][a-z]+)+|"
    r"[A-Z]{2,}[a-z0-9_]*|"
    r"[a-z]+_[a-z0-9_]+|"
    r"[A-Za-z_][\w]*::[A-Za-z_]\w*|"
    r"\$[0-9]+|"
    r"/[\w./-]+|"
    r"\./[\w./-]+"
    r")\b"
)

HANGUL_RE = re.compile(r"[\uAC00-\uD7A3]")
ENGLISH_WORD_RE = re.compile(r"[A-Za-z]{2,}")


@dataclass
class Stats:
    files_processed: int = 0
    files_changed: int = 0
    encoding_fixes: int = 0
    comment_changes: int = 0
    samples: List[str] = field(default_factory=list)


def read_with_encoding(path: Path) -> Tuple[str, Optional[str], bool]:
    raw = path.read_bytes()
    if not raw:
        return "", "utf-8", False

    had_bom = raw.startswith(b"\xef\xbb\xbf")
    source_encoding: Optional[str] = None
    text: Optional[str] = None

    for enc in ENCODINGS:
        try:
            text = raw.decode(enc)
            source_encoding = "utf-8" if enc == "utf-8-sig" else enc
            break
        except UnicodeDecodeError:
            continue

    if text is None:
        text = raw.decode("latin-1")
        source_encoding = "latin-1"

    encoding_fixed = had_bom or source_encoding not in ("utf-8", None)
    return text, source_encoding, encoding_fixed


def is_license_block(text: str) -> bool:
    lower = text.lower()
    return any(marker.lower() in lower for marker in LICENSE_MARKERS)


def has_korean(text: str) -> bool:
    return bool(HANGUL_RE.search(text))


def has_english_words(text: str) -> bool:
    for word in ENGLISH_WORD_RE.findall(text):
        if word not in PROTECTED_TOKENS and word.lower() not in PROTECTED_TOKENS:
            return True
    return False


def should_translate_comment(text: str) -> bool:
    if is_license_block(text):
        return False
    if "@file" in text and not has_english_words(text):
        return False
    return has_english_words(text)


def protect_identifiers(text: str) -> Tuple[str, List[Tuple[str, str]]]:
    placeholders: List[Tuple[str, str]] = []

    def repl(m: re.Match) -> str:
        token = m.group(0)
        key = f"§§{len(placeholders)}§§"
        placeholders.append((key, token))
        return key

    protected = BRACKET_RE.sub(repl, text)
    protected = IDENT_RE.sub(repl, protected)
    return protected, placeholders


def restore_identifiers(text: str, placeholders: List[Tuple[str, str]]) -> str:
    for key, token in placeholders:
        text = text.replace(key, token)
    return text


def apply_phrase_replacements(text: str) -> str:
    for src, dst in PHRASE_REPLACEMENTS:
        text = text.replace(src, dst)
    return text


def fix_return_lines(text: str) -> str:
    text = re.sub(
        r"(@return\s+)true\s*,\s*false\b",
        r"\1true(성공), false(실패)",
        text,
    )
    text = re.sub(
        r"^true\s*,\s*false\b",
        "true(성공), false(실패)",
        text,
    )
    text = re.sub(
        r"(@return\s+true\([^)]+\))\s*,\s*false\b(?!\()",
        r"\1, false(실패)",
        text,
    )
    text = re.sub(
        r"^(true\([^)]+\))\s*,\s*false\b(?!\()",
        r"\1, false(실패)",
        text,
    )
    text = re.sub(
        r"(@return\s+)false\s*,\s*true\b",
        r"\1false(실패), true(성공)",
        text,
    )
    return text


def translate_words(text: str) -> str:
    def repl_word(m: re.Match) -> str:
        word = m.group(0)
        lower = word.lower()
        if word in PROTECTED_TOKENS or lower in PROTECTED_TOKENS:
            return word
        if word.isupper() and len(word) >= 2:
            return word
        if "_" in word or any(c.isupper() for c in word[1:]):
            return word
        mapped = WORD_MAP.get(lower)
        if mapped is None:
            return word
        if word.isupper():
            return mapped.upper()
        if word[0].isupper():
            return mapped[0].upper() + mapped[1:] if mapped else mapped
        return mapped

    return ENGLISH_WORD_RE.sub(repl_word, text)


def cleanup_spacing(text: str) -> str:
    text = re.sub(r"\s{2,}", " ", text)
    text = re.sub(r"\s+([,.;:])", r"\1", text)
    return text.strip()


def translate_doxygen_line(line: str) -> str:
    m = DOXYGEN_TAG_RE.match(line)
    if not m:
        return translate_comment_body(line)

    prefix, tag, body = m.group(1), m.group(2), m.group(3)

    if tag == "@file":
        return line

    if tag.startswith("@param"):
        pm = PARAM_NAME_RE.match(body)
        if pm:
            bracket, name, desc = pm.group(1) or "", pm.group(2), pm.group(3)
            new_desc = translate_comment_body(desc)
            return f"{prefix}{tag} {bracket}{name} {new_desc}".rstrip()
        return f"{prefix}{tag} {translate_comment_body(body)}".rstrip()

    if tag == "@return":
        return f"{prefix}{tag} {fix_return_lines(translate_comment_body(body))}".rstrip()

    return f"{prefix}{tag} {translate_comment_body(body)}".rstrip()


def translate_comment_body(text: str) -> str:
    if not should_translate_comment(text):
        return text

    protected, placeholders = protect_identifiers(text)
    out = apply_phrase_replacements(protected)
    out = fix_return_lines(out)
    out = translate_words(out)
    out = restore_identifiers(out, placeholders)
    out = cleanup_spacing(out) if out != text else out
    return out


def translate_comment_line(line: str, in_block: bool) -> str:
    if in_block:
        if line.strip().startswith("*"):
            content = line
            stripped = line.lstrip()
            if stripped.startswith("/**"):
                rest = stripped[3:].lstrip()
                if rest:
                    return line[: len(line) - len(stripped)] + "/** " + translate_doxygen_line(rest)
                return line
            if stripped.startswith("*/"):
                return line
            prefix_len = len(line) - len(stripped)
            star_and_space = ""
            body = stripped
            if body.startswith("*"):
                idx = 1
                while idx < len(body) and body[idx] == " ":
                    idx += 1
                star_and_space = body[:idx]
                body = body[idx:]
            translated = translate_doxygen_line(body) if body else body
            return line[:prefix_len] + star_and_space + translated
        return translate_comment_body(line)

    # line comment
    idx = line.find("//")
    if idx == -1:
        return line
    prefix = line[: idx + 2]
    body = line[idx + 2 :]
    if body.startswith(" "):
        prefix += " "
        body = body[1:]
    translated = translate_doxygen_line(body) if body.lstrip().startswith("@") else translate_comment_body(body)
    return prefix + translated


@dataclass
class LexResult:
    text: str
    comment_changes: int


def process_source(text: str) -> LexResult:
    out: List[str] = []
    i = 0
    n = len(text)
    comment_changes = 0
    state = "code"
    block_start = 0

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "code":
            if ch == "/" and nxt == "/":
                line_end = text.find("\n", i)
                if line_end == -1:
                    line_end = n
                old_line = text[i:line_end]
                new_line = translate_comment_line(old_line, False)
                if new_line != old_line:
                    comment_changes += 1
                out.append(new_line)
                i = line_end
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                block_start = i
                i += 2
                continue
            if ch == '"':
                state = "string"
                out.append(ch)
                i += 1
                continue
            if ch == "'":
                state = "char"
                out.append(ch)
                i += 1
                continue
            out.append(ch)
            i += 1
            continue

        if state == "string":
            out.append(ch)
            if ch == "\\":
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
            elif ch == '"':
                state = "code"
            i += 1
            continue

        if state == "char":
            out.append(ch)
            if ch == "\\":
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
            elif ch == "'":
                state = "code"
            i += 1
            continue

        if state == "block":
            if ch == "*" and nxt == "/":
                block = text[block_start : i + 2]
                new_block = transform_block_comment(block)
                if new_block != block:
                    comment_changes += 1
                out.append(new_block)
                i += 2
                state = "code"
                continue
            i += 1
            continue

    return LexResult("".join(out), comment_changes)


def transform_block_comment(block: str) -> str:
    if is_license_block(block):
        return block

    lines = block.split("\n")
    new_lines = [translate_comment_line(line, True) for line in lines]
    return "\n".join(new_lines)


def collect_files(dirs: Iterable[Path]) -> List[Path]:
    files: List[Path] = []
    for base in dirs:
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix.lower() not in {".h", ".cpp", ".c"}:
                continue
            parts = set(path.parts)
            if parts & EXCLUDE_DIRS:
                continue
            if any(part in EXCLUDE_DIRS for part in path.parts):
                continue
            files.append(path)
    return files


def write_utf8_no_bom(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))


def process_file(path: Path, stats: Stats, dry_run: bool = False) -> None:
    original, source_enc, encoding_fixed = read_with_encoding(path)
    result = process_source(original)
    changed = encoding_fixed or result.comment_changes > 0 or result.text != original

    stats.files_processed += 1
    if encoding_fixed:
        stats.encoding_fixes += 1
    if result.comment_changes > 0:
        stats.comment_changes += result.comment_changes

    if changed:
        stats.files_changed += 1
        if len(stats.samples) < 20:
            if result.comment_changes > 0:
                old_lines = original.splitlines()
                new_lines = result.text.splitlines()
                for o, n in zip(old_lines, new_lines):
                    if o != n and ("//" in o or "/*" in o or "*" in o or "@" in o):
                        stats.samples.append(f"{path}: {o.strip()[:100]} -> {n.strip()[:100]}")
                        break
            elif encoding_fixed:
                stats.samples.append(f"{path}: encoding {source_enc} -> utf-8 (no BOM)")

        if not dry_run:
            write_utf8_no_bom(path, result.text)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="UTF-8 normalize and Koreanize C/C++ comments")
    parser.add_argument(
        "directories",
        nargs="*",
        default=[
            "MapMatchSvr/src",
            "Simulator/src",
            "CreateData/src",
        ],
        help="Source directories to process",
    )
    parser.add_argument("--root", default=".", help="Repository root")
    parser.add_argument("--dry-run", action="store_true", help="Analyze only, do not write")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    dirs = [root / d for d in args.directories]
    stats = Stats()

    for path in collect_files(dirs):
        process_file(path, stats, dry_run=args.dry_run)

    print("=== koreanize_comments summary ===")
    print(f"Files processed:   {stats.files_processed}")
    print(f"Files changed:     {stats.files_changed}")
    print(f"Encoding fixes:    {stats.encoding_fixes}")
    print(f"Comment changes:   {stats.comment_changes}")
    print()
    print("Sample changes:")
    if stats.samples:
        for sample in stats.samples:
            print(f"  - {sample}")
    else:
        print("  (none)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
