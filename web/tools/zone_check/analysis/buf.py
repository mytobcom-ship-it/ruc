# -*- coding: utf-8 -*-
"""버퍼값별로 G 범위와 M 범위가 일치하는 과금행 비율"""
import psycopg2
cn=psycopg2.connect(host='127.0.0.1',user='mytobcom',password='my664761',dbname='ruc'); cn.set_session(readonly=True)
cu=cn.cursor()
SQL="""
WITH zone AS (
  SELECT road_id, geom_type, link_ids,
         CASE WHEN geom_type='POLY'
           THEN ST_MakePolygon(ST_AddPoint(ST_MakeLine(ARRAY(
                  SELECT ST_SetSRID(ST_MakePoint((e->>0)::float8,(e->>1)::float8),4326)
                  FROM jsonb_array_elements(coords) e)),
                  ST_SetSRID(ST_MakePoint((coords->0->>0)::float8,(coords->0->>1)::float8),4326)))
           ELSE ST_MakeLine(ARRAY(SELECT ST_SetSRID(ST_MakePoint((e->>0)::float8,(e->>1)::float8),4326)
                  FROM jsonb_array_elements(coords) e)) END AS geom
  FROM ruc.base_roadlink WHERE use_yn='Y'),
gps AS (SELECT trip_id, gps_seq, gps_lat, gps_lon, match_lat, match_lon, match_link_id
        FROM ruc.prim_rawgps),
g_hit AS (
  SELECT p.trip_id, z.road_id, p.gps_seq FROM gps p JOIN zone z ON
    CASE WHEN z.geom_type='POLY'
      THEN ST_Contains(z.geom, ST_SetSRID(ST_MakePoint(COALESCE(p.match_lon,p.gps_lon),
                                                       COALESCE(p.match_lat,p.gps_lat)),4326))
      ELSE ST_DWithin(z.geom::geography, ST_SetSRID(ST_MakePoint(COALESCE(p.match_lon,p.gps_lon),
                                                    COALESCE(p.match_lat,p.gps_lat)),4326)::geography, %s)
    END
  WHERE COALESCE(p.match_lat,p.gps_lat) IS NOT NULL),
g_rng AS (SELECT trip_id, road_id, min(gps_seq) s, max(gps_seq) e FROM (
    SELECT trip_id, road_id, gps_seq,
           gps_seq - ROW_NUMBER() OVER (PARTITION BY trip_id,road_id ORDER BY gps_seq) grp
    FROM g_hit) t GROUP BY trip_id,road_id,grp),
m_hit AS (SELECT p.trip_id, z.road_id, p.gps_seq FROM gps p JOIN zone z
          ON z.link_ids IS NOT NULL AND p.match_link_id IN (SELECT jsonb_array_elements_text(z.link_ids))),
m_rng AS (SELECT trip_id, road_id, min(gps_seq) s, max(gps_seq) e FROM (
    SELECT trip_id, road_id, gps_seq,
           gps_seq - ROW_NUMBER() OVER (PARTITION BY trip_id,road_id ORDER BY gps_seq) grp
    FROM m_hit) t GROUP BY trip_id,road_id,grp)
SELECT count(*) AS 총,
       count(*) FILTER (WHERE g.s=m.s AND g.e=m.e) AS 일치,
       count(*) FILTER (WHERE g.s IS NULL) AS "G없음"
FROM m_rng m LEFT JOIN g_rng g USING (trip_id, road_id)
"""
print('버퍼   M구간 총   G와 완전일치   일치율')
for b in (2.0,3.0,5.0,8.0,10.0,15.0,20.0):
    cu.execute(SQL,(b,))
    tot,same,none=cu.fetchone()
    print('%5.0fm  %6d      %6d       %5.1f%%'%(b,tot,same,100.0*same/max(1,tot)))
