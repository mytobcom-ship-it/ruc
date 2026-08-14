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
	double		dfTickSec;			// GPS 표본 생성 주기 (초) — CVehicle 이 실제 사용하는 값.
	//   SimServer::Initialize 가 차량마다 [tick_sec_min,tick_sec_max] 범위에서 랜덤으로 뽑아
	//   이 필드에 채운 뒤 그 차량 전용 config 사본을 넘김 — 실제 단말마다 보고 간격이 다른 것을
	//   흉내낸다 (2026-07-22 최정우 추가, 2026-07-22 최정우 수정 — 차량별 랜덤화로 확장)
	double		dfTickSecMin;		// tick_sec 랜덤 범위 하한 (초)
	double		dfTickSecMax;		// tick_sec 랜덤 범위 상한 (초, min 과 같으면 전 차량 고정값)
	int			nFlushSec;			// DB INSERT 주기 (초) — 차량들과 무관한 서버 공용 배치, tick_sec 과 독립
	int			nReportSec;			// 통계 로그 출력 주기 (초)
	int			nMaxSamples;		// 총 GPS 생성 상한 (0=무제한). 도달 시 flush 후 자동 종료
	double		dfIdleProb;			// 매 tick 정차(IDLE) 확률 (0~1)
	double		dfOmitAllProb;		// 보조 GPS 필드 전부 누락 확률 (0~1, 드물게)
	double		dfOmitPartialProb;	// 보조 GPS 필드 1~2개 누락 확률 (0~1, 가끔)

	// [area] 주행 영역 (WGS-84 경위도 bounding box)
	double		dfMinLon;
	double		dfMinLat;
	double		dfMaxLon;
	double		dfMaxLat;

	// [경로] 경로 생성
	int			nRouteMinM;			// 최소 경로 길이 (m)
	int			nRouteMaxLinks;		// 최대 연결 링크 수
	int			nSeedCandidates;	// 시작 링크 후보 수

	// [noise] 도로 이탈 노이즈
	double		dfNoiseSigmaM;		// GPS 오차 표준편차 (m) — 현실적 스마트폰 GPS 수준
	double		dfNoiseMaxM;		// 일반 오차 최대값 (m, 정규분포 cap)
	// 예외: 큰 튀는 좌표(멀티패스·도심협곡) 주입 — 예외처리 검증용 (2026-07-16 최정우 추가)
	double		dfOutlierProb;		// 튀는 좌표 발생 확률 (0~1)
	double		dfOutlierMinM;		// 튀는 좌표 최소 오프셋 (m)
	double		dfOutlierMaxM;		// 튀는 좌표 최대 오프셋 (m)
	// 서행/정차 시 GPS 노이즈 확대 — 실제로 저속·정지 상태에서 스마트폰 GPS 멀티패스 영향이
	//   커지는 현상 반영(2026-08-13 최정우 추가, 사용자 지시)
	double		dfNoiseSlowKmh;		// 이 속도(km/h) 미만일 때 노이즈 확대 적용
	double		dfNoiseSlowMult;	// sigma_m·max_m 에 곱할 배율

	// [speed] 속도 모델
	double		dfSpeedFactorMin;	// 제한속도 대비 최소 비율
	double		dfSpeedFactorMax;	// 제한속도 대비 최대 비율
	double		dfDefaultMaxSpd;	// 제한속도 없을 때 기본값 (km/h)

	// [status] DRIVE_STATUS 속도 구간 — 0(ON_ROAD)/1(IDLE=서행)/2(PARKED=주차) 3단계
	//   (2026-08-13 최정우 추가, 사용자 지시 — 기존엔 0.5m/s 미만만 IDLE, 나머지 전부 ON_ROAD인
	//   2단계였음). PARKED(정지 근접)가 IDLE(서행)보다 낮은 속도 구간
	double		dfOnRoadKmh;		// 이 속도(km/h) 이상이면 DRIVE_STATUS=0(ON_ROAD)
	double		dfParkKmh;			// 이 속도(km/h) 미만이면 DRIVE_STATUS=2(PARKED), 그 사이는 1(IDLE)

	// [deadzone] GPS 음영구간(터널·지하차도) — 신호 두절 시뮬레이션 (2026-07-21 최정우 추가)
	double		dfDeadZoneProb;		// 터널(002)·지하(004) 구간 tick 당 표본 미생성(신호 두절) 확률 (0~1)

	sConfig() :
		nLogLevel(2), nDBPort(5432), nVehicles(10), dfTickSec(1.0),
		dfTickSecMin(3.0), dfTickSecMax(8.0), nFlushSec(3),
		nReportSec(30), nMaxSamples(0), dfIdleProb(0.05),
		dfOmitAllProb(0.005), dfOmitPartialProb(0.08),
		dfMinLon(126.90), dfMinLat(37.48), dfMaxLon(127.10), dfMaxLat(37.62),
		nRouteMinM(2000), nRouteMaxLinks(20), nSeedCandidates(20),
		dfNoiseSigmaM(4.0), dfNoiseMaxM(20.0),
		dfOutlierProb(0.03), dfOutlierMinM(25.0), dfOutlierMaxM(80.0),
		dfNoiseSlowKmh(20.0), dfNoiseSlowMult(2.5),
		dfSpeedFactorMin(0.5), dfSpeedFactorMax(1.0), dfDefaultMaxSpd(50.0),
		dfOnRoadKmh(30.0), dfParkKmh(10.0),
		dfDeadZoneProb(0.9)
	{}
} SIM_CONFIG, *PSIM_CONFIG;

#endif // __SIM_CONFIG_H__
