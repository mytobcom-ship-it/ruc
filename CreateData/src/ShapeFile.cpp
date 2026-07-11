/**
 * @file ShapeFile.cpp
 * @brief ESRI Shapefile(.shp) + 속성(.dbf) 자체 리더 클래스 소스 파일
 * @remark host(x86_64) 는 little-endian 가정. (.shp 의 double/정수 본문은 little-endian)
*/
#include "ShapeFile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "log4z.h"

using namespace zsummer::log4z;

// 빅엔디안 32bit 정수 읽기 (.shp 파일/레코드 헤더)
static sint32 ReadBE32(const unsigned char *p)
{
	return (static_cast<sint32>(p[0]) << 24) | (static_cast<sint32>(p[1]) << 16) | 
		(static_cast<sint32>(p[2]) << 8) | static_cast<sint32>(p[3]);
}

// 리틀엔디안 32bit 정수 읽기 (.shp 본문)
static sint32 ReadLE32(const unsigned char *p)
{
	return (static_cast<sint32>(p[3]) << 24) | (static_cast<sint32>(p[2]) << 16) | 
		(static_cast<sint32>(p[1]) << 8) | static_cast<sint32>(p[0]);
}

// 리틀엔디안 32bit 부호없는 정수 읽기 (.dbf 헤더)
static uint32 ReadLE32U(const unsigned char *p)
{
	return (static_cast<uint32>(p[3]) << 24) | (static_cast<uint32>(p[2]) << 16) | 
		(static_cast<uint32>(p[1]) << 8) | static_cast<uint32>(p[0]);
}

// 리틀엔디안 16bit 부호없는 정수 읽기 (.dbf 헤더)
static uint16 ReadLE16U(const unsigned char *p)
{
	return static_cast<uint16>(static_cast<uint16>(p[0]) | (static_cast<uint16>(p[1]) << 8));
}

// 리틀엔디안 double 읽기 (.shp 본문, host little-endian 가정)
static double ReadLEDouble(const unsigned char *p)
{
	double d;
	memcpy(&d, p, sizeof(double));
	return d;
}

// 파일 전체를 메모리로 읽기
static bool ReadWholeFile(const string& strPath, vector<char>& vtBuf)
{
	// shapefile 바이너리 열기 (2026-07-08 최정우 주석 추가)
	FILE *fp = fopen(strPath.c_str(), "rb");
	if (fp == nullptr)
	{
		LOGFMTE("shapefile open failed!file=[%s]", strPath.c_str());
		return false;
	}

	fseek(fp, 0, SEEK_END);
	long lSize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (lSize <= 0)
	{
		fclose(fp);
		LOGFMTE("shapefile size invalid!file=[%s]", strPath.c_str());
		return false;
	}

	vtBuf.resize(static_cast<size_t>(lSize));
	// 파일 전체 바이트 읽기 (2026-07-08 최정우 주석 추가)
	size_t nRead = fread(&vtBuf[0], 1, static_cast<size_t>(lSize), fp);
	// 파일 핸들 닫기 (2026-07-08 최정우 주석 추가)
	fclose(fp);

	if (nRead != static_cast<size_t>(lSize))
	{
		LOGFMTE("shapefile read failed!file=[%s]", strPath.c_str());
		return false;
	}

	return true;
}

static string TrimString(const char *p, size_t nLen)
{
	size_t nStart = 0;
	size_t nEnd = nLen;
	while (nStart < nEnd && (p[nStart] == ' ' || p[nStart] == '\0' || p[nStart] == '\t'))
		++nStart;
	while (nEnd > nStart && (p[nEnd-1] == ' ' || p[nEnd-1] == '\0' || p[nEnd-1] == '\t'))
		--nEnd;
	return string(p + nStart, nEnd - nStart);
}

static string ToUpper(const string& strIn)
{
	string strOut = strIn;
	transform(strOut.begin(), strOut.end(), strOut.begin(), ::toupper);
	return strOut;
}

/**
 * @brief 생성자
*/
CShapeFile::CShapeFile() : 
	m_nShapeType(SHP_TYPE_NULL), 
	m_dwRecordCount(0), 
	m_dwDbfRecordCount(0), 
	m_wDbfRecordSize(0)
{
}

/**
 * @brief 소멸자
*/
CShapeFile::~CShapeFile()
{
	// 로드된 shapefile 메모리 해제 (2026-07-08 최정우 주석 추가)
	Close();
}

/**
 * @brief .shp + .dbf 로드
 * @param[in] strShpFile .shp 파일 경로
 * @return true(성공), false(실패)
*/
bool CShapeFile::Open(const string& strShpFile)
{
	// 이전 로드 데이터 해제 (2026-07-08 최정우 주석 추가)
	Close();

	// .shp 형상 파일 파싱 (2026-07-08 최정우 주석 추가)
	if (!LoadShp(strShpFile))
		return false;

	string strDbfFile = strShpFile;
	size_t nPos = strDbfFile.find_last_of('.');
	if (nPos != string::npos)
		strDbfFile = strDbfFile.substr(0, nPos);
	strDbfFile.append(".dbf");

	// .dbf 속성 파일 파싱 (2026-07-08 최정우 주석 추가)
	if (!LoadDbf(strDbfFile))
		return false;

	if (m_dwRecordCount != m_dwDbfRecordCount)
	{
		LOGFMTW("shape record count[%u] != dbf record count[%u]!file=[%s]", 
			m_dwRecordCount, m_dwDbfRecordCount, strShpFile.c_str());
	}

	return true;
}

/**
 * @brief 메모리 반환
 * @return void
*/
void CShapeFile::Close()
{
	m_nShapeType = SHP_TYPE_NULL;
	m_dwRecordCount = 0;
	m_dwDbfRecordCount = 0;
	m_wDbfRecordSize = 0;
	m_vtPoint.clear();
	m_vtPolyLine.clear();
	m_vtFields.clear();
	m_mapFieldIdx.clear();
	m_vtDbfRecords.clear();
}

/**
 * @brief .shp 형상 파일 파싱
 * @param[in] strShpFile .shp 파일 경로
 * @return true(성공), false(실패)
*/
bool CShapeFile::LoadShp(const string& strShpFile)
{
	vector<char> vtBuf;
	// .shp 파일 전체를 메모리로 읽기 (2026-07-08 최정우 주석 추가)
	if (!ReadWholeFile(strShpFile, vtBuf))
		return false;

	if (vtBuf.size() < 100)
	{
		LOGFMTE("shp header too small!file=[%s]", strShpFile.c_str());
		return false;
	}

	const unsigned char *pBase = reinterpret_cast<const unsigned char *>(vtBuf.data());

	sint32 nFileCode = ReadBE32(pBase);
	if (nFileCode != 9994)
	{
		LOGFMTE("shp file code invalid![%d] file=[%s]", nFileCode, strShpFile.c_str());
		return false;
	}

	m_nShapeType = ReadLE32(pBase + 32);

	size_t nPos = 100;
	uint32 dwRec = 0;
	while (nPos + 8 <= vtBuf.size())
	{
		sint32 nContentLen = ReadBE32(pBase + nPos + 4);
		size_t nContentBytes = static_cast<size_t>(nContentLen) * 2;
		nPos += 8;
		if (nPos + nContentBytes > vtBuf.size())
			break;

		const unsigned char *c = pBase + nPos;
		sint32 nShpType = ReadLE32(c);

		if (nShpType == SHP_TYPE_POINT || nShpType == SHP_TYPE_POINTZ || nShpType == SHP_TYPE_POINTM)
		{
			POINT stPoint;
			// Point X 좌표(double) 읽기 (2026-07-08 최정우 주석 추가)
			stPoint.dfX = ReadLEDouble(c + 4);
			// Point Y 좌표(double) 읽기 (2026-07-08 최정우 주석 추가)
			stPoint.dfY = ReadLEDouble(c + 12);
			m_vtPoint.push_back(stPoint);
		}
		else if (nShpType == SHP_TYPE_POLYLINE || nShpType == SHP_TYPE_POLYGON || 
				nShpType == SHP_TYPE_POLYLINEZ || nShpType == SHP_TYPE_POLYGONZ || 
				nShpType == SHP_TYPE_POLYLINEM || nShpType == SHP_TYPE_POLYGONM)
		{
			sint32 nNumParts = ReadLE32(c + 36);
			sint32 nNumPoints = ReadLE32(c + 40);
			size_t nPtStart = 44 + static_cast<size_t>(nNumParts) * 4;

			vector<POINT> vtPoints;
			if (nNumPoints > 0) vtPoints.reserve(static_cast<size_t>(nNumPoints));
			for (sint32 i=0; i<nNumPoints; ++i)
			{
				POINT stPoint;
				// PolyLine 버텍스 X 좌표(double) 읽기 (2026-07-08 최정우 주석 추가)
				stPoint.dfX = ReadLEDouble(c + nPtStart + static_cast<size_t>(i) * 16);
				// PolyLine 버텍스 Y 좌표(double) 읽기 (2026-07-08 최정우 주석 추가)
				stPoint.dfY = ReadLEDouble(c + nPtStart + static_cast<size_t>(i) * 16 + 8);
				vtPoints.push_back(stPoint);
			}
			m_vtPolyLine.push_back(vtPoints);
		}
		else
		{
			if (m_nShapeType == SHP_TYPE_POINT || m_nShapeType == SHP_TYPE_POINTZ || m_nShapeType == SHP_TYPE_POINTM)
			{
				POINT stPoint;
				m_vtPoint.push_back(stPoint);
			}
			else
				m_vtPolyLine.push_back(vector<POINT>());
		}

		nPos += nContentBytes;
		++dwRec;
	}

	m_dwRecordCount = dwRec;
	LOGFMTI("shp load complete!type=[%d] record=[%u] file=[%s]", 
		m_nShapeType, m_dwRecordCount, strShpFile.c_str());
	return true;
}

/**
 * @brief .dbf 속성 파일 파싱 (dBASE III)
 * @param[in] strDbfFile .dbf 파일 경로
 * @return true(성공), false(실패)
*/
bool CShapeFile::LoadDbf(const string& strDbfFile)
{
	vector<char> vtBuf;
	// .dbf 파일 전체를 메모리로 읽기 (2026-07-08 최정우 주석 추가)
	if (!ReadWholeFile(strDbfFile, vtBuf))
		return false;

	if (vtBuf.size() < 32)
	{
		LOGFMTE("dbf header too small!file=[%s]", strDbfFile.c_str());
		return false;
	}

	const unsigned char *pBase = reinterpret_cast<const unsigned char *>(vtBuf.data());

	m_dwDbfRecordCount = ReadLE32U(pBase + 4);
	uint16 wHeaderSize = ReadLE16U(pBase + 8);
	m_wDbfRecordSize = ReadLE16U(pBase + 10);

	if ((wHeaderSize == 0) || (m_wDbfRecordSize == 0))
	{
		LOGFMTE("dbf header/record size invalid!file=[%s]", strDbfFile.c_str());
		return false;
	}

	uint32 dwFieldOffset = 1;
	size_t nPos = 32;
	while ((nPos + 32) <= wHeaderSize && nPos < vtBuf.size())
	{
		if (pBase[nPos] == 0x0D)
			break;

		DBF_FIELD stField;
		char szName[12];
		memset(szName, 0, sizeof(szName));
		memcpy(szName, pBase + nPos, 11);
		szName[11] = '\0';

		stField.strName = ToUpper(TrimString(szName, 11));
		stField.cType = static_cast<char>(pBase[nPos + 11]);
		stField.nLength = static_cast<uint8>(pBase[nPos + 16]);
		stField.dwOffset = dwFieldOffset;

		dwFieldOffset += stField.nLength;

		m_mapFieldIdx[stField.strName] = static_cast<int>(m_vtFields.size());
		m_vtFields.push_back(stField);

		nPos += 32;
	}

	size_t nRecordsBytes = static_cast<size_t>(m_dwDbfRecordCount) * static_cast<size_t>(m_wDbfRecordSize);
	if (wHeaderSize + nRecordsBytes > vtBuf.size())
	{
		nRecordsBytes = (vtBuf.size() > wHeaderSize) ? (vtBuf.size() - wHeaderSize) : 0;
		m_dwDbfRecordCount = static_cast<uint32>(nRecordsBytes / m_wDbfRecordSize);
		LOGFMTW("dbf truncated! adjusted record count=[%u] file=[%s]", m_dwDbfRecordCount, strDbfFile.c_str());
	}

	m_vtDbfRecords.assign(vtBuf.begin() + wHeaderSize, vtBuf.begin() + wHeaderSize + nRecordsBytes);

	LOGFMTI("dbf load complete!field=[%zu] record=[%u] file=[%s]", 
		m_vtFields.size(), m_dwDbfRecordCount, strDbfFile.c_str());
	return true;
}

/**
 * @brief Point 형상 구하기
 * @return true(성공), false(실패)
*/
bool CShapeFile::GetPoint(uint32 dwIndex, POINT& stPoint) const
{
	if (dwIndex >= m_vtPoint.size())
		return false;

	stPoint = m_vtPoint[dwIndex];
	return true;
}

/**
 * @brief PolyLine 형상 구하기
 * @return true(성공), false(실패)
*/
bool CShapeFile::GetPolyLine(uint32 dwIndex, vector<POINT>& vtPoints) const
{
	if (dwIndex >= m_vtPolyLine.size())
		return false;

	vtPoints = m_vtPolyLine[dwIndex];
	return true;
}

/**
 * @brief 필드 존재 여부
 * @return true(성공), false(실패)
*/
bool CShapeFile::HasField(const string& strField) const
{
	// 필드명 대문자 변환 후 존재 여부 조회 (2026-07-08 최정우 주석 추가)
	return (m_mapFieldIdx.find(ToUpper(strField)) != m_mapFieldIdx.end());
}

/**
 * @brief 레코드 필드 원본 값(trim) 구하기
 * @return true(성공), false(실패)
*/
bool CShapeFile::GetRawField(uint32 dwIndex, const string& strField, string& strValue) const
{
	strValue.clear();

	if (dwIndex >= m_dwDbfRecordCount)
		return false;

	// 필드명 대문자 변환 후 인덱스 조회 (2026-07-08 최정우 주석 추가)
	map<string, int>::const_iterator it = m_mapFieldIdx.find(ToUpper(strField));
	if (it == m_mapFieldIdx.end())
		return false;

	const DBF_FIELD& stField = m_vtFields[it->second];
	size_t nBase = static_cast<size_t>(dwIndex) * static_cast<size_t>(m_wDbfRecordSize);
	size_t nFieldPos = nBase + stField.dwOffset;
	if (nFieldPos + stField.nLength > m_vtDbfRecords.size())
		return false;

	// DBF 필드 원본값 trim 처리 (2026-07-08 최정우 주석 추가)
	strValue = TrimString(&m_vtDbfRecords[nFieldPos], stField.nLength);
	return true;
}

/**
 * @brief 필드 문자열 값
 * @return true(성공), false(실패)
*/
bool CShapeFile::GetString(uint32 dwIndex, const string& strField, string& strValue) const
{
	// DBF 필드 원본 문자열 조회 (2026-07-08 최정우 주석 추가)
	return GetRawField(dwIndex, strField, strValue);
}

/**
 * @brief 필드 정수 값 (없으면 0)
 * @return 정수 값
*/
long CShapeFile::GetLong(uint32 dwIndex, const string& strField) const
{
	string strValue;
	// DBF 필드 원본 문자열 조회 (2026-07-08 최정우 주석 추가)
	if (!GetRawField(dwIndex, strField, strValue) || strValue.empty())
		return 0;
	// 문자열 → 정수 변환 (2026-07-08 최정우 주석 추가)
	return atol(strValue.c_str());
}

/**
 * @brief 필드 실수 값 (없으면 0.0)
 * @return 실수 값
*/
double CShapeFile::GetDouble(uint32 dwIndex, const string& strField) const
{
	string strValue;
	// DBF 필드 원본 문자열 조회 (2026-07-08 최정우 주석 추가)
	if (!GetRawField(dwIndex, strField, strValue) || strValue.empty())
		return 0.0;
	// 문자열 → 실수 변환 (2026-07-08 최정우 주석 추가)
	return atof(strValue.c_str());
}
