/**
 * @file MakeBinary.cpp
 * @brief Shapefile(표준노드링크) → 맵매칭 바이너리(link.psf) 생성 독립 실행 프로그램 main
 * @remark
 *   Oracle 미사용 버전. 링크/노드 Shapefile 을 읽어 바이너리를 생성한다.
 *   - 외부 GIS 라이브러리 의존성 없음 (자체 SHP/DBF 리더 사용)
 *   - 사용법 : ./MakeBinary   (config.ini 는 실행 경로에 위치)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "log4z.h"
#include "IniReader.h"
#include "ShapeLoader.h"
#include "BinaryMaker.h"
#include "TurnInfoLoader.h"

using namespace zsummer::log4z;
using namespace std;

/**
 * @struct sMakerConfig
 * @brief 바이너리 생성기 구동 환경
*/
typedef struct sMakerConfig
{
	string			strLogPath;			// 로그 경로
	int				nLogLevel;			// 로그 레벨
	string			strLinkShp;			// 링크 PolyLine shapefile
	string			strNodeShp;			// 노드 Point shapefile (선택)
	string			strTurnInfoDbf;		// 회전 제한 TURNINFO.dbf
	string			strMultiLinkDbf;	// 다중노선 MULTILINK.dbf (참고 경로)
	string			strDataDir;			// 원본 데이터 디렉토리
	int				nSourceCoord;		// 입력 좌표계 (eSourceCoord)
	string			strGeometryFile;	// 생성할 바이너리 파일명
	SHAPE_FIELD_MAP	stFieldMap;			// DBF 필드 매핑

	sMakerConfig() : nLogLevel(LOG_LEVEL_DEBUG), nSourceCoord(SRC_GRS80UTMK) {}
} MAKER_CONFIG, *PMAKER_CONFIG;

/**
 * @brief 환경 설정 파일 읽기
 * @return true(성공), false(실패)
*/
bool Initialize(const string& strConfigFile, PMAKER_CONFIG pstConfig)
{
	// config.ini 파일 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(strConfigFile.c_str(), F_OK) != 0)
	{
		perror("config.ini file not found!\n");
		return false;
	}

	CIniReader cIniReader(strConfigFile.c_str());
	// ini 파일 열기·파싱 (2026-07-08 최정우 주석 추가)
	if (!cIniReader.Open())
	{
		perror("config file open failed!\n");
		return false;
	}

	// 로그
	cIniReader.GetProfileStr("log", "path", "./log", pstConfig->strLogPath);
	if (pstConfig->strLogPath.empty())
	{
		perror("log path is empty!\n");
		return false;
	}

	cIniReader.GetProfileInt("log", "level", 1, pstConfig->nLogLevel);
	switch (pstConfig->nLogLevel)
	{
	case 0: pstConfig->nLogLevel = LOG_LEVEL_TRACE; break;
	case 1: pstConfig->nLogLevel = LOG_LEVEL_DEBUG; break;
	case 2: pstConfig->nLogLevel = LOG_LEVEL_INFO; break;
	case 3: pstConfig->nLogLevel = LOG_LEVEL_WARN; break;
	case 4: pstConfig->nLogLevel = LOG_LEVEL_ERROR; break;
	default: pstConfig->nLogLevel = LOG_LEVEL_INFO; break;
	}

	// shapefile 경로
	cIniReader.GetProfileStr("shapefile", "link", "", pstConfig->strLinkShp);
	if (pstConfig->strLinkShp.empty())
	{
		perror("link shapefile path is empty!\n");
		return false;
	}
	cIniReader.GetProfileStr("shapefile", "node", "", pstConfig->strNodeShp);	// 선택

	// 원본 데이터 디렉토리 및 DBF 경로
	cIniReader.GetProfileStr("data", "datadir", "../data", pstConfig->strDataDir);
	cIniReader.GetProfileStr("dbf", "turninfo", "", pstConfig->strTurnInfoDbf);
	if (pstConfig->strTurnInfoDbf.empty())
		cIniReader.GetProfileStr("data", "turninfo", "../data/TURNINFO.dbf", pstConfig->strTurnInfoDbf);
	cIniReader.GetProfileStr("dbf", "multilink", "", pstConfig->strMultiLinkDbf);
	if (pstConfig->strMultiLinkDbf.empty())
		cIniReader.GetProfileStr("data", "multilink", "../data/MULTILINK.dbf", pstConfig->strMultiLinkDbf);

	// 입력 좌표계 (1:WGS84GEO,2:GRS80GEO,3:GRS80TM,4:GRS80UTMK,5:KATECH,6:BESSELGEO,7:BESSELTM,8:EPSG3857)
	cIniReader.GetProfileInt("shapefile", "coordtype", SRC_GRS80UTMK, pstConfig->nSourceCoord);

	// 생성할 바이너리 파일명
	cIniReader.GetProfileStr("data", "file", "", pstConfig->strGeometryFile);
	if (pstConfig->strGeometryFile.empty())
	{
		perror("geometry binary file is empty!\n");
		return false;
	}

	// DBF 필드 매핑 (지정 없으면 MOCT 표준 기본값 사용)
	cIniReader.GetProfileStr("link_field", "id",       "LINK_ID",   pstConfig->stFieldMap.strLinkID);
	cIniReader.GetProfileStr("link_field", "maxspeed", "MAX_SPD",   pstConfig->stFieldMap.strMaxSpeed);
	cIniReader.GetProfileStr("link_field", "length",   "LENGTH",    pstConfig->stFieldMap.strLength);
	cIniReader.GetProfileStr("link_field", "roadrank", "ROAD_RANK", pstConfig->stFieldMap.strRoadRank);
	cIniReader.GetProfileStr("link_field", "connect",  "CONNECT",   pstConfig->stFieldMap.strConnect);
	cIniReader.GetProfileStr("link_field", "roadtype", "ROAD_TYPE", pstConfig->stFieldMap.strRoadType);
	cIniReader.GetProfileStr("link_field", "lanes",    "LANES",     pstConfig->stFieldMap.strLanes);
	cIniReader.GetProfileStr("link_field", "roadname", "ROAD_NAME", pstConfig->stFieldMap.strRoadName);
	cIniReader.GetProfileStr("link_field", "fnode",    "F_NODE",    pstConfig->stFieldMap.strFNode);
	cIniReader.GetProfileStr("link_field", "tnode",    "T_NODE",    pstConfig->stFieldMap.strTNode);
	cIniReader.GetProfileStr("node_field", "id",       "NODE_ID",   pstConfig->stFieldMap.strNodeID);
	cIniReader.GetProfileStr("node_field", "type",     "NODE_TYPE", pstConfig->stFieldMap.strNodeType);

	return true;
}

/**
 * @brief main 함수
 * @return 0 (성공), 1 (실패)
*/
int main()
{
	MAKER_CONFIG stConfig;
	string strConfigFile = "./config.ini";

	// 환경설정 파일 읽기
	if (!Initialize(strConfigFile, &stConfig))
		return 1;

	// 출력 바이너리 파일을 현재 작업 경로 기준 절대 경로로 변환
	char szPath[MAX_PATH];
	memset(szPath, 0, MAX_PATH);
	// 현재 작업 디렉터리 경로 조회 (2026-07-08 최정우 주석 추가)
	if (getcwd(szPath, MAX_PATH) == nullptr)
	{
		perror("get current working directory failed!\n");
		return 1;
	}
	string strGeometryPath = szPath;

	string strGeometryFile;
	std::size_t nPos = 0;
	if ((nPos = stConfig.strGeometryFile.find_last_of("/")) != string::npos)
		strGeometryFile = strGeometryPath + "/" + stConfig.strGeometryFile.substr(nPos + 1);
	else
		strGeometryFile = strGeometryPath + "/" + stConfig.strGeometryFile;

	// 로그 시작
	ILog4zManager::getRef().setLoggerPath(LOG4Z_MAIN_LOGGER_ID, stConfig.strLogPath.c_str());
	ILog4zManager::getRef().setLoggerLevel(LOG4Z_MAIN_LOGGER_ID, stConfig.nLogLevel);
	ILog4zManager::getRef().setLoggerDisplay(LOG4Z_MAIN_LOGGER_ID, true);
	ILog4zManager::getRef().setLoggerOutFile(LOG4Z_MAIN_LOGGER_ID, true);
	if (!ILog4zManager::getRef().start())
	{
		perror("log open fail!\n");
		return 1;
	}

	LOGFMTI("================ binary maker start ================");
	LOGFMTI("link shapefile=[%s]", stConfig.strLinkShp.c_str());
	LOGFMTI("node shapefile=[%s]", stConfig.strNodeShp.empty() ? "(none)" : stConfig.strNodeShp.c_str());
	LOGFMTI("data directory=[%s]", stConfig.strDataDir.c_str());
	LOGFMTI("turninfo dbf=[%s]", stConfig.strTurnInfoDbf.c_str());
	LOGFMTI("multilink dbf=[%s]", stConfig.strMultiLinkDbf.c_str());
	LOGFMTI("source coordtype=[%d], output binary=[%s]", stConfig.nSourceCoord, strGeometryFile.c_str());

	int nRet = 0;

	CTurnInfoLoader cTurnInfoLoader;
	if (!cTurnInfoLoader.Load(stConfig.strTurnInfoDbf))
	{
		LOGFMTE("turninfo dbf load failed!file=[%s]", stConfig.strTurnInfoDbf.c_str());
		ILog4zManager::getRef().stop();
		return 1;
	}

	// 형상 원본(shapefile) 로더
	CShapeFileLoader cShapeLoader;
	// shapefile 로더 초기화 (2026-07-08 최정우 주석 추가)
	if (!cShapeLoader.Initialize(stConfig.strLinkShp, stConfig.strNodeShp, 
			stConfig.nSourceCoord, stConfig.stFieldMap))
	{
		LOGFMTE("shape loader initialize failed!");
		ILog4zManager::getRef().stop();
		return 1;
	}

	// 바이너리 생성
	CBinaryMaker cBinaryMaker;
	// 바이너리 생성기 초기화 (2026-07-08 최정우 주석 추가)
	if (!cBinaryMaker.Initialize(&cShapeLoader, strGeometryPath, strGeometryFile, &cTurnInfoLoader))
	{
		LOGFMTE("binary maker initialize failed!");
		nRet = 1;
	}
	else
	{
		// link.psf 바이너리 파일 생성 (2026-07-08 최정우 주석 추가)
		if (cBinaryMaker.Create())
			LOGFMTI("binary file create success!");
		else
		{
			LOGFMTE("binary file create failed!");
			nRet = 1;
		}
	}

	LOGFMTI("================ binary maker end ================");
	ILog4zManager::getRef().stop();

	return nRet;
}
