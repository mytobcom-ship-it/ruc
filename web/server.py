#!/usr/bin/env python3
"""
RUC 맵매칭 GPS 시각화 API 서버 (2026-07-10 최정우)
  - 주변 표준노드링크(network.moct_link) + prim_rawgps 점/매칭 결과
"""
import configparser
import json
import math
import os
from pathlib import Path

import psycopg2
import psycopg2.extras
from flask import Flask, jsonify, request, send_from_directory

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.ini"

app = Flask(__name__, static_folder=str(BASE_DIR), static_url_path="")


def load_config():
    cfg = configparser.ConfigParser()
    cfg.read(CONFIG_PATH, encoding="utf-8-sig")
    db = cfg["database"]
    web = cfg["web"] if cfg.has_section("web") else {}
    return {
        "host": db.get("host", "127.0.0.1"),
        "port": int(db.get("port", "5432")),
        "name": db.get("name", "roadnet"),
        "userid": db.get("userid", "mytobcom"),
        "password": db.get("password", ""),
        "web_port": int(web.get("port", "8088")),
        "road_buffer_m": int(web.get("road_buffer_m", "1000")),
        "poll_sec": int(web.get("poll_sec", "5")),
    }


def get_conn():
    c = load_config()
    return psycopg2.connect(
        host=c["host"],
        port=c["port"],
        dbname=c["name"],
        user=c["userid"],
        password=c["password"],
    )


def get_link_srid(cur):
    cur.execute(
        "SELECT ST_SRID(geom) FROM network.moct_link WHERE geom IS NOT NULL LIMIT 1"
    )
    row = cur.fetchone()
    return int(row[0]) if row and row[0] else 5186


@app.route("/")
def index():
    return send_from_directory(BASE_DIR, "index.html")


@app.route("/app.js")
def app_js():
    return send_from_directory(BASE_DIR, "app.js")


@app.route("/api/config")
def api_config():
    c = load_config()
    return jsonify({
        "road_buffer_m": c["road_buffer_m"],
        "poll_sec": c["poll_sec"],
    })


@app.route("/api/trips")
def api_trips():
    limit = min(int(request.args.get("limit", 30)), 200)
    with get_conn() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT trip_id, device_key,
                       MIN(gps_dt) AS gps_dt_min,
                       MAX(gps_dt) AS gps_dt_max,
                       COUNT(*) AS cnt
                FROM roadnet.prim_rawgps
                GROUP BY trip_id, device_key
                ORDER BY MAX(gps_dt) DESC
                LIMIT %s
                """,
                (limit,),
            )
            rows = [
                {
                    "trip_id": r[0],
                    "device_key": r[1],
                    "gps_dt_min": r[2],
                    "gps_dt_max": r[3],
                    "count": r[4],
                }
                for r in cur.fetchall()
            ]
    return jsonify(rows)


@app.route("/api/trip/<path:trip_id>/points")
def api_trip_points(trip_id):
    with get_conn() as conn:
        with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(
                """
                SELECT gps_seq, gps_dt, trip_event, drive_status, match_status,
                       gps_lat, gps_lon, match_lat, match_lon, intersect_len
                FROM roadnet.prim_rawgps
                WHERE trip_id = %s
                ORDER BY gps_seq ASC
                """,
                (trip_id,),
            )
            rows = []
            for r in cur.fetchall():
                rows.append({
                    "gps_seq": int(r["gps_seq"]),
                    "gps_dt": r["gps_dt"],
                    "trip_event": int(r["trip_event"]),
                    "drive_status": int(r["drive_status"]),
                    "match_status": int(r["match_status"]),
                    "gps_lat": float(r["gps_lat"]) if r["gps_lat"] is not None else None,
                    "gps_lon": float(r["gps_lon"]) if r["gps_lon"] is not None else None,
                    "match_lat": float(r["match_lat"]) if r["match_lat"] is not None else None,
                    "match_lon": float(r["match_lon"]) if r["match_lon"] is not None else None,
                    "intersect_len": int(r["intersect_len"]) if r["intersect_len"] is not None else None,
                })
    return jsonify(rows)


@app.route("/api/trip/<path:trip_id>/roads")
def api_trip_roads(trip_id):
    cfg = load_config()
    buffer_m = int(request.args.get("buffer", cfg["road_buffer_m"]))
    with get_conn() as conn:
        with conn.cursor() as cur:
            link_srid = get_link_srid(cur)
            cur.execute(
                """
                WITH pts AS (
                    SELECT ST_SetSRID(ST_MakePoint(gps_lon::float8, gps_lat::float8), 4326) AS g
                    FROM roadnet.prim_rawgps
                    WHERE trip_id = %s
                      AND gps_lat IS NOT NULL
                      AND gps_lon IS NOT NULL
                ),
                env AS (
                    SELECT ST_Buffer(ST_Collect(g)::geography, %s)::geometry AS geom4326
                    FROM pts
                )
                SELECT l.link_id,
                       ST_AsGeoJSON(ST_Transform(l.geom, 4326)) AS geojson
                FROM network.moct_link l
                CROSS JOIN env e
                WHERE l.geom IS NOT NULL
                  AND l.geom && ST_Transform(e.geom4326, %s)
                  AND ST_Intersects(l.geom, ST_Transform(e.geom4326, %s))
                """,
                (trip_id, buffer_m, link_srid, link_srid),
            )
            features = []
            for link_id, geojson_text in cur.fetchall():
                geom = json.loads(geojson_text)
                features.append({
                    "type": "Feature",
                    "properties": {"link_id": link_id},
                    "geometry": geom,
                })
    return jsonify({"type": "FeatureCollection", "features": features})


@app.route("/api/roads")
def api_roads_bbox():
    """지도 pan/zoom 시 화면 bbox 도로망 (선택) (2026-07-10 최정우)"""
    try:
        min_lon = float(request.args["min_lon"])
        min_lat = float(request.args["min_lat"])
        max_lon = float(request.args["max_lon"])
        max_lat = float(request.args["max_lat"])
    except (KeyError, ValueError):
        return jsonify({"error": "min_lon,min_lat,max_lon,max_lat required"}), 400

    with get_conn() as conn:
        with conn.cursor() as cur:
            link_srid = get_link_srid(cur)
            cur.execute(
                """
                SELECT l.link_id,
                       ST_AsGeoJSON(ST_Transform(l.geom, 4326)) AS geojson
                FROM network.moct_link l
                WHERE l.geom IS NOT NULL
                  AND l.geom && ST_Transform(
                        ST_MakeEnvelope(%s, %s, %s, %s, 4326), %s)
                  AND ST_Intersects(l.geom, ST_Transform(
                        ST_MakeEnvelope(%s, %s, %s, %s, 4326), %s))
                LIMIT 8000
                """,
                (
                    min_lon, min_lat, max_lon, max_lat, link_srid,
                    min_lon, min_lat, max_lon, max_lat, link_srid,
                ),
            )
            features = []
            for link_id, geojson_text in cur.fetchall():
                features.append({
                    "type": "Feature",
                    "properties": {"link_id": link_id},
                    "geometry": json.loads(geojson_text),
                })
    return jsonify({"type": "FeatureCollection", "features": features})


if __name__ == "__main__":
    c = load_config()
    print(f"RUC map viewer http://0.0.0.0:{c['web_port']}  road_buffer_m={c['road_buffer_m']}")
    app.run(host="0.0.0.0", port=c["web_port"], debug=False, threaded=True)
