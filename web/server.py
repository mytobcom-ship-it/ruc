#!/usr/bin/env python3
"""
RUC 맵매칭 GPS 시각화 API 서버 (2026-07-10 최정우)
  - 주변 도로: roadnet.prim_link_info (PSF WGS84, 임시) + prim_rawgps 점/매칭 결과
  - 레거시: network.moct_link (/api/roads, /api/trip/.../roads)
"""
import configparser
import json
import math
import os
import subprocess
from pathlib import Path

import psycopg2
import psycopg2.extras
from flask import Flask, jsonify, request, send_from_directory

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.ini"
REPO_ROOT = BASE_DIR.parent
MM_CONFIG_PATH = REPO_ROOT / "MapMatchSvr" / "bin" / "config.ini"
MM_PIDFILE = REPO_ROOT / "MapMatchSvr" / "bin" / "MapMatchSvr.pid"

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


@app.route("/app.js")
def app_js():
    resp = send_from_directory(BASE_DIR, "app.js")
    resp.headers["Cache-Control"] = "no-cache"
    return resp


@app.route("/")
def index():
    resp = send_from_directory(BASE_DIR, "index.html")
    resp.headers["Cache-Control"] = "no-cache"
    return resp


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


def mapmatch_config_stale():
    """MapMatchSvr/bin/config.ini 가 마지막 기동 이후 수정됐는지 — 설정은 기동 시
    1회만 읽으므로(핫리로드 없음), 이 경우 재시작해야 값이 반영된다 (2026-07-21 최정우 추가)"""
    if not MM_CONFIG_PATH.exists():
        return False
    if not MM_PIDFILE.exists():
        return True
    return MM_CONFIG_PATH.stat().st_mtime > MM_PIDFILE.stat().st_mtime


def restart_mapmatch():
    """MapMatchSvr 만 재시작(Simulator·web_viewer 는 유지) — test_svr.sh mm-restart 위임
    (2026-07-21 최정우 추가)"""
    result = subprocess.run(
        ["./test_svr.sh", "mm-restart"],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        timeout=90,
    )
    return result.returncode == 0, (result.stdout + result.stderr)


@app.route("/api/trip/<path:trip_id>/retest", methods=["POST"])
def api_trip_retest(trip_id):
    """기존 수신 GPS(prim_rawgps)는 그대로 두고 매칭 결과만 초기화 — MapMatchSvr 가
    PENDING(0)을 재폴링해 동일 좌표로 재매칭하게 한다. config.ini 가 마지막 기동 이후
    바뀌었으면 재테스트 전 MapMatchSvr 를 먼저 재시작해 새 설정을 반영한다 (2026-07-21 최정우 추가)"""
    restarted = False
    if mapmatch_config_stale():
        ok, log = restart_mapmatch()
        if not ok:
            return jsonify({"error": "MapMatchSvr 재시작 실패", "log": log[-2000:]}), 500
        restarted = True

    with get_conn() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                UPDATE roadnet.prim_rawgps
                SET match_status = 0, match_link_id = NULL,
                    match_lat = NULL, match_lon = NULL, intersect_len = NULL
                WHERE trip_id = %s
                """,
                (trip_id,),
            )
            reset_count = cur.rowcount
    return jsonify({"trip_id": trip_id, "reset": reset_count, "restarted": restarted})


@app.route("/api/trip/<path:trip_id>/points")
def api_trip_points(trip_id):
    with get_conn() as conn:
        with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            # 엔진 저장 match_link_id 기준, 매칭 좌표를 매칭 링크 기하에 재-스냅해 도로선 위에 표출 (2026-07-15 최정우 복구)
            #   · match_link_name : prim_link_info 조인 도로명(팝업용)
            #   · match_lat/lon   : ST_ClosestPoint(매칭 링크 기하, 저장 매칭점) → 정확히 도로선 위
            #       링크 없거나 좌표 없으면 원본 좌표 유지
            cur.execute(
                """
                SELECT g.gps_seq, g.gps_dt, g.trip_event, g.drive_status, g.match_status,
                       g.gps_lat, g.gps_lon, g.intersect_len,
                       g.match_link_id,
                       l.name AS match_link_name,
                       CASE
                         WHEN g.match_lat IS NOT NULL AND l.geom IS NOT NULL
                         THEN ST_Y(ST_ClosestPoint(l.geom,
                              ST_SetSRID(ST_MakePoint(g.match_lon::float8, g.match_lat::float8), 4326)))
                         ELSE g.match_lat
                       END AS match_lat,
                       CASE
                         WHEN g.match_lon IS NOT NULL AND l.geom IS NOT NULL
                         THEN ST_X(ST_ClosestPoint(l.geom,
                              ST_SetSRID(ST_MakePoint(g.match_lon::float8, g.match_lat::float8), 4326)))
                         ELSE g.match_lon
                       END AS match_lon
                FROM roadnet.prim_rawgps g
                LEFT JOIN roadnet.prim_link_info l ON l.link_id = g.match_link_id
                WHERE g.trip_id = %s
                ORDER BY g.gps_seq ASC
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
                    "match_link_id": r["match_link_id"],
                    "match_link_name": r["match_link_name"],
                })
    return jsonify(rows)


def prim_link_feature(link_id, geojson_text, extra=None):
    props = {"link_id": link_id}
    if extra:
        props.update(extra)
    return {
        "type": "Feature",
        "properties": props,
        "geometry": json.loads(geojson_text),
    }


@app.route("/api/prim/info")
def api_prim_info():
    with get_conn() as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM roadnet.prim_link_info")
            cnt = int(cur.fetchone()[0])
    return jsonify({"table": "roadnet.prim_link_info", "count": cnt})


@app.route("/api/trip/<path:trip_id>/prim-roads")
def api_trip_prim_roads(trip_id):
    cfg = load_config()
    buffer_m = int(request.args.get("buffer", cfg["road_buffer_m"]))
    with get_conn() as conn:
        with conn.cursor() as cur:
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
                SELECT l.link_id, l.name, l.len,
                       l.st_nd_id, l.st_nd_name, l.ed_nd_id, l.ed_nd_name,
                       ST_AsGeoJSON(l.geom) AS geojson, l.road_type
                FROM roadnet.prim_link_info l
                CROSS JOIN env e
                WHERE l.geom IS NOT NULL
                  AND l.geom && e.geom4326
                  AND ST_Intersects(l.geom, e.geom4326)
                LIMIT 8000
                """,
                (trip_id, buffer_m),
            )
            features = []
            for row in cur.fetchall():
                features.append(prim_link_feature(row[0], row[7], {
                    "name": row[1],
                    "len": float(row[2]) if row[2] is not None else None,
                    "st_nd_id": row[3],
                    "st_nd_name": row[4],
                    "ed_nd_id": row[5],
                    "ed_nd_name": row[6],
                    # 시설 유형 (0=일반, 1=교량, 2=터널, 3=고가, 4=지하) — hover 표시용 (2026-07-21 최정우 추가)
                    "road_type": int(row[8]) if row[8] is not None else None,
                }))
    return jsonify({"type": "FeatureCollection", "features": features})


@app.route("/api/prim/roads")
def api_prim_roads_bbox():
    try:
        min_lon = float(request.args["min_lon"])
        min_lat = float(request.args["min_lat"])
        max_lon = float(request.args["max_lon"])
        max_lat = float(request.args["max_lat"])
    except (KeyError, ValueError):
        return jsonify({"error": "min_lon,min_lat,max_lon,max_lat required"}), 400

    with get_conn() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT l.link_id, l.name, l.len,
                       l.st_nd_id, l.st_nd_name, l.ed_nd_id, l.ed_nd_name,
                       ST_AsGeoJSON(l.geom) AS geojson, l.road_type
                FROM roadnet.prim_link_info l
                WHERE l.geom IS NOT NULL
                  AND l.geom && ST_MakeEnvelope(%s, %s, %s, %s, 4326)
                  AND ST_Intersects(l.geom, ST_MakeEnvelope(%s, %s, %s, %s, 4326))
                LIMIT 8000
                """,
                (min_lon, min_lat, max_lon, max_lat, min_lon, min_lat, max_lon, max_lat),
            )
            features = []
            for row in cur.fetchall():
                features.append(prim_link_feature(row[0], row[7], {
                    "name": row[1],
                    "len": float(row[2]) if row[2] is not None else None,
                    "st_nd_id": row[3],
                    "st_nd_name": row[4],
                    "ed_nd_id": row[5],
                    "ed_nd_name": row[6],
                    "road_type": int(row[8]) if row[8] is not None else None,
                }))
    return jsonify({"type": "FeatureCollection", "features": features})


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


@app.after_request
def disable_api_cache(resp):
    if request.path.startswith("/api/"):
        resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
        resp.headers["Pragma"] = "no-cache"
    return resp


if __name__ == "__main__":
    c = load_config()
    print(f"RUC map viewer http://0.0.0.0:{c['web_port']}  road_buffer_m={c['road_buffer_m']}")
    app.run(host="0.0.0.0", port=c["web_port"], debug=False, threaded=True)
