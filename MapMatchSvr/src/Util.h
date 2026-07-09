/**
 * @file Util.h
 * @brief 유틸리티 클래스 헤더
*/
#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdio.h>
#include <unistd.h>
#include <iconv.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <netinet/in.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include "TypeDefine.h"
#include "log4z.h"

using namespace zsummer::log4z;
using namespace std;

#define MIN(x, y)					((x) > (y) ? (y) : (x))
#define MAX(x, y)					((x) > (y) ? (x) : (y))

#define TRIM_SPACE					" \t\n\v"

namespace uspace
{
	inline string trim(string& origin, const string& drop = TRIM_SPACE)
	{
		string revise = origin.erase(origin.find_last_not_of(drop) + 1);
		return revise.erase(0, revise.find_first_not_of(drop));
	}

	inline string ltrim(string& origin, const string& drop = TRIM_SPACE)
	{
		return origin.erase(0, origin.find_first_not_of(drop));
	}

	inline string rtrim(string& origin, const string& drop = TRIM_SPACE)
	{
		return origin.erase(origin.find_last_not_of(drop) + 1);
	}
}

using uspace::trim;
using uspace::ltrim;
using uspace::rtrim;

/**
 * @brief endian 변환
 * @param[in] data 원본 데이터
 * @return endian 변환 데이터
*/
template <typename T>
T swap_endian(T data)
{
	union
	{
		T data1;
		byte data2[sizeof(T)];
	} source, dest;

	source.data1 = data;

	for (size_t i=0; i<sizeof(T); ++i)
		dest.data2[i] = source.data2[sizeof(T) - i - 1];

	return dest.data1;
}

/**
 * @class CUtil
 * @brief 유틸리티 클래스
*/
class CUtil
{
public:
	CUtil() {};
	~CUtil() {};

	bool StringSplit(string data, string delimiter, vector<string> *pvtStringList);
	bool StringSplit(string data, string delimiter, vector<int> *pvtIntList);
	bool StringSplit(string data, string delimiter, set<uint16> *psetIntList);
	bool StringSplit(string data, string delimiter, map<string, string> *pmapEntries);
	void SetUpper(char *pszdata);
	void SetLower(char *pzdata);
	bool SetEucKrToUtf8(string inBuff, string& outBuff);
	void Sleep(int sec, int micro);
	bool Isdigit(string data);
	bool Isdecimal(string data);
	uint32 GetDiffTime(struct timespec stTime);
	float fswap(float fData);
};

#endif	//__UTIL_H__
