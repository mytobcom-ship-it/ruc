/**
 * @file BinaryMaker.cpp
 * @brief 형상 정보 → 맵매칭 바이너리(link.psf) 생성 클래스 소스 파일
 * @remark CDataManager 의 생성(CreateData) 로직을 독립 실행용으로 분리
*/
#include "BinaryMaker.h"

/**
 * @brief 생성자
*/
CBinaryMaker::CBinaryMaker() : 
	m_pcShapeLoader(nullptr), 
	m_pcTurnInfoLoader(nullptr),
	m_mapShapeLinkInfoList(nullptr), 
	m_mapShapeNodeInfoList(nullptr), 
	m_mapGridSgmtInfoList(nullptr), 
	m_vtGridInfoList(nullptr), 
	m_vtGridSgmtInfoList(nullptr), 
	m_vtLinkSgmtInfoList(nullptr), 
	m_vtLinkInfoDataList(nullptr), 
	m_vtTurnInfoList(nullptr), 
	m_dwTurnOffset(0), 
	m_dwLinkSgmtOffset(0), 
	m_dwGridSgmtOffset(0), 
	m_dwGridOffset(0),
	m_dwTurnRestrictedSkipCount(0)
{
}

/**
 * @brief 소멸자
*/
CBinaryMaker::~CBinaryMaker()
{
	// 생성 작업 메모리 해제 (2026-07-08 최정우 주석 추가)
	Uninitialize();
}

/**
 * @brief 초기화
 * @param[in] pcShapeLoader 형상 정보(shapefile) 로딩 클래스 포인터
 * @param[in] strGeometryPath 형상 정보 바이너리 파일 경로
 * @param[in] strGeometryFile 형상 정보 바이너리 파일명 및 경로 (절대 경로)
 * @return true, false
*/
bool CBinaryMaker::Initialize(CShapeFileLoader *pcShapeLoader, 
		const string& strGeometryPath, const string& strGeometryFile,
		CTurnInfoLoader *pcTurnInfoLoader)
{
	m_pcShapeLoader = pcShapeLoader;
	m_pcTurnInfoLoader = pcTurnInfoLoader;
	m_strGeometryPath = strGeometryPath;
	m_strGeometryFile = strGeometryFile;
	m_dwTurnRestrictedSkipCount = 0;

	if (m_pcShapeLoader == nullptr)
	{
		LOGFMTE("shape loader is null!");
		return false;
	}

	if (m_pcTurnInfoLoader == nullptr)
	{
		LOGFMTE("turn info loader is null!");
		return false;
	}

	if (m_strGeometryFile.empty())
	{
		LOGFMTE("geometry binary file is empty!");
		return false;
	}

	return true;
}

/**
 * @brief 메모리 반환
 * @return void
*/
void CBinaryMaker::Uninitialize()
{
	// 메모리 반환
	// 생성 작업 중 할당 메모리 전량 해제 (2026-07-08 최정우 주석 추가)
	SetCreateUninitial();
}

/**
 * @brief 데이터 생성 (구 CDataManager::CreateData)
 * @return true, false
*/
bool CBinaryMaker::Create()
{
	CClock cCreateClock;
	// 바이너리 생성 소요시간 측정 시작 (2026-07-08 최정우 주석 추가)
	cCreateClock.Start();

	// 데이터 생성 초기화
	// 생성용 맵·벡터 메모리 초기화 (2026-07-08 최정우 주석 추가)
	if (!SetCreateInitial())
	{
	// 메모리 반환
		// 생성 작업 중 할당 메모리 전량 해제 (2026-07-08 최정우 주석 추가)
		SetCreateUninitial();
		// 바이너리 생성 소요시간 측정 종료 (2026-07-08 최정우 주석 추가)
		cCreateClock.Stop();
		return false;
	}

	// shapefile 형상 정보 로딩 (2026-07-08 최정우 주석 추가)
	if (!m_pcShapeLoader->Load(m_mapShapeLinkInfoList, m_mapShapeNodeInfoList))
	{
	// 메모리 반환
		SetCreateUninitial();
		cCreateClock.Stop();
		return false;
	}

	if (m_mapShapeLinkInfoList->empty())
	{
		LOGFMTE("link data is empty!");
	// 메모리 반환
		SetCreateUninitial();
		cCreateClock.Stop();
		return false;
	}

	if (m_mapShapeNodeInfoList->empty())
	{
		LOGFMTE("node data is empty!");
	// 메모리 반환
		SetCreateUninitial();
		cCreateClock.Stop();
		return false;
	}

	LOGFMTI("map match data create start!");
	// GRID·링크·회전 정보 생성 (2026-07-08 최정우 주석 추가)
	if (!SetGridMapData())
	{
		LOGFMTE("map match data create failed!");
	// 메모리 반환
		SetCreateUninitial();
		cCreateClock.Stop();
		return false;
	}
	LOGFMTI("map match data create end! turn_info=[%u], turn_restricted_skip=[%u]",
		static_cast<uint32>(m_vtTurnInfoList->size()), m_dwTurnRestrictedSkipCount);

	// 임시 바이너리 파일 생성
	char szTempFile[MAX_PATH];
	time_t dtNow = time(nullptr);
	// 오늘 날자
	struct tm ltm;

	// init
	memset(szTempFile, 0, MAX_PATH);
	memset(&ltm, 0, sizeof(struct tm));

	// 현재 시각을 로컬 시간으로 변환 (2026-07-08 최정우 주석 추가)
	localtime_r(&dtNow, &ltm);
	sprintf(szTempFile, "%04d%02d%02d%02d%02d.psf", 
		ltm.tm_year + 1900, ltm.tm_mon + 1, ltm.tm_mday, 
		ltm.tm_hour, ltm.tm_min);
	string strGeometryTempFile = m_strGeometryPath + "/";
	strGeometryTempFile.append(szTempFile);

	LOGFMTI("file exist check start!file=[%s]", strGeometryTempFile.c_str());
	// 새로 생성된 파일 이름 변경
	// 임시 바이너리 파일 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(strGeometryTempFile.c_str(), F_OK) == 0)
	{
		// 기존 임시 바이너리 파일 삭제 (2026-07-08 최정우 주석 추가)
		if (remove(strGeometryTempFile.c_str()) != 0)
		{
			LOGFMTE("temp binary file delete failed!file=[%s]", strGeometryTempFile.c_str());
	// 메모리 반환
			SetCreateUninitial();
			cCreateClock.Stop();
			return false;
		}
	}
	LOGFMTI("file exist check end!file=[%s]", strGeometryTempFile.c_str());

	LOGFMTI("temp binary file open!file=[%s]", strGeometryTempFile.c_str());
	// 임시 바이너리 파일 쓰기 모드로 열기 (2026-07-08 최정우 주석 추가)
	FILE *fp = fopen(strGeometryTempFile.c_str(), "wb");
	if (fp == nullptr)
	{
		LOGFMTE("geometry binary file open failed!file=[%s]", strGeometryTempFile.c_str());
	// 메모리 반환
		SetCreateUninitial();
		cCreateClock.Stop();
		return false;
	}

	LOGFMTI("temp binary file write start!file=[%s]", strGeometryTempFile.c_str());
	// 바이너리 데이터를 임시 파일에 기록 (2026-07-08 최정우 주석 추가)
	if (!SetCreateBinary(fp))
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
	// 메모리 반환
		SetCreateUninitial();
		cCreateClock.Stop();
		return false;
	}

	if (fp != nullptr) fclose(fp);
	fp = nullptr;
	LOGFMTI("temp binary file write close!file=[%s]", strGeometryTempFile.c_str());

	LOGFMTI("rename file [%s]->[%s] start!", strGeometryTempFile.c_str(), m_strGeometryFile.c_str());

	// 기존 파일 삭제
	// 최종 바이너리 파일 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(m_strGeometryFile.c_str(), F_OK) == 0)
	{
		// 기존 최종 바이너리 파일 삭제 (2026-07-08 최정우 주석 추가)
		if (remove(m_strGeometryFile.c_str()) != 0)
		{
			LOGFMTE("binary file delete failed!file=[%s]", m_strGeometryFile.c_str());
	// 메모리 반환
			SetCreateUninitial();
			cCreateClock.Stop();
			return false;
		}
	}

	// 새로 생성된 파일 이름 변경
	// 임시 바이너리 파일 존재 여부 확인 (2026-07-08 최정우 주석 추가)
	if (access(strGeometryTempFile.c_str(), F_OK) == 0)
	{
		// 임시 파일을 최종 바이너리 파일명으로 변경 (2026-07-08 최정우 주석 추가)
		if (rename(strGeometryTempFile.c_str(), m_strGeometryFile.c_str()) != 0)
		{
			LOGFMTE("file rename [%s]->[%s] failed!errno=[%d : %s]", 
				strGeometryTempFile.c_str(), m_strGeometryFile.c_str(), 
				errno, strerror(errno));
			// 메모리 반환
			SetCreateUninitial();
			cCreateClock.Stop();
			return false;
		}
	}

	LOGFMTI("rename file [%s]->[%s] end!", strGeometryTempFile.c_str(), m_strGeometryFile.c_str());

	// 메모리 반환
	SetCreateUninitial();

	cCreateClock.Stop();
	LOGFMTI("binary data file create complete!elapsed time : %.06lf sec", 
		cCreateClock.GetElapsedTime());

	return true;
}

/**
 * @brief 데이터 생성 메모리 초기화
 * @return true, false
*/
bool CBinaryMaker::SetCreateInitial()
{
	m_mapShapeLinkInfoList = new (std::nothrow)mapShapeLinkInfo;
	if (m_mapShapeLinkInfoList == nullptr)
	{
		LOGFMTE("link shap info memory allocate failed!");
		return false;
	}

	m_mapShapeNodeInfoList = new (std::nothrow)mapShapeNodeInfo;
	if (m_mapShapeNodeInfoList == nullptr)
	{
		LOGFMTE("node shape info memory allocate failed!");
		return false;
	}

	m_mapGridSgmtInfoList = new (std::nothrow)mapGridSgmtInfo;
	if (m_mapGridSgmtInfoList == nullptr)
	{
		LOGFMTE("grid segment map memory allocate failed!");
		return false;
	}

	m_vtGridInfoList = new (std::nothrow)vector<GRID_INFO>;
	if (m_vtGridInfoList == nullptr)
	{
		LOGFMTE("grid info mamory allocate failed!");
		return false;
	}

	m_vtGridSgmtInfoList = new (std::nothrow)vector<GRID_SGMT_INFO>;
	if (m_vtGridSgmtInfoList == nullptr)
	{
		LOGFMTE("grid segment memory allocate failed!");
		return false;
	}

	m_vtLinkSgmtInfoList = new (std::nothrow)vector<LINK_SGMT_INFO>;
	if (m_vtLinkSgmtInfoList == nullptr)
	{
		LOGFMTE("link segment list memory allocate failed!");
		return false;
	}

	m_vtLinkInfoDataList = new (std::nothrow)vector<LINK_INFO_DATA>;
	if (m_vtLinkInfoDataList == nullptr)
	{
		LOGFMTE("link info memory allocate failed!");
		return false;
	}

	m_vtTurnInfoList = new (std::nothrow)vector<TURN_INFO>;
	if (m_vtTurnInfoList == nullptr)
	{
		LOGFMTE("turn info memory allocate failed!");
		return false;
	}

	m_mapShapeLinkInfoList->clear();
	m_mapShapeNodeInfoList->clear();
	m_mapGridSgmtInfoList->clear();
	m_vtGridInfoList->clear();
	m_vtGridSgmtInfoList->clear();
	m_vtLinkSgmtInfoList->clear();
	m_vtLinkInfoDataList->clear();
	m_vtTurnInfoList->clear();
	m_dwTurnOffset = 0;
	m_dwLinkSgmtOffset = 0;
	m_dwGridSgmtOffset = 0;
	m_dwGridOffset = 0;
	m_dwTurnRestrictedSkipCount = 0;

	return true;
}

/**
 * @brief 데이터 생성 메모리 반환
 * @return void
*/
void CBinaryMaker::SetCreateUninitial()
{
	if (m_vtTurnInfoList != nullptr)
		delete m_vtTurnInfoList;
	m_vtTurnInfoList = nullptr;

	if (m_vtLinkSgmtInfoList != nullptr)
		delete m_vtLinkSgmtInfoList;
	m_vtLinkSgmtInfoList = nullptr;

	if (m_vtLinkInfoDataList != nullptr)
		delete m_vtLinkInfoDataList;
	m_vtLinkInfoDataList = nullptr;

	if (m_vtGridSgmtInfoList != nullptr)
		delete m_vtGridSgmtInfoList;
	m_vtGridSgmtInfoList = nullptr;

	if (m_mapGridSgmtInfoList != nullptr)
	{
		mapGridSgmtInfo::iterator it = m_mapGridSgmtInfoList->begin();
		for (; it!=m_mapGridSgmtInfoList->end(); ++it)
			it->second.clear();
		delete m_mapGridSgmtInfoList;
	}
	m_mapGridSgmtInfoList = nullptr;

	if (m_vtGridInfoList != nullptr)
		delete m_vtGridInfoList;
	m_vtGridInfoList = nullptr;

	if (m_mapShapeLinkInfoList != nullptr)
	{
		mapShapeLinkInfo::iterator it = m_mapShapeLinkInfoList->begin();
		for (; it!=m_mapShapeLinkInfoList->end(); ++it)
			delete it->second;
		delete m_mapShapeLinkInfoList;
	}
	m_mapShapeLinkInfoList = nullptr;

	if (m_mapShapeNodeInfoList != nullptr)
	{
		// Node shape 의 Vertex 포인터는 link Vertex 에서 이미 삭제 됨
		mapShapeNodeInfo::iterator it = m_mapShapeNodeInfoList->begin();
		for (; it!=m_mapShapeNodeInfoList->end(); ++it)
			delete it->second;
		delete m_mapShapeNodeInfoList;
	}
	m_mapShapeNodeInfoList = nullptr;

	m_dwGridOffset = 0;
	m_dwGridSgmtOffset = 0;
	m_dwLinkSgmtOffset = 0;
	m_dwTurnOffset = 0;
}

/**
 * @brief 바이너리 데이터 생성
 * @param[in] fp 바이너리 파일 포인터
 * @return true, false
*/
bool CBinaryMaker::SetCreateBinary(FILE *fp)
{
	if (fp == nullptr)
	{
		LOGFMTE("geometry binary file handle is null!");
		return false;
	}

	DATA_FILE_HEAD stDataFileHead;

	// init
	memset(&stDataFileHead, 0, DATA_FILE_HEAD_SIZE);

	stDataFileHead.dwGridInfoCount = static_cast<uint32>(m_vtGridInfoList->size());
	stDataFileHead.dwGridSgmtInfoCount = static_cast<uint32>(m_vtGridSgmtInfoList->size());
	stDataFileHead.dwLinkSgmtInfoCount = static_cast<uint32>(m_vtLinkSgmtInfoList->size());
	stDataFileHead.dwLinkInfoCount = static_cast<uint32>(m_vtLinkInfoDataList->size());
	stDataFileHead.dwTurnInfoCount = static_cast<uint32>(m_vtTurnInfoList->size());
	stDataFileHead.dwGridInfoStartOffset = DATA_FILE_HEAD_SIZE;
	stDataFileHead.dwGridInfoSize = GRID_INFO_SIZE * stDataFileHead.dwGridInfoCount;
	stDataFileHead.dwGridSgmtInfoStartOffset = stDataFileHead.dwGridInfoStartOffset + stDataFileHead.dwGridInfoSize;
	stDataFileHead.dwGridSgmtInfoSize = GRID_SGMT_INFO_SIZE * stDataFileHead.dwGridSgmtInfoCount;
	stDataFileHead.dwLinkSgmtInfoStartOffset = stDataFileHead.dwGridSgmtInfoStartOffset + stDataFileHead.dwGridSgmtInfoSize;
	stDataFileHead.dwLinkSgmtInfoSize = LINK_SGMT_INFO_SIZE * stDataFileHead.dwLinkSgmtInfoCount;
	stDataFileHead.dwLinkInfoStartOffset = stDataFileHead.dwLinkSgmtInfoStartOffset + stDataFileHead.dwLinkSgmtInfoSize;
	stDataFileHead.dwLinkInfoSize = LINK_INFO_DATA_SIZE * stDataFileHead.dwLinkInfoCount;
	stDataFileHead.dwTurnInfoStartOffset = stDataFileHead.dwLinkInfoStartOffset + stDataFileHead.dwLinkInfoSize;
	stDataFileHead.dwTurnInfoSize = TURN_INFO_DISK_SIZE * stDataFileHead.dwTurnInfoCount;

	// 헤더
	fwrite(&stDataFileHead, DATA_FILE_HEAD_SIZE, 1, fp);

	// 그리드별 세그먼트 범위
	for (vector<GRID_INFO>::iterator it=m_vtGridInfoList->begin(); it!=m_vtGridInfoList->end(); ++it)
		fwrite(&(*it), GRID_INFO_SIZE, 1, fp);

	// GRID 별 세그먼트 정보
	for (vector<GRID_SGMT_INFO>::iterator it=m_vtGridSgmtInfoList->begin(); it!=m_vtGridSgmtInfoList->end(); ++it)
		fwrite(&(*it), GRID_SGMT_INFO_SIZE, 1, fp);

	// 링크별 세그먼트 정보
	for (vector<LINK_SGMT_INFO>::iterator it=m_vtLinkSgmtInfoList->begin(); it!=m_vtLinkSgmtInfoList->end(); ++it)
		fwrite(&(*it), LINK_SGMT_INFO_SIZE, 1, fp);

	// 세그먼트별 링크 정보
	for (vector<LINK_INFO_DATA>::iterator it=m_vtLinkInfoDataList->begin(); it!=m_vtLinkInfoDataList->end(); ++it)
		fwrite(&(*it), LINK_INFO_DATA_SIZE, 1, fp);

	// 시작 링크 기준으로 연결된 링크 회전 정보 (MapMatchSvr 호환 22바이트)
	for (vector<TURN_INFO>::iterator it=m_vtTurnInfoList->begin(); it!=m_vtTurnInfoList->end(); ++it)
	{
		TURN_INFO_DISK stTurnInfoDisk;
		memset(reinterpret_cast<void *>(&stTurnInfoDisk), 0, TURN_INFO_DISK_SIZE);
		stTurnInfoDisk.dwTurnOffset = it->dwTurnOffset;
		stTurnInfoDisk.qwInLinkID = it->qwInLinkID;
		stTurnInfoDisk.qwOutLinkID = it->qwOutLinkID;
		stTurnInfoDisk.nTurnAng = it->nTurnAng;
		fwrite(&stTurnInfoDisk, TURN_INFO_DISK_SIZE, 1, fp);
	}

	return true;
}

/**
 * @brief 링크 정보 데이터 생성 후 회전 정보, 그리드 정보 생성
 * @return true, false
*/
bool CBinaryMaker::SetGridMapData()
{
	for (mapShapeLinkInfo::iterator it=m_mapShapeLinkInfoList->begin(); it!=m_mapShapeLinkInfoList->end(); ++it)
	{
		LINK_INFO_DATA stLinkInfoData;

		memset(reinterpret_cast<void *>(&stLinkInfoData), 0, LINK_INFO_DATA_SIZE);

		// 링크별 회전 정보 Offset, Count 구하기
		memcpy(&stLinkInfoData, it->second, LINK_INFO_DATA_SIZE);
		// 종료 노드 기준 회전 정보 생성 (2026-07-08 최정우 주석 추가)
		if (!GetTurnInfo(it->first, it->second->vtVertexs, stLinkInfoData))
		{
			LOGFMTE("turn info create failed!");
			return false;
		}

		// 링크별 세그먼트 정보 Offset, Count 구하기
		// 링크 버텍스 기반 세그먼트 정보 생성 (2026-07-08 최정우 주석 추가)
		if (!GetLinkSgmtInfo(it->first, it->second->vtVertexs, stLinkInfoData))
		{
			LOGFMTE("link segment info create failed!");
			return false;
		}

		// 링크별 세그먼트 정보 구하기
		// 링크 속성 정보 목록에 적재 (2026-07-08 최정우 주석 추가)
		GetLinkInfoData(stLinkInfoData);

		// GRID 별 세그먼트 정보 저장
		// 링크 형상 기반 GRID 세그먼트 정보 생성 (2026-07-08 최정우 주석 추가)
		if (!GetGridSgmtInfo(it->first, it->second->vtVertexs))
		{
			LOGFMTE("grid segment info temp create failed!");
			return false;
		}
	}

	GRID_INFO stGridInfo;
	GRID_SGMT_INFO stGridSgmtInfo;
	// 전체 GRID 개수 조회 (2026-07-08 최정우 주석 추가)
	m_dwGridOffset = m_cGISUtil.GetMaxGridCount();

	for (uint32 i=0; i<m_dwGridOffset; ++i)
	{
		// init
		memset(reinterpret_cast<void *>(&stGridInfo), 0, GRID_INFO_SIZE);

		mapGridSgmtInfo::iterator it = m_mapGridSgmtInfoList->find(i);
		if (it != m_mapGridSgmtInfoList->end())
		{
			// GRID 별 세그먼트 범위 구하기
			stGridInfo.dwGridID = i;										// GRID ID
			stGridInfo.dwSgmtOffset = m_dwGridSgmtOffset;					// GRID 별 세그먼트 시작 Offset
			stGridInfo.wSgmtCount = static_cast<uint16>(it->second.size());	// GRID 별 세그먼트 개수

			// GRID 별 세그먼트 정보
			setGridSgmtInfo::iterator iter = it->second.begin();
			for (; iter!=it->second.end(); ++iter)
			{
				// init
				memset(&stGridSgmtInfo, 0, GRID_SGMT_INFO_SIZE);
				memcpy(&stGridSgmtInfo, &(*iter), GRID_SGMT_INFO_SIZE);
				stGridSgmtInfo.dwSgmtOffset = m_dwGridSgmtOffset++;

				// GRID 별 세그먼트 정보 로딩
				m_vtGridSgmtInfoList->push_back(stGridSgmtInfo);
			}
		}
		else
			// GRID 별 세그먼트 범위 구하기
			stGridInfo.dwGridID = i;										// GRID ID

		// GRID 별 세그먼트 범위 로딩
		m_vtGridInfoList->push_back(stGridInfo);
	}

	return true;
}

/**
 * @brief 회전 정보 구하기
 * @param[in] qwInLinkID 진입 링크 ID
 * @param[in] vtVertexs 진입링크 버텍스 정보
 * @param[in,out] stLinkInfoData 진입링크 속성에 회전정보 Offset, Count 업데이트
 * @return true, false
*/
bool CBinaryMaker::GetTurnInfo(const uint64& qwInLinkID, 
		const vector<POINT>& vtVertexs, LINK_INFO_DATA& stLinkInfoData)
{
	vector<TURN_INFO> vtTurnInfo;

	// 종료 NODE ID 가 시작 NODE ID 되는 링크 정보 찾기
	pair<mapShapeNodeInfo::iterator, mapShapeNodeInfo::iterator> node_pair;
	node_pair = m_mapShapeNodeInfoList->equal_range(stLinkInfoData.qwEdNodeID);
	if (node_pair.first != node_pair.second)
	{
		mapShapeNodeInfo::iterator it;
		for (it=node_pair.first; it!=node_pair.second; ++it)
		{
			// 진입 링크 회전 각도 구하기
			uint16 wInSize = static_cast<uint16>(vtVertexs.size());
			if (wInSize < 2)
			{
				LOGFMTE("node count is too few!link_id=[%lu],node_id=[%lu]", 
					qwInLinkID, stLinkInfoData.qwEdNodeID);
				return false;
			}
			
			POINT stInPoint1;
			POINT stInPoint2;

			// init
			memset(reinterpret_cast<void *>(&stInPoint2), 0, POINT_SIZE);
			memcpy(&stInPoint2, &vtVertexs[wInSize-1], POINT_SIZE);

			for (sint16 i=static_cast<sint16>(wInSize-2); i>=0; --i)
			{
				// init
				memset(reinterpret_cast<void *>(&stInPoint1), 0, POINT_SIZE);
				memcpy(&stInPoint1, &vtVertexs[i], POINT_SIZE);

				// 버텍스가 같은지 비교
				if (stInPoint1 == stInPoint2)
					break;
			}
			// 진입 링크 진행 방위각 계산 (2026-07-08 최정우 주석 추가)
			sint16 nInAngle = m_cGISUtil.GetDirAngleDegree(stInPoint1, stInPoint2);

			// 진출 링크 회전 각도 구하기
			uint32 dwOutSize = static_cast<uint32>(it->second->vtVertexs.size());
			if (dwOutSize < 2)
			{
				LOGFMTE("node count is too few!node_id=[%lu]", it->first);
				return false;
			}

			POINT stOutPoint1;
			POINT stOutPoint2;

			memset(reinterpret_cast<void *>(&stOutPoint1), 0, POINT_SIZE);
			memcpy(&stOutPoint1, &it->second->vtVertexs[0], POINT_SIZE);
			for (uint32 i=1; i<dwOutSize; ++i)
			{
				memset(reinterpret_cast<void *>(&stOutPoint2), 0, POINT_SIZE);
				memcpy(&stOutPoint2, &it->second->vtVertexs[i], POINT_SIZE);

				// 버텍스가 같은지 비교
				if (stOutPoint1 == stOutPoint2)
					break;
			}
			// 진출 링크 진행 방위각 계산 (2026-07-08 최정우 주석 추가)
			sint16 nOutAngle = m_cGISUtil.GetDirAngleDegree(stOutPoint1, stOutPoint2);

			// 각도 차이 (-179 ~ 180)
			// 진입·진출 방위각 차이(회전각) 계산 (2026-07-08 최정우 주석 추가)
			sint16 nTurnAng = m_cGISUtil.GetAngleDiff(nInAngle, nOutAngle);
			if (nTurnAng == -180) nTurnAng = abs(nTurnAng);

			const uint64 qwOutLinkID = it->second->qwLinkID;

			// MOCT TURNINFO 회전 제한(금지) 쌍 제외
			if (m_pcTurnInfoLoader->IsRestricted(qwInLinkID, qwOutLinkID))
			{
				++m_dwTurnRestrictedSkipCount;
				continue;
			}

			TURN_INFO stTurnInfo;

			// init
			memset(reinterpret_cast<void *>(&stTurnInfo), 0, TURN_INFO_SIZE);

			stTurnInfo.qwInLinkID = qwInLinkID;
			stTurnInfo.qwOutLinkID = qwOutLinkID;
			stTurnInfo.nTurnAng = nTurnAng;
			stTurnInfo.nTurnOper = TURN_OPER_ALLOW;

			TURN_RULE stRule;
			if (m_pcTurnInfoLoader->GetRule(qwInLinkID, qwOutLinkID, stRule))
			{
				stTurnInfo.nTurnType = stRule.nTurnType;
				stTurnInfo.nTurnOper = stRule.nTurnOper;
			}
			else
			{
				stTurnInfo.nTurnType = InferTurnTypeFromAngle(nTurnAng);
			}

			vtTurnInfo.push_back(stTurnInfo);
		}

		if (vtTurnInfo.size() > 0)
		{
			sort(vtTurnInfo.begin(), vtTurnInfo.end());
			vector<TURN_INFO>::iterator turn_it = vtTurnInfo.begin();
			for (; turn_it!=vtTurnInfo.end(); ++turn_it)
			{
				turn_it->dwTurnOffset = m_dwTurnOffset++;

				// 링크별 회전정보 로딩
				m_vtTurnInfoList->push_back(*turn_it);
			}

			// 링크 속성 정보에 회전정보 Offset, Count 설정
			stLinkInfoData.nTurnCount = static_cast<uint8>(vtTurnInfo.size());
			stLinkInfoData.dwTurnOffset = m_dwTurnOffset - stLinkInfoData.nTurnCount;
		}
		else
		{
			// 링크 속성 정보에 회전정보 Offset, Count 설정
			stLinkInfoData.dwTurnOffset = 0;
			stLinkInfoData.nTurnCount = 0;
		}
	}
	else
	{
			// 링크 속성 정보에 회전정보 Offset, Count 설정
		stLinkInfoData.dwTurnOffset = 0;
		stLinkInfoData.nTurnCount = 0;
	}

	return true;
}

/**
 * @brief 링크별 세그먼트 정보 구하기
 * @param[in] qwLinkID 링크 ID
 * @param[in] vtVertexs 버텍스 정보
 * @param[in,out] stLinkInfoData 링크 정보에 Offset, Count 업데이트
 * @return true, false
*/
bool CBinaryMaker::GetLinkSgmtInfo(const uint64& qwLinkID, 
		const vector<POINT>& vtVertexs, LINK_INFO_DATA& stLinkInfoData)
{
	if (vtVertexs.empty())
	{
		LOGFMTE("vertex is empty!");
		return false;
	}

	uint16 wSize = static_cast<uint16>(vtVertexs.size());
	if (wSize < 2)
	{
		LOGFMTE("vertex count is too few!");
		return false;
	}

	LINK_SGMT_INFO stLinkSgmtInfo;
	uint16 wLinkSgmtCount = 0;
	POINT stStPoint;
	POINT stPrePoint;
	POINT stPoint1;
	POINT stPoint2;
	uint16 wLenFromLink = 0;

	// init
	memset(reinterpret_cast<void *>(&stStPoint), 0, POINT_SIZE);
	memcpy(&stStPoint, &vtVertexs[0], POINT_SIZE);

	bool bRun = true;
	for (uint16 i=0; i<wSize-1; ++i)
	{	
		// init
		memset(reinterpret_cast<void *>(&stLinkSgmtInfo), 0, LINK_SGMT_INFO_SIZE);
		memset(reinterpret_cast<void *>(&stPoint1), 0, POINT_SIZE);

		memcpy(&stPoint1, &vtVertexs[i], POINT_SIZE);

		for (uint16 j=i+1; j<wSize; ++j)
		{
			// init
			memset(reinterpret_cast<void *>(&stPoint2), 0, POINT_SIZE);

			memcpy(&stPoint2, &vtVertexs[j], POINT_SIZE);

			// 버텍스가 같은지 비교
			if (stPoint1 == stPoint2)
			{
				if (j == (wSize - 1))			// 마지막 버텍스 이면
				{
					bRun = false;
					break;
				}
				else
					continue;
			}
			else
				break;
		}

		if (!bRun) break;

		// 링크별 세그먼트 정보
		stLinkSgmtInfo.dwSgmtOffset = m_dwLinkSgmtOffset++;
		stLinkSgmtInfo.dwX = static_cast<uint32>(stPoint1.dfX * 360000);
		stLinkSgmtInfo.dwY = static_cast<uint32>(stPoint1.dfY * 360000);
		// 세그먼트 진행 방위각 계산 (2026-07-08 최정우 주석 추가)
		stLinkSgmtInfo.wDirAng = static_cast<uint16>(m_cGISUtil.GetDirAngleDegree(stPoint1, stPoint2));

		// 세그먼트 길이 (0.01sec)
		// 세그먼트 길이 계산 (2026-07-08 최정우 주석 추가)
		stLinkSgmtInfo.wLenSgmt = m_cGISUtil.GetSgmtLength(stPoint1, stPoint2);

		stLinkSgmtInfo.qwLinkID = qwLinkID;

		// 링크의 시작점부터 세그먼트 시작점까지의 거리 (m)
		if ((stStPoint == stPoint1) && (i == 0))
		{
			stLinkSgmtInfo.wLenFromLink = 0;
			memset(reinterpret_cast<void *>(&stPrePoint), 0, POINT_SIZE);

			memcpy(&stPrePoint, &stPoint1, POINT_SIZE);
		}
		else if ((i > 0) && (stPrePoint != stPoint1))
		{
			// 이전 버텍스부터 세그먼트 시작점까지 경위도 거리 (2026-07-08 최정우 주석 추가)
			uint16 wSgmtLen = static_cast<uint16>(round(m_cGISUtil.GetDistanceGEO2(stPrePoint, stPoint1)));
			if (wSgmtLen <= 0) wSgmtLen = 1;
			wLenFromLink += wSgmtLen;
			stLinkSgmtInfo.wLenFromLink = wLenFromLink;

			memcpy(&stPrePoint, &stPoint1, POINT_SIZE);
		}

		// 링크별 세그먼트 정보 로딩
		m_vtLinkSgmtInfoList->push_back(stLinkSgmtInfo);
		wLinkSgmtCount++;
	}

	// 링크 속성 정보에 세그먼트 정보 Offset, Count 설정
	stLinkInfoData.dwSgmtOffset = m_dwLinkSgmtOffset - wLinkSgmtCount;
	stLinkInfoData.wSgmtCount = wLinkSgmtCount;

	return true;
}

/**
 * @brief 링크별 세그먼트 정보 로딩
 * @param[in] stLinkInfoData 세그먼트별 링크 정보
 * @return void
*/
void CBinaryMaker::GetLinkInfoData(LINK_INFO_DATA& stLinkInfoData)
{
	// 링크별 속성정보 로딩
	m_vtLinkInfoDataList->push_back(stLinkInfoData);
}

/**
 * @brief GRID 별 세그먼트 정보 구하기
 * @param[in] qwLinkID 링크 ID 
 * @param[in] vtVertexs 버텍스 정보
 * @return true, false
*/
bool CBinaryMaker::GetGridSgmtInfo(const uint64& qwLinkID, const vector<POINT>& vtVertexs)
{
	if (vtVertexs.empty())
	{
		LOGFMTE("vertex is empty!");
		return false;
	}

	uint16 wSize = static_cast<uint16>(vtVertexs.size());
	if (wSize < 2)
	{
		LOGFMTE("vertex count is too few!");
		return false;
	}

	POINT stStPoint;
	POINT stPrePoint;
	POINT stPoint1;
	POINT stPoint2;
	GRID_NO_INFO stGridNoInfo1;
	GRID_NO_INFO stGridNoInfo2;
	GRID_NO_MIN_MAX_INFO stGridNoMinMaxInfo;
	uint16 wLenFromLink = 0;
	uint16 wSgmtLen = 0;

	// init
	memset(reinterpret_cast<void *>(&stStPoint), 0, POINT_SIZE);
	memset(reinterpret_cast<void *>(&stPrePoint), 0, POINT_SIZE);
	memcpy(&stStPoint, &vtVertexs[0], POINT_SIZE);

	bool bRun = true;
	for (uint16 i=0; i<wSize-1; ++i)
	{
		// init
		memset(reinterpret_cast<void *>(&stGridNoMinMaxInfo), 0, GRID_NO_MIN_MAX_INFO_SIZE);
		memcpy(&stPoint1, &vtVertexs[i], POINT_SIZE);

		// 세그먼트 시작점 GRID 열 번호 계산 (2026-07-08 최정우 주석 추가)
		stGridNoInfo1.nColNo = m_cGISUtil.GetGridColNo(stPoint1.dfX);
		// 세그먼트 시작점 GRID 행 번호 계산 (2026-07-08 최정우 주석 추가)
		stGridNoInfo1.nRowNo = m_cGISUtil.GetGridRowNo(stPoint1.dfY);
		if ((stGridNoInfo1.nColNo == INVALID_GRID_COL_NO) || 
			(stGridNoInfo1.nRowNo == INVALID_GRID_ROW_NO))
			continue;
		
		for (uint16 j=i+1; j<wSize; ++j)
		{
			memcpy(&stPoint2, &vtVertexs[j], POINT_SIZE);

			// 세그먼트 종료점 GRID 열 번호 계산 (2026-07-08 최정우 주석 추가)
			stGridNoInfo2.nColNo = m_cGISUtil.GetGridColNo(stPoint2.dfX);
			// 세그먼트 종료점 GRID 행 번호 계산 (2026-07-08 최정우 주석 추가)
			stGridNoInfo2.nRowNo = m_cGISUtil.GetGridRowNo(stPoint2.dfY);
			if ((stGridNoInfo2.nColNo == INVALID_GRID_COL_NO) || 
				(stGridNoInfo2.nRowNo == INVALID_GRID_ROW_NO))
				continue;

			// 버텍스가 같은지 비교
			if (stPoint1 == stPoint2)
			{
				if (j == (wSize - 1))			// 마지막 버텍스 이면
				{
					bRun = false;
					break;
				}
				else
					continue;
			}
			else
				break;
		}

		if (!bRun) break;

		// GRID 교차 여부 확인
		if (stGridNoInfo1.nColNo <= stGridNoInfo2.nColNo)
		{
			stGridNoMinMaxInfo.dwColNoMin = static_cast<uint32>(stGridNoInfo1.nColNo);
			stGridNoMinMaxInfo.dwColNoMax = static_cast<uint32>(stGridNoInfo2.nColNo);
		}
		else
		{
			stGridNoMinMaxInfo.dwColNoMin = static_cast<uint32>(stGridNoInfo2.nColNo);
			stGridNoMinMaxInfo.dwColNoMax = static_cast<uint32>(stGridNoInfo1.nColNo);
		}

		if (stGridNoInfo1.nRowNo <= stGridNoInfo2.nRowNo)
		{
			stGridNoMinMaxInfo.dwRowNoMin = static_cast<uint32>(stGridNoInfo1.nRowNo);
			stGridNoMinMaxInfo.dwRowNoMax = static_cast<uint32>(stGridNoInfo2.nRowNo);
		}
		else
		{
			stGridNoMinMaxInfo.dwRowNoMin = static_cast<uint32>(stGridNoInfo2.nRowNo);
			stGridNoMinMaxInfo.dwRowNoMax = static_cast<uint32>(stGridNoInfo1.nRowNo);
		}

		if ((stGridNoMinMaxInfo.dwColNoMin == stGridNoMinMaxInfo.dwColNoMax) && 
			(stGridNoMinMaxInfo.dwRowNoMin == stGridNoMinMaxInfo.dwRowNoMax))
		{
			// 동일 GRID 내 세그먼트 — GRID ID 계산 (2026-07-08 최정우 주석 추가)
			uint32 dwGridID = m_cGISUtil.GetGridID(stGridNoMinMaxInfo.dwColNoMin, stGridNoMinMaxInfo.dwRowNoMin);
		// 링크의 시작점부터 세그먼트 시작점까지의 거리 (m)
			if ((stStPoint == stPoint1) && (i == 0))
			{
				wLenFromLink = 0;
				memcpy(&stPrePoint, &stPoint1, POINT_SIZE);
			}
			else if ((i > 0) && (stPrePoint != stPoint1))
			{
				wSgmtLen = static_cast<uint16>(round(m_cGISUtil.GetDistanceGEO2(stPrePoint, stPoint1)));
				if (wSgmtLen <= 0) wSgmtLen = 1;
				wLenFromLink += wSgmtLen;
				memcpy(&stPrePoint, &stPoint1, POINT_SIZE);
			}

			// GRID 별 세그먼트 정보 임시 저장
			// 단일 GRID 세그먼트 정보 등록 (2026-07-08 최정우 주석 추가)
			SetGridSgmtInfo(dwGridID, qwLinkID, wLenFromLink, stPoint1, stPoint2);
		}
		else
		{
			// 세그먼트의 시작점과 종료점이 동일한 GRID 에 있지 않으면 GRID 교차 확인
			uint32 dwColNo = stGridNoMinMaxInfo.dwColNoMin;
			while (dwColNo <= stGridNoMinMaxInfo.dwColNoMax)
			{
				uint32 dwRowNo = stGridNoMinMaxInfo.dwRowNoMin;
				while (dwRowNo <= stGridNoMinMaxInfo.dwRowNoMax)
				{
					// 세그먼트와 GRID 셀 교차 여부 확인 (2026-07-08 최정우 주석 추가)
					if (m_cGISUtil.IsCrossSgmt2Grid(stPoint1, stPoint2, dwColNo, dwRowNo))
					{
						// 교차 GRID ID 계산 (2026-07-08 최정우 주석 추가)
						uint32 dwGridID = m_cGISUtil.GetGridID(dwColNo, dwRowNo);

		// 링크의 시작점부터 세그먼트 시작점까지의 거리 (m)
						if ((stStPoint == stPoint1) && (i == 0))
						{
							wLenFromLink = 0;
							memcpy(&stPrePoint, &stPoint1, POINT_SIZE);
						}
						else if ((i > 0) && (stPrePoint != stPoint1))
						{
							wSgmtLen = static_cast<uint16>(round(m_cGISUtil.GetDistanceGEO2(stPrePoint, stPoint1)));
							if (wSgmtLen <= 0) wSgmtLen = 1;
							wLenFromLink += wSgmtLen;
							memcpy(&stPrePoint, &stPoint1, POINT_SIZE);
						}

			// GRID 별 세그먼트 정보 임시 저장
						// 교차 GRID 세그먼트 정보 등록 (2026-07-08 최정우 주석 추가)
						SetGridSgmtInfo(dwGridID, qwLinkID, wLenFromLink, stPoint1, stPoint2);
					}
					++dwRowNo;
				}
				++dwColNo;
			}
		}
	}

	return true;
}

/**
 * @brief GRID 별 세그먼트 정보 임시 저장
 * @param[in] dwGridID GRID ID
 * @param[in] qwLinkID 링크 ID
 * @param[in] wLenFromLink 링크의 시작점부터 세그먼트의 시작점까지 거리 (m)
 * @param[in] stPoint1 세그먼트 시작점 X,Y 좌표
 * @param[in] stPoint2 세그먼트 종료점 X,Y 좌표
 * @return void
*/
void CBinaryMaker::SetGridSgmtInfo(uint32& dwGridID, const uint64& qwLinkID, 
		uint16& wLenFromLink, POINT& stPoint1, POINT& stPoint2)
{
	GRID_SGMT_INFO stGridSgmtInfo;

				// init
	memset(&stGridSgmtInfo, 0, GRID_SGMT_INFO_SIZE);

	// GRID 별 세그먼트 정보
	stGridSgmtInfo.dwX = static_cast<uint32>(stPoint1.dfX * 360000);
	stGridSgmtInfo.dwY = static_cast<uint32>(stPoint1.dfY * 360000);
	// GRID 세그먼트 진행 방위각 계산 (2026-07-08 최정우 주석 추가)
	stGridSgmtInfo.wDirAng = static_cast<uint16>(m_cGISUtil.GetDirAngleDegree(stPoint1, stPoint2));

	// 세그먼트 길이 (0.01sec)
	// GRID 세그먼트 길이 계산 (2026-07-08 최정우 주석 추가)
	stGridSgmtInfo.wLenSgmt = m_cGISUtil.GetSgmtLength(stPoint1, stPoint2);

	// 링크 ID
	stGridSgmtInfo.qwLinkID = qwLinkID;

	// 링크의 시작점부터 세그먼트 시작점까지의 거리
	stGridSgmtInfo.wLenFromLink = wLenFromLink;

	mapGridSgmtInfo::iterator it = m_mapGridSgmtInfoList->find(dwGridID);
	if (it == m_mapGridSgmtInfoList->end())
	{
		setGridSgmtInfo setGridSgmtInfoList;
		setGridSgmtInfoList.insert(stGridSgmtInfo);
		m_mapGridSgmtInfoList->insert(pair<uint32, setGridSgmtInfo>(dwGridID, setGridSgmtInfoList));
	}
	else
		it->second.insert(stGridSgmtInfo);
}

/**
 * @brief 회전각으로 MOCT TURN_TYPE 추정 (TURNINFO 미등록 쌍)
 * @param[in] nTurnAng 회전각 (-180~180)
 * @return MOCT TURN_TYPE (11:직진, 101:좌회전, 102:우회전, 103:유턴)
*/
uint16 CBinaryMaker::InferTurnTypeFromAngle(sint16 nTurnAng) const
{
	sint16 nAbsAng = static_cast<sint16>(abs(nTurnAng));

	if (nAbsAng <= 30)
		return 11;
	if (nAbsAng >= 150)
		return 103;
	if (nTurnAng > 0)
		return 101;
	return 102;
}
