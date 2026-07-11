/**
 * @file Coordinate.cpp
 * @brief 좌표 변환 클래스 소스 파일
*/
#include "Coordinate.h"

/**
 * @brief 생성자
*/
CCoordinate::CCoordinate()
{
}

/**
 * @brief 소멸자
*/
CCoordinate::~CCoordinate()
{
}

/**
 * @brief WGS84GEO 좌표로 변환
 * @param[in] eCoordType 측지계 코드
 * @param[in,out] dfX X 좌표
 * @param[in,out] dfY Y 좌표
 * @return true(성공), false(실패)
*/
bool CCoordinate::ConvertCoordinateToWGS84GEO(enum eCoordinateType &eCoordType, double *dfX, double *dfY)
{
	switch (eCoordType)
	{
	case EPSG3857:
		m_cCoordConvert.EPSG3857ToWGS84GEO(*dfY, *dfX, dfY, dfX);
		return true;
	case WGS84GEO:
		return true;
	case KATECH:
		m_cCoordConvert.KATECHToWGS84GEO(*dfY, *dfX, dfY, dfX);
		return true;
	case BESSELGEO:
		m_cCoordConvert.BESSELGEOToWGS84GEO(*dfY, *dfX, dfY, dfX);
		return true;
	}

	return false;
}

/**
 * @brief WGS84GEO 좌표 유효성 검사
 * @param[in] dfX X 좌표
 * @param[in] dfY Y 좌표
 * @return true(성공), false(실패)
*/
bool CCoordinate::IsValidWGS84GEO(double dfX, double dfY)
{
	if ((dfX < WGS84GEO_LON_MIN || dfX > WGS84GEO_LON_MAX) || 
		(dfY < WGS84GEO_LAT_MIN || dfY > WGS84GEO_LAT_MAX))
		return false;

	return true;
}
