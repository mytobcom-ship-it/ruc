/*
 * Log4z License
 * -----------
 * 
 * Log4z is licensed under the terms of the MIT license reproduced below.
 * This means that Log4z is free software and can be used for both academic
 * and commercial purposes at absolutely no cost.
 * 
 * 
 * ===============================================================================
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * 
 * ===============================================================================
 * 
 * (end of COPYRIGHT)
 */
#include "log4z.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <list>
#include <algorithm>
#include <iostream>
#include <random>                                                       // 2025-12-03 최정우 추가

#ifdef WIN32
#include <io.h>
#include <shlwapi.h>
#include <process.h>
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "User32.lib")
#pragma warning(disable:4996)

#else
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/syscall.h>
#endif

#ifdef __APPLE__
#include "TargetConditionals.h"
#include <dispatch/dispatch.h>
#if !TARGET_OS_IPHONE
#define LOG4Z_HAVE_LIBPROC
#include <libproc.h>
#endif
#endif

_ZSUMMER_BEGIN
_ZSUMMER_LOG4Z_BEGIN

static const char *const LOG_STRING[]=
{
    "LOG_TRACE",
    "LOG_DEBUG",
    "LOG_INFO ",
    "LOG_WARN ",
    "LOG_ERROR",
    "LOG_ALARM",
    "LOG_FATAL",
};
static const size_t LOG_STRING_LEN[] =
{
    sizeof("LOG_TRACE") - 1,
    sizeof("LOG_DEBUG") - 1,
    sizeof("LOG_INFO") - 1,
    sizeof("LOG_WARN") - 1,
    sizeof("LOG_ERROR") - 1,
    sizeof("LOG_ALARM") - 1,
    sizeof("LOG_FATAL") - 1,
};

thread_local std::mt19937 backpressure_gen(std::random_device{}());
thread_local std::uniform_int_distribution<size_t> backpressure_dis(0, 99);

#ifdef WIN32
const static WORD LOG_COLOR[LOG_LEVEL_FATAL+1] = {
    0,
    0,
    FOREGROUND_BLUE | FOREGROUND_GREEN,
    FOREGROUND_GREEN | FOREGROUND_RED,
    FOREGROUND_RED,
    FOREGROUND_GREEN,
    FOREGROUND_RED | FOREGROUND_BLUE };
#else

const static char LOG_COLOR[LOG_LEVEL_FATAL+1][50] = {
    "\e[0m",
    "\e[0m",
    "\e[34m\e[1m",//hight blue
    "\e[33m", //yellow
    "\e[31m", //red
    "\e[32m", //green
    "\e[35m" };
#endif

//////////////////////////////////////////////////////////////////////////
//! Log4zFileHandler
//////////////////////////////////////////////////////////////////////////
class Log4zFileHandler
{
public:
	Log4zFileHandler(){ _file = nullptr; }
	~Log4zFileHandler()
    {
        if (_file != nullptr)
        {
            fclose(_file);
            _file = nullptr;
        }                       // if (_file != nullptr)
    }
	inline bool isOpen(){ return _file != nullptr; }
	inline long open(const char *path, const char * mod)
	{
		// 예외 처리 추가 (2025-12-04 최정우 추가)
		if ((strlen(path) <= 0) || (strlen(path) > static_cast<size_t>(PATH_MAX)))
			return -1;

		if (_file != nullptr)
		{
			fclose(_file);
			_file = nullptr;
		}						// if (_file != nullptr)

		_file = fopen(path, mod);
		if (_file != nullptr)
		{
			long tel = 0;
			long cur = ftell(_file);
			fseek(_file, 0L, SEEK_END);
			tel = ftell(_file);
			fseek(_file, cur, SEEK_SET);
			return tel;
		}                       // if (_file != nullptr)
		return -1;
	}
	inline bool clean(int index, int len)
	{
#if !defined(__APPLE__) && !defined(WIN32) 
		if (_file != nullptr)
		{
			int fd = fileno(_file);

			// 반환 값 검증 추가 (2025-12-05 최정우 추가)
			if (fd < 0) return false;

			// 반환값 검증 추가 (2025-12-05 최정우 추가)
			if (fsync(fd) != 0) return false;

			posix_fadvise(fd, index, len, POSIX_FADV_DONTNEED);

			// 반환값 검증 추가 (2025-12-05 최정우 추가)
			if (fsync(fd) != 0) return false;

			return true;
		}						// if (_file != nullptr)
#endif
		return true;
	}
	inline void close()
	{
		if (_file != nullptr)
		{
            clean(0, 0);

            FILE *fileToClose = _file;
            _file = nullptr;
			if (fileToClose != nullptr)
			{
				fclose(fileToClose);
                fileToClose = nullptr;
			}					// if (fileToClose != nullptr)
		}						// if (_file != nullptr)
	}
	inline void write(const char * data, size_t len)
	{
		if ((_file != nullptr) && (len > 0))
		{
			if (fwrite(data, 1, len, _file) != len)
			{
				close();
			}					// if (fwrite(data, 1, len, _file) != len)
		}						// if ((_file != nullptr) && (len > 0))
	}
	inline void flush(){ if (_file) fflush(_file); }

	inline std::string readLine()
	{
		char buf[500] = { 0 };
		if (_file && fgets(buf, 500, _file) != nullptr)
		{
			return std::string(buf);
		}
		return std::string();
	}
	inline const std::string readContent();
	inline bool removeFile(const std::string & path) { return ::remove(path.c_str()) == 0; }
public:
	FILE *_file;
};

//////////////////////////////////////////////////////////////////////////
//! 유틸리티
//////////////////////////////////////////////////////////////////////////
static void fixPath(std::string &path);
static void trimLogConfig(std::string &str, std::string extIgnore = std::string());
static std::pair<std::string, std::string> splitPairString(const std::string & str, const std::string & delimiter);
static bool isDirectory(std::string path);
static bool createRecursionDir(std::string path);
static std::string getProcessID();
static std::string getProcessName();

//////////////////////////////////////////////////////////////////////////
//! LockHelper
//////////////////////////////////////////////////////////////////////////
class LockHelper
{
public:
    LockHelper();
    virtual ~LockHelper();

public:
    void lock();
    void unLock();
private:
#ifdef WIN32
    CRITICAL_SECTION _crit;
#else
    pthread_mutex_t  _crit;
#endif
};

//////////////////////////////////////////////////////////////////////////
//! AutoLock
//////////////////////////////////////////////////////////////////////////
class AutoLock
{
public:
    explicit AutoLock(LockHelper & lk):_lock(lk){_lock.lock();}
    ~AutoLock(){_lock.unLock();}
private:
    LockHelper & _lock;
};

//////////////////////////////////////////////////////////////////////////
//! SemHelper
//////////////////////////////////////////////////////////////////////////
class SemHelper
{
public:
    SemHelper();
    virtual ~SemHelper();
public:
    bool create(int initcount);
    bool wait(int timeout = 0);
    bool post();
private:
#ifdef WIN32
    HANDLE _hSem;
#elif defined(__APPLE__)
    dispatch_semaphore_t _semid;
#else
    sem_t _semid;
    bool  _isCreate;
#endif

};

//////////////////////////////////////////////////////////////////////////
//! ThreadHelper
//////////////////////////////////////////////////////////////////////////
#ifdef WIN32
static unsigned int WINAPI  threadProc(LPVOID lpParam);
#else
static void * threadProc(void * pParam);
#endif

class ThreadHelper
{
public:
    ThreadHelper(){_hThreadID = 0;}
    virtual ~ThreadHelper(){}
public:
    bool start();
    bool wait();
    virtual void run() = 0;
private:
    unsigned long long _hThreadID;
#ifndef WIN32
    pthread_t _phtreadID;
#endif
};

#ifdef WIN32
unsigned int WINAPI  threadProc(LPVOID lpParam)
{
    ThreadHelper * p = (ThreadHelper *) lpParam;
    p->run();
    return 0;
}
#else
void * threadProc(void * pParam)
{
    ThreadHelper * p = (ThreadHelper *) pParam;
    p->run();
    return nullptr;
}
#endif

//////////////////////////////////////////////////////////////////////////
//! LogData
//////////////////////////////////////////////////////////////////////////
enum LogDataType
{
    LDT_GENERAL,
    LDT_ENABLE_LOGGER,
    LDT_SET_LOGGER_NAME,
    LDT_SET_LOGGER_PATH,
    LDT_SET_LOGGER_LEVEL,
    LDT_SET_LOGGER_FILELINE,
    LDT_SET_LOGGER_DISPLAY,
    LDT_SET_LOGGER_OUTFILE,
    LDT_SET_LOGGER_LIMITSIZE,
	LDT_SET_LOGGER_DAYDIR,
	LDT_SET_LOGGER_MONTHDIR,
	LDT_SET_LOGGER_RESERVETIME,
	//    LDT_SET_LOGGER_,
};

//////////////////////////////////////////////////////////////////////////
//! LoggerInfo
//////////////////////////////////////////////////////////////////////////
struct LoggerInfo 
{
	//! 속성
	std::string						_key;						// 로거 키
	std::string						_name;						// 로거당 이름 1개.
	std::string						_path;						// 로그 파일 경로.
	int								_level;						// 필터 레벨
	bool							_display;					// 화면 표시
	bool							_outfile;					// 파일 출력
	bool							_daydir;					// 일별 디렉터리 생성
	bool							_monthdir;					// 월별 디렉터리 생성 
	unsigned int					_limitsize;					// 파일 크기 제한(백만 바이트).
	bool							_enable;					// 로거 활성화 여부 
	bool							_fileLine;					// 로그 접미사(파일명:행번호) 활성/비활성
	time_t							_logReserveTime;			// 로그 파일 보관 시간(초).
	//! 런타임 정보
	time_t							_curFileCreateTime;			// 파일 생성 시각
	time_t							_curFileCreateDay;			// 파일 생성 일자
	unsigned int					_curFileIndex;				// 롤링 파일 인덱스
	unsigned int					_curWriteLen;				// 현재 파일 길이
	Log4zFileHandler				_handle;					// 파일 핸들.
	//! 이력
	std::list<std::pair<time_t, std::string> > _historyLogs;

	LoggerInfo()
	{
		_enable = false; 
		_path = LOG4Z_DEFAULT_PATH; 
		_level = LOG4Z_DEFAULT_LEVEL; 
		_display = LOG4Z_DEFAULT_DISPLAY; 
		_outfile = LOG4Z_DEFAULT_OUTFILE;

		_daydir = LOG4Z_DEFAULT_DAYDIR; 
		_monthdir = LOG4Z_DEFAULT_MONTHDIR; 
		_limitsize = LOG4Z_DEFAULT_LIMITSIZE;
		_fileLine = LOG4Z_DEFAULT_SHOWSUFFIX;

		_curFileCreateTime = 0;
		_curFileCreateDay = 0;
		_curFileIndex = 0;
		_curWriteLen = 0;
		_logReserveTime = 0;
	}
};

//////////////////////////////////////////////////////////////////////////
//! LogerManager
//////////////////////////////////////////////////////////////////////////
class LogerManager : public ThreadHelper, public ILog4zManager
{
public:
    LogerManager();
    virtual ~LogerManager();
    
    bool configFromStringImpl(std::string content, bool isUpdate);
    //! 读取配置文件并覆写
    virtual bool config(const char* configPath);
    virtual bool configFromString(const char* configContent);

    //! 覆写式创建
    virtual LoggerId createLogger(const char* key);
    virtual bool start();
    virtual bool stop();
    virtual bool prePushLog(LoggerId id, int level);
    virtual bool pushLog(LogData * pLog, const char * file, int line);
    //!ID 조회
    virtual LoggerId findLogger(const char*  key);
    bool hotChange(LoggerId id, LogDataType ldt, int num, const std::string & text);
    virtual bool enableLogger(LoggerId id, bool enable);
    virtual bool setLoggerName(LoggerId id, const char * name);
    virtual bool setLoggerPath(LoggerId id, const char * path);
    virtual bool setLoggerLevel(LoggerId id, int nLevel);
    virtual bool setLoggerFileLine(LoggerId id, bool enable);
    virtual bool setLoggerDisplay(LoggerId id, bool enable);
    virtual bool setLoggerOutFile(LoggerId id, bool enable);
    virtual bool setLoggerLimitsize(LoggerId id, unsigned int limitsize);
    virtual bool setLoggerDaydir(LoggerId id, bool enable);
    virtual bool setLoggerMonthdir(LoggerId id, bool enable);
	virtual bool setLoggerReserveTime(LoggerId id, time_t sec);
    virtual bool setAutoUpdate(int interval);
    virtual bool updateConfig();
    virtual bool isLoggerEnable(LoggerId id);
    virtual unsigned long long getStatusTotalWriteCount(){return _ullStatusTotalWriteFileCount;}
    virtual unsigned long long getStatusTotalWriteBytes() { return _ullStatusTotalWriteFileBytes; }
    virtual unsigned long long getStatusTotalPushQueue() { return _ullStatusTotalPushLog; }
    virtual unsigned long long getStatusTotalPopQueue() { return _ullStatusTotalPopLog; }
    virtual unsigned int getStatusActiveLoggers();
protected:
    virtual LogData * makeLogData(LoggerId id, int level);
    virtual void freeLogData(LogData *log);
    void showColorText(const char *text, int level = LOG_LEVEL_DEBUG);
    bool onHotChange(LoggerId id, LogDataType ldt, int num, const std::string & text);
    bool openLogger(LogData * log);
    bool closeLogger(LoggerId id);
    bool popLog(LogData *& log);
    virtual void run();
private:

    //! thread status.
    bool        _runing;
    //! 스레드 시작 대기.
    SemHelper        _semaphore;

    //! 단일 로거 이름/경로 핫 변경
    int _hotUpdateInterval;
    unsigned int _checksum;

    //! 프로세스 정보.
    std::string _pid;
    std::string _proName;

    //! 설정 파일명
    std::string _configFile;

    //! 로거 ID 관리자 [로거명]:[로거ID].
    std::map<std::string, LoggerId> _ids; 
    // _loggers 마지막 사용 ID
    LoggerId    _lastId; 
    LoggerInfo _loggers[LOG4Z_LOGGER_MAX];

    //! log queue
    char _chunk1[256];
    LockHelper    _logLock;
    std::deque<LogData *> _logs;
    unsigned long long _ullStatusTotalPushLog;

    char _chunk2[256];
    LockHelper    _freeLock;
    std::vector<LogData*> _freeLogDatas;

    char _chunk3[256];
    //show color 잠금
    LockHelper _scLock;
    //status statistics
    //write file
    char _chunk4[256];
    std::deque<LogData *> _logsCache;
    unsigned long long _ullStatusTotalPopLog;
    unsigned long long _ullStatusTotalWriteFileCount;
    unsigned long long _ullStatusTotalWriteFileBytes;
};

//////////////////////////////////////////////////////////////////////////
//! Log4zFileHandler
//////////////////////////////////////////////////////////////////////////
const std::string Log4zFileHandler::readContent()
{
    std::string content;

    if (!_file)
    {
        return content;
    }

    char buf[BUFSIZ];
    size_t ret = 0;
    do  
    {
        ret = fread(reinterpret_cast<void *>(buf), sizeof(char), static_cast<size_t>(BUFSIZ), _file);
        content.append(buf, ret);
    }
    while (ret == static_cast<size_t>(BUFSIZ));

    return content;
}

//////////////////////////////////////////////////////////////////////////
//! 유틸리티
//////////////////////////////////////////////////////////////////////////
static inline void sleepMillisecond(unsigned int ms)
{
#ifdef WIN32
    ::Sleep(ms);
#else
    usleep(1000*ms);
#endif
}

static inline struct tm timeToTm(time_t t)
{
#ifdef WIN32
#if _MSC_VER < 1400 //VS2003
    return * localtime(&t);
#else //vs2005->vs2013->
    struct tm tt = { 0 };
    localtime_s(&tt, &t);
    return tt;
#endif
#else //linux
    struct tm tt;
    localtime_r(&t, &tt);
    return tt;
#endif
}

static void fixPath(std::string &path)
{
    if (path.empty()){return;}
    for (std::string::iterator iter = path.begin(); iter != path.end(); ++iter)
    {
        if (*iter == '\\'){*iter = '/';}
    }
    if (path.at(path.length()-1) != '/'){path.append("/");}
}

static void trimLogConfig(std::string &str, std::string extIgnore)
{
    if (str.empty()){return;}
    extIgnore += "\n\t ";
    int length = (int)str.length();
    int posBegin = 0;
    int posEnd = 0;

    //UTF-8 파일 BOM 제거
    if (str.length() >= 3 
        && (unsigned char)str[0] == 0xef
        && (unsigned char)str[1] == 0xbb
        && (unsigned char)str[2] == 0xbf)
    {
        posBegin = 3;
    }

    //trim character 
    for (int i = posBegin; i<length; i++)
    {
        bool bCheck = false;
        for (int j = 0; j < (int)extIgnore.length(); j++)
        {
            if (str[i] == extIgnore[j])
            {
                bCheck = true;
            }
        }
        if (bCheck)
        {
            if (i == posBegin)
            {
                posBegin++;
            }
        }
        else
        {
            posEnd = i + 1;
        }
    }

    if (posBegin < posEnd)
    {
        str = str.substr(posBegin, posEnd-posBegin);
    }
    else
    {
        str.clear();
    }
}

//split
static std::pair<std::string, std::string> splitPairString(const std::string & str, const std::string & delimiter)
{
    std::string::size_type pos = str.find(delimiter.c_str());
    if (pos == std::string::npos)
    {
        return std::make_pair(str, "");
    }
    return std::make_pair(str.substr(0, pos), str.substr(pos+delimiter.length()));
}

static bool parseConfigLine(const std::string& line, int curLineNum, std::string & key, std::map<std::string, LoggerInfo> & outInfo)
{
    std::pair<std::string, std::string> kv = splitPairString(line, "=");
    if (kv.first.empty())
    {
        return false;
    }

    trimLogConfig(kv.first);
    trimLogConfig(kv.second);
    if (kv.first.empty() || kv.first.at(0) == '#')
    {
        return true;
    }

    if (kv.first.at(0) == '[')
    {
        trimLogConfig(kv.first, "[]");
        key = kv.first;
        {
            std::string tmpstr = kv.first;
            std::transform(tmpstr.begin(), tmpstr.end(), tmpstr.begin(), ::tolower);
            if (tmpstr == "main")
            {
                key = "Main";
            }
        }
        std::map<std::string, LoggerInfo>::iterator iter = outInfo.find(key);
        if (iter == outInfo.end())
        {
            LoggerInfo li;
            li._enable = true;
            li._key = key;
            li._name = key;
            outInfo.insert(std::make_pair(li._key, li));
        }
        else
        {
			printf("configure warning: duplicate logger key:[%s] at line: %d\n", key.c_str(), curLineNum);
        }
        return true;
    }
    trimLogConfig(kv.first);
    trimLogConfig(kv.second);
    std::map<std::string, LoggerInfo>::iterator iter = outInfo.find(key);
    if (iter == outInfo.end())
    {
		printf("configure warning: not found current logger name:[%s] at line:%d, key=%s, value=%s\n", 
			key.c_str(), curLineNum, kv.first.c_str(), kv.second.c_str());
        return true;
    }
    std::transform(kv.first.begin(), kv.first.end(), kv.first.begin(), ::tolower);
    //! path
    if (kv.first == "path")
    {
        iter->second._path = kv.second;
        return true;
    }
    else if (kv.first == "name")
    {
        iter->second._name = kv.second;
        return true;
    }
    std::transform(kv.second.begin(), kv.second.end(), kv.second.begin(), ::tolower);
    //! level
    if (kv.first == "level")
    {
        if (kv.second == "trace" || kv.second == "all")
        {
            iter->second._level = LOG_LEVEL_TRACE;
        }
        else if (kv.second == "debug")
        {
            iter->second._level = LOG_LEVEL_DEBUG;
        }
        else if (kv.second == "info")
        {
            iter->second._level = LOG_LEVEL_INFO;
        }
        else if (kv.second == "warn" || kv.second == "warning")
        {
            iter->second._level = LOG_LEVEL_WARN;
        }
        else if (kv.second == "error")
        {
            iter->second._level = LOG_LEVEL_ERROR;
        }
        else if (kv.second == "alarm")
        {
            iter->second._level = LOG_LEVEL_ALARM;
        }
        else if (kv.second == "fatal")
        {
            iter->second._level = LOG_LEVEL_FATAL;
        }
    }
    //! display
    else if (kv.first == "display")
    {
        if (kv.second == "false" || kv.second == "0")
        {
            iter->second._display = false;
        }
        else
        {
            iter->second._display = true;
        }
    }
    //! 파일 출력
    else if (kv.first == "outfile")
    {
        if (kv.second == "false" || kv.second == "0")
        {
            iter->second._outfile = false;
        }
        else
        {
            iter->second._outfile = true;
        }
    }
	//! daydir
	else if (kv.first == "daydir")
	{
		if (kv.second == "false" || kv.second == "0")
		{
			iter->second._daydir = false;
		}
		else
		{
			iter->second._daydir = true;
		}
	}
    //! monthdir
    else if (kv.first == "monthdir")
    {
        if (kv.second == "false" || kv.second == "0")
        {
            iter->second._monthdir = false;
        }
        else
        {
            iter->second._monthdir = true;
        }
    }
    //! limit file size
    else if (kv.first == "limitsize")
    {
        iter->second._limitsize = atoi(kv.second.c_str());
    }
    //! 파일·행번호 로그 표시
    else if (kv.first == "fileline")
    {
        if (kv.second == "false" || kv.second == "0")
        {
            iter->second._fileLine = false;
        }
        else
        {
            iter->second._fileLine = true;
        }
    }
    //! enable/disable one logger
    else if (kv.first == "enable")
    {
        if (kv.second == "false" || kv.second == "0")
        {
            iter->second._enable = false;
        }
        else
        {
            iter->second._enable = true;
        }
    }
	//! set reserve time
	else if (kv.first == "reserve")
	{
		iter->second._logReserveTime = atoi(kv.second.c_str());
	}
    return true;
}

static bool parseConfigFromString(std::string content, std::map<std::string, LoggerInfo> & outInfo)
{

    std::string key;
    int curLine = 1;
    std::string line;
    std::string::size_type curPos = 0;
    if (content.empty())
    {
        return true;
    }
    do
    {
        std::string::size_type pos = std::string::npos;
        for (std::string::size_type i = curPos; i < content.length(); ++i)
        {
            //support linux/unix/windows LRCF
            if (content[i] == '\r' || content[i] == '\n')
            {
                pos = i;
                break;
            }
        }
        line = content.substr(curPos, pos - curPos);
        parseConfigLine(line, curLine, key, outInfo);
        curLine++;

        if (pos == std::string::npos)
        {
            break;
        }
        else
        {
            curPos = pos+1;
        }
    } while (1);
    return true;
}

bool isDirectory(std::string path)
{
#ifdef WIN32
    return PathIsDirectoryA(path.c_str()) ? true : false;
#else
    DIR * pdir = opendir(path.c_str());
    if (pdir == nullptr)
    {
        return false;
    }
    else
    {
        closedir(pdir);
        pdir = nullptr;
        return true;
    }
#endif
}

bool createRecursionDir(std::string path)
{
    if (path.length() == 0) return true;
    std::string sub;
    fixPath(path);

    std::string::size_type pos = path.find('/');
    while (pos != std::string::npos)
    {
        std::string cur = path.substr(0, pos-0);
        if (cur.length() > 0 && !isDirectory(cur))
        {
            bool ret = false;
#ifdef WIN32
            ret = CreateDirectoryA(cur.c_str(), nullptr) ? true : false;
#else
            ret = (mkdir(cur.c_str(), S_IRWXU|S_IRWXG|S_IRWXO) == 0);
#endif
            if (!ret)
            {
                return false;
            }
        }
        pos = path.find('/', pos+1);
    }

    return true;
}

std::string getProcessID()
{
    std::string pid = "0";
    char buf[260] = {0};
#ifdef WIN32
    DWORD winPID = GetCurrentProcessId();
    sprintf(buf, "%06u", winPID);
    pid = buf;
#else
    sprintf(buf, "%06d", getpid());
    pid = buf;
#endif
    return pid;
}

std::string getProcessName()
{
    std::string name = "process";
    char buf[260] = {0};
#ifdef WIN32
    if (GetModuleFileNameA(nullptr, buf, 259) > 0)
    {
        name = buf;
    }
    std::string::size_type pos = name.rfind("\\");
    if (pos != std::string::npos)
    {
        name = name.substr(pos+1, std::string::npos);
    }
    pos = name.rfind(".");
    if (pos != std::string::npos)
    {
        name = name.substr(0, pos-0);
    }

#elif defined(LOG4Z_HAVE_LIBPROC)
    proc_name(getpid(), buf, 260);
    name = buf;
    return name;;
#else
    sprintf(buf, "/proc/%d/cmdline", (int)getpid());
    Log4zFileHandler i;
    i.open(buf, "rb");
    if (!i.isOpen())
    {
        return name;
    }
    name = i.readLine();

    // 주석 처리 (2025-12-08 최정우 주석 처리)
    //i.close();

    std::string::size_type pos = name.rfind("/");
    if (pos != std::string::npos)
    {
        name = name.substr(pos+1, std::string::npos);
    }
#endif

    return name;
}

//////////////////////////////////////////////////////////////////////////
// LockHelper
//////////////////////////////////////////////////////////////////////////
LockHelper::LockHelper()
{
#ifdef WIN32
    InitializeCriticalSection(&_crit);
#else
    //_crit = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&_crit, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}
LockHelper::~LockHelper()
{
#ifdef WIN32
    DeleteCriticalSection(&_crit);
#else
    pthread_mutex_destroy(&_crit);
#endif
}

void LockHelper::lock()
{
#ifdef WIN32
    EnterCriticalSection(&_crit);
#else
    int nResult = pthread_mutex_lock(&_crit);
	if (nResult != 0)
	{
#ifdef LOG4Z_DEBUG
		fprintf(stderr, "pthread_mutex_lock failed! error=[%d] %s\n", 
            nResult, strerror(nResult));
#endif							// if (nResult != 0)
	}
#endif
}
void LockHelper::unLock()
{
#ifdef WIN32
    LeaveCriticalSection(&_crit);
#else
    int nResult = pthread_mutex_unlock(&_crit);
	if (nResult != 0)
	{
#ifdef LOG4Z_DEBUG
		fprintf(stderr, "pthread_mutex_lock failed! error=[%d] %s\n", 
            nResult, strerror(nResult));
#endif
	}							// if (nResult != 0)
#endif
}
//////////////////////////////////////////////////////////////////////////
// SemHelper
//////////////////////////////////////////////////////////////////////////
SemHelper::SemHelper()
{
#ifdef WIN32
    _hSem = nullptr;
#elif defined(__APPLE__)
    _semid = nullptr;
#else
    _isCreate = false;
#endif

}
SemHelper::~SemHelper()
{
#ifdef WIN32
    if (_hSem != nullptr)
    {
        CloseHandle(_hSem);
        _hSem = nullptr;
    }
#elif defined(__APPLE__)
    if (_semid)
    {
        dispatch_release(_semid);
        _semid = nullptr;
    }
#else
    if (_isCreate)
    {
        _isCreate = false;
        sem_destroy(&_semid);
    }
#endif

}

bool SemHelper::create(int initcount)
{
    if (initcount < 0)
    {
        initcount = 0;
    }
#ifdef WIN32
    if (initcount > 64)
    {
        return false;
    }
    _hSem = CreateSemaphore(nullptr, initcount, 64, nullptr);
    if (_hSem == nullptr)
    {
        return false;
    }
#elif defined(__APPLE__)
    _semid = dispatch_semaphore_create(initcount);
    if (!_semid)
    {
        return false;
    }
#else
    if (sem_init(&_semid, 0, initcount) != 0)
    {
        return false;
    }
    _isCreate = true;
#endif

    return true;
}
bool SemHelper::wait(int timeout)
{
#ifdef WIN32
    if (timeout <= 0)
    {
        timeout = INFINITE;
    }
    if (WaitForSingleObject(_hSem, timeout) != WAIT_OBJECT_0)
    {
        return false;
    }
#elif defined(__APPLE__)
    if (dispatch_semaphore_wait(_semid, dispatch_time(DISPATCH_TIME_NOW, timeout*1000)) != 0)
    {
        return false;
    }
#else
    if (timeout <= 0)
    {
        return (sem_wait(&_semid) == 0);
    }
    else
    {
        struct timeval tm;
        gettimeofday(&tm, nullptr);
        long long endtime = tm.tv_sec *1000 + tm.tv_usec/1000 + timeout;
        do 
        {
            sleepMillisecond(50);
            int ret = sem_trywait(&_semid);
            if (ret == 0)
            {
                return true;
            }
            struct timeval tv_cur;
            gettimeofday(&tv_cur, nullptr);
            if (tv_cur.tv_sec*1000 + tv_cur.tv_usec/1000 > endtime)
            {
                return false;
            }

            if (ret == -1 && errno == EAGAIN)
            {
                continue;
            }
            else
            {
                return false;
            }
        } while (true);
        return false;
    }
#endif
    return true;
}

bool SemHelper::post()
{
#ifdef WIN32
    return ReleaseSemaphore(_hSem, 1, nullptr) ? true : false;
#elif defined(__APPLE__)
    return dispatch_semaphore_signal(_semid) == 0;
#else
    return (sem_post(&_semid) == 0);
#endif

}

//////////////////////////////////////////////////////////////////////////
//! ThreadHelper
//////////////////////////////////////////////////////////////////////////
bool ThreadHelper::start()
{
#ifdef WIN32
	unsigned long long ret = _beginthreadex(nullptr, 0, threadProc, (void *) this, 0, nullptr);

	if ((ret == -1) || (ret == 0))
	{
		LOGFMTE("create thread error!");
		return false;
	}
	_hThreadID = ret;
#else
	int ret = pthread_create(&_phtreadID, nullptr, threadProc, (void*)this);
	if (ret != 0)
	{
		LOGFMTE("create thread error!");
		return false;
	}							// if (ret != 0)
#endif
	return true;
}

bool ThreadHelper::wait()
{
#ifdef WIN32
    if (WaitForSingleObject((HANDLE)_hThreadID, INFINITE) != WAIT_OBJECT_0)
    {
        return false;
    }
#else
    if (pthread_join(_phtreadID, nullptr) != 0)
    {
        return false;
    }
#endif
    return true;
}

//////////////////////////////////////////////////////////////////////////
//! LogerManager
//////////////////////////////////////////////////////////////////////////
LogerManager::LogerManager()
{
    _runing = false;
    _lastId = LOG4Z_MAIN_LOGGER_ID;
    _hotUpdateInterval = 0;

    _ullStatusTotalPushLog = 0;
    _ullStatusTotalPopLog = 0;
    _ullStatusTotalWriteFileCount = 0;
    _ullStatusTotalWriteFileBytes = 0;
    
    _pid = getProcessID();
    _proName = getProcessName();
    _loggers[LOG4Z_MAIN_LOGGER_ID]._enable = true;
    _ids[LOG4Z_MAIN_LOGGER_KEY] = LOG4Z_MAIN_LOGGER_ID;
    _loggers[LOG4Z_MAIN_LOGGER_ID]._key = LOG4Z_MAIN_LOGGER_KEY;
    _loggers[LOG4Z_MAIN_LOGGER_ID]._name = LOG4Z_MAIN_LOGGER_KEY;

    _chunk1[0] = '\0';
    _chunk2[1] = '\0';
    _chunk3[2] = '\0';
    _chunk4[3] = '\0';
}
LogerManager::~LogerManager()
{
    stop();
}


LogData * LogerManager::makeLogData(LoggerId id, int level)
{
    LogData * pLog = nullptr;
    if (true)
    {
        if (!_freeLogDatas.empty())
        {
            AutoLock l(_freeLock);
            if (!_freeLogDatas.empty())
            {
                pLog = _freeLogDatas.back();
                _freeLogDatas.pop_back();
            }
        }
        if (pLog == nullptr)
        {
            pLog = new(malloc(sizeof(LogData) + LOG4Z_LOG_BUF_SIZE-1))LogData();
        }
    }
    //append precise time to log
    if (true)
    {
        pLog->_id = id;
        pLog->_level = level;
        pLog->_type = LDT_GENERAL;
        pLog->_typeval = 0;
        pLog->_threadID = 0;
        pLog->_contentLen = 0;
#ifdef WIN32
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long now = ft.dwHighDateTime;
        now <<= 32;
        now |= ft.dwLowDateTime;
        now /= 10;
        now -= 11644473600000000ULL;
        now /= 1000;
        pLog->_time = now / 1000;
        pLog->_precise = (unsigned int)(now % 1000);
#else
        struct timeval tm;
        gettimeofday(&tm, nullptr);
        pLog->_time = tm.tv_sec;
        pLog->_precise = tm.tv_usec / 1000;
#endif
#ifdef WIN32
        pLog->_threadID = GetCurrentThreadId();
#elif defined(__APPLE__)
        unsigned long long tid = 0;
        pthread_threadid_np(nullptr, &tid);
        pLog->_threadID = (unsigned int) tid;
#else
        pLog->_threadID = (unsigned int)syscall(SYS_gettid);
#endif
    }

    //format log
    if (true)
    {
#ifdef WIN32
        static __declspec(thread) tm g_tt = { 0 };
        static __declspec(thread) time_t g_curDayTime =  0 ;
#else
        static __thread tm g_tt;
        static __thread time_t g_curDayTime = 0;
#endif // WIN32
        if (pLog->_time < g_curDayTime || pLog->_time >= g_curDayTime + 24*3600)
        {
            g_tt = timeToTm(pLog->_time);
            g_tt.tm_hour = 0;
            g_tt.tm_min = 0;
            g_tt.tm_sec = 0;
            g_curDayTime = mktime(&g_tt);
        }
        time_t sec = pLog->_time - g_curDayTime;
        Log4zStream ls(pLog->_content, LOG4Z_LOG_BUF_SIZE);
        ls.writeChar('[');
        ls.writeULongLong(g_tt.tm_year+1900, 4);
        ls.writeChar('-');
        ls.writeULongLong(g_tt.tm_mon+1, 2);
        ls.writeChar('-');
        ls.writeULongLong(g_tt.tm_mday, 2);
        ls.writeChar(' ');
        ls.writeULongLong(sec / 3600, 2);
        ls.writeChar(':');
        ls.writeULongLong((sec % 3600) / 60 , 2);
        ls.writeChar(':');
        ls.writeULongLong(sec % 60, 2);
        ls.writeChar('.');
        ls.writeULongLong(pLog->_precise, 3);
        ls.writeChar(']');
        ls.writeChar(' ');
        ls.writeChar('[');
        ls.writeULongLong(pLog->_threadID, 4);
        ls.writeChar(']');

        ls.writeChar(' ');
        ls.writeString(LOG_STRING[pLog->_level], LOG_STRING_LEN[pLog->_level]);
        ls.writeChar(' ');
        pLog->_contentLen = ls.getCurrentLen();
    }
    return pLog;
}
void LogerManager::freeLogData(LogData *log)
{
	// log 가 null 인지 확인 (2025-12-05 최정우추가)
	if (log == nullptr) return;

    if (_freeLogDatas.size() < 200)
    {
        AutoLock l(_freeLock);
        _freeLogDatas.push_back(log);
    }
    else
    {
        log->~LogData();
        free( log);
    }
}

void LogerManager::showColorText(const char *text, int level)
{

#if defined(WIN32) && defined(LOG4Z_OEM_CONSOLE)
    char oem[LOG4Z_LOG_BUF_SIZE] = { 0 };
    CharToOemBuffA(text, oem, LOG4Z_LOG_BUF_SIZE);
#endif

    if (level <= LOG_LEVEL_DEBUG || level > LOG_LEVEL_FATAL)
    {
#if defined(WIN32) && defined(LOG4Z_OEM_CONSOLE)
        printf("%s", oem);
#else
        printf("%s", text);
#endif
        return;
    }
#ifndef WIN32
    printf("%s%s\e[0m", LOG_COLOR[level], text);
#else
    AutoLock l(_scLock);
    HANDLE hStd = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStd == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO oldInfo;
    if (!GetConsoleScreenBufferInfo(hStd, &oldInfo))
    {
        return;
    }
    else
    {
        SetConsoleTextAttribute(hStd, LOG_COLOR[level]);
#ifdef LOG4Z_OEM_CONSOLE
		printf("%s", oem);
#else
		printf("%s", text);
#endif
		SetConsoleTextAttribute(hStd, oldInfo.wAttributes);
    }
#endif
    return;
}

bool LogerManager::configFromStringImpl(std::string content, bool isUpdate)
{
	unsigned int sum = 0;
	for (std::string::iterator iter = content.begin(); iter != content.end(); ++iter)
	{
		sum += (unsigned char)*iter;
	}							// for (std::string::iterator iter = content.시작(); iter != content.끝(); ++iter)

	if (sum == _checksum)
	{
		return true;
	}							// if (sum == _checksum)
	_checksum = sum;

	std::map<std::string, LoggerInfo> loggerMap;
	if (!parseConfigFromString(content, loggerMap))
	{
		// printf 에러 표출 주석 처리 (2025-12-04 최정우 주석 처리)
		/*printf(" !!! !!! !!! !!!\n");
printf(" !!! !!! 설정 파일 로드 오류\n");
		printf(" !!! !!! !!! !!!\n");*/

		// stderr 로 에러 표출 (2025-12-04 최정우 추가)
		fprintf(stderr, " !!! !!! !!! !!!\n");
		fprintf(stderr, " !!! !!! load config file error\n");
		fprintf(stderr, " !!! !!! !!! !!!\n");
		return false;
	}							// if (!parseConfigFromString(content, loggerMap)) 

	for (std::map<std::string, LoggerInfo>::iterator iter = loggerMap.begin(); iter != loggerMap.end(); ++iter)
	{
		LoggerId id = LOG4Z_INVALID_LOGGER_ID;
		id = findLogger(iter->second._key.c_str());
		if (id == LOG4Z_INVALID_LOGGER_ID)
		{
			if (isUpdate)
			{
				continue;
			}
			else
			{
				id = createLogger(iter->second._key.c_str());
				if (id == LOG4Z_INVALID_LOGGER_ID)
				{
					continue;
				}				// if (id == LOG4Z_INVALID_LOGGER_ID)
			}					// if (isUpdate)
		}						// if (id == LOG4Z_INVALID_LOGGER_ID)
		enableLogger(id, iter->second._enable);
		setLoggerName(id, iter->second._name.c_str());
		setLoggerPath(id, iter->second._path.c_str());
		setLoggerLevel(id, iter->second._level);
		setLoggerFileLine(id, iter->second._fileLine);
		setLoggerDisplay(id, iter->second._display);
		setLoggerOutFile(id, iter->second._outfile);
		setLoggerLimitsize(id, iter->second._limitsize);
		setLoggerDaydir(id, iter->second._daydir);
		setLoggerMonthdir(id, iter->second._monthdir);
	}							// for (std::map<std::string, LoggerInfo>::iterator iter = loggerMap.시작(); iter != loggerMap.끝(); ++iter)
	return true;
}

//! 설정 읽기 및 덮어쓰기 생성
bool LogerManager::config(const char* configPath)
{
	// printf 로 에러 표출 주석 처리 (2025-12-04 최정우 주석 처리)
	/*if (!_configFile.empty())
	{
		printf(" !!! !!! !!! !!!\n");
printf(" !!! !!! Config 호출 과다 오류. 이전 설정 파일=%s, 새 설정 파일=%s !!! !!!\n"
		, _configFile.c_str(), configPath);
		printf(" !!! !!! !!! !!!\n");
		return false;
	}*/
	// stderr 로 에러 표출 (2025-12-04 최정우 추가)
	if (!_configFile.empty())
	{
		fprintf(stderr, " !!! !!! !!! !!!\n");

		if (configPath != nullptr)
		{
			fprintf(stderr, " !!! !!! configure error: too many calls to Config. the old config file=%s, the new config file=%s !!! !!!", 
				_configFile.c_str(), configPath);
		}
		else
		{
			fprintf(stderr, " !!! !!! configure error: too many calls to Config. the old config file=%s, the new config file is null !!! !!!", 
				_configFile.c_str());
		}						// if (configPath != nullptr)
		fprintf(stderr, " !!! !!! !!! !!!\n");
		return false;
	}							// if (!_configFile.empty())

	_configFile = configPath;

	Log4zFileHandler f;
	f.open(_configFile.c_str(), "rb");
	if (!f.isOpen())
	{
		// printf 함수 사용 주석 처리 (2025-12-04 최정우 주석 처리)
		/*printf(" !!! !!! !!! !!!\n");
printf(" !!! !!! 설정 파일 로드 오류. filename=%s !!! !!!\n", configPath);
		printf(" !!! !!! !!! !!!\n");*/

		// stderr 로 에러 표출 (2025-12-04 최정우 추가)
		fprintf(stderr, " !!! !!! !!! !!!\n");
		fprintf(stderr, " !!! !!! load config file error. filename=%s  !!! !!!\n", configPath);
		fprintf(stderr, " !!! !!! !!! !!!\n");
		return false;
	}							// if (!f.isOpen())
	return configFromStringImpl(f.readContent().c_str(), false);
}

//! 설정 읽기 및 덮어쓰기 생성
bool LogerManager::configFromString(const char* configContent)
{
    return configFromStringImpl(configContent, false);
}

//! 덮어쓰기로 생성
LoggerId LogerManager::createLogger(const char* key)
{
    if (key == nullptr)
    {
        return LOG4Z_INVALID_LOGGER_ID;
    }							// if (키 == nullptr)
    
    std::string copyKey = key;
    trimLogConfig(copyKey);

	LoggerId newID = LOG4Z_INVALID_LOGGER_ID;
	{
		std::map<std::string, LoggerId>::iterator iter = _ids.find(copyKey);
		if (iter != _ids.end())
		{
			newID = iter->second;
		}						// if (iter != _ids.끝())
	}

	if (newID == LOG4Z_INVALID_LOGGER_ID)
	{
		if (_lastId +1 >= LOG4Z_LOGGER_MAX)
		{
			showColorText("CreateLogger can not create|writeover, because loggerid need < LOGGER_MAX!\n", LOG_LEVEL_FATAL);
			return LOG4Z_INVALID_LOGGER_ID;
		}						// if (_lastId +1 >= LOG4Z_LOGGER_MAX)
		newID = ++ _lastId;
		_ids[copyKey] = newID;
		_loggers[newID]._enable = true;
		_loggers[newID]._key = copyKey;
		_loggers[newID]._name = copyKey;
	}							// if (newID == LOG4Z_INVALID_LOGGER_ID)

    return newID;
}

bool LogerManager::start()
{
	if (_runing)
	{
		showColorText("already start\n", LOG_LEVEL_FATAL);
		return false;
	}							// if (_runing)
	_semaphore.create(0);
	bool ret = ThreadHelper::start();
	return ret && _semaphore.wait(3000);
}

bool LogerManager::stop()
{
	if (_runing)
	{
		showColorText("stopping\n", LOG_LEVEL_FATAL);
		_runing = false;
		wait();

		AutoLock l(_freeLock);
		while (!_freeLogDatas.empty())
		{
			// 메모리 해제 후 초기화 해제() 사용을 추가로 안정성 강화 (2025-12-05 최정우 수정)
			LogData *pLog = _freeLogDatas.back();
			if (pLog != nullptr)
			{
				pLog->~LogData();
				free(pLog);
			}					// if (pLog != nullptr)
			_freeLogDatas.pop_back();
		}						// while (!_freeLogDatas.empty())
		return true;
	}							// if (_runing)
	return false;
}

bool LogerManager::prePushLog(LoggerId id, int level)
{
	if (id < 0 || id > _lastId || !_runing || !_loggers[id]._enable)
	{
		return false;
	}							// if (id < 0 || id > _lastId || !_runing || !_loggers[id]._enable)
    
    if (level < _loggers[id]._level)
    {
        return false;
    }							// if (level < _loggers[id]._level)

	size_t count = _logs.size();
	if (count > LOG4Z_LOG_QUEUE_LIMIT_SIZE)
	{
		size_t rate = (count - LOG4Z_LOG_QUEUE_LIMIT_SIZE) * 100 / LOG4Z_LOG_QUEUE_LIMIT_SIZE;
		if (rate > 100)
		{
			rate = 100;
		}						// if (rate > 100)

		//if ((size_t)rand() % 100 < rate)								// 최정우 주석 처리
		// 주석 처리 보완 내용 (2025-12-03 최정우 수정)
		if (backpressure_dis(backpressure_gen) < rate)
		{
			if (rate > 50)
			{
				AutoLock l(_logLock);
				count = _logs.size();
			}					// if (rate > 50)

			if (count > LOG4Z_LOG_QUEUE_LIMIT_SIZE)
			{
				sleepMillisecond((unsigned int)(rate));
			}					// if (count > LOG4Z_LOG_QUEUE_LIMIT_SIZE)
		}						// if (backpressure_dis(backpressure_gen) < rate)
	}							// if (count > LOG4Z_LOG_QUEUE_LIMIT_SIZE)

    return true;
}

bool LogerManager::pushLog(LogData * pLog, const char * file, int line)
{
    // discard log
    if (pLog->_id < 0 || pLog->_id > _lastId || !_runing || !_loggers[pLog->_id]._enable)
    {
        freeLogData(pLog);
		pLog = nullptr;													// pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
        return false;
    }							// if (pLog->_id < 0 || pLog->_id > _lastId || !_runing || !_loggers[pLog->_id]._enable)

    //filter log
    if (pLog->_level < _loggers[pLog->_id]._level)
    {
        freeLogData(pLog);
		pLog = nullptr;													// pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
        return false;
    }							// if (pLog->_level < _loggers[pLog->_id]._level)

    if (_loggers[pLog->_id]._fileLine && file)
    {
		const char * pNameEnd = file + strlen(file);
		const char * pNameBegin = pNameEnd;
		do
		{
			if (*pNameBegin == '\\' || *pNameBegin == '/') { pNameBegin++; break; }
			if (pNameBegin == file) { break; }
			pNameBegin--;
		} while (true);			// do
		zsummer::log4z::Log4zStream ss(pLog->_content + pLog->_contentLen, LOG4Z_LOG_BUF_SIZE - pLog->_contentLen);
		ss.writeChar(' ');
		ss.writeString(pNameBegin, pNameEnd - pNameBegin);
		ss.writeChar(':');
		ss.writeULongLong((unsigned long long)line);
		pLog->_contentLen += ss.getCurrentLen();
    }							//  if (_loggers[pLog->_id]._fileLine && file)

    if (pLog->_contentLen + 2 > LOG4Z_LOG_BUF_SIZE) pLog->_contentLen = LOG4Z_LOG_BUF_SIZE - 3;
    pLog->_content[pLog->_contentLen+0] = '\n';
    pLog->_content[pLog->_contentLen+1] = '\0';
    pLog->_contentLen += 1;


    if (_loggers[pLog->_id]._display && LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
    {
        showColorText(pLog->_content, pLog->_level);
    }							// if (_loggers[pLog->_id]._display && LOG4Z_ALL_SYNCHRONOUS_OUTPUT)

    if (LOG4Z_ALL_DEBUGOUTPUT_DISPLAY && LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
    {
#ifdef WIN32
        OutputDebugStringA(pLog->_content);
#endif
    }							// if (LOG4Z_ALL_DEBUGOUTPUT_DISPLAY && LOG4Z_ALL_SYNCHRONOUS_OUTPUT)

    if (_loggers[pLog->_id]._outfile && LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
    {
        AutoLock l(_logLock);
        if (openLogger(pLog))
        {
            _loggers[pLog->_id]._handle.write(pLog->_content, pLog->_contentLen);
            _loggers[pLog->_id]._curWriteLen += static_cast<unsigned int>(pLog->_contentLen);
            closeLogger(pLog->_id);
            _ullStatusTotalWriteFileCount++;

            // 타입 캐스팅을 통한 파일 길이 안정성 확보 (2025-12-05 최정우 수정)
            _ullStatusTotalWriteFileBytes += static_cast<unsigned long long>(pLog->_contentLen);
        }						// if (openLogger(pLog))
    }							// if (_loggers[pLog->_id]._outfile && LOG4Z_ALL_SYNCHRONOUS_OUTPUT)

    if (LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
    {
        freeLogData(pLog);
		pLog = nullptr;													// pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
        return true;
    }							// if (LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
    
    AutoLock l(_logLock);
    _logs.push_back(pLog);
    _ullStatusTotalPushLog ++;
    return true;
}

LoggerId LogerManager::findLogger(const char * key)
{
    std::map<std::string, LoggerId>::iterator iter;
    iter = _ids.find(key);
    if (iter != _ids.end())
    {
        return iter->second;
    }							// if (iter != _ids.끝())
    return LOG4Z_INVALID_LOGGER_ID;
}

bool LogerManager::hotChange(LoggerId id, LogDataType ldt, int num, const std::string & text)
{
    if (id < 0 || id > _lastId) return false;
    if (text.length() >= LOG4Z_LOG_BUF_SIZE) return false;
    if (!_runing || LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
    {
        return onHotChange(id, ldt, num, text);
    }							// if (!_runing || LOG4Z_ALL_SYNCHRONOUS_OUTPUT)

    LogData * pLog = makeLogData(id, LOG4Z_DEFAULT_LEVEL);
    pLog->_id = id;
    pLog->_type = ldt;
    pLog->_typeval = num;
    memcpy(reinterpret_cast<void *>(pLog->_content), reinterpret_cast<const void *>(text.c_str()), static_cast<size_t>(text.length()));
    pLog->_contentLen = (int)text.length();
    AutoLock l(_logLock);
    _logs.push_back(pLog);
    return true;
}

bool LogerManager::onHotChange(LoggerId id, LogDataType ldt, int num, const std::string & text)
{
    if (id < LOG4Z_MAIN_LOGGER_ID || id > _lastId)
    {
        return false;
    }							// if (id < LOG4Z_MAIN_LOGGER_ID || id > _lastId)

    LoggerInfo &logger = _loggers[id];
    if (ldt == LDT_ENABLE_LOGGER) logger._enable = num != 0;
    else if (ldt == LDT_SET_LOGGER_NAME) logger._name = text;
    else if (ldt == LDT_SET_LOGGER_PATH) logger._path = text;
    else if (ldt == LDT_SET_LOGGER_LEVEL) logger._level = num;
    else if (ldt == LDT_SET_LOGGER_FILELINE) logger._fileLine = num != 0;
    else if (ldt == LDT_SET_LOGGER_DISPLAY) logger._display = num != 0;
    else if (ldt == LDT_SET_LOGGER_OUTFILE) logger._outfile = num != 0;
    else if (ldt == LDT_SET_LOGGER_LIMITSIZE) logger._limitsize = num;
	else if (ldt == LDT_SET_LOGGER_DAYDIR) logger._daydir = num != 0;
	else if (ldt == LDT_SET_LOGGER_MONTHDIR) logger._monthdir = num != 0;
	else if (ldt == LDT_SET_LOGGER_RESERVETIME) logger._logReserveTime = num >= 0 ? num : 0;
	return true;
}

bool LogerManager::enableLogger(LoggerId id, bool enable) 
{
	if (id < 0 || id > _lastId) return false;
	if (enable)
	{
		_loggers[id]._enable = true;
		return true;
	}							// if (enable)
	return hotChange(id, LDT_ENABLE_LOGGER, false, "");
}
bool LogerManager::setLoggerLevel(LoggerId id, int level) 
{ 
	if (id < 0 || id > _lastId) return false;
	if (level <= _loggers[id]._level)
	{
		_loggers[id]._level = level;
		return true;
	}							// if (level <= _loggers[id]._level)
	return hotChange(id, LDT_SET_LOGGER_LEVEL, level, ""); 
}

bool LogerManager::setLoggerDisplay(LoggerId id, bool enable) { return hotChange(id, LDT_SET_LOGGER_DISPLAY, enable, ""); }
bool LogerManager::setLoggerOutFile(LoggerId id, bool enable) { return hotChange(id, LDT_SET_LOGGER_OUTFILE, enable, ""); }
bool LogerManager::setLoggerDaydir(LoggerId id, bool enable) { return hotChange(id, LDT_SET_LOGGER_DAYDIR, enable, ""); }
bool LogerManager::setLoggerMonthdir(LoggerId id, bool enable) { return hotChange(id, LDT_SET_LOGGER_MONTHDIR, enable, ""); }
bool LogerManager::setLoggerFileLine(LoggerId id, bool enable) { return hotChange(id, LDT_SET_LOGGER_FILELINE, enable, ""); }
bool LogerManager::setLoggerReserveTime(LoggerId id, time_t sec) { return hotChange(id, LDT_SET_LOGGER_RESERVETIME, (int)sec, ""); }
bool LogerManager::setLoggerLimitsize(LoggerId id, unsigned int limitsize)
{
    if (limitsize == 0) {limitsize = (unsigned int)-1;}
    return hotChange(id, LDT_SET_LOGGER_LIMITSIZE, limitsize, "");
}

bool LogerManager::setLoggerName(LoggerId id, const char * name)
{
	if (id < 0 || id > _lastId) return false;
	if (name == nullptr || strlen(name) == 0) 
	{
		return false;
	}							// if (이름 == nullptr || strlen(이름) == 0)

	return hotChange(id, LDT_SET_LOGGER_NAME, 0, name);
}

bool LogerManager::setLoggerPath(LoggerId id, const char * path)
{
	if (id <0 || id > _lastId) return false;
	if (path == nullptr || strlen(path) == 0)  return false;
	std::string copyPath = path;
	{
		char ch = copyPath.at(copyPath.length() - 1);
		if (ch != '\\' && ch != '/')
		{
			copyPath.append("/");
		}						// if (ch != '\\' && ch != '/')
	}

	return hotChange(id, LDT_SET_LOGGER_PATH, 0, copyPath);
}

bool LogerManager::setAutoUpdate(int interval)
{
	_hotUpdateInterval = interval;
	return true;
}

bool LogerManager::updateConfig()
{
	if (_configFile.empty())
	{
		//LOGW("update config file 오류. filename is empty.");
		return false;
	}							// if (_configFile.empty())

	Log4zFileHandler f;
	f.open(_configFile.c_str(), "rb");

	// 주석 처리 (2025-12-04 최정우 주석 처리)
#if 0
	if (!f.isOpen())
	{
		printf(" !!! !!! !!! !!!\n");
		printf(" !!! !!! load config file error. filename=%s  !!! !!!\n", _configFile.c_str());
		printf(" !!! !!! !!! !!!\n");
		return false;
	}
#endif

	// 에러 처리 대안 방안 (2025-12-04 최정우 추가)
	if (!f.isOpen())
	{
		fprintf(stderr, " !!! !!! !!! !!!\n");
		fprintf(stderr, " !!! !!! load config file error. filename=%s  !!! !!!\n", _configFile.c_str());
		fprintf(stderr, " !!! !!! !!! !!!\n");
		return false;
	}							// if (!f.isOpen())
	return configFromStringImpl(f.readContent().c_str(), true);
}

bool LogerManager::isLoggerEnable(LoggerId id)
{
    if (id < 0 || id > _lastId) return false;
    return _loggers[id]._enable;
}

unsigned int LogerManager::getStatusActiveLoggers()
{
	unsigned int actives = 0;
	for (int i=0; i<= _lastId; i++)
	{
		if (_loggers[i]._enable)
		{
			actives ++;
		}						// if (_loggers[i]._enable)
	}							// for (int i=0; i<= _lastId; i++)
    return actives;
}

bool LogerManager::openLogger(LogData * pLog)
{
	int id = pLog->_id;
	if (id < 0 || id >_lastId)
	{
		showColorText("openLogger can not open, invalide logger id!\n", LOG_LEVEL_FATAL);
		return false;
	}							// if (id < 0 || id >_lastId)

	LoggerInfo * pLogger = &_loggers[id];
	if (!pLogger->_enable || !pLogger->_outfile || pLog->_level < pLogger->_level)
	{
		return false;
	}							// if (!pLogger->_enable || !pLogger->_outfile || pLog->_level < pLogger->_level)

	bool sameday = pLog->_time >= pLogger->_curFileCreateDay && pLog->_time - pLogger->_curFileCreateDay < 24*3600;
	bool needChageFile = pLogger->_curWriteLen > pLogger->_limitsize * 1024 * 1024;
	if (!sameday || needChageFile)
	{
		if (!sameday)
		{
			pLogger->_curFileIndex = 0;
		}
		else
		{
			pLogger->_curFileIndex++;
		}						// if (!sameday)

		if (pLogger->_handle.isOpen())
		{
			pLogger->_handle.close();
		}						// if (pLogger->_handle.isOpen())
	}							// if (!sameday || needChageFile)

	if (!pLogger->_handle.isOpen())
	{
		pLogger->_curFileCreateTime = pLog->_time;
		pLogger->_curWriteLen = 0;

		tm t = timeToTm(pLogger->_curFileCreateTime);
		if (true) //process 일 time
		{
			tm day = t;
			day.tm_hour = 0;
			day.tm_min = 0;
			day.tm_sec = 0;
			pLogger->_curFileCreateDay = mktime(&day);
		}						// if (true)
		
		std::string name;
		std::string path;

		name = pLogger->_name;
		path = pLogger->_path;
		
		char buf[500] = { 0 };

		if (pLogger->_daydir)
		{
			sprintf(buf, "%04d%02d%02d/", t.tm_year+1900, t.tm_mon+1, t.tm_mday);
			path += buf;
		}						// if (pLogger->_daydir)

		if (pLogger->_monthdir)
		{
			sprintf(buf, "%04d%02d/", t.tm_year+1900, t.tm_mon+1);
			path += buf;
		}						// if (pLogger->_monthdir)

		if (!isDirectory(path))
		{
			createRecursionDir(path);
		}						// if (!isDirectory(path))

		if (LOG4Z_ALL_SYNCHRONOUS_OUTPUT)
		{
			//sprintf(buf, "%s_%s_%04d%02d%02d%02d_%s_%03u.log",
			// _proName.c_str(), 이름.c_str(), t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			//    t.tm_hour, _pid.c_str(), pLogger->_curFileIndex);

			// 2020-01-16 Modify
			//sprintf(buf, "%s_%04d%02d%02d%02d_%03u.log",
			//	_proName.c_str(), t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			//	t.tm_hour, pLogger->_curFileIndex);

			// 2022-02-04 Modify
			sprintf(buf, "%s_%04d%02d%02d%02d_%03u.log",
				_proName.c_str(), t.tm_year+1900, t.tm_mon+1, t.tm_mday,
				t.tm_hour, pLogger->_curFileIndex);
		}
		else
		{
			//sprintf(buf, "%s_%s_%04d%02d%02d%02d%02d_%s_%03u.log",
			// _proName.c_str(), 이름.c_str(), t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			//    t.tm_hour, t.tm_min, _pid.c_str(), pLogger->_curFileIndex);
			
			// 2020-01-16 Modify
			//sprintf(buf, "%s_%04d%02d%02d%02d%02d_%03u.log",
			//	_proName.c_str(), t.tm_year+1900, t.tm_mon+1, t.tm_mday,
			//	t.tm_hour, t.tm_min, pLogger->_curFileIndex);
			
			// 2022-02-04 Modify
			sprintf(buf, "%s_%04d%02d%02d%02d_%03u.log",
				_proName.c_str(), t.tm_year+1900, t.tm_mon+1, t.tm_mday,
				t.tm_hour, pLogger->_curFileIndex);
		}						// if (LOG4Z_ALL_SYNCHRONOUS_OUTPUT)

		path += buf;
		long curLen = pLogger->_handle.open(path.c_str(), "ab");
		if (!pLogger->_handle.isOpen() || curLen < 0)
		{
			sprintf(buf, "can not open log file %s.\n", path.c_str());
			showColorText("!!!!!!!!!!!!!!!!!!!!!!!!!!\n", LOG_LEVEL_FATAL);
			showColorText(buf, LOG_LEVEL_FATAL);
			showColorText("!!!!!!!!!!!!!!!!!!!!!!!!!!\n", LOG_LEVEL_FATAL);
			pLogger->_outfile = false;
			return false;
		}						// if (!pLogger->_handle.isOpen() || curLen < 0)
		pLogger->_curWriteLen = (unsigned int)curLen;

		if (pLogger->_logReserveTime > 0)
		{
			if (pLogger->_historyLogs.size() > LOG4Z_FORCE_RESERVE_FILE_COUNT)
			{
				while (!pLogger->_historyLogs.empty() && pLogger->_historyLogs.front().first < time(nullptr) - pLogger->_logReserveTime)
				{
					pLogger->_handle.removeFile(pLogger->_historyLogs.front().second.c_str());
					pLogger->_historyLogs.pop_front();
				}				// while (!pLogger->_이력Logs.empty() && pLogger->_이력Logs.앞().첫 < time(nullptr) - pLogger->_logReserveTime)
			}					// if (pLogger->_이력Logs.size() > LOG4Z_FORCE_RESERVE_FILE_COUNT)

			if (pLogger->_historyLogs.empty() || pLogger->_historyLogs.back().second != path)
			{
				pLogger->_historyLogs.push_back(std::make_pair(time(nullptr), path));
			}					// if (pLogger->_이력Logs.empty() || pLogger->_이력Logs.뒤().초 != path)
		}						// if (pLogger->_logReserveTime > 0)
		return true;
	}							// if (!pLogger->_handle.isOpen())
	return true;
}

bool LogerManager::closeLogger(LoggerId id)
{
	if (id < 0 || id >_lastId)
	{
		showColorText("closeLogger can not close, invalide logger id!", LOG_LEVEL_FATAL);
		return false;
	}							// if (id < 0 || id >_lastId)

	LoggerInfo * pLogger = &_loggers[id];
	if (pLogger->_handle.isOpen())
	{
		pLogger->_handle.close();
		return true;
	}							// if (pLogger->_handle.isOpen())
	return false;
}

bool LogerManager::popLog(LogData *& log)
{
	if (_logsCache.empty())
	{
		if (!_logs.empty())
		{
			AutoLock l(_logLock);
			if (_logs.empty())
			{
				// LogData 인 log 초기화 (2025-12-05 최정우 추가)
				log = nullptr;
				return false;
			}					// if (_logs.empty())
			_logsCache.swap(_logs);
		}						// if (!_logs.empty())
	}							// if (_logsCache.empty())

	if (!_logsCache.empty())
	{
		log = _logsCache.front();
		_logsCache.pop_front();

		// 반환 전 포인터 유효성 검증 추가 (2025-12-05 최정우 추가)
		if (log == nullptr) return false;
		return true;
	}							// if (!_logsCache.empty())

	// log 초기화 추가 (2025-12-05 최정우 추가)
	log = nullptr;
	return false;
}

void LogerManager::run()
{
	_runing = true;

	LOGA("-----------------  thread started!   ----------------------------");

	// _lastId 범위 검증 및 안전란 반복 (2025-12-05 최정우 수정)
	int nMaxId = (_lastId < LOG4Z_LOGGER_MAX) ? _lastId : (LOG4Z_LOGGER_MAX - 1);
	if (nMaxId < 0) nMaxId = 0;

	for (int i=0; i<=nMaxId; i++)
	{
		// 배열 범위 추가 검증 (2025-12-03 최정우 추가)
		if  ((i >= 0) && (i < LOG4Z_LOGGER_MAX))
		{
			if (_loggers[i]._enable)
			{
				LOGA("logger id=" << i
					<< " key=" << _loggers[i]._key
					<< " name=" << _loggers[i]._name
					<< " path=" << _loggers[i]._path
					<< " level=" << _loggers[i]._level
					<< " display=" << _loggers[i]._display);
			}						// if (_loggers[i]._enable)
		}							// if  ((i >= 0) && (i < LOG4Z_LOGGER_MAX))
	}								// for (int i=0; i<=nMaxId; i++)

	_semaphore.post();

	LogData *pLog = nullptr;
	int needFlush[LOG4Z_LOGGER_MAX] = {0};
	time_t lastCheckUpdate = time(nullptr);

	while (true)
	{
		while (popLog(pLog))
		{
			// 포인터 유효성 검증 (2025-12-05 최정우 추가)
			if (pLog == nullptr) continue;

			// log id 가 최소값 이상인지 유효성 검사 (2025-12-05 최정우 추가)
			if (pLog->_id < 0)
			{
				freeLogData(pLog);
				pLog = nullptr;
				continue;
			}					// if (pLog->_id < 0)

			// log id 가 최대값 이상인지 유효성 검사
			// 인댁스이므로 (n-1) 까지 유효 (2025-12-05 최정우 추가)
			if (pLog->_id >= LOG4Z_LOGGER_MAX)
			{
				freeLogData(pLog);
				pLog = nullptr;
				continue;
			}					// if (pLog->_id >= LOG4Z_LOGGER_MAX)

			// _lastId가 배열 크기를 초과할 수 있으므로 안전한 값으로 제한 (2025-12-05 최정우 추가)
			int nSafeLastId = (_lastId < LOG4Z_LOGGER_MAX) ? _lastId : (LOG4Z_LOGGER_MAX - 1);
			if (nSafeLastId < 0) nSafeLastId = 0;

			// _lastId 범위 초과 방지 (2025-12-05 최정우 추가)
			if (pLog->_id > nSafeLastId)
			{
				freeLogData(pLog);
				pLog = nullptr;
				continue;
			}					// if (pLog->_id > nSafeLastId)

			LoggerInfo &curLogger = _loggers[pLog->_id];

			if (pLog->_type != LDT_GENERAL)
			{
				onHotChange(pLog->_id, (LogDataType)pLog->_type, pLog->_typeval, std::string(pLog->_content, pLog->_contentLen));
				curLogger._handle.close();
				freeLogData(pLog);
                pLog = nullptr;											// pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
				continue;
			}					// if (pLog->_type != LDT_GENERAL)

			_ullStatusTotalPopLog++;
			
			if (!curLogger._enable || pLog->_level <curLogger._level)
			{
				freeLogData(pLog);
                pLog = nullptr;											// pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
				continue;
			}					// if (!curLogger._enable || pLog->_level <curLogger._level)

			if (curLogger._display)
			{
				showColorText(pLog->_content, pLog->_level);
			}					// if (curLogger._display)

			if (LOG4Z_ALL_DEBUGOUTPUT_DISPLAY)
			{
#ifdef WIN32
				OutputDebugStringA(pLog->_content);
#endif
			}					// if (LOG4Z_ALL_DEBUGOUTPUT_DISPLAY)

			if (curLogger._outfile)
			{
				if (!openLogger(pLog))
				{
					freeLogData(pLog);
                    pLog = nullptr;                                     // pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
					continue;
				}				// if (!openLogger(pLog))

				curLogger._handle.write(pLog->_content, pLog->_contentLen);
				curLogger._curWriteLen += (unsigned int)pLog->_contentLen;
				needFlush[pLog->_id] ++;
				_ullStatusTotalWriteFileCount++;

                // 타입 캐스팅을 통한 파일 길이 안정성 확보 (2025-12-05 최정우 수정)
				_ullStatusTotalWriteFileBytes += static_cast<unsigned long long>(pLog->_contentLen);
			}
			else 
			{
				_ullStatusTotalWriteFileCount++;

                // 타입 캐스팅을 통한 파일 길이 안정성 확보 (2025-12-05 최정우 수정)
				_ullStatusTotalWriteFileBytes += static_cast<unsigned long long>(pLog->_contentLen);
			}					// if (curLogger._outfile)

			freeLogData(pLog);
            pLog = nullptr;                                             // pLog 메모리 해제 후 초기화 추가 (2025-12-05 최정우 추가)
		}						// while (popLog(pLog))

		for (int i=0; i<=_lastId; i++)
		{
			if (_loggers[i]._enable && needFlush[i] > 0)
			{
				_loggers[i]._handle.flush();
				needFlush[i] = 0;
			}					// if (_loggers[i]._enable && needFlush[i] > 0)

			if (!_loggers[i]._enable && _loggers[i]._handle.isOpen())
			{
				_loggers[i]._handle.close();
			}					// if (!_loggers[i]._enable && _loggers[i]._handle.isOpen())
		}						// for (int i=0; i<=_lastId; i++)

		//! delay. 
		sleepMillisecond(50);

		//! quit
		if (!_runing && _logs.empty())
		{
			break;
		}						// if (!_runing && _logs.empty())

		if (_hotUpdateInterval != 0 && time(nullptr) - lastCheckUpdate > _hotUpdateInterval)
		{
			updateConfig();
			lastCheckUpdate = time(nullptr);
		}						// if (_hotUpdateInterval != 0 && time(nullptr) - lastCheckUpdate > _hotUpdateInterval)
	}

	for (int i=0; i <= _lastId; i++)
	{
		if (_loggers[i]._enable)
		{
			_loggers[i]._enable = false;
			closeLogger(i);
		}						// if (_loggers[i]._enable)
	}							// for (int i=0; i <= _lastId; i++)
}

//////////////////////////////////////////////////////////////////////////
//ILog4zManager::getInstance
//////////////////////////////////////////////////////////////////////////
ILog4zManager * ILog4zManager::getInstance()
{
    static LogerManager m;
    return &m;
}

_ZSUMMER_LOG4Z_END
_ZSUMMER_END
