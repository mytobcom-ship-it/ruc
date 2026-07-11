/**
 * @file DataLoader.cpp
 * @brief 세그먼트 및 링크 정보 로딩 클래스 소스 파일
*/
#include "DataLoader.h"

/**
 * @brief 생성자
*/
CDataLoader::CDataLoader() :
	m_bLoad(false), 
	m_nMaxStep(0), 
	m_dwGridInfoSize(0), 
	m_dwGridSgmtInfoSize(0), 
	m_dwLinkSgmtInfoSize(0), 
	m_dwLinkInfoSize(0), 
	m_dwTurnInfoSize(0), 
	m_dwGridInfoStartOffset(0), 
	m_dwGridSgmtInfoStartOffset(0), 
	m_dwLinkSgmtInfoStartOffset(0), 
	m_dwLinkInfoStartOffset(0), 
	m_dwTurnInfoStartOffset(0), 
	m_dwGridInfoCount(0), 
	m_dwGridSgmtInfoCount(0), 
	m_dwLinkSgmtInfoCount(0), 
	m_dwLinkInfoCount(0), 
	m_dwTurnInfoCount(0), 
	m_pstDataFileHead(nullptr), 
	m_pstGridInfoList(nullptr), 
	m_pstGridSgmtInfoList(nullptr), 
	m_pstLinkSgmtInfoList(nullptr), 
	m_mapLinkInfoList(nullptr), 
	m_pstTurnInfoList(nullptr)
{
}

/**
 * @brief 소멸자
*/
CDataLoader::~CDataLoader()
{
	Uninitialize();
}

/**
 * @brief 초기화
 * @param[in] strDataFile 데이터 바이너리 파일명
 * @param[in] nMaxStep 연속 맵매칭 최대 검색 단계 설정
 * @return void
*/
void CDataLoader::Initialize(string& strDataFile, const sint16 nMaxStep)
{
	m_strDataFile = strDataFile;
	m_nMaxStep = nMaxStep;			// 연속 맵매칭 최대 검색 단계
}

/**
 * @brief 메모리 삭제
 * @return void
*/
void CDataLoader::Uninitialize()
{
	SetDataInit();
	m_bLoad = false;
}

/**
 * @brief 데이터 업데이트
 * @return true(성공), false(실패)
*/
bool CDataLoader::SetDataUpdate()
{
	if (access(m_strDataFile.c_str(), F_OK) != 0)
	{
		LOGFMTE("data binary file not found!file=[%s]", m_strDataFile.c_str());
		return false;
	}

	FILE *fp = fopen(m_strDataFile.c_str(), "rb");
	if (fp == nullptr)
	{
		LOGFMTE("data binary file open failed!");
		return false;
	}

	m_pstDataFileHead = new (std::nothrow)DATA_FILE_HEAD;
	if (m_pstDataFileHead == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("head memory allocate failed!");
		return false;
	}
	fread(m_pstDataFileHead, 1, DATA_FILE_HEAD_SIZE, fp);

	// 데이터 크기 검증
	if ((m_pstDataFileHead->dwGridInfoCount * GRID_INFO_SIZE) != m_pstDataFileHead->dwGridInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("grid info size is not same!");
		return m_bLoad;
	}

	if ((m_pstDataFileHead->dwGridSgmtInfoCount * GRID_SGMT_INFO_SIZE) != m_pstDataFileHead->dwGridSgmtInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("grid segment info size is not same!");
		return m_bLoad;
	}
	
	if ((m_pstDataFileHead->dwLinkSgmtInfoCount * LINK_SGMT_INFO_SIZE) != m_pstDataFileHead->dwLinkSgmtInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("link segment info size is not same!");
		return m_bLoad;
	}
	
	if ((m_pstDataFileHead->dwLinkInfoCount * LINK_INFO_DATA_SIZE) != m_pstDataFileHead->dwLinkInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("link info size is not same!");
		return m_bLoad;
	}

	if ((m_pstDataFileHead->dwTurnInfoCount * TURN_INFO_SIZE) != m_pstDataFileHead->dwTurnInfoSize)
	{
		if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
		m_pstDataFileHead = nullptr;
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		LOGFMTE("turn info size is not same!");
		return m_bLoad;
	}

	// 데이터 초기화
	SetDataInit();

	m_dwGridInfoSize = m_pstDataFileHead->dwGridInfoSize;
	m_dwGridInfoStartOffset = m_pstDataFileHead->dwGridInfoStartOffset;
	m_dwGridInfoCount = m_pstDataFileHead->dwGridInfoCount;

	m_dwGridSgmtInfoSize = m_pstDataFileHead->dwGridSgmtInfoSize;
	m_dwGridSgmtInfoStartOffset = m_pstDataFileHead->dwGridSgmtInfoStartOffset;
	m_dwGridSgmtInfoCount = m_pstDataFileHead->dwGridSgmtInfoCount;

	m_dwLinkSgmtInfoSize = m_pstDataFileHead->dwLinkSgmtInfoSize;
	m_dwLinkSgmtInfoStartOffset = m_pstDataFileHead->dwLinkSgmtInfoStartOffset;
	m_dwLinkSgmtInfoCount = m_pstDataFileHead->dwLinkSgmtInfoCount;
 
	m_dwLinkInfoSize = m_pstDataFileHead->dwLinkInfoSize;
	m_dwLinkInfoStartOffset = m_pstDataFileHead->dwLinkInfoStartOffset;
	m_dwLinkInfoCount = m_pstDataFileHead->dwLinkInfoCount;

	m_dwTurnInfoSize = m_pstDataFileHead->dwTurnInfoSize;
	m_dwTurnInfoStartOffset = m_pstDataFileHead->dwTurnInfoStartOffset;
	m_dwTurnInfoCount = m_pstDataFileHead->dwTurnInfoCount;
	
	if (m_pstDataFileHead != nullptr) delete m_pstDataFileHead;
	m_pstDataFileHead = nullptr;

	// 그리드별 세그먼트 범위 로딩 메모리
	LOGFMTT("grid info read start!");
	m_pstGridInfoList = new (std::nothrow)GRID_INFO[m_dwGridInfoCount];
	if (m_pstGridInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("grid info memory allocate failed!");
		return false;
	}
	LOGFMTT("grid info read end!");

	fseek(fp, m_dwGridInfoStartOffset, SEEK_SET);
	fread(m_pstGridInfoList, 1, m_dwGridInfoSize, fp);

	// 그리드별 세그먼트 정보 로딩 메모리
	LOGFMTT("grid segment info read start!");
	m_pstGridSgmtInfoList = new (std::nothrow)GRID_SGMT_INFO[m_dwGridSgmtInfoCount];
	if (m_pstGridSgmtInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("grid segment info memory allocate failed!");
		return false;
	}
	fseek(fp, m_dwGridSgmtInfoStartOffset, SEEK_SET);
	fread(m_pstGridSgmtInfoList, 1, m_dwGridSgmtInfoSize, fp);
	LOGFMTT("grid segment info read end!");

	// 링크별 세그먼트 정보 로딩 메모리
	LOGFMTT("link segment info read start!");
	m_pstLinkSgmtInfoList = new (std::nothrow)LINK_SGMT_INFO[m_dwLinkSgmtInfoCount];
	if (m_pstLinkSgmtInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link segment info memory allocate failed!");
		return false;
	}
	fseek(fp, m_dwLinkSgmtInfoStartOffset, SEEK_SET);
	fread(m_pstLinkSgmtInfoList, 1, m_dwLinkSgmtInfoSize, fp);
	LOGFMTT("link segment info read end!");

	// 세그먼트별 링크 정보
	LOGFMTT("link data information read start!");
	PLINK_INFO_DATA pstLinkInfoData = new (std::nothrow)LINK_INFO_DATA[m_dwLinkInfoCount];
	if (pstLinkInfoData == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link data info memory allocate failed!");
		return false;
	}
	fseek(fp, m_dwLinkInfoStartOffset, SEEK_SET);
	fread(pstLinkInfoData, 1, m_dwLinkInfoSize, fp);
	LOGFMTT("link data information read end!");

	m_mapLinkInfoList = new (std::nothrow)mapLinkInfo;
	if (m_mapLinkInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		if (pstLinkInfoData != nullptr) delete [] pstLinkInfoData;
		pstLinkInfoData = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("link data info memory allocate failed!");
		return false;
	}

	LOGFMTT("link information insert start!");
	for (uint32 i=0; i<m_dwLinkInfoCount; ++i)
	{
		LINK_INFO stLinkInfo;

		// init
		memset(reinterpret_cast<void *>(&stLinkInfo), 0, LINK_INFO_SIZE);

		uint64 qwLinkID = pstLinkInfoData[i].qwLinkID;
		memcpy(reinterpret_cast<void *>(&stLinkInfo), reinterpret_cast<const void *>(&pstLinkInfoData[i].dwSgmtOffset), LINK_INFO_SIZE);

		m_mapLinkInfoList->insert(pair<uint64, LINK_INFO>(qwLinkID, stLinkInfo));
	}
	LOGFMTT("link information insert end!");

	if (pstLinkInfoData != nullptr) delete [] pstLinkInfoData;
	pstLinkInfoData = nullptr;

	// 시작 링크 기준으로 연결된 링크 회전 정보 로딩 메모리
	LOGFMTT("turn info read start!");
	m_pstTurnInfoList = new (std::nothrow)TURN_INFO[m_dwTurnInfoCount];
	if (m_pstTurnInfoList == nullptr)
	{
		if (fp != nullptr) fclose(fp);
		fp = nullptr;
		SetDataInit();
		m_bLoad = false;
		LOGFMTE("turn info memory allocate failed!");
		return false;
	}
	fseek(fp, m_dwTurnInfoStartOffset, SEEK_SET);
	fread(m_pstTurnInfoList, 1, m_dwTurnInfoSize, fp);
	LOGFMTT("turn info read end!");

	if (fp != nullptr) fclose(fp);
	fp = nullptr;

	m_bLoad = true;
	return true;
}

/**
 * @brief 데이터 정보 출력
 * @return void
*/
void CDataLoader::SetDataInfoDisplay()
{
	LOGFMTI("-----------------------------------------------------------------");
	LOGFMTI("GRID_INFO : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwGridInfoCount, static_cast<uint16>(GRID_INFO_SIZE), m_dwGridInfoSize);
	LOGFMTI("SGMT_INFO_GRID : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwGridSgmtInfoCount, static_cast<uint16>(GRID_SGMT_INFO_SIZE), m_dwGridSgmtInfoSize);
	LOGFMTI("SGMT_INFO_LINK : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwLinkSgmtInfoCount, static_cast<uint16>(LINK_SGMT_INFO_SIZE), m_dwLinkSgmtInfoSize);
	LOGFMTI("LINK_INFO : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwLinkInfoCount, static_cast<uint16>(LINK_INFO_DATA_SIZE), m_dwLinkInfoSize);
	LOGFMTI("TURN_INFO : [%u] Count * [%d] Size = [%u] bytes", 
		m_dwTurnInfoCount, static_cast<uint16>(TURN_INFO_SIZE), m_dwTurnInfoSize);
	LOGFMTI("-----------------------------------------------------------------");
}

/**
 * @brief 데이터 초기화
 * @return void
*/
void CDataLoader::SetDataInit()
{
	// 데이터 byte 크기 초기화
	m_dwGridInfoSize = 0; 
	m_dwGridSgmtInfoSize = 0;
	m_dwLinkSgmtInfoSize = 0;
	m_dwLinkInfoSize = 0;
	m_dwTurnInfoSize = 0;

	// 데이터 시작 위치
	m_dwGridInfoStartOffset = 0;
	m_dwGridSgmtInfoStartOffset = 0;
	m_dwLinkSgmtInfoStartOffset = 0;
	m_dwLinkInfoStartOffset = 0;
	m_dwTurnInfoStartOffset = 0;

	// 데이터 개수 초기화
	m_dwGridInfoCount = 0;
	m_dwGridSgmtInfoCount = 0;
	m_dwLinkSgmtInfoCount = 0;
	m_dwLinkInfoCount = 0;
	m_dwTurnInfoCount = 0;

	// 데이터 삭제
	if (m_pstGridInfoList != nullptr) delete [] m_pstGridInfoList;
	if (m_pstGridSgmtInfoList != nullptr) delete [] m_pstGridSgmtInfoList;
	if (m_pstLinkSgmtInfoList != nullptr) delete [] m_pstLinkSgmtInfoList;
	if (m_mapLinkInfoList != nullptr)
	{
		m_mapLinkInfoList->clear();
		delete m_mapLinkInfoList;
	}
	if (m_pstTurnInfoList != nullptr) delete [] m_pstTurnInfoList;

	m_pstGridInfoList = nullptr;
	m_pstGridSgmtInfoList = nullptr;
	m_pstLinkSgmtInfoList = nullptr;
	m_mapLinkInfoList = nullptr;
	m_pstTurnInfoList = nullptr;
}

/**
 * @brief GRID 별 세그먼트 범위 구하기
 * @param[in] dwGridID GRID ID
 * @return GRID 별 세그먼트 범위 정보
*/
PGRID_INFO CDataLoader::GetGridInfo(const uint32 dwGridID)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (m_dwGridInfoCount <= 0)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (dwGridID >= m_dwGridInfoCount)
		return nullptr;

	return &m_pstGridInfoList[dwGridID];
}

/**
 * @brief GRID 별 세그먼트 정보 구하기
 * @param[in] dwOffset 세그먼트 Offset
 * @return GRID 별 세그먼트 정보
*/
PGRID_SGMT_INFO CDataLoader::GetGridSgmtInfo(const uint32 dwOffset)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (dwOffset >= m_dwGridSgmtInfoCount)
		return nullptr;

	return &m_pstGridSgmtInfoList[dwOffset];
}

/**
 * @brief 세그먼트별 링크 정보 구하기
 * @param[in] dwOffset 세그먼트 Offset
 * @return 세그먼트별 링크 정보
*/
PLINK_SGMT_INFO CDataLoader::GetLinkSgmtInfo(const uint32 dwOffset)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if (dwOffset >= m_dwLinkSgmtInfoCount)
		return nullptr;

	return &m_pstLinkSgmtInfoList[dwOffset];
}

/**
 * @brief 세그먼트별 링크 정보 구하기
 * @param[in] nLinkID 링크 ID
 * @return 링크 정보
*/
PLINK_INFO CDataLoader::GetLinkInfo(const uint64 qwLinkID)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if ((qwLinkID <= 0 ) || (m_dwLinkInfoCount <= 0))
		return nullptr;

	mapLinkInfo::iterator it = m_mapLinkInfoList->find(qwLinkID);
	if (it == m_mapLinkInfoList->end())
		return nullptr;

	return &it->second;
}

/**
 * @brief 시작 링크 기준으로 연결된 링크 회전 정보 구하기
 * @param[in] dwOffset 회전정보 Offset
 * @return 시작 링크 기준으로 연결된 링크 회전 정보
*/
PTURN_INFO CDataLoader::GetTurnInfo(const uint32 dwOffset)
{
	if (!m_bLoad)
	{
		LOGFMTE("data loading failed!");
		return nullptr;
	}

	if ((dwOffset < 0) || (dwOffset >= m_dwTurnInfoCount))
		return nullptr;

	return &m_pstTurnInfoList[dwOffset];
}
