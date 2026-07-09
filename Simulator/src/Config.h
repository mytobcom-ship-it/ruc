/**
 * @file Config.h
 * @brief 시뮬레이터 환경설정 구조체
*/
#ifndef __SIM_CONFIG_H__
#define __SIM_CONFIG_H__

#include <string>
#include "TypeDefine.h"

using namespace std;

/**
 * @struct sConfig
 * @brief 시뮬레이터 구동 환경 (config.ini)
*/
typedef struct sConfig
{
	// [log]
	string		strLogPath;			// 로그 경로
	int			nLogLevel;			// 로그 레벨 (0~4)

	// [database]
	string		strDBHost;			// DB host
	int			nDBPort;			// DB port
	string		strDBName;			// DB 이름 (roadnet)
	string		strDBUserID;		// DB 접속 ID (mytobcom)
	string		strDBPasswd;		// DB 접속 비밀번호

	// [query]
	string		strSQLFile;			// SQL 파일 경로

	// [sim] 시뮬레이션 파라미터
	int			nVehicles;			// 동시 운행 차량 수
	int			nFlushSec;			// DB INSERT 주기 (초)
	int			nReportSec;			// 통계 로그 출력 주기 (초)
	double		dfIdleProb;			// 매 tick 정차(IDLE) 확률 (0~1)

	// [area] 주행 영역 (WGS-84 경위도 bounding box)
	double		dfMinLon;
	double		dfMinLat;
	double		dfMaxLon;
	double		dfMaxLat;

	// [route] 경로 생성
	int			nRouteMinM;			// 최소 경로 길이 (m)
	int			nRouteMaxLinks;		// 최대 연결 링크 수
	int			nSeedCandidates;	// 시작 링크 후보 수

	// [noise] 도로 이탈 노이즈
	double		dfNoiseSigmaM;		// GPS 오차 표준편차 (m)
	double		dfNoiseMaxM;		// GPS 오차 최대값 (m)

	// [speed] 속도 모델
	double		dfSpeedFactorMin;	// 제한속도 대비 최소 비율
	double		dfSpeedFactorMax;	// 제한속도 대비 최대 비율
	double		dfDefaultMaxSpd;	// 제한속도 없을 때 기본값 (km/h)

	sConfig() :
		nLogLevel(2), nDBPort(5432), nVehicles(10), nFlushSec(3),
		nReportSec(30), dfIdleProb(0.05),
		dfMinLon(126.90), dfMinLat(37.48), dfMaxLon(127.10), dfMaxLat(37.62),
		nRouteMinM(2000), nRouteMaxLinks(20), nSeedCandidates(20),
		dfNoiseSigmaM(10.0), dfNoiseMaxM(30.0),
		dfSpeedFactorMin(0.5), dfSpeedFactorMax(1.0), dfDefaultMaxSpd(50.0)
	{}
} SIM_CONFIG, *PSIM_CONFIG;

#endif // __SIM_CONFIG_H__
