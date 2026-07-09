/**
 * @file Coordinate.h
 * @brief 좌표 변환 클래스 헤더 파일
*/
#ifndef __COORDINATE_H__
#define __COORDINATE_H__

#include <stdio.h>
#include "TypeDefine.h"
#include "DataDefine.h"
#include "CoordConvert.h"

/**
 * @class CCoordinate
 * @brief 좌표 변환 클래스
*/
class CCoordinate
{
public:
	CCoordinate();
	virtual ~CCoordinate();

	bool ConvertCoordinateToWGS84GEO(enum eCoordinateType &eCoordType, double *dfX, double *dfY);
	bool IsValidWGS84GEO(double dfX, double dfY);

private:
	CCoordConvert						m_cCoordConvert;
};

#endif //__COORDINATE_H__
