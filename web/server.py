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
import re
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
SIM_CONFIG_PATH = REPO_ROOT / "Simulator" / "bin" / "config.ini"
SIM_VEHICLES_MIN = 1
SIM_VEHICLES_MAX = 10

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


def read_sim_vehicles():
    """Simulator/bin/config.ini [sim] vehicles= 현재값 조회 — 웹 콤보박스 초기값용
    (2026-07-22 최정우 추가)"""
    cfg = configparser.ConfigParser()
    cfg.read(SIM_CONFIG_PATH, encoding="utf-8-sig")
    try:
        return int(cfg["sim"]["vehicles"])
    except (KeyError, ValueError):
        return SIM_VEHICLES_MIN


def write_sim_vehicles(n):
    """Simulator/bin/config.ini [sim] vehicles= 값만 라인 단위로 치환 — configparser 로
    다시 쓰면 주석·서식이 다 날아가므로, 정규식으로 해당 줄만 바꾼다 (2026-07-22 최정우 추가)"""
    n = max(SIM_VEHICLES_MIN, min(SIM_VEHICLES_MAX, int(n)))
    text = SIM_CONFIG_PATH.read_text(encoding="utf-8-sig")
    new_text, count = re.subn(
        r"(?m)^vehicles\s*=.*$", "vehicles=%d" % n, text, count=1
    )
    if count == 0:
        raise ValueError("config.ini 에서 vehicles= 줄을 찾지 못함")
    SIM_CONFIG_PATH.write_text(new_text, encoding="utf-8-sig")
    return n


@app.route("/api/config")
def api_config():
    c = load_config()
    return jsonify({
        "road_buffer_m": c["road_buffer_m"],
        "poll_sec": c["poll_sec"],
        "sim_vehicles": read_sim_vehicles(),
        "sim_vehicles_min": SIM_VEHICLES_MIN,
        "sim_vehicles_max": SIM_VEHICLES_MAX,
    })


@app.route("/api/trips")
def api_trips():
    # trip 시작 시각(MIN) 기준 정렬 — 동시 운행 차량이 여러 대일 때 갱신 시각(MAX)으로 정렬하면
    #   각 차량의 flush 타이밍 차이로 폴링마다 1위가 바뀌어, app.js "최신 Trip" 자동추적이
    #   차량 사이를 계속 튀어다니는 원인이 된다. 시작 시각은 trip 생애 동안 고정값이라, 정말
    #   새 trip 이 시작될 때만 순위가 바뀐다 (2026-07-22 최정우 수정 — vehicles=3 전환에 대응)
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
                  ORDER BY MIN(gps_dt) DESC, trip_id DESC, device_key DESC
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


@app.route("/api/system/start-engines", methods=["POST"])
def api_start_engines():
    """웹 페이지 "신규테스트" 버튼 — MapMatchSvr → Simulator 순서로 1초 확인 + 최대 3회
    재시도 기동 (test_svr.sh start-mm-sim-retry 위임). 웹 자신은 이미 이 요청을 처리
    중이므로 재기동 대상에서 제외 — 어느 단계에서 실패했는지 stdout 의 FAILED_STAGE= 를
    파싱해 응답에 포함한다 (2026-07-21 최정우 추가)

    body(optional): {"vehicles": N} — 동시 운행 차량 대수 콤보박스 값. Simulator 는 설정을
    기동 시 1회만 읽으므로(핫리로드 없음), 재시작 전에 config.ini 를 먼저 갱신해야 반영된다
    (2026-07-22 최정우 추가)"""
    body = request.get_json(silent=True) or {}
    applied_vehicles = None
    if "vehicles" in body:
        try:
            applied_vehicles = write_sim_vehicles(body["vehicles"])
        except (ValueError, OSError) as err:
            return jsonify({"ok": False, "error": "vehicles 설정 반영 실패: %s" % err}), 400

    try:
        result = subprocess.run(
            ["./test_svr.sh", "start-mm-sim-retry"],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=120,
        )
    except subprocess.TimeoutExpired:
        return jsonify({"ok": False, "error": "기동 스크립트 타임아웃(120s)"}), 500

    output = result.stdout + result.stderr
    failed_stage = None
    for line in output.splitlines():
        if line.startswith("FAILED_STAGE="):
            failed_stage = line.split("=", 1)[1].strip()
    ok = (result.returncode == 0) and (failed_stage is None)
    return jsonify({
        "ok": ok,
        "failed_stage": failed_stage,
        "log": output[-3000:],
        "vehicles": applied_vehicles,
    })


@app.route("/api/trip/<path:trip_id>/delete", methods=["POST"])
def api_trip_delete(trip_id):
    """선택된 Trip을 PRIM_RAWGPS 에서 완전히 삭제 — 되돌릴 수 없음. 웹 "삭제" 버튼 전용
    (2026-07-22 최정우 추가)"""
    with get_conn() as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM roadnet.prim_rawgps WHERE trip_id = %s", (trip_id,))
            deleted = cur.rowcount
    return jsonify({"trip_id": trip_id, "deleted": deleted})


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


# SKIP(3)/ERROR(4) 원인 컬럼(MATCH_REASON)이 없어, 이미 저장된 값(MATCH_LINK_ID·INTERSECT_LEN·
# ACCURACY_M)만으로 가능한 원인을 추정 표시한다. 엔진이 실제로 기록한 사유가 아니라 "근사치"이므로
# 문구에 항상 "추정"을 붙인다 — MapMatchSvr 쪽 SKIP/ERROR 분기(RawLogWorker.cpp)와 완전히 1:1
# 대응하지 않을 수 있음 (2026-07-21 최정우 추가)
RADIUS_SKIP_ACCURACY_M = 50  # MapMatchSvr/bin/config.ini [mapmatch] radius_skip 기본값과 동일 가정


def infer_match_reason(match_status, match_link_id, intersect_len, accuracy_m):
    if match_status == 1:
        return None
    if match_status == 4:
        return "매칭 실패 추정 (반경 내 후보 없음·처리 오류 등 — 로그 확인 필요)"
    if match_status == 3:
        if accuracy_m is not None and accuracy_m > RADIUS_SKIP_ACCURACY_M:
            return "정확도 초과 추정 (ACCURACY_M={}m > {}m)".format(accuracy_m, RADIUS_SKIP_ACCURACY_M)
        if match_link_id is None:
            return "후보 없음 추정 (반경 밖·좌표 유효성 오류 등)"
        if intersect_len is not None and intersect_len > 10:
            return "저신뢰 매칭 추정 (세그먼트 클램프 등으로 오차 {}m)".format(intersect_len)
        return "역행 의심 미확정 추정 (또는 이상속도 등 정합성 검사)"
    return None


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
                       g.gps_lat, g.gps_lon, g.intersect_len, g.accuracy_m,
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
                match_status = int(r["match_status"])
                intersect_len = int(r["intersect_len"]) if r["intersect_len"] is not None else None
                accuracy_m = int(r["accuracy_m"]) if r["accuracy_m"] is not None else None
                rows.append({
                    "gps_seq": int(r["gps_seq"]),
                    "gps_dt": r["gps_dt"],
                    "trip_event": int(r["trip_event"]),
                    "drive_status": int(r["drive_status"]),
                    "match_status": match_status,
                    "gps_lat": float(r["gps_lat"]) if r["gps_lat"] is not None else None,
                    "gps_lon": float(r["gps_lon"]) if r["gps_lon"] is not None else None,
                    "match_lat": float(r["match_lat"]) if r["match_lat"] is not None else None,
                    "match_lon": float(r["match_lon"]) if r["match_lon"] is not None else None,
                    "intersect_len": intersect_len,
                    "match_link_id": r["match_link_id"],
                    "match_link_name": r["match_link_name"],
                    "match_reason": infer_match_reason(match_status, r["match_link_id"], intersect_len, accuracy_m),
                })
    return jsonify(rows)


@app.route("/api/trips/points")
def api_trips_points():
    """여러 trip 의 GPS/매칭 점을 한 번에 조회 — 다중 차량 동시 지도 표시용
    (2026-07-23 최정우 추가). /api/trip/<id>/points 와 동일 SQL을 trip_id 배열로 확장.
    응답: {trip_id: [...points...]} """
    trip_ids = [t for t in request.args.get("trip_ids", "").split(",") if t]
    if not trip_ids:
        return jsonify({"error": "trip_ids required"}), 400
    with get_conn() as conn:
        with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(
                """
                SELECT g.trip_id, g.gps_seq, g.gps_dt, g.trip_event, g.drive_status, g.match_status,
                       g.gps_lat, g.gps_lon, g.intersect_len, g.accuracy_m,
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
                WHERE g.trip_id = ANY(%s)
                ORDER BY g.trip_id ASC, g.gps_seq ASC
                """,
                (trip_ids,),
            )
            result = {t: [] for t in trip_ids}
            for r in cur.fetchall():
                match_status = int(r["match_status"])
                intersect_len = int(r["intersect_len"]) if r["intersect_len"] is not None else None
                accuracy_m = int(r["accuracy_m"]) if r["accuracy_m"] is not None else None
                result[r["trip_id"]].append({
                    "gps_seq": int(r["gps_seq"]),
                    "gps_dt": r["gps_dt"],
                    "trip_event": int(r["trip_event"]),
                    "drive_status": int(r["drive_status"]),
                    "match_status": match_status,
                    "gps_lat": float(r["gps_lat"]) if r["gps_lat"] is not None else None,
                    "gps_lon": float(r["gps_lon"]) if r["gps_lon"] is not None else None,
                    "match_lat": float(r["match_lat"]) if r["match_lat"] is not None else None,
                    "match_lon": float(r["match_lon"]) if r["match_lon"] is not None else None,
                    "intersect_len": intersect_len,
                    "match_link_id": r["match_link_id"],
                    "match_link_name": r["match_link_name"],
                    "match_reason": infer_match_reason(match_status, r["match_link_id"], intersect_len, accuracy_m),
                })
    return jsonify(result)


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


@app.route("/api/trips/prim-roads")
def api_trips_prim_roads():
    """여러 trip 의 주변 도로를 한 번에 조회 — 다중 차량 동시 지도 표시용 (2026-07-23 최정우 추가).
    /api/trip/<id>/prim-roads 와 동일 패턴이나 envelope 를 여러 trip 점의 union 으로 계산 —
    ST_Buffer(ST_Collect(...))는 점들의 union 을 버퍼링하는 것이라 trip 마다 각각 버퍼링해
    합친 것과 동일한 "경로를 따라가는 회랑" 모양이 됨(지리적으로 먼 차량이어도 그 사이
    거대한 영역이 딸려오지 않음). link_id 기준 자연 중복제거된 단일 FeatureCollection 반환."""
    trip_ids = [t for t in request.args.get("trip_ids", "").split(",") if t]
    if not trip_ids:
        return jsonify({"error": "trip_ids required"}), 400
    cfg = load_config()
    buffer_m = int(request.args.get("buffer", cfg["road_buffer_m"]))
    with get_conn() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                WITH pts AS (
                    SELECT ST_SetSRID(ST_MakePoint(gps_lon::float8, gps_lat::float8), 4326) AS g
                    FROM roadnet.prim_rawgps
                    WHERE trip_id = ANY(%s)
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
                (trip_ids, buffer_m),
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
