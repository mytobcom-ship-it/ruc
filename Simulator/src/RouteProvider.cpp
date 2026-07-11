/**
 * @file RouteProvider.cpp
 * @brief 도로망 링크 조회 구현
*/
#include "RouteProvider.h"
#include "GeoUtil.h"
#include "log4z.h"
#include <cstdio>
#include <cstring>
#include <libpq-fe.h>

using namespace zsummer::log4z;

CRouteProvider::CRouteProvider()
	: m_pcPool(nullptr), m_pcSQL(nullptr), m_nSrid(4326)
{
}

CRouteProvider::~CRouteProvider()
{
}

/**
 * @brief 초기화 - 풀/SQL 보관 후 도로망 SRID 자동 감지
*/
bool CRouteProvider::Initialize(CPostgrePool *pcPool, CSQLAccessor *pcSQL, const SIM_CONFIG& stConfig)
{
	m_pcPool = pcPool;
	m_pcSQL = pcSQL;
	m_stConfig = stConfig;

	// 도로망 저장 SRID 자동 감지 (2026-07-08 최정우 주석 추가)
	m_nSrid = DetectSrid();
	if (m_nSrid <= 0)
	{
		LOGFMTE("road network SRID detect fail!");
		return false;
	}
	LOGFMTI("road network SRID=[%d]", m_nSrid);
	return true;
}

/**
 * @brief network.moct_link 의 저장 SRID 1건 조회
 *        (Python importer=4326, ogr2ogr=5179 양쪽 모두 대응)
*/
int CRouteProvider::DetectSrid()
{
	// DB 커넥션 풀에서 연결 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = m_pcPool->getConnection();
	if (!pcConn) return -1;

	// SQL 파일에서 SRID 감지 쿼리 조회 (2026-07-08 최정우 주석 추가)
	string strSQL = m_pcSQL->GetSQL("srid_detect");
	if (strSQL.empty())
		strSQL = "SELECT ST_SRID(geom) FROM network.moct_link WHERE geom IS NOT NULL LIMIT 1";

	int nSrid = -1;
	// SRID 감지 SQL 실행 (2026-07-08 최정우 주석 추가)
	PGresult *res = PQexec(pcConn, strSQL.c_str());
	if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
		nSrid = atoi(PQgetvalue(res, 0, 0));
	else
		LOGFMTE("srid_detect query fail!error=[%s]", PQerrorMessage(pcConn));

	if (res) PQclear(res);
	// DB 커넥션 풀에 연결 반환 (2026-07-08 최정우 주석 추가)
	m_pcPool->releaseConnection(pcConn);
	return nSrid;
}

/**
 * @brief bounding box 내 임의 시작 링크
*/
bool CRouteProvider::SeedLink(LINK_GEOM& stOut)
{
	// SQL 파일에서 시작 링크 시드 쿼리 조회 (2026-07-08 최정우 주석 추가)
	string strSQL = m_pcSQL->GetSQL("moct_link_seed");
	if (strSQL.empty()) { LOGFMTE("moct_link_seed sql empty!"); return false; }

	char szMinLon[32], szMinLat[32], szMaxLon[32], szMaxLat[32], szSrid[16], szLimit[16];
	snprintf(szMinLon, sizeof(szMinLon), "%.6f", m_stConfig.dfMinLon);
	snprintf(szMinLat, sizeof(szMinLat), "%.6f", m_stConfig.dfMinLat);
	snprintf(szMaxLon, sizeof(szMaxLon), "%.6f", m_stConfig.dfMaxLon);
	snprintf(szMaxLat, sizeof(szMaxLat), "%.6f", m_stConfig.dfMaxLat);
	snprintf(szSrid, sizeof(szSrid), "%d", m_nSrid);
	snprintf(szLimit, sizeof(szLimit), "%d", m_stConfig.nSeedCandidates > 0 ? 1 : 1);

	const char *aszParams[6] = { szMinLon, szMinLat, szMaxLon, szMaxLat, szSrid, szLimit };
	// bbox 내 시작 링크 조회 SQL 실행 (2026-07-08 최정우 주석 추가)
	return RunLinkQuery(strSQL, aszParams, 6, stOut);
}

/**
 * @brief 다음 연결 링크
*/
bool CRouteProvider::NextLink(const string& strFromNode, const string& strExcludeLink, LINK_GEOM& stOut)
{
	if (strFromNode.empty()) return false;

	// SQL 파일에서 다음 연결 링크 쿼리 조회 (2026-07-08 최정우 주석 추가)
	string strSQL = m_pcSQL->GetSQL("moct_link_next");
	if (strSQL.empty()) { LOGFMTE("moct_link_next sql empty!"); return false; }

	const char *aszParams[2] = { strFromNode.c_str(), strExcludeLink.c_str() };
	// 다음 연결 링크 조회 SQL 실행 (2026-07-08 최정우 주석 추가)
	return RunLinkQuery(strSQL, aszParams, 2, stOut);
}

/**
 * @brief 링크 조회 공통 실행 (결과: link_id, t_node, max_spd, wkt)
*/
bool CRouteProvider::RunLinkQuery(const string& strSQL, const char * const *paszParams,
	int nParams, LINK_GEOM& stOut)
{
	// DB 커넥션 풀에서 연결 획득 (2026-07-08 최정우 주석 추가)
	PGconn *pcConn = m_pcPool->getConnection();
	if (!pcConn) { LOGFMTE("getConnection fail (link query)"); return false; }

	bool bResult = false;
	// 파라미터 바인딩 링크 조회 SQL 실행 (2026-07-08 최정우 주석 추가)
	PGresult *res = PQexecParams(pcConn, strSQL.c_str(), nParams, nullptr,
		paszParams, nullptr, nullptr, 0);

	if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
	{
		stOut.strLinkID = PQgetvalue(res, 0, 0);
		stOut.strToNode = PQgetvalue(res, 0, 1);
		stOut.nMaxSpd = atoi(PQgetvalue(res, 0, 2));
		string strWkt = PQgetvalue(res, 0, 3);

		// WKT LineString → 좌표 배열 파싱 (2026-07-08 최정우 주석 추가)
		if (CGeoUtil::ParseLineString(strWkt, stOut.vtPoints))
			bResult = true;
	}
	else if (res && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		LOGFMTE("link query fail!error=[%s]", PQerrorMessage(pcConn));
	}

	if (res) PQclear(res);
	// DB 커넥션 풀에 연결 반환 (2026-07-08 최정우 주석 추가)
	m_pcPool->releaseConnection(pcConn);
	return bResult;
}
