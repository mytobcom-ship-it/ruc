/**
 * @file GISUtil.cpp
 * @brief 맵매칭 GIS 유틸리티 클래스 소스 파일 (WGS84 경위도 용)
*/
#include "GISUtil.h"
#include "MessageType.h"

namespace {

double HaversineMetersDeg(const double dfLon1, const double dfLat1,
		const double dfLon2, const double dfLat2)
{
	const double dfR = 6378137.0;
	const double dfLat1Rad = RAD(dfLat1);
	const double dfLat2Rad = RAD(dfLat2);
	const double dfDLat = RAD(dfLat2 - dfLat1);
	const double dfDLon = RAD(dfLon2 - dfLon1);

	double dfA = sin(dfDLat / 2.0) * sin(dfDLat / 2.0)
		+ cos(dfLat1Rad) * cos(dfLat2Rad) * sin(dfDLon / 2.0) * sin(dfDLon / 2.0);
	if (dfA > 1.0) dfA = 1.0;
	return 2.0 * dfR * asin(sqrt(dfA));
}

} // namespace

/**
 * @brief 생성자
*/
CGISUtil::CGISUtil()
{
}

/**
 * @brief 소멸자
*/
CGISUtil::~CGISUtil()
{
}

/**
 * @brief GRID ID 계산
 * @param[in] dfX X 좌표
 * @param[in] dfY Y 좌표
 * @return GRID ID
*/
const uint32 CGISUtil::GetGridID(double& dfX, double& dfY)
{
	uint32 dwRowNo = floor((dfY - WGS84GEO_LAT_MIN) / GRID_CELL_SIZE);
	uint32 dwColNo = floor((dfX - WGS84GEO_LON_MIN) / GRID_CELL_SIZE);

	return (dwRowNo * X_GRID_COUNT) + dwColNo;
}

/**
 * @brief GRID X 좌표 번호 계산
 * @param[in] dfX X 좌표
 * @return GRID X 좌표 번호
*/
const sint32 CGISUtil::GetGridColNo(double& dfX)
{
	sint32 nColNo = floor((dfX - WGS84GEO_LON_MIN) / GRID_CELL_SIZE);
	if ((nColNo < 0) || (nColNo >= X_GRID_COUNT))
		return INVALID_GRID_COL_NO;

	return nColNo;
}

/**
 * @brief GRID Y 좌표 번호 계산
 * @param[in] dfY Y 좌표
 * @return GRID Y 좌표 번호
*/
const sint32 CGISUtil::GetGridRowNo(double& dfY)
{
	sint32 nRowNo = floor((dfY - WGS84GEO_LAT_MIN) / GRID_CELL_SIZE);
	if ((nRowNo < 0) || (nRowNo >= Y_GRID_COUNT))
		return INVALID_GRID_ROW_NO;

	return nRowNo;
}

/**
 * @brief 세그먼트 GRID 교차 확인
 * @param[in] stPoint1 세그먼트 시작 X,Y 좌표
 * @param[in] stPoint2 세그먼트 종료 X,Y 좌표
 * @param[in] dwGridColNo X 좌표 GRID 번호
 * @param[in] dwGridRowNo Y 좌표 GRID 번호
 * @return true(성공), false(실패)
*/
bool CGISUtil::IsCrossSgmt2Grid(POINT& stPoint1, POINT& stPoint2, 
		uint32& dwGridColNo, uint32& dwGridRowNo)
{
	// GRID 번호로 X,Y 최소, 최대 좌표 구하기
	bool bRet = false;
	double dfGridXMin = WGS84GEO_LON_MIN + dwGridColNo * GRID_CELL_SIZE;
	double dfGridXMax = WGS84GEO_LON_MIN + (dwGridColNo + 1) * GRID_CELL_SIZE;
	double dfGridYMin = WGS84GEO_LAT_MIN + dwGridRowNo * GRID_CELL_SIZE;
	double dfGridYMax = WGS84GEO_LAT_MIN + (dwGridRowNo + 1) * GRID_CELL_SIZE;

	// GRID 경계(4변)와 세그먼트 교차 여부 판정 (2026-07-08 최정우 주석 추가)
	if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMin, dfGridYMin, dfGridXMin, dfGridYMax))
		bRet = true;
	else if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMin, dfGridYMax, dfGridXMax, dfGridYMax))
		bRet = true;
	else if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMax, dfGridYMax, dfGridXMax, dfGridYMin))
		bRet = true;
	else if (IsCrossSgmt2Sgmt(stPoint1, stPoint2, dfGridXMax, dfGridYMin, dfGridXMin, dfGridYMin))
		bRet = true;

	return bRet;
}

/**
 * @brief 세그먼트와 세그먼트 교차 확인
 * @param[in] stPoint1 세그먼트 시작 X,Y 좌표
 * @param[in] stPoint2 세그먼트 종료 X,Y 좌표
 * @param[in] dfXMin 세그먼트 시작 X 좌표 (GRID)
 * @param[in] dfYMin 세그먼트 시작 Y 좌표 (GRID)
 * @param[in] dfXMax 세그먼트 종료 X 좌표 (GRID)
 * @param[in] dfYMax 세그먼트 종료 Y 좌표 (GRID)
 * @return true(성공), false(실패)
*/
bool CGISUtil::IsCrossSgmt2Sgmt(POINT& stPoint1, POINT& stPoint2, 
		double& dfXMin, double& dfYMin, double& dfXMax, double& dfYMax)
{
	bool bRet = false;
	double dfSign = (dfYMax - dfYMin) * (stPoint2.dfX - stPoint1.dfX) - (dfXMax - dfXMin) * (stPoint2.dfY - stPoint1.dfY);
	if (dfSign != 0.0)					// 교차하지 않음
	{
		double dfSign1 = ((dfXMax - dfXMin) * (stPoint1.dfY - dfYMin) - (dfYMax - dfYMin) * (stPoint1.dfX - dfXMin)) / dfSign;
		double dfSign2 = ((stPoint2.dfX - stPoint1.dfX) * (stPoint1.dfY - dfYMin) - (stPoint2.dfY - stPoint1.dfY) * (stPoint1.dfX -dfXMin)) / dfSign;

		if ((dfSign1 >= 0.0 && dfSign1 <= 1.0) && (dfSign2 >= 0.0 && dfSign2 <= 1.0))		// 교차함
			bRet = true;
	}

	return bRet;
}

/**
 * @brief 세그먼트 거리 (0.01 sec)
 * @param[in] stPoint1 세그먼트 시작 좌표
 * @param[in] stPoint2 세그먼트 종료 좌표
 * @return 세그먼트 길이
*/
const uint16 CGISUtil::GetSgmtLength(const POINT& stPoint1, const POINT& stPoint2)
{
	uint16 wLenSgmt = 0;
	wLenSgmt = static_cast<uint16>(round(sqrt(pow((stPoint1.dfX - stPoint2.dfX) * 360000, 2.0) + pow((stPoint1.dfY - stPoint2.dfY) * 360000, 2.0))));
	if (wLenSgmt <= 0) wLenSgmt = 1;

	return wLenSgmt;
}

/**
 * @brief GRID 경계·모서리까지의 거리(m) — nRadius(미터)와 단위 정합 (#C-1)
 * @param[in] dwGridID GRID ID
 * @param[in] dfX X 좌표 (도, WGS84)
 * @param[in] dfY Y 좌표 (도, WGS84)
 * @param[out] stGridBorderDist GRID 경계 거리(모서리 거리 포함, m)
 * @return 8방향 중 최소 거리(m)
*/
const double CGISUtil::GridBorderDistance(const uint32& dwGridID, const double& dfX, 
		const double& dfY, GRID_BORDER_DIST& stGridBorderDist)
{
	uint32 dwRowNo = floor(dwGridID / X_GRID_COUNT);
	uint32 dwColNo = floor(dwGridID - dwRowNo * X_GRID_COUNT);

	set<double> setDistance;
	set<double>::iterator it;
	setDistance.clear();

	double dfXMin = WGS84GEO_LON_MIN + dwColNo * GRID_CELL_SIZE;
	double dfYMin = WGS84GEO_LAT_MIN + dwRowNo * GRID_CELL_SIZE;
	double dfXMax = WGS84GEO_LON_MIN + (dwColNo + 1) * GRID_CELL_SIZE;
	double dfYMax = WGS84GEO_LAT_MIN + (dwRowNo + 1) * GRID_CELL_SIZE;

	stGridBorderDist.dfLeftDist = HaversineMetersDeg(dfX, dfY, dfXMin, dfY);
	setDistance.insert(stGridBorderDist.dfLeftDist);

	stGridBorderDist.dfBottomDist = HaversineMetersDeg(dfX, dfY, dfX, dfYMin);
	setDistance.insert(stGridBorderDist.dfBottomDist);

	stGridBorderDist.dfRightDist = HaversineMetersDeg(dfX, dfY, dfXMax, dfY);
	setDistance.insert(stGridBorderDist.dfRightDist);

	stGridBorderDist.dfTopDist = HaversineMetersDeg(dfX, dfY, dfX, dfYMax);
	setDistance.insert(stGridBorderDist.dfTopDist);

	stGridBorderDist.dfLeftTopDist = HaversineMetersDeg(dfX, dfY, dfXMin, dfYMax);
	setDistance.insert(stGridBorderDist.dfLeftTopDist);

	stGridBorderDist.dfLeftBottomDist = HaversineMetersDeg(dfX, dfY, dfXMin, dfYMin);
	setDistance.insert(stGridBorderDist.dfLeftBottomDist);

	stGridBorderDist.dfRightTopDist = HaversineMetersDeg(dfX, dfY, dfXMax, dfYMax);
	setDistance.insert(stGridBorderDist.dfRightTopDist);

	stGridBorderDist.dfRightBottomDist = HaversineMetersDeg(dfX, dfY, dfXMax, dfYMin);
	setDistance.insert(stGridBorderDist.dfRightBottomDist);

	it = setDistance.begin();
	return *it;
}

/**
 * @brief GRID 9등분 인덱스 위치 및 경계 좌표
 * @param[in] dwGridID GRID ID
 * @param[in] dfX X 좌표
 * @param[in] dfY Y 좌표
 * @return 인덱스 (0 ~ 8)
*/
const uint8 CGISUtil::GridSplitIndex(const uint32& dwGridID, const double& dfX, const double& dfY)
{
	uint32 dwRowNo = floor(dwGridID / X_GRID_COUNT);
	uint32 dwColNo = floor(dwGridID - dwRowNo * X_GRID_COUNT);

	double dfXMin = WGS84GEO_LON_MIN + dwColNo * GRID_CELL_SIZE;
	uint8 nXIndex = static_cast<uint8>((dfX - dfXMin) / (GRID_CELL_SIZE / 3));

	double dfYMin = WGS84GEO_LAT_MIN + dwRowNo * GRID_CELL_SIZE;
	uint8 nYIndex = static_cast<uint8>((dfY - dfYMin) / (GRID_CELL_SIZE / 3));

	return nYIndex * 3 + nXIndex;
}

/**
 * @brief 인접 GRID ID 계산 (중심 그리드 제외, 최대 8방향)
 * @param[in] dwGridID GRID ID
 * @param[in] stSgmtMatchInput 맵매칭 입력 정보
 * @param[in,out] vtNearGridIDList 인접 GRID ID 목록
 * @return void
 * @remark 2026-07-08 최정우 수정
 *   유효 인덱스: col 0..X_GRID_COUNT-1, 행 0..Y_GRID_COUNT-1
 *   경계 검사 (off-by-one 방지):
 *     · 서/남서/남: col>0 또는 행>0
 *     · 동/북/대각: col+1<X_GRID_COUNT, 행+1<Y_GRID_COUNT
 *   반경(nRadius)이 셀 경계거리보다 크면 해당 방향 이웃만 push.
 *   잘못된 ID(다음 행 col=0 등) 산출 시 엉뚱한 그리드 검색 → INVALID_GRID_ID 유지.
*/
void CGISUtil::GetNearGridID(const uint32& dwGridID, const SGMT_MATCH_INPUT& stSgmtMatchInput, 
		vector<uint32>& vtNearGridIDList)
{
	uint32 dwRowNo = floor(dwGridID / X_GRID_COUNT);
	uint32 dwColNo = floor(dwGridID - dwRowNo * X_GRID_COUNT);

	GRID_BORDER_DIST stGridBorderDist;
	sint32 nGridLeftID = INVALID_GRID_ID;
	sint32 nGridRightID = INVALID_GRID_ID;
	sint32 nGridBottomID = INVALID_GRID_ID;
	sint32 nGridTopID = INVALID_GRID_ID;
	sint32 nGridLeftTopID = INVALID_GRID_ID;
	sint32 nGridLeftBottomID = INVALID_GRID_ID;
	sint32 nGridRightTopID = INVALID_GRID_ID;
	sint32 nGridRightBottomID = INVALID_GRID_ID;

	// GRID 경계 거리
	GridBorderDistance(dwGridID, stSgmtMatchInput.stPoint.dfX, stSgmtMatchInput.stPoint.dfY, stGridBorderDist);

	// 인접 GRID ID (맵 경계 밖은 INVALID_GRID_ID — BeginMapMatch에서 GetGridInfo null 시 건너뜀)
	if (dwColNo > 0)
		nGridLeftID = static_cast<sint32>(dwRowNo * X_GRID_COUNT + dwColNo - 1);

	if (dwColNo + 1 < X_GRID_COUNT)
		nGridRightID = static_cast<sint32>(dwRowNo * X_GRID_COUNT + dwColNo + 1);

	if (dwRowNo > 0)
		nGridBottomID = static_cast<sint32>((dwRowNo - 1) * X_GRID_COUNT + dwColNo);

	if (dwRowNo + 1 < Y_GRID_COUNT)
		nGridTopID = static_cast<sint32>((dwRowNo + 1) * X_GRID_COUNT + dwColNo);

	if (dwColNo > 0 && dwRowNo + 1 < Y_GRID_COUNT)
		nGridLeftTopID = static_cast<sint32>((dwRowNo + 1) * X_GRID_COUNT + dwColNo - 1);

	if (dwColNo > 0 && dwRowNo > 0)
		nGridLeftBottomID = static_cast<sint32>((dwRowNo - 1) * X_GRID_COUNT + dwColNo - 1);

	if (dwColNo + 1 < X_GRID_COUNT && dwRowNo + 1 < Y_GRID_COUNT)
		nGridRightTopID = static_cast<sint32>((dwRowNo + 1) * X_GRID_COUNT + dwColNo + 1);

	if (dwColNo + 1 < X_GRID_COUNT && dwRowNo > 0)
		nGridRightBottomID = static_cast<sint32>((dwRowNo - 1) * X_GRID_COUNT + dwColNo + 1);

	// 조건에 맞는 인접 GRID ID 구하기
	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfLeftDist) && 
		(nGridLeftID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridLeftID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfRightDist) && 
		(nGridRightID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridRightID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfTopDist) && 
		(nGridTopID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridTopID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfBottomDist) && 
		(nGridBottomID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridBottomID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfLeftTopDist) && 
		(nGridLeftTopID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridLeftTopID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfLeftBottomDist) && 
		(nGridLeftBottomID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridLeftBottomID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfRightTopDist) && 
		(nGridRightTopID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridRightTopID));

	if ((stSgmtMatchInput.nRadius > stGridBorderDist.dfRightBottomDist) && 
		(nGridRightBottomID != INVALID_GRID_ID))
		vtNearGridIDList.push_back(static_cast<uint32>(nGridRightBottomID));
}

/**
 * @brief 좌표와 세그먼트 매핑
 * @param[in] stSgmtMatchInput 세그먼트 매칭 정보
 * @param[in] stSgmtInfo 세그먼트 정보
 * @param[out] pstSgmtMatchRes 세그먼트 맵매칭 결과
 * @param[in] bIgnoreRadiusCheck true 이면 nRadius 초과여도 기하 매칭 허용(진단용 최근접) (2026-07-10 최정우 수정)
 * @return true(성공), false(실패)
*/
bool CGISUtil::SgmtMatch(SGMT_MATCH_INPUT& stSgmtMatchInput, SGMT_INFO& stSgmtInfo, SGMT_MATCH_RES *pstSgmtMatchRes,
		bool bIgnoreRadiusCheck)
{
	if ((stSgmtMatchInput.stPoint.dfX == stSgmtInfo.stPoint.dfX) && 
		(stSgmtMatchInput.stPoint.dfY == stSgmtInfo.stPoint.dfY))
	{
		pstSgmtMatchRes->stMatchPoint.dfX = stSgmtInfo.stPoint.dfX;
		pstSgmtMatchRes->stMatchPoint.dfY = stSgmtInfo.stPoint.dfY;
		pstSgmtMatchRes->dfSgmtMatchLen = 0;
		pstSgmtMatchRes->dfIntersectLenSgmt = 0;
		pstSgmtMatchRes->dfCost = 0;			// 정확히 일치 → 최소 비용 (2026-07-08 최정우 추가)
		pstSgmtMatchRes->nDirAngleDiff = 0;
		pstSgmtMatchRes->qwLinkID = stSgmtInfo.qwLinkID;
		return true;
	}

	// 2026-07-08 최정우 주석 처리
#if 0
	if (stSgmtMatchInput.nDirAng != NO_ANGLE)
	{
		// 각도 차이
		if (abs(GetAngleDiff(stSgmtInfo.nDirAng, stSgmtMatchInput.nDirAng)) > 45)
			return false;
	}
#endif

	// 차량 방위각 vs 세그먼트 방위각 차이 (heading 있을 때만).
	// ±45 하드컷 대신 소프트 비용 사용 (2026-07-08 최정우 수정)
	sint16 nHeadingDiff = 0;
	bool bHasHeading = (stSgmtMatchInput.nDirAng != NO_ANGLE);
	if (bHasHeading)
	{
		// 양방향 단일 링크 대응: 세그먼트 F→T 방위각과 그 반대(T→F=+180°) 둘 다 비교해
		//   더 가까운 쪽을 heading 차이로 채택 → 반대방향 주행도 정상 매칭(120° 오배제 방지) (2026-07-16 최정우 수정)
		sint16 nSegDirFwd = stSgmtInfo.nDirAng;
		sint16 nSegDirRev = static_cast<sint16>((stSgmtInfo.nDirAng + 180) % 360);
		sint16 nDiffFwd = GetAngleDiff(nSegDirFwd, stSgmtMatchInput.nDirAng);
		sint16 nDiffRev = GetAngleDiff(nSegDirRev, stSgmtMatchInput.nDirAng);
		nHeadingDiff = (abs(nDiffFwd) <= abs(nDiffRev)) ? nDiffFwd : nDiffRev;
		// 하드 상한: 정·역 어느 쪽으로도 크게 어긋난(≈수직 이상) 후보만 배제. 그 안은 소프트 비용으로 경쟁
		if (abs(nHeadingDiff) > MM_DIR_MAX_DEG)
			return false;
	}

	// 진행 각도
	sint16 nDirAngle = 0;
	if (!GetDirAngle(stSgmtInfo.stPoint, stSgmtMatchInput.stPoint, &nDirAngle))
		return false;

	// 진행 각도 차이
	sint16 nDirAngleDiff = GetAngleDiff(stSgmtInfo.nDirAng, nDirAngle);

	// 요청 좌표(GPS)와 세그먼트 시작점 사이 거리
	double dfVertexDistance = sqrt(pow(stSgmtMatchInput.stPoint.dfX - stSgmtInfo.stPoint.dfX, 2.0) + pow(stSgmtMatchInput.stPoint.dfY - stSgmtInfo.stPoint.dfY, 2.0));

	// ── INTERSECT_LEN (dfIntersectLenSgmt) — GPS 좌표와 세그먼트 교차점 거리(m) (2026-07-11 최정우 수정)
	//   stSgmtMatchInput.stPoint = GPS(요청) 좌표, stIntersect = 세그먼트 위 교차점(수선의 발 또는 끝점 snap)
	//   1) nDirAngle     = 세그먼트 시작점 → GPS 방향각
	//   2) nDirAngleDiff = 세그먼트 방위각 − nDirAngle
	//   3) dfIntersectSgmtDistance = dfVertexDistance × cos(nDirAngleDiff)   // 좌표 정수단위 투영 길이
	//   3b) dfSegLenCoord = stSgmtInfo.dfLen  (wLenSgmt, 좌표정수 단위 — dfVertexDistance 와 동일 단위)
	//   4) stIntersect   = 세그먼트 시작점 + 방위각 × dfIntersectSgmtDistance (범위 밖이면 끝점 snap)
	//   5) dfIntersectLenSgmt = GetDistanceGEO1(GPS, stIntersect)             // 최종 GPS↔교차점 거리(m)
	//   ※ DB INTERSECT_LEN = (int)(dfIntersectLenSgmt + 0.5)
	double dfIntersectLenSgmt = 0;
	double dfIntersectSgmtDistance = dfVertexDistance * cos(RAD(static_cast<double>(nDirAngleDiff)));
	const double dfSegLenCoord = stSgmtInfo.dfLen;
	POINT stIntersect;

	if ((dfIntersectSgmtDistance <= dfSegLenCoord) && (dfIntersectSgmtDistance >= 0))		// 수선의 발이 세그먼트 위
	{
		stIntersect.dfX = stSgmtInfo.stPoint.dfX + fabs(dfIntersectSgmtDistance) * sin(RAD(static_cast<double>(stSgmtInfo.nDirAng)));
		stIntersect.dfY = stSgmtInfo.stPoint.dfY + fabs(dfIntersectSgmtDistance) * cos(RAD(static_cast<double>(stSgmtInfo.nDirAng)));
	}
	else
	{
		// 수선 발이 세그먼트 밖이면 가까운 끝점으로 snap (#C-2)
		if (dfIntersectSgmtDistance > dfSegLenCoord)
		{
			stIntersect.dfX = stSgmtInfo.stPoint.dfX
				+ dfSegLenCoord * sin(RAD(static_cast<double>(stSgmtInfo.nDirAng)));
			stIntersect.dfY = stSgmtInfo.stPoint.dfY
				+ dfSegLenCoord * cos(RAD(static_cast<double>(stSgmtInfo.nDirAng)));
		}
		else
		{
			stIntersect.dfX = stSgmtInfo.stPoint.dfX;
			stIntersect.dfY = stSgmtInfo.stPoint.dfY;
		}
	}

	// GPS 좌표와 세그먼트 교차점(stIntersect) 사이 거리(m) → INTERSECT_LEN
	dfIntersectLenSgmt = GetDistanceGEO1(stSgmtMatchInput.stPoint, stIntersect);

	// 맵매칭 유효거리내에 포함되는지 검사 (진단 최근접 시 생략) (2026-07-10 최정우 수정)
	if (!bIgnoreRadiusCheck && (dfIntersectLenSgmt > stSgmtMatchInput.nRadius))
		return false;

	// 세그먼트 시작 좌표부터 매핑좌표까지 거리(m)
	double dfSgmtMatchLen = GetDistanceGEO1(stIntersect, stSgmtInfo.stPoint);

	// ── 링크 선택 비용(값이 작을수록 우선) — 쉬운 설명 (2026-07-08 최정우 추가) ──
	//   비용 = (GPS↔세그먼트 교차점 거리 INTERSECT_LEN, m) + (방위각 비용)
	//   방위각 비용 = 방향가중치 × (차량 방위각과 도로 방위각의 차이, 도)
	//     · 방향가중치 : 정지·저속(5km/h 이하)=0(방위각 무시) → 20km/h 이상=1.0(1도당 1m), 그 사이는 비례
	//     · 예) 거리 10m·각도차 40° → 10 + 1.0×40 = 50
	//            거리 30m·각도차  5° → 30 + 1.0×5 = 35  ⇒ 더 작은 35(방향 맞는 도로) 선택
	//   ※ 방위각 차이가 120°를 넘는 후보는 아예 제외(역방향 오매칭 방지)
	//
	//   [변수 매핑] dfCost = dfIntersectLenSgmt + dfDirWeight*|nHeadingDiff|,
	//              nHeadingDiff = GetAngleDiff(세그먼트 방위각, 차량 방위각),
	//              dfDirWeight  = 속도(nSpeed)로 0~MM_DIR_WEIGHT 사이 결정
	// ────────────────────────────────────────────────────────────────────────────
	double dfDirWeight = MM_DIR_WEIGHT;
	if (stSgmtMatchInput.nSpeed >= 0)
	{
		if (stSgmtMatchInput.nSpeed <= MM_SPEED_LOW_KMH)
			dfDirWeight = 0.0;										// 저속: 방위각 미반영(거리만)
		else if (stSgmtMatchInput.nSpeed < MM_SPEED_HIGH_KMH)
			dfDirWeight = MM_DIR_WEIGHT
				* static_cast<double>(stSgmtMatchInput.nSpeed - MM_SPEED_LOW_KMH)
				/ static_cast<double>(MM_SPEED_HIGH_KMH - MM_SPEED_LOW_KMH);
	}
	double dfAnglePenalty = bHasHeading ? (dfDirWeight * fabs(static_cast<double>(nHeadingDiff))) : 0.0;

	pstSgmtMatchRes->stMatchPoint.dfX = stIntersect.dfX;
	pstSgmtMatchRes->stMatchPoint.dfY = stIntersect.dfY;
	pstSgmtMatchRes->dfSgmtMatchLen = dfSgmtMatchLen;
	pstSgmtMatchRes->dfIntersectLenSgmt = dfIntersectLenSgmt;
	pstSgmtMatchRes->dfCost = dfIntersectLenSgmt + dfAnglePenalty;		// 링크 선택 기준 (2026-07-08 최정우 추가)
	pstSgmtMatchRes->nDirAngleDiff = nDirAngleDiff;
	pstSgmtMatchRes->qwLinkID = stSgmtInfo.qwLinkID;

	return true;
}

namespace {

bool IsElevatedRoad(uint8 nRoadType)
{
	return (nRoadType == ROAD_TYPE_ELEVATED || nRoadType == ROAD_TYPE_BRIDGE);
}

bool IsUndergroundRoad(uint8 nRoadType)
{
	return (nRoadType == ROAD_TYPE_UNDERGROUND);
}

bool IsRoadTypeCompatible(uint8 nCandRoadType, uint8 nPrevRoadType)
{
	if (nCandRoadType == nPrevRoadType)
		return true;
	if (IsElevatedRoad(nCandRoadType) && IsElevatedRoad(nPrevRoadType))
		return true;
	return false;
}

double RoadTypeDirectionPenalty(double dfDeltaAlt, uint8 nCandRoadType,
		const ALTITUDE_SCORE_CONFIG& stAltConfig)
{
	if (dfDeltaAlt > static_cast<double>(stAltConfig.nGap))
	{
		if (IsUndergroundRoad(nCandRoadType))
			return static_cast<double>(stAltConfig.nPenalty);
	}
	else if (dfDeltaAlt < -static_cast<double>(stAltConfig.nGap))
	{
		if (IsElevatedRoad(nCandRoadType))
			return static_cast<double>(stAltConfig.nPenalty);
	}
	return 0.0;
}

} // namespace

/**
 * @brief 연속 맵매칭 고도·ROAD_TYPE 보조 비용 (m 환산, 양수=불리·음수=유리)
 * @param[in] stSgmtMatchInput 세그먼트 매칭 입력 (고도 컨텍스트)
 * @param[in] nCandRoadType 후보 링크 ROAD_TYPE
 * @param[in] stAltConfig config altitude_* 설정
 * @return 고도 보조 비용 (dfCost 가산분)
 * @remark
 * ── 쉬운 설명 (ACCURACY_M 적응 반경 주석과 동일 형식) ──
 *   전제: bUseAltScore, 직전·현재 ALTITUDE_M 유효, TUNNELING 아님, altitude_weight>0
 *   Δalt = nAltitudeM − nPrevAltitudeM  (직전 매칭 성공 시 GPS 고도 앵커)
 *
 *   |Δalt| ≤ altitude_gap:
 *     · 후보 ROAD_TYPE = 직전  → −altitude_bonus
 *     · 호환(고가↔교량)        → 0
 *     · 불일치                 → +altitude_penalty
 *
 *   |Δalt| > altitude_gap:
 *     · altitude_weight × (|Δalt| − altitude_gap) + 방향 패널티
 *     · Δalt > +차이 이고 후보=지하 → +altitude_penalty
 *     · Δalt < −차이 이고 후보=고가/교량 → +altitude_penalty
 *
 *   |Δalt|/dfHorizMoveM > altitude_slope → 0 (GPS 고도 불신, 폴백)
 *
 *   [변수 매핑]
 *     dfAltAdj = CalcAltRoadPenalty(...)
 *     dfCost   = dfIntersectLenSgmt + dfAnglePenalty + dfAltAdj
 *
 *   예) 차이=8, 보너스=3, 페널티=10, 가중치=0.5
 *       직전100m·현재106m(Δ=6), 후보=직전과 동일 고가 → −3
 *       직전100m·현재106m(Δ=6), 후보=일반(직전 고가)   → +10
 *       직전100m·현재120m(Δ=20), 후보=지하             → 0.5×(20−8)+10 = +16
 * ────────────────────────────────────────────────────────────────────────────
*/
double CGISUtil::CalcAltRoadPenalty(const SGMT_MATCH_INPUT& stSgmtMatchInput, uint8 nCandRoadType,
		const ALTITUDE_SCORE_CONFIG& stAltConfig) const
{
	if (!stSgmtMatchInput.bUseAltScore || stAltConfig.dfWeight <= 0.0)
		return 0.0;
	if (stSgmtMatchInput.nAltitudeM < 0 || stSgmtMatchInput.nPrevAltitudeM < 0)
		return 0.0;
	if (stSgmtMatchInput.nDriveStatus == DRIVE_STATUS_TUNNELING)
		return 0.0;

	double dfDeltaAlt = static_cast<double>(stSgmtMatchInput.nAltitudeM)
		- static_cast<double>(stSgmtMatchInput.nPrevAltitudeM);

	if (stSgmtMatchInput.dfHorizMoveM >= MM_CALC_MIN_DIST && stAltConfig.dfSlope > 0.0)
	{
		double dfSlope = fabs(dfDeltaAlt) / stSgmtMatchInput.dfHorizMoveM;
		if (dfSlope > stAltConfig.dfSlope)
			return 0.0;
	}

	if (fabs(dfDeltaAlt) <= static_cast<double>(stAltConfig.nGap))
	{
		if (nCandRoadType == stSgmtMatchInput.nPrevRoadType)
			return -static_cast<double>(stAltConfig.nBonus);
		// 직전·후보 ROAD_TYPE 호환성(고가↔교량 등) 판정 (2026-07-08 최정우 주석 추가)
		if (IsRoadTypeCompatible(nCandRoadType, stSgmtMatchInput.nPrevRoadType))
			return 0.0;
		return static_cast<double>(stAltConfig.nPenalty);
	}

	// Δalt 방향·ROAD_TYPE 불일치 추가 패널티 산출 (2026-07-08 최정우 주석 추가)
	return stAltConfig.dfWeight * (fabs(dfDeltaAlt) - static_cast<double>(stAltConfig.nGap))
		+ RoadTypeDirectionPenalty(dfDeltaAlt, nCandRoadType, stAltConfig);
}

/**
 * @brief 각도 차이 계산 (-180 ~ 180)
 * @param[in] nAngle1 이전 진행 방향
 * @param[in] nAngle2 다음 진행 방향
 * @return 각도 차이 (회전각)
*/
sint16 CGISUtil::GetAngleDiff(sint16& nAngle1, sint16& nAngle2)
{
	sint16 nAngleDiff = (nAngle2 - nAngle1) % 360;

	if (nAngleDiff > 180)
		nAngleDiff -= 360;
	else if (nAngleDiff < -180)
		nAngleDiff += 360;

	return nAngleDiff;
}

/**
 * @brief 진행 각도 계산
 * @param[in] stSgmtPoint 세그먼트 X,Y 좌표
 * @param[in] stPoint X,Y 좌표
 * @param[out] pnDirAngle 진행 각도 (방위각)
 * @return true(성공), false(실패)
*/
bool CGISUtil::GetDirAngle(POINT& stSgmtPoint, POINT& stPoint, sint16 *pnDirAngle)
{
	if ((stPoint.dfY - stSgmtPoint.dfY) > 0)
		*pnDirAngle = DEG(atan((stPoint.dfX - stSgmtPoint.dfX) / (stPoint.dfY - stSgmtPoint.dfY)));
	else if (((stPoint.dfY - stSgmtPoint.dfY) < 0) && ((stPoint.dfX - stSgmtPoint.dfX) > 0))
		*pnDirAngle = DEG(atan((stPoint.dfX - stSgmtPoint.dfX) / (stPoint.dfY - stSgmtPoint.dfY))) + 180;
	else if (((stPoint.dfY - stSgmtPoint.dfY) < 0) && ((stPoint.dfX - stSgmtPoint.dfX) < 0))
		*pnDirAngle = DEG(atan((stPoint.dfX - stSgmtPoint.dfX) / (stPoint.dfY - stSgmtPoint.dfY))) - 180;
	else if (((stPoint.dfY - stSgmtPoint.dfY) < 0) && (stPoint.dfX == stSgmtPoint.dfX))
		*pnDirAngle = 180;
	else if ((stPoint.dfY == stSgmtPoint.dfY) && ((stPoint.dfX - stSgmtPoint.dfX) > 0))
		*pnDirAngle = 90;
	else if ((stPoint.dfY == stSgmtPoint.dfY) && ((stPoint.dfX - stSgmtPoint.dfX) < 0))
		*pnDirAngle = -90;
	else
		return false;

	return true;
}

/**
 * @brief 진행 각도 계산
 * @param[in] stPoint1 진입 좌표
 * @param[in] stPoint2 진출 좌표
 * @return 진행각
*/
const sint16 CGISUtil::GetDirAngleDegree(POINT& stPoint1, POINT& stPoint2)
{
	sint16 nDirAngle = round(DEG(atan2(stPoint2.dfX - stPoint1.dfX, stPoint2.dfY - stPoint1.dfY)));

	if (nDirAngle < 0)
		nDirAngle += 360;
	else if (nDirAngle >= 360)
		nDirAngle -= 360;

	return nDirAngle;
}

/**
 * @brief 경위도간 거리 계산 (Degree * 360000)
 * @param[in] stPoint X,Y 좌표
 * @param[in] stIntersect 세그먼트 교차점 X,Y 좌표
 * @return 세그먼트 시작부터 교차점까지 거리
*/
const double CGISUtil::GetDistanceGEO1(POINT& stPoint, POINT& stIntersect)
{
	if ((stPoint.dfX == stIntersect.dfX) && (stPoint.dfY == stIntersect.dfY))
		return 0;

	double dfLon = (stIntersect.dfX - stPoint.dfX) / 360000.0;
	double dfLat = (stIntersect.dfY - stPoint.dfY) / 360000.0;

	double dfValue = pow(sin(RAD(dfLat) / 2.0), 2.0) + cos(RAD(stPoint.dfY / 360000.0)) * cos(RAD(stIntersect.dfY / 360000.0)) * pow(sin(RAD(dfLon) / 2.0), 2.0);
	return 2.0 * atan(sqrt(dfValue) / sqrt(1 - dfValue)) * 6378137;
}

/**
 * @brief 경위도간 거리 계산 (360000 곱하기 전)
 * @param[in] stPoint X,Y 좌표
 * @param[in] stIntersect 세그먼트 교차점 X,Y 좌표
 * @return 세그먼트 시작부터 교차점까지 거리
*/
const double CGISUtil::GetDistanceGEO2(POINT& stPoint, POINT& stIntersect)
{
	if ((stPoint.dfX == stIntersect.dfX) && (stPoint.dfY == stIntersect.dfY))
		return 0;

	double dfLon = stIntersect.dfX - stPoint.dfX;
	double dfLat = stIntersect.dfY - stPoint.dfY;

	double dfValue = pow(sin(RAD(dfLat) / 2.0), 2.0) + cos(RAD(stPoint.dfY / 360000.0)) * cos(RAD(stIntersect.dfY / 360000.0)) * pow(sin(RAD(dfLon) / 2.0), 2.0);
	return 2.0 * atan(sqrt(dfValue) / sqrt(1 - dfValue)) * 6378137;
}
