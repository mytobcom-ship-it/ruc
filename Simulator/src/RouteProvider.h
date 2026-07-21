/**
 * @file RouteProvider.h
 * @brief 도로망(network.moct_link)에서 주행 경로용 링크를 조회
*/
#ifndef __ROUTE_PROVIDER_H__
#define __ROUTE_PROVIDER_H__

#include <string>
#include "SimTypes.h"
#include "Config.h"
#include "PostgrePool.h"
#include "SQLAccessor.h"

using namespace std;

/**
 * @class CRouteProvider
 * @brief 도로 링크 형상 제공 (시작 링크 랜덤 선택 + 연결 링크 추적)
*/
class CRouteProvider
{
public:
	CRouteProvider();
	~CRouteProvider();

	bool Initialize(CPostgrePool *pcPool, CSQLAccessor *pcSQL, const SIM_CONFIG& stConfig);

	// bounding box 내 임의의 시작 링크 1개
	bool SeedLink(LINK_GEOM& stOut);
	// 노드(strFromNode)에서 출발하는 다음 링크 1개 (strExcludeLink 제외, strExcludeToNode
	//   로 향하는 역방향(맞은편) 링크도 제외 — 방금 지나온 도로를 곧바로 되돌아가는 것 방지) (2026-07-22 최정우 수정)
	bool NextLink(const string& strFromNode, const string& strExcludeLink,
		const string& strExcludeToNode, LINK_GEOM& stOut);

private:
	int  DetectSrid();
	bool RunLinkQuery(const string& strSQL, const char * const *paszParams,
		int nParams, LINK_GEOM& stOut);

private:
	CPostgrePool	*m_pcPool;
	CSQLAccessor	*m_pcSQL;
	SIM_CONFIG		m_stConfig;
	int				m_nSrid;			// network.moct_link geom 의 실제 SRID
};

#endif // __ROUTE_PROVIDER_H__
