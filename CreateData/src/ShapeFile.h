/**
 * @file ShapeFile.h
 * @brief ESRI Shapefile(.shp) + 속성(.dbf) 자체 리더 클래스 헤더 파일
 * @remark
 *   외부 라이브러리(GDAL/shapelib) 없이 .shp(형상) + .dbf(속성)를 직접 파싱한다.
 *   - 지원 형상 : Point(1), PolyLine(3), Polygon(5) 및 Z/M 변형(11/13/15/21/23/25) 의 X,Y
 *   - 좌표계 변환은 하지 않음(원본 좌표 그대로 반환). 변환은 상위 로더에서 수행.
 *   - 좌표/속성은 레코드 순서(index, 0-base)로 정렬되어 1:1 대응.
*/
#ifndef __SHAPEFILE_H__
#define __SHAPEFILE_H__

#include <string>
#include <vector>
#include <map>
#include "TypeDefine.h"
#include "DataFormat.h"

using namespace std;

#define SHP_TYPE_NULL				0
#define SHP_TYPE_POINT				1
#define SHP_TYPE_POLYLINE			3
#define SHP_TYPE_POLYGON			5
#define SHP_TYPE_POINTZ				11
#define SHP_TYPE_POLYLINEZ			13
#define SHP_TYPE_POLYGONZ			15
#define SHP_TYPE_POINTM				21
#define SHP_TYPE_POLYLINEM			23
#define SHP_TYPE_POLYGONM			25

/**
 * @struct sDbfField
 * @brief DBF 필드 정의
*/
typedef struct sDbfField
{
	string		strName;
	char		cType;
	uint32		dwOffset;
	uint8		nLength;

	sDbfField() : cType(' '), dwOffset(0), nLength(0) {}
} DBF_FIELD, *PDBF_FIELD;

/**
 * @class CShapeFile
 * @brief Shapefile(.shp/.dbf) 리더
*/
class CShapeFile
{
public:
	CShapeFile();
	virtual ~CShapeFile();

	bool Open(const string& strShpFile);
	void Close();

	inline int GetShapeType() const { return m_nShapeType; }
	inline uint32 GetRecordCount() const { return m_dwRecordCount; }

	bool GetPoint(uint32 dwIndex, POINT& stPoint) const;
	bool GetPolyLine(uint32 dwIndex, vector<POINT>& vtPoints) const;

	bool HasField(const string& strField) const;
	bool GetString(uint32 dwIndex, const string& strField, string& strValue) const;
	long GetLong(uint32 dwIndex, const string& strField) const;
	double GetDouble(uint32 dwIndex, const string& strField) const;

private:
	bool LoadShp(const string& strShpFile);
	bool LoadDbf(const string& strDbfFile);
	bool GetRawField(uint32 dwIndex, const string& strField, string& strValue) const;

private:
	int						m_nShapeType;
	uint32					m_dwRecordCount;
	vector<POINT>			m_vtPoint;
	vector< vector<POINT> >	m_vtPolyLine;

	uint32					m_dwDbfRecordCount;
	uint16					m_wDbfRecordSize;
	vector<DBF_FIELD>		m_vtFields;
	map<string, int>		m_mapFieldIdx;
	vector<char>			m_vtDbfRecords;
};

#endif //__SHAPEFILE_H__
