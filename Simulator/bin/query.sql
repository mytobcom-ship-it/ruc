-- RawGpsSimSvr query.sql (PostgreSQL / libpq: $1, $2, ...)
-- 도로망(network.moct_link)에서 경로 링크를 조회하고, roadnet.prim_rawgps 에 원시 GPS 를 적재한다.
--
-- 좌표계 주의:
--   network.moct_link.geom 은 EPSG:5186 (신규 표준노드링크). 런타임 SRID 감지 후 4326 출력.
--   런타임에 SRID 를 감지($5)하고, 출력은 항상 ST_Transform(geom,4326) 로 WGS-84 변환한다.

-- ── 도로망 SRID 감지 ───────────────────────────────────────────────────────
[srid_detect]
SELECT ST_SRID(geom)
FROM network.moct_link
WHERE geom IS NOT NULL
LIMIT 1;

-- ── 시작 링크 (bounding box 내 임의 1개) ──────────────────────────────────
-- $1=min_lon $2=min_lat $3=max_lon $4=max_lat $5=srid $6=limit(=1)
[moct_link_seed]
SELECT
	link_id,
	COALESCE(t_node, '') AS t_node,
	COALESCE(max_spd, 0) AS max_spd,
	ST_AsText(ST_Transform(geom, 4326)) AS wkt,
	COALESCE(road_type, '000') AS road_type,
	COALESCE(f_node, '') AS f_node
FROM network.moct_link
WHERE geom && ST_Transform(
		ST_MakeEnvelope($1::float8, $2::float8, $3::float8, $4::float8, 4326), $5::int)
	AND COALESCE(road_use, '0') = '0'
	AND t_node IS NOT NULL
ORDER BY random()
LIMIT $6::int;

-- ── 다음 연결 링크 (from_node 출발, 직전 링크·역방향(맞은편) 링크 제외) ───
-- $1=from_node $2=exclude_link_id $3=exclude_to_node(직전 출발 노드 — 이 노드로
--   돌아가는 링크는 방금 지나온 도로의 반대방향(맞은편 차로)이므로 제외해
--   차량이 곧바로 왔던 길을 되돌아가는 것을 막는다) (2026-07-22 최정우 수정)
[moct_link_next]
SELECT
	link_id,
	COALESCE(t_node, '') AS t_node,
	COALESCE(max_spd, 0) AS max_spd,
	ST_AsText(ST_Transform(geom, 4326)) AS wkt,
	COALESCE(road_type, '000') AS road_type,
	COALESCE(f_node, '') AS f_node
FROM network.moct_link
WHERE f_node = $1
	AND link_id <> $2
	AND (t_node IS DISTINCT FROM NULLIF($3, ''))
	AND t_node IS NOT NULL
ORDER BY random()
LIMIT 1;

-- ── 원시 GPS 적재 ─────────────────────────────────────────────────────────
-- $1=device_key $2=gps_dt $3=trip_event $4=drive_status $5=gps_lat $6=gps_lon
-- $7=speed_kmh $8=heading $9=altitude_m $10=accuracy_m $11=battery (NULL=미수집)
-- $12=trip_id (없으면 '') $13=gps_seq (운행마다 1~N) $14=raw_vld (좌표 유효='t'/'f')
-- — PRIM_RAWGPS 컬럼 순서 (PK→INDEX→속성, 2026-07-10)
[raw_gps_insert]
INSERT INTO roadnet.prim_rawgps (
	trip_id, gps_seq, device_key, gps_dt,
	trip_event, drive_status,
	gps_lat, gps_lon, raw_vld,
	speed_kmh, heading, altitude_m, accuracy_m, battery,
	recv_dt, match_status
) VALUES (
	NULLIF($12, ''),
	$13::bigint,
	$1,
	$2::char(14),
	$3::smallint, $4::smallint,
	$5::numeric, $6::numeric, $14::boolean,
	$7::numeric, $8::numeric, $9::numeric, $10::numeric, $11::smallint,
	$2::char(14), 0
);
