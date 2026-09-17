/**
 * @file Util.cpp
 * @brief 유틸리티 클래스 소스
 * @remark [사용 현황, 2026-09-17 최정우 조사] 이 클래스에서 실제로 호출되는 멤버는
 *   **Isdigit()(IniReader), Isdecimal()(IniReader), Sleep()(Server)** 셋뿐이다.
 *   StringSplit() 4종 · SetUpper() · SetLower() · SetEucKrToUtf8() · GetDiffTime() · fswap() 은
 *   전 소스에서 호출처가 하나도 없다(지우지 않고 남겨두되, 되살려 쓸 때는 아래 주의사항을 볼 것).
 *   - StringSplit() 은 find_first_of() 를 쓰므로 delimiter 의 **각 문자**가 개별 구분자다
 *     (여러 글자를 넘기면 "그 문자열"이 아니라 "그 문자들 중 아무거나"로 잘린다).
 *   - SetUpper()/SetLower() 는 toupper()/tolower() 에 unsigned char 캐스트가 없다 — UTF-8
 *     멀티바이트(한글 등) 0x80 이상 바이트가 signed char 에서 음수가 되어 정의되지 않은 동작이다
 *     (SQLAccessor.cpp·IniReader.cpp 는 같은 패턴을 이미 캐스트로 고쳤다). 되살릴 때 함께 고칠 것.
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

	// [버그 수정, 2026-09-17 최정우] 마지막 토큰에도 루프 안과 **동일한 검증**을 적용한다.
	//   종전에는 루프에서만 공백 제거·Isdigit()·250 초과를 걸렀고, 루프를 빠져나온 마지막 토큰은
	//   무검증으로 insert 해서 비숫자면 atoi() 가 돌려주는 0 이, 250 초과면 그 값이 그대로 들어갔다
	//   (같은 입력이 위치에 따라 다르게 처리되는 비일관 동작).
	//   ※ 이 함수는 현재 호출처가 없다(파일 헤더 주석 참고) — 되살려 쓸 때를 위한 정합성 수정이다.
	{
		string strLast = data.substr(0, pos);
		strLast.erase(remove(strLast.begin(), strLast.end(), ' '), strLast.end());
		if (Isdigit(strLast) && (atoi(strLast.c_str()) <= 250))
			psetIntList->insert(static_cast<uint16>(atoi(strLast.c_str())));
	}

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
 * @param[in,out] pszData 변환 문자열
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