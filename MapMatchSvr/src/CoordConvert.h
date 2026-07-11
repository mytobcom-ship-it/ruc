/**
 * @file CoordConvert.h
 * @brief 측지계 변환 클래스 헤더 파일
*/
#ifndef __COORDCONVERT_H__
#define __COORDCONVERT_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include "TypeDefine.h"

#define R_MAJOR						6378137.0
#define R_MINOR						6356752.3142

/**
 * @brief EPSG3857 좌표계 최대, 최소 값
*/
#define EPSG3857_LON_MIN			-21000000							// EPSG3857 x 최소 값
#define EPSG3857_LON_MAX			21000000							// EPSG3857 x 최대 값
#define EPSG3857_LAT_MIN			-310000000							// EPSG3857 y 최소 값
#define EPSG3857_LAT_MAX			200000000							// EPSG3857 y 최대 값

/**
 * @brief WGS84GEO 좌표계 최대, 최소 값
*/
#define WGS84GEO_LON_MIN			124									// WGS84GEO x 최소 값 (경도, 도)
#define WGS84GEO_LON_MAX			133									// WGS84GEO x 최대 값
#define WGS84GEO_LAT_MIN			32									// WGS84GEO y 최소 값 (위도, 도)
#define WGS84GEO_LAT_MAX			39									// WGS84GEO y 최대 값

/**
 * @brief KATECH 좌표계 최대, 최소 값
*/
#define KATECH_LON_MIN				900000								// KATECH x 최소 값
#define KATECH_LON_MAX				8000000								// KATECH x 최대 값
#define KATECH_LAT_MIN				400000								// KATECH y 최소 값
#define KATECH_LAT_MAX				700000								// KATECH y 최대 값

/**
 * @brief BESSELGEO 좌표계 최대, 최소 값
*/
#define BESSELGEO_LON_MIN			1240								// BESSELGEO x 최소 값
#define BESSELGEO_LON_MAX			1330								// BESSELGEO x 최대 값
#define BESSELGEO_LAT_MIN			320									// BESSELGEO y 최소 값
#define BESSELGEO_LAT_MAX			390									// BESSELGEO y 최대 값

/**
 * @brief BESSELTM 좌표계 최대, 최소 값
*/
#define BESSELTM_LON_MIN			-900000								// BESSELTM x 최소 값
#define BESSELTM_LON_MAX			8000000								// BESSELTM x 최대 값
#define BESSELTM_LAT_MIN			-1700000							// BESSELTM y 최소 값
#define BESSELTM_LAT_MAX			7000000								// BESSELTM y 최대 값

/**
 * @brief GRS80GEO 좌표계 최대, 최소 값
*/
#define GRS80GEO_LON_MIN			1240								// GRS80GEO x 최소 값
#define GRS80GEO_LON_MAX			1330								// GRS80GEO x 최대 값
#define GRS80GEO_LAT_MIN			320									// GRS80GEO y 최소 값
#define GRS80GEO_LAT_MAX			390									// GRS80GEO y 최대 값

/**
 * @brief GRS80TM 좌표계 최대, 최소 값
*/
#define GRS80TM_LON_MIN				-900000								// GRS80TM x 최소 값
#define GRS80TM_LON_MAX				8000000								// GRS80TM x 최대 값
#define GRS80TM_LAT_MIN				-1700000							// GRS80TM y 최소 값
#define GRS80TM_LAT_MAX				7000000								// GRS80TM y 최대 값

/**
 * @brief GRS80UTMK 좌표계 최대, 최소 값
*/
#define GRS80UTMK_LON_MIN			6900000								// GRS80UTMK x 최소값
#define GRS80UTMK_LON_MAX			14000000							// GRS80UTMK x
#define GRS80UTMK_LAT_MIN			14400000							// GRS80UTMK y 최소값
#define GRS80UTMK_LAT_MAX			21000000							// GRS80UTMK y

/**
 * @class CCoordConvert
 * @brief 좌표 변한 클래스
*/
class CCoordConvert
{
public:
	CCoordConvert(){};
	~CCoordConvert(){};

	void WGS84ToSearchCoord(double inLat, double inLon, uint32 *outLat, uint32 *outLon);

	// from BESSELGEO
	void BESSELGEOToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELGEOToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELGEOToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELGEOToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELGEOToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELGEOToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELGEOToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from GRS80GEO
	void GRS80GEOToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80GEOToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80GEOToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80GEOToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80GEOToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80GEOToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80GEOToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from WGS84GEO
	void WGS84GEOToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void WGS84GEOToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void WGS84GEOToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void WGS84GEOToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
	void WGS84GEOToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void WGS84GEOToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void WGS84GEOToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from KATECH
	void KATECHToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void KATECHToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void KATECHToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
	void KATECHToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void KATECHToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void KATECHToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void KATECHToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from BESSELTM
	void BESSELTMToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELTMToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELTMToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELTMToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELTMToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELTMToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void BESSELTMToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from GRS80TM
	void GRS80TMToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TMToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TMToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TMToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TMToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TMToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TMToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from GRS80TM2010 (Korea 2000 / Central Belt 2010, EPSG:5186, FN=600000)
	void GRS80TM2010ToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80TM2010ToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);

	// from EPSG3857
	void EPSG3857ToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void EPSG3857ToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void EPSG3857ToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void EPSG3857ToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void EPSG3857ToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void EPSG3857ToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
	void EPSG3857ToGRS80UTMK(double inLat, double inLon, double *outLat, double *outLon);

	// from GRS80UTMK
	void GRS80UTMKToBESSELTM(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80UTMKToGRS80GEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80UTMKToKATECH(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80UTMKToWGS84GEO(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80UTMKToGRS80TM(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80UTMKToEPSG3857(double inLat, double inLon, double *outLat, double *outLon);
	void GRS80UTMKToBESSELGEO(double inLat, double inLon, double *outLat, double *outLon);
};

#endif //__COORDCONVERT_H__
