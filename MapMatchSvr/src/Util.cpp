/**
 * @file Util.cpp
 * @brief 유틸리티 클래스 소스
 * @remark [사용 현황, 2026-09-17 최정우 조사] 이 클래스에서 실제로 호출되는 멤버는
 *   **Isdigit()(IniReader), Isdecimal()(IniReader), Sleep()(Server)** 셋뿐이다.
 *   StringSplit() 4종 · SetUpper() · SetLower() · SetEucKrToUtf8() · GetDiffTime() · fswap() 은
 *   전 소스에서 호출처가 하나도 없다(지우지 않고 남겨두되, 되살려 쓸 때는 아래 주의사항을 볼 것).
 *   - StringSplit() 은 find_first_of() 를 쓰므로 delimiter 의 **각 문자**가 개별 구분자다
 *     (여러 글자를 넘기면 "그 문자열"이 아니라 "그 문자들 중 아무거나"로 잘린다).
 *   - SetUpper()/SetLower() 의 unsigned char 캐스트 누락(UTF-8 UB)과 SetEucKrToUtf8() 의
 *     c_str() 버퍼 직접 쓰기는 **2026-09-21 에 수정 완료**했다 — 호출부가 없더라도 되살리는
 *     순간 발현하는 결함이라 미리 고쳐뒀다(사용자 지시). 각 함수 본문 주석 참고.
 *   - Sleep() 은 위 "미사용" 목록에 해당하지 않는다 — CServer 타이머 스레드가 상시 호출한다.
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
	// [버그 수정, 2026-09-21 최정우] toupper() 는 인자가 unsigned char 범위(또는 EOF)여야 한다.
	//   char 가 signed 인 플랫폼에서 UTF-8 멀티바이트(한글 등)의 0x80 이상 바이트를 그대로
	//   넘기면 음수가 되어 정의되지 않은 동작이다 — SQLAccessor.cpp·IniReader.cpp 는 같은
	//   패턴을 이미 캐스트로 고쳤는데 여기만 남아 있었다. 호출부는 현재 0 건이지만,
	//   되살려 쓰는 순간 한글이 섞인 문자열에서 바로 발현한다.
	//   결과를 다시 char 에 넣을 때도 unsigned char 를 거쳐 구현 정의 변환을 피한다.
	char *pszBuff = nullptr;

	if (pszData == nullptr) return;
	for (pszBuff=pszData; *pszBuff; ++pszBuff)
		*pszBuff = static_cast<char>(static_cast<unsigned char>(
			toupper(static_cast<unsigned char>(*pszBuff))));
}

/**
 * @brief 소문자로 변환
 * @param[in,out] pszData 변환 문자열
 * @return void
*/
void CUtil::SetLower(char *pszData)
{
	// SetUpper() 와 동일 근거 (2026-09-21 최정우 버그 수정)
	char *pszBuff = nullptr;

	if (pszData == nullptr) return;
	for (pszBuff=pszData; *pszBuff; ++pszBuff)
		*pszBuff = static_cast<char>(static_cast<unsigned char>(
			tolower(static_cast<unsigned char>(*pszBuff))));
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

	// [버그 수정, 2026-09-21 최정우] 종전에는 out 버퍼를 c_str() 로 받아 const 를 떼고 iconv 가
	//   **그 안에 직접 쓰게** 했다 — c_str() 이 돌려준 영역에 쓰는 것은 정의되지 않은 동작이다
	//   (읽기 전용 접근만 보장된다). C++11 부터 std::string 은 연속 저장이 보장되므로 &s[0] 으로
	//   쓰기 가능한 버퍼를 얻는 것이 올바른 방법이다. in 쪽은 iconv 가 포인터만 전진시키고
	//   내용을 바꾸지 않지만, 같은 이유로 함께 맞춰둔다. 호출부는 현재 0 건이다.
	in = (inByte > 0) ? &inBuf[0] : nullptr;
	out = (outByte > 0) ? &outBuf[0] : nullptr;
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

	// [버그 수정, 2026-09-21 최정우] **이 함수는 죽은 코드가 아니다** — CServer 의 타이머 스레드가
	//   매 10ms 호출한다(Server.cpp TimerThread). 그런데 인자를 그대로 timeval 에 넣고 있어
	//   결함이 둘 있었다.
	//   ① select() 는 tv_usec 가 0~999,999 범위를 벗어나면 **EINVAL 로 즉시 반환**한다. 호출측이
	//      "1초"를 Sleep(0, 1000000) 으로 표현하면 자는 대신 곧바로 돌아오고, 호출부가 루프
	//      안이면 그대로 **CPU 를 태우는 busy loop** 가 된다(지금 호출부는 10000 이라 안전하지만,
	//      단위를 오해하기 쉬운 인터페이스다 — 실제로 Thread::sleep() 의 단위 주석이 틀려 있던
	//      전례가 있다). 초과분을 tv_sec 으로 올려 정규화한다.
	//   ② 음수가 들어오면 그대로 넘겨 역시 EINVAL 이 된다. 0 으로 잘라낸다.
	//   ③ nfds 를 1 로 넘기고 있었다 — 감시할 디스크립터가 없으므로(세 집합 모두 NULL) 0 이 맞다.
	//      1 은 "fd 0(stdin)까지 검사하라"는 뜻이라, 집합이 NULL 인 현재는 무해하지만 의미가 틀렸다.
	//   ※ EINTR(시그널로 조기 기상) 재시도는 **일부러 넣지 않았다** — 남은 시간을 다시 자면
	//      종료 시그널에 둔감해진다. 호출부(타이머 루프)는 조기 반환돼도 다음 회차에 다시 잔다.
	long lSec = (sec > 0) ? static_cast<long>(sec) : 0;
	long lUsec = (micro > 0) ? static_cast<long>(micro) : 0;
	if (lUsec >= 1000000L)
	{
		lSec += lUsec / 1000000L;
		lUsec = lUsec % 1000000L;
	}
	stVal.tv_sec = lSec;
	stVal.tv_usec = lUsec;
	select(0, reinterpret_cast<fd_set *>(0), reinterpret_cast<fd_set *>(0), reinterpret_cast<fd_set *>(0), &stVal);
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