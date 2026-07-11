/**
 * @file Util.cpp
 * @brief 유틸리티 클래스 소스
*/
#include "Util.h"

/**
 * @brief 구분자을 이용하여 문자열 파싱
 * @param[in] data 데이터
 * @param[in] delimiter 구분자
 * @param[out] pvtStringList 문자열 결과 값
 * @return true(성공), false(실패)
*/
bool CUtil::StringSplit(string data, string delimiter, vector<string> *pvtStringList)
{
	std::size_t pos = 0;

	if (data.empty() || delimiter.empty()) return false;

	while ((pos = data.find_first_of(delimiter)) != string::npos)
	{
		pvtStringList->push_back(data.substr(0, pos));
		data = data.substr(pos + 1, data.length() - pos + 1);
	}

	pvtStringList->push_back(data.substr(0, pos));

	return (pvtStringList->size() > 0) ? true : false;
}

/**
 * @brief 구분자을 이용하여 문자열 파싱
 * @param[in] data 데이터
 * @param[in] delimiter 구분자
 * @param[out] pvtIntList 정수형 결과 값
 * @return true(성공), false(실패)
*/
bool CUtil::StringSplit(string data, string delimiter, vector<int> *pvtIntList)
{
	std::size_t pos = 0;

	if (data.empty() || delimiter.empty()) return false;

	while ((pos = data.find_first_of(delimiter)) != string::npos)
	{
		pvtIntList->push_back(atoi(data.substr(0, pos).c_str()));
		data = data.substr(pos + 1, data.length() - pos + 1);
	}

	pvtIntList->push_back(atoi(data.substr(0, pos).c_str()));

	return (pvtIntList->size() > 0) ? true : false;
}

/**
 * @brief 구분자을 이용하여 문자열 파싱
 * @param[in] data 데이터
 * @param[in] delimiter 구분자
 * @param[out] psetIntList 정수형 결과 값
 * @return true(성공), false(실패)
*/
bool CUtil::StringSplit(string data, string delimiter, set<uint16> *psetIntList)
{
	std::size_t pos = 0;

	if (data.empty() || delimiter.empty()) return false;

	while ((pos = data.find_first_of(delimiter)) != string::npos)
	{
		string number = data.substr(0, pos);

		number.erase(remove(number.begin(), number.end(), ' '), number.end());
		if (!Isdigit(number))
		{
			data = data.substr(pos + 1, data.length() - pos + 1);
			continue;
		}

		if (atoi(number.c_str()) > 250)
		{
			data = data.substr(pos + 1, data.length() - pos + 1);
			continue;
		}

		psetIntList->insert(static_cast<uint16>(atoi(number.c_str())));
		data = data.substr(pos + 1, data.length() - pos + 1);
	}

	psetIntList->insert(atoi(data.substr(0, pos).c_str()));

	return (psetIntList->size() > 0) ? true : false;
}

/**
 * @brief 구분자을 이용하여 문자열 파싱
 * @param[in] data 데이터
 * @param[in] delimiter 구분자
 * @param[out] pmapEntries 결과 값
 * @return true(성공), false(실패)
*/
bool CUtil::StringSplit(string data, string delimiter, map<string, string> *pmapEntries)
{
	std::size_t pos = 0;

	if (data.empty() || delimiter.empty()) return false;

	if ((pos = data.find_first_of(delimiter)) == string::npos)
		return false;

	string key = data.substr(0, pos);
	string value = data.substr(pos + 1, data.length() - pos + 1);

	pmapEntries->insert(pair<string, string>(key, value));

	return true;
}

/**
 * @brief 대문자로 변환
 * @param[in,out] pszData 변환 문자열
 * @return void
*/
void CUtil::SetUpper(char *pszData)
{
	char *pszBuff = nullptr;

	for (pszBuff=pszData; *pszBuff; ++pszBuff)
		*pszBuff = toupper(*pszBuff);
}

/**
 * @brief 소문자로 변환
 * @param[in,out] data 변환 문자열
 * @return void
*/
void CUtil::SetLower(char *pszData)
{
	char *pszBuff = nullptr;

	for (pszBuff=pszData; *pszBuff; ++pszBuff)
		*pszBuff = tolower(*pszBuff);
}

/**
 * @brief 문자열 characterset 변환
 * @param[in] inBuff 변환전 문자열
 * @param[out] outBuff 변환된 문자열
 * @return true(성공), false(실패)
*/
bool CUtil::SetEucKrToUtf8(string inBuff, string& outBuff)
{
	string inBuf, outBuf;
	char *in; 
	char *out;
	iconv_t charset;
	size_t inByte;
	size_t AllocatedByte;
	size_t outByte;

	inBuf.clear();
	outBuf.clear();

	inBuf = inBuff;
	inByte = inBuff.length();
	AllocatedByte = inByte * 2;
	outByte = AllocatedByte;
	outBuf.resize(outByte);

	charset = iconv_open("UTF-8", "EUC-KR");
	if (charset == (iconv_t)-1)
		return false;

	in = (char *)inBuf.c_str();
	out = (char *)outBuf.c_str();
	if (iconv(charset, &in, &inByte, &out, &outByte) == (size_t)-1)
	{    
		iconv_close(charset);
		LOGFMTE("conversion failed!error=[%d : %s]", errno, strerror(errno));
		return false;
	}

	iconv_close(charset);

	outBuf.resize(AllocatedByte - outByte);
	outBuff = outBuf;

	return true;
}

/**
 * @brief sleep 함수
 * @param[in] sec 초
 * @param[in] micro 마이크로 초
 * @return void
*/
void CUtil::Sleep(int sec, int micro)
{
	struct timeval stVal;

	stVal.tv_sec = sec;
	stVal.tv_usec = micro;
	select(1, reinterpret_cast<fd_set *>(0), reinterpret_cast<fd_set *>(0), reinterpret_cast<fd_set *>(0), &stVal);
}

/**
 * @brief 문자열이 숫자인지 검사
 * @param[in] data 검사 문자열
 * @return true(성공), false(실패)
*/
bool CUtil::Isdigit(string data)
{
	return (data.find_first_not_of("0123456789") == string::npos) ? true : false;
}

/**
 * @brief 문자열이 정수,실수 숫자인지 검사
 * @param[in] data 검사 문자열
 * @return true(성공), false(실패)
*/
bool CUtil::Isdecimal(string data)
{
	if (data.size() == 0) return false;
	if ((data.front() == '.') || (data.back() == '.')) return false;
	return (data.find_first_not_of("0123456789.") == string::npos) ? true : false;
}

/**
 * @brief 현재 시간과의 시간 차이
 * @param[in] stTime 처리 시간
 * @return 시간 차이 (초)
*/
uint32 CUtil::GetDiffTime(struct timespec stTime)
{
	time_t dtNow;

	time(&dtNow);
	return static_cast<uint32>(difftime(dtNow, stTime.tv_sec));
}

/**
 * @brief endian 변환
 * @param[in] fData byte endian 변환 전 값
 * @return byte endian 변환 후 값
*/
float CUtil::fswap(float fData)
{
	union swap
	{
		float		fValue;
		uint32		dwValue;
	};

	swap uswap;
	uswap.fValue = fData;
	uswap. dwValue = htonl(uswap.dwValue);
	return uswap.fValue;
}