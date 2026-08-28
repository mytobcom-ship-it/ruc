#!/usr/bin/env python3
"""
RUC 맵매칭 GPS 시각화 API 서버 (2026-07-10 최정우)
  - 주변 도로: ruc.road_link + ruc.prim_rawgps 점/매칭 결과 (2026-08-26 최정우 수정 —
    roadnet DB(roadnet.prim_link_info, network.moct_link) 참조 전부 제거, 사용자 지시:
    웹뷰어는 ruc 스키마만 참조. 미사용 레거시 라우트(/api/roads, /api/trip/.../roads)도
    같이 삭제)
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
    remote = cfg["remote_database"] if cfg.has_section("remote_database") else None
    return {
        "host": db.get("host", "127.0.0.1"),
        "port": int(db.get("port", "5432")),
        "name": db.get("name", "roadnet"),
        "points_name": db.get("points_name", db.get("name", "roadnet")),
        "userid": db.get("userid", "mytobcom"),
        "password": db.get("password", ""),
        "remote": {
            "host": remote.get("host", ""),
            "port": int(remote.get("port", "5432")),
            "name": remote.get("name", "ruc"),
            "userid": remote.get("userid", ""),
            "password": remote.get("password", ""),
        } if remote is not None else None,
        "web_port": int(web.get("port", "8088")),
        "road_buffer_m": int(web.get("road_buffer_m", "1000")),
        "poll_sec": int(web.get("poll_sec", "5")),
        # 과금 구역이 LINE(선)일 때 "원시 GPS 가 구역 안"으로 볼 허용 거리(m).
        #   POLY 는 폴리곤 포함 판정이라 이 값을 쓰지 않는다 (2026-08-22 최정우 추가)
        "zone_line_buf_m": int(web.get("zone_line_buf_m", "20")),
    }


def get_conn_points():
    """GPS/매칭 점(prim_rawgps) 전용 연결 — Simulator/MapMatchSvr 가 2026-08-11
    ruc DB(schema: ruc)로 전환해 도로망(roadnet DB)과 분리됨 (2026-08-12 최정우 추가)"""
    c = load_config()
    return psycopg2.connect(
        host=c["host"],
        port=c["port"],
        dbname=c["points_name"],
        user=c["userid"],
        password=c["password"],
    )


def get_conn_remote():
    """원격 ruc DB(실서비스 DB) 조회 전용 연결 — [remote_database] 섹션이 없으면 호출측이
    404 로 처리하도록 None 반환. default_transaction_read_only=on 으로 세션을 열어, 이
    연결로는 애초에 DELETE/UPDATE/INSERT 가 DB 레벨에서 거부된다 — 이 파일에 원격용 쓰기
    라우트를 실수로 추가해도 안전판이 되도록 하는 방어 (2026-08-18 최정우 추가)"""
    c = load_config()
    r = c["remote"]
    if r is None or not r["host"]:
        return None
    return psycopg2.connect(
        host=r["host"],
        port=r["port"],
        dbname=r["name"],
        user=r["userid"],
        password=r["password"],
        options="-c default_transaction_read_only=on",
    )


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


def _query_trips(conn, limit):
    # trip 시작 시각(MIN) 기준 정렬 — 동시 운행 차량이 여러 대일 때 갱신 시각(MAX)으로 정렬하면
    #   각 차량의 flush 타이밍 차이로 폴링마다 1위가 바뀌어, app.js "최신 Trip" 자동추적이
    #   차량 사이를 계속 튀어다니는 원인이 된다. 시작 시각은 trip 생애 동안 고정값이라, 정말
    #   새 trip 이 시작될 때만 순위가 바뀐다 (2026-07-22 최정우 수정 — vehicles=3 전환에 대응)
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT trip_id, device_key,
                   MIN(gps_dt) AS gps_dt_min,
                   MAX(gps_dt) AS gps_dt_max,
                   COUNT(*) AS cnt,
                   SUM(CASE WHEN match_status = 0 THEN 1 ELSE 0 END) AS pending_cnt,
                   BOOL_OR(trip_event = 2) AS has_end
            FROM ruc.prim_rawgps
            GROUP BY trip_id, device_key
              ORDER BY MIN(gps_dt) DESC, trip_id DESC, device_key DESC
            LIMIT %s
            """,
            (limit,),
        )
        return [
            {
                "trip_id": r[0],
                "device_key": r[1],
                "gps_dt_min": r[2],
                "gps_dt_max": r[3],
                "count": r[4],
                # 이 trip 의 맵매칭이 완전히 끝났는지 — pending 0건 & END 이벤트 도달.
                #   app.js 자동 갱신(폴링)이 더 이상 볼 게 없는 완료된 trip 을 계속
                #   찔러보지 않고 스스로 멈추는 데 사용 (2026-07-24 최정우 추가)
                "complete": (r[5] == 0 and bool(r[6])),
            }
            for r in cur.fetchall()
        ]


@app.route("/api/trips")
def api_trips():
    limit = min(int(request.args.get("limit", 30)), 200)
    with get_conn_points() as conn:
        rows = _query_trips(conn, limit)
    return jsonify(rows)


def _query_trips_with_car(conn, limit):
    """원격 전용 — TRIP_ID 생성 포맷이 device_key 형식(예: CAR000197_...)에서
    base_carinfo.car_seq_no 를 6자리 0-패딩한 형식(예: 000093_20260813175008)으로
    변경됨(2026-08-19 확인 — project_trip_id_format_change 참고). 옛 device_key 포맷은
    포맷 변경 이전의 레거시 데이터라 목록에서 제외 — 아래 정규식(6자리 숫자_14자리 시각)으로
    신규 포맷만 필터링(2026-08-19 최정우 수정). prim_rawgps.device_key 컬럼 자체는 항상
    정확하지만, base_carinfo 를 조인해 실제 차량(car_no/car_seq_no)까지 검증해서 같이
    반환한다. device_key 매칭을 우선하고, (그 조건이 안 맞을 때만) TRIP_ID 앞 6자리 =
    LPAD(car_seq_no,6,'0') 매칭으로 보완 (2026-08-18 최정우 추가)"""
    with conn.cursor() as cur:
        cur.execute(
            """
            WITH trips AS (
                SELECT trip_id, device_key,
                       MIN(gps_dt) AS gps_dt_min,
                       MAX(gps_dt) AS gps_dt_max,
                       COUNT(*) AS cnt,
                       SUM(CASE WHEN match_status = 0 THEN 1 ELSE 0 END) AS pending_cnt,
                       BOOL_OR(trip_event = 2) AS has_end
                FROM ruc.prim_rawgps
                WHERE trip_id ~ '^[0-9]{6}_[0-9]{14}$'
                GROUP BY trip_id, device_key
                ORDER BY MIN(gps_dt) DESC, trip_id DESC, device_key DESC
                LIMIT %s
            )
            SELECT t.trip_id, t.device_key, t.gps_dt_min, t.gps_dt_max, t.cnt,
                   t.pending_cnt, t.has_end, c.car_no, c.car_seq_no
            FROM trips t
            LEFT JOIN LATERAL (
                SELECT car_no, car_seq_no
                FROM ruc.base_carinfo b
                WHERE b.device_key = t.device_key
                   OR LPAD(b.car_seq_no::text, 6, '0') = split_part(t.trip_id, '_', 1)
                ORDER BY (b.device_key = t.device_key) DESC
                LIMIT 1
            ) c ON true
            ORDER BY t.gps_dt_min DESC, t.trip_id DESC, t.device_key DESC
            """,
            (limit,),
        )
        return [
            {
                "trip_id": r[0],
                "device_key": r[1],
                "gps_dt_min": r[2],
                "gps_dt_max": r[3],
                "count": r[4],
                "complete": (r[5] == 0 and bool(r[6])),
                "car_no": r[7],
                "car_seq_no": r[8],
            }
            for r in cur.fetchall()
        ]


@app.route("/api/remote/trips")
def api_remote_trips():
    """원격 ruc DB(실서비스 DB) 조회 전용 — get_conn_remote() 세션 자체가 읽기전용이라
    쓰기 라우트가 여기 섞일 수 없음 (2026-08-18 최정우 추가). base_carinfo 조인 포함
    (car_no/car_seq_no) — _query_trips_with_car() 참고"""
    limit = min(int(request.args.get("limit", 30)), 200)
    conn = get_conn_remote()
    if conn is None:
        return jsonify({"error": "remote_database 설정 없음 (config.ini [remote_database])"}), 404
    with conn:
        rows = _query_trips_with_car(conn, limit)
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
    with get_conn_points() as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM ruc.prim_rawgps WHERE trip_id = %s", (trip_id,))
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

    with get_conn_points() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                UPDATE ruc.prim_rawgps
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


def _query_trip_points(conn, trip_id):
    with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
        # ruc DB(2026-08-11 전환) 는 road_link 가 geom 없이 jsonb coords 라 도로선
        # 재-스냅(ST_ClosestPoint)은 불가 — 엔진이 저장한 match_lat/lon 을 그대로 쓰고
        # road_link 조인은 도로명 표시(match_link_name)에만 사용 (2026-08-12 최정우 수정)
        cur.execute(
            """
            SELECT g.gps_seq, g.gps_dt, g.trip_event, g.drive_status, g.match_status,
                   g.gps_lat, g.gps_lon, g.intersect_len, g.accuracy_m,
                   g.match_link_id,
                   l.road_name AS match_link_name,
                   g.match_lat, g.match_lon
            FROM ruc.prim_rawgps g
            LEFT JOIN ruc.road_link l ON l.link_id = g.match_link_id
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
        return rows


# base_roadlink.coords(jsonb [[lon,lat],...]) → PostGIS geometry.
#   POLY 는 첫 점을 끝에 다시 붙여 닫힌 링으로 만든다(원본이 닫혀 있지 않음).
_ZONE_GEOM_SQL = """
    CASE b.geom_type
      WHEN 'POLY' THEN ST_MakePolygon(ST_AddPoint(
             ST_MakeLine(ARRAY(SELECT ST_SetSRID(ST_MakePoint((e->>0)::float8,(e->>1)::float8),4326)
                               FROM jsonb_array_elements(b.coords) e)),
             ST_SetSRID(ST_MakePoint((b.coords->0->>0)::float8,(b.coords->0->>1)::float8),4326)))
      ELSE ST_MakeLine(ARRAY(SELECT ST_SetSRID(ST_MakePoint((e->>0)::float8,(e->>1)::float8),4326)
                             FROM jsonb_array_elements(b.coords) e))
    END
"""


def _query_trip_charges(conn, trip_id, line_buf_m):
    """trip_id 의 과금 이력(prim_chargehand) + GPS 순번 범위 — PostGIS 있으면 정식 쿼리,
    없으면(원격 실서비스 DB — extension 목록에 postgis 자체가 없어 설치 권한도 없음, 2026-08-24
    확인) 형상 없이 계산하는 폴백 쿼리로 자동 전환한다. 원격 /api/remote/trip/.../charges 라우트가
    이 함수를 그대로 재사용하면서 처음 드러남 — 로컬은 항상 PostGIS 경로만 탄다. (2026-08-24 최정우 추가)
    """
    try:
        return _query_trip_charges_postgis(conn, trip_id, line_buf_m)
    except psycopg2.errors.UndefinedFunction:
        conn.rollback()
        return _query_trip_charges_no_postgis(conn, trip_id)


def _query_trip_charges_postgis(conn, trip_id, line_buf_m):
    """trip_id 의 과금 이력(prim_chargehand) + GPS 순번 범위

      G 순번 : prim_chargehand.start_gps_seq~end_gps_seq — 엔진(RawLogWorker)이 진입/구역 안에서
               실제로 마지막 확인된 tick 을 실시간으로 직접 기록한 값(2026-08-28 최정우 수정 —
               이전엔 GPS 순번 컬럼이 없어 prim_rawgps 시간창과 조인해 사후 역산했으나, 엔진이
               직접 채우도록 바뀌면서 그 역산 로직(g_hit/g_run/g_rng, t_rng 의 s_g/e_g)을 제거하고
               engine 값을 그대로 신뢰한다. 0(과거 미구현 시절 레코드의 기본값)은 NULL 처리)
      M 순번 : 매칭 링크(match_link_id)가 그 구역의 link_ids 에 속한 구간 — 여전히 prim_rawgps 와
               조인해 계산(엔진이 따로 기록하지 않음)
    같은 구역을 여러 번 지나면 gps_seq 연속 구간(run)으로 나눠, 구역별 등장 순서대로
    해당 구역의 과금 이력(trip_seq 순)에 하나씩 대응시킨다.
    주정차(POLY)는 link_ids 가 비어 있어 M 순번이 나오지 않는다. (2026-08-22 최정우 추가)
    M 범위는 엔진이 확정한 시간창(occur_dt~occur_dt+stay_seconds, t_rng CTE)을 우선 적용하며,
    run 기반 m_rng 는 t_rng 가 없을 때만 쓰는 폴백이다 — 구간 내부 맵매칭 끊김으로 run 이 둘로
    갈려도 대응이 어긋나지 않는다. (2026-08-24 최정우 추가)
    """
    with conn.cursor() as cur:
        cur.execute(
            """
            WITH zone AS (
                SELECT b.road_id, b.geom_type, b.link_ids, """ + _ZONE_GEOM_SQL + """ AS geom
                FROM ruc.base_roadlink b WHERE b.use_yn = 'Y'
            ),
            gps AS (
                SELECT gps_seq, gps_lat, gps_lon, match_lat, match_lon, match_link_id, gps_dt
                FROM ruc.prim_rawgps WHERE trip_id = %(tid)s
            ),
            -- M: 매칭 링크가 구역 link_ids 에 속함
            m_hit AS (
                SELECT z.road_id, p.gps_seq
                FROM gps p JOIN zone z
                  ON z.link_ids IS NOT NULL
                 AND p.match_link_id IN (SELECT jsonb_array_elements_text(z.link_ids))
            ),
            m_run AS (
                SELECT road_id, gps_seq,
                       gps_seq - ROW_NUMBER() OVER (PARTITION BY road_id ORDER BY gps_seq) AS grp
                FROM m_hit
            ),
            m_rng AS (
                SELECT road_id, MIN(gps_seq) AS s, MAX(gps_seq) AS e,
                       ROW_NUMBER() OVER (PARTITION BY road_id ORDER BY MIN(gps_seq)) AS rn
                FROM m_run GROUP BY road_id, grp
            ),
            ch AS (
                SELECT c.*,
                       ROW_NUMBER() OVER (PARTITION BY c.zone_id ORDER BY c.trip_seq) AS zone_rn
                FROM ruc.prim_chargehand c WHERE c.trip_id = %(tid)s
            ),
            -- M 범위를 연속구간(run) 순번 대응으로 구하면, 그 구간 "안에서" 맵매칭이 한 틱이라도
            --   끊기면(SKIP) 실제로는 하나인 과금기록이 run 두 개로 쪼개져 순번 대응이 어긋난다
            --   (실측 000376_20260819094414 RL-Z00003 trip_seq=5). "engine 이 이미 확정한 시간창으로
            --   gps_seq 를 복원"하는 t_rng 를 우선 적용해 이를 방지한다.
            --   주의: occur_dt 의미가 과금유형마다 다르다(RawLogWorker.cpp 실측) — PARKING(4)·
            --   CLOSED_ROAD(2)·SPEED_ZONE(3)·OPEN_ROAD(1)은 "진입"(또는 순간통과) 시각이라 창이
            --   [occur_dt, occur_dt+stay] 이지만, NODE_STEP(0)·EXEMPT(5)는 "진출" 시각으로 기록돼
            --   창이 거꾸로 [occur_dt-stay, occur_dt] 다 — 방향을 안 가리면 진입시각형은 stay_seconds
            --   만큼 미래로 밀려 실제 구간을 완전히 벗어난다(실측 000376_20260819140532 RL-Z00002
            --   trip_seq=1). t_rng 가 못 찾으면(예외 상황) 기존 run 기반 m_rng 로 폴백한다
            --   (2026-08-22 최정우 추가, 2026-08-24 최정우 수정 — POLY 전용에서 전체 확장 + 과금유형별
            --   진입/진출 방향 반영, 2026-08-28 최정우 수정 — G 관련 부분 제거, M 전용으로 축소)
            t_win AS (
                SELECT ch.trip_seq, ch.zone_id,
                       CASE WHEN ch.charge_type IN (0, 5)
                            THEN to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                                 - (COALESCE(ch.stay_seconds, 0) || ' seconds')::interval
                            ELSE to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                       END AS dt_from,
                       CASE WHEN ch.charge_type IN (0, 5)
                            THEN to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                            ELSE to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                                 + (COALESCE(ch.stay_seconds, 0) || ' seconds')::interval
                       END AS dt_to
                -- CLOSED_ROAD(2)·SPEED_ZONE(3)는 출구 게이트를 못 만나고 트립종료/TTL로 강제
                --   마감되면(AppendExpired*Charge, exit_tollgate_id NULL) stay_seconds 가 "입구~강제
                --   마감 시각"이라 실제 구역 이탈 이후 시간까지 포함해버린다 — 그 창을 그대로 믿으면
                --   실제로는 구역을 떠난 뒤의 GPS 까지 범위에 끌려 들어온다(실측
                --   000370_20260824103155 RL-Z00003 — 실제 매칭은 G42~53 인데 stay_seconds=90 이
                --   트립종료 시각까지 포함해 G73 까지로 표출됨). 이 경우엔 t_rng 를 만들지 않고
                --   기존 run 기반 g_rng/m_rng(실제 매칭 링크 소속 여부)로 폴백한다. 정상 종료(출구
                --   게이트 확인, exit_tollgate_id 있음)는 그대로 t_rng 를 신뢰한다 (2026-08-24 최정우 추가)
                FROM ch WHERE ch.occur_dt IS NOT NULL
                  AND NOT (ch.charge_type IN (2, 3) AND ch.exit_tollgate_id IS NULL)
            ),
            -- t_rng 원래 범위(s_all/e_all)는 시간창 안의 GPS 전부를 잡아, 게이트 통과시각 보간이
            --   두 tick 사이 직선거리로 못 가르는 경우(비율이 0~1 밖으로 나가 dtPrev/dtCur 로 클램프,
            --   InterpolateGateCrossingTime() 참고) 시간창 끝이 "실제로는 구역 밖인" 경계 참조용 tick과
            --   정확히 같은 시각이 되어 그 tick까지 범위에 끌려들어온다(실측 000376_20260819094414
            --   RL-Z00002 trip_seq=1). s_m/e_m 은 그 안에서 "실제 구역 소속(m_hit)"인 tick으로만
            --   다시 좁힌 값 — 시간창 내부의 SKIP 갭은 여전히 안 끊기고(MIN/MAX라 중간 결측은 무시),
            --   경계의 비소속 tick만 깎여나간다. 소속 tick이 시간창 안에 하나도 없으면(예외) s_all/e_all
            --   로 폴백 (2026-08-26 최정우 추가, 2026-08-28 최정우 수정 — s_g/e_g/g_hit 조인 제거)
            t_rng AS (
                SELECT w.trip_seq,
                       MIN(p.gps_seq) AS s_all, MAX(p.gps_seq) AS e_all,
                       MIN(p.gps_seq) FILTER (WHERE mh.gps_seq IS NOT NULL) AS s_m,
                       MAX(p.gps_seq) FILTER (WHERE mh.gps_seq IS NOT NULL) AS e_m
                FROM t_win w
                JOIN gps p ON to_timestamp(p.gps_dt, 'YYYYMMDDHH24MISS') BETWEEN w.dt_from AND w.dt_to
                LEFT JOIN m_hit mh ON mh.road_id = w.zone_id AND mh.gps_seq = p.gps_seq
                GROUP BY w.trip_seq
            )
            SELECT ch.trip_seq, ch.charge_type, ch.zone_id, ch.zone_name,
                   ch.tollgate_id, ch.entry_tollgate_id, ch.exit_tollgate_id,
                   ch.dist_m, ch.speed_kmh, ch.speed_limit_kmh, ch.stay_seconds,
                   ch.occur_dt, ch.charge_yn,
                   NULLIF(ch.start_gps_seq, 0) AS g_from,
                   NULLIF(ch.end_gps_seq, 0) AS g_to,
                   COALESCE(t_rng.s_m, t_rng.s_all, m_rng.s) AS m_from,
                   COALESCE(t_rng.e_m, t_rng.e_all, m_rng.e) AS m_to
            FROM ch
            LEFT JOIN t_rng ON t_rng.trip_seq = ch.trip_seq
            LEFT JOIN m_rng ON m_rng.road_id = ch.zone_id AND m_rng.rn = ch.zone_rn
            -- G 순번(주행 순서) 오름차순 — 엔진이 직접 기록한 값이라 항상 채워져 있음(구 레코드의
            --   0=NULLIF 로 미상 처리된 경우만 뒤로 감), 그 안에서는 trip_seq 순 (2026-08-22 최정우
            --   수정, 2026-08-28 최정우 수정 — start_gps_seq 기준으로 변경)
            ORDER BY NULLIF(ch.start_gps_seq, 0) ASC NULLS LAST, ch.trip_seq
            """,
            {"tid": trip_id, "buf": line_buf_m},
        )
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]


def _query_trip_charges_no_postgis(conn, trip_id):
    """PostGIS 없는 DB(원격 실서비스 — postgis extension 자체가 미설치)용 폴백.

    G 순번은 postgis 버전과 동일하게 prim_chargehand.start_gps_seq~end_gps_seq 를 그대로 쓴다
    (형상판정이 필요 없어 애초에 postgis 유무와 무관 — 2026-08-28 최정우 수정). M 순번(LINE 구역만,
    매칭 링크가 zone.link_ids 에 속하는 구간)만 이 폴백 특유의 로직 — POLY(주정차)는 형상판정이
    필요해 여기선 못 구해 항상 t_rng 폴백에 의존한다. (2026-08-24 최정우 추가)
    """
    with conn.cursor() as cur:
        cur.execute(
            """
            WITH gps AS (
                SELECT gps_seq, gps_lat, gps_lon, match_lat, match_lon, match_link_id, gps_dt
                FROM ruc.prim_rawgps WHERE trip_id = %(tid)s
            ),
            zone AS (
                SELECT b.road_id, b.geom_type, b.link_ids
                FROM ruc.base_roadlink b WHERE b.use_yn = 'Y'
            ),
            -- LINE 구역만 대상 — 매칭 링크가 구역 link_ids 에 속하는 구간. G·M 둘 다 이 결과를 쓴다
            --   (POLY 는 형상판정이 필요해 여기선 못 구하고 t_rng 에만 의존)
            m_hit AS (
                SELECT z.road_id, p.gps_seq
                FROM gps p JOIN zone z
                  ON z.link_ids IS NOT NULL
                 AND p.match_link_id IN (SELECT jsonb_array_elements_text(z.link_ids))
            ),
            m_run AS (
                SELECT road_id, gps_seq,
                       gps_seq - ROW_NUMBER() OVER (PARTITION BY road_id ORDER BY gps_seq) AS grp
                FROM m_hit
            ),
            m_rng AS (
                SELECT road_id, MIN(gps_seq) AS s, MAX(gps_seq) AS e,
                       ROW_NUMBER() OVER (PARTITION BY road_id ORDER BY MIN(gps_seq)) AS rn
                FROM m_run GROUP BY road_id, grp
            ),
            ch AS (
                SELECT c.*,
                       ROW_NUMBER() OVER (PARTITION BY c.zone_id ORDER BY c.trip_seq) AS zone_rn
                FROM ruc.prim_chargehand c WHERE c.trip_id = %(tid)s
            ),
            t_win AS (
                SELECT ch.trip_seq,
                       CASE WHEN ch.charge_type IN (0, 5)
                            THEN to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                                 - (COALESCE(ch.stay_seconds, 0) || ' seconds')::interval
                            ELSE to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                       END AS dt_from,
                       CASE WHEN ch.charge_type IN (0, 5)
                            THEN to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                            ELSE to_timestamp(ch.occur_dt, 'YYYYMMDDHH24MISS')
                                 + (COALESCE(ch.stay_seconds, 0) || ' seconds')::interval
                       END AS dt_to
                -- CLOSED_ROAD(2)·SPEED_ZONE(3)는 출구 게이트를 못 만나고 트립종료/TTL로 강제
                --   마감되면(AppendExpired*Charge, exit_tollgate_id NULL) stay_seconds 가 "입구~강제
                --   마감 시각"이라 실제 구역 이탈 이후 시간까지 포함해버린다 — 그 창을 그대로 믿으면
                --   실제로는 구역을 떠난 뒤의 GPS 까지 범위에 끌려 들어온다(실측
                --   000370_20260824103155 RL-Z00003 — 실제 매칭은 G42~53 인데 stay_seconds=90 이
                --   트립종료 시각까지 포함해 G73 까지로 표출됨). 이 경우엔 t_rng 를 만들지 않고
                --   기존 run 기반 g_rng/m_rng(실제 매칭 링크 소속 여부)로 폴백한다. 정상 종료(출구
                --   게이트 확인, exit_tollgate_id 있음)는 그대로 t_rng 를 신뢰한다 (2026-08-24 최정우 추가)
                FROM ch WHERE ch.occur_dt IS NOT NULL
                  AND NOT (ch.charge_type IN (2, 3) AND ch.exit_tollgate_id IS NULL)
            ),
            t_rng AS (
                SELECT w.trip_seq, MIN(p.gps_seq) AS s, MAX(p.gps_seq) AS e
                FROM t_win w
                JOIN gps p ON to_timestamp(p.gps_dt, 'YYYYMMDDHH24MISS') BETWEEN w.dt_from AND w.dt_to
                GROUP BY w.trip_seq
            )
            SELECT ch.trip_seq, ch.charge_type, ch.zone_id, ch.zone_name,
                   ch.tollgate_id, ch.entry_tollgate_id, ch.exit_tollgate_id,
                   ch.dist_m, ch.speed_kmh, ch.speed_limit_kmh, ch.stay_seconds,
                   ch.occur_dt, ch.charge_yn,
                   NULLIF(ch.start_gps_seq, 0) AS g_from,
                   NULLIF(ch.end_gps_seq, 0) AS g_to,
                   COALESCE(t_rng.s, m_rng.s) AS m_from,
                   COALESCE(t_rng.e, m_rng.e) AS m_to
            FROM ch
            LEFT JOIN t_rng ON t_rng.trip_seq = ch.trip_seq
            LEFT JOIN m_rng ON m_rng.road_id = ch.zone_id AND m_rng.rn = ch.zone_rn
            ORDER BY NULLIF(ch.start_gps_seq, 0) ASC NULLS LAST, ch.trip_seq
            """,
            {"tid": trip_id},
        )
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]


def _query_zones(conn):
    """과금·비과금 구역(base_roadlink)과 그 게이트(base_tollgate) 전량

    뷰어가 구역 좌표·이름·유형을 소스에 하드코딩하고 있어 DB 와 어긋났다(2026-08-22 확인 —
    RL-Z00002 이름 불일치, RL-Z00009 를 개방형으로 표시, 강릉 구역 7개 누락).
    구역 14개·좌표 477점·10kB 로 작고 거의 바뀌지 않아 페이지 로드 시 1회만 읽으면 된다.
    """
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT r.road_id, r.road_kind, r.road_nm, r.geom_type, r.coords,
                   r.speed_limit_kmh, r.link_ids,
                   COALESCE((
                     SELECT jsonb_agg(jsonb_build_object(
                              'tollgate_id', t.tollgate_id, 'gate_div', t.gate_div,
                              'tollgate_nm', t.tollgate_nm,
                              'lon', t.lon, 'lat', t.lat, 'link_id', t.link_id)
                            ORDER BY t.gate_div, t.tollgate_id)
                     FROM ruc.base_tollgate t
                     WHERE t.road_id = r.road_id AND t.use_yn = 'Y'
                   ), '[]'::jsonb) AS gates
            FROM ruc.base_roadlink r
            WHERE r.use_yn = 'Y'
            ORDER BY r.road_kind, r.road_id
            """
        )
        cols = [d[0] for d in cur.description]
        rows = []
        for r in cur.fetchall():
            d = dict(zip(cols, r))
            # numeric → float (JSON 직렬화)
            if d.get("speed_limit_kmh") is not None:
                d["speed_limit_kmh"] = float(d["speed_limit_kmh"])
            for g in (d.get("gates") or []):
                for k in ("lon", "lat"):
                    if g.get(k) is not None:
                        g[k] = float(g[k])
            rows.append(d)
        return rows


@app.route("/api/zones")
def api_zones():
    with get_conn_points() as conn:
        return jsonify(_query_zones(conn))


@app.route("/api/trip/<path:trip_id>/charges")
def api_trip_charges(trip_id):
    buf = load_config()["zone_line_buf_m"]
    with get_conn_points() as conn:
        rows = _query_trip_charges(conn, trip_id, buf)
    return jsonify(rows)


@app.route("/api/trip/<path:trip_id>/points")
def api_trip_points(trip_id):
    with get_conn_points() as conn:
        rows = _query_trip_points(conn, trip_id)
    return jsonify(rows)


@app.route("/api/remote/trip/<path:trip_id>/points")
def api_remote_trip_points(trip_id):
    """원격 ruc DB 조회 전용 — 위 api_remote_trips() 와 동일한 읽기전용 세션 (2026-08-18 최정우 추가)"""
    conn = get_conn_remote()
    if conn is None:
        return jsonify({"error": "remote_database 설정 없음 (config.ini [remote_database])"}), 404
    with conn:
        rows = _query_trip_points(conn, trip_id)
    return jsonify(rows)


@app.route("/api/remote/trip/<path:trip_id>/charges")
def api_remote_trip_charges(trip_id):
    """원격 ruc DB 과금 이력 — 프런트(apiBase())는 이미 원격 모드에서 이 경로를 호출하고
    있었는데 라우트가 없어 404 였다(2026-08-24 최정우 추가 — 원격 선택 시 지도 아래
    과금 이력이 로컬 DB 내용으로 표시되던 버그 수정)"""
    conn = get_conn_remote()
    if conn is None:
        return jsonify({"error": "remote_database 설정 없음 (config.ini [remote_database])"}), 404
    buf = load_config()["zone_line_buf_m"]
    with conn:
        rows = _query_trip_charges(conn, trip_id, buf)
    return jsonify(rows)


@app.route("/api/remote/zones")
def api_remote_zones():
    """원격 ruc DB 의 과금구역(base_roadlink) + 게이트(base_tollgate) 조회 — 읽기전용 세션.
    원래는 로컬(_query_zones)과 별개로 손으로 짠 쿼리였는데, 2026-08-22 로컬 쪽만 "구역별로
    게이트를 중첩한 배열" 구조로 고치면서(RL-Z00002 이름 불일치·구역 누락 등도 그때 같이 수정)
    원격은 그대로 남아 {"zones":[...],"gates":[...]} 평평한 구조를 계속 반환했다 — 프런트
    renderZones() 는 로컬 구조(구역 배열, 각 구역에 z.gates 중첩)만 기대해서 원격 선택 시
    zones.forEach 가 조용히 실패해(catch 로 콘솔 경고만) 과금구역 레이어가 통째로 안 그려졌다.
    로컬과 완전히 같은 함수로 통일해 구조·버그수정 내용이 항상 같이 간다 (2026-08-24 최정우 수정)"""
    conn = get_conn_remote()
    if conn is None:
        return jsonify({"error": "remote_database 설정 없음 (config.ini [remote_database])"}), 404
    with conn:
        return jsonify(_query_zones(conn))


@app.route("/api/trips/points")
def api_trips_points():
    """여러 trip 의 GPS/매칭 점을 한 번에 조회 — 다중 차량 동시 지도 표시용
    (2026-07-23 최정우 추가). /api/trip/<id>/points 와 동일 SQL을 trip_id 배열로 확장.
    응답: {trip_id: [...points...]} """
    trip_ids = [t for t in request.args.get("trip_ids", "").split(",") if t]
    if not trip_ids:
        return jsonify({"error": "trip_ids required"}), 400
    with get_conn_points() as conn:
        with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
            cur.execute(
                """
                SELECT g.trip_id, g.gps_seq, g.gps_dt, g.trip_event, g.drive_status, g.match_status,
                       g.gps_lat, g.gps_lon, g.intersect_len, g.accuracy_m,
                       g.match_link_id,
                       l.road_name AS match_link_name,
                       g.match_lat, g.match_lon
                FROM ruc.prim_rawgps g
                LEFT JOIN ruc.road_link l ON l.link_id = g.match_link_id
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


def _road_link_feature(link_id, road_name, length_m, f_node, t_node, road_type, coords):
    """ruc.road_link 1행 → GeoJSON Feature. coords 는 jsonb `[[[lon,lat],...]]` — PostGIS
    geometry 가 아니라 좌표 배열 그대로라 ST_AsGeoJSON 없이 직접 조립한다. 노드 이름
    (st_nd_name/ed_nd_name)은 ruc 스키마에 별도 노드 테이블이 없어 항상 None — roadnet DB
    참조를 없애며 감수한 손실(사용자 확인, 2026-08-26 최정우 추가)"""
    line = coords[0] if coords else []
    return {
        "type": "Feature",
        "properties": {
            "link_id": link_id,
            "name": road_name,
            "len": float(length_m) if length_m is not None else None,
            "st_nd_id": f_node,
            "st_nd_name": None,
            "ed_nd_id": t_node,
            "ed_nd_name": None,
            # 도로 유형 000:일반 001:교량 002:터널 003:고가 004:지하 (MOCT_LINK.ROAD_TYPE,
            #   road_link 는 zero-padded 문자열 — int 로 캐스팅 (2026-07-21/2026-08-26 최정우)
            "road_type": int(road_type) if road_type is not None else None,
        },
        "geometry": {"type": "LineString", "coordinates": line},
    }


def _meters_to_deg_buffer(lats, buffer_m):
    """buffer_m(미터)를 위경도 버퍼(도)로 근사 변환. road_link 는 PostGIS geometry 컬럼이
    없어(coords jsonb) ST_Buffer 를 못 쓰므로, min/max_lon/lat bbox 인덱스(idx_road_link_bbox)
    기반 사각형 필터로 근사한다 — 경도는 평균위도의 cos 보정 (2026-08-26 최정우 추가)"""
    avg_lat = (sum(lats) / len(lats)) if lats else 37.5
    lat_buf = buffer_m / 111320.0
    lon_buf = buffer_m / (111320.0 * max(math.cos(math.radians(avg_lat)), 0.01))
    return lon_buf, lat_buf


def _query_ruc_roads_near_points(conn, lons, lats, buffer_m):
    """좌표 배열 주변 도로망(ruc.road_link) 조회 — 호출측이 이미 연 커넥션(로컬/원격 ruc DB
    무관)을 그대로 받아 쓴다. roadnet DB 참조 제거(사용자 지시, 2026-08-26 최정우 추가)"""
    if not lons:
        return []
    lon_buf, lat_buf = _meters_to_deg_buffer(lats, buffer_m)
    min_lon, max_lon = min(lons) - lon_buf, max(lons) + lon_buf
    min_lat, max_lat = min(lats) - lat_buf, max(lats) + lat_buf
    with conn.cursor() as cur:
        cur.execute(
            """
            SELECT link_id, road_name, length_m, f_node, t_node, road_type, coords
            FROM ruc.road_link
            WHERE min_lon <= %s AND max_lon >= %s
              AND min_lat <= %s AND max_lat >= %s
            LIMIT 8000
            """,
            (max_lon, min_lon, max_lat, min_lat),
        )
        return [_road_link_feature(*row) for row in cur.fetchall()]


@app.route("/api/prim/info")
def api_prim_info():
    with get_conn_points() as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT COUNT(*) FROM ruc.road_link")
            cnt = int(cur.fetchone()[0])
    return jsonify({"table": "ruc.road_link", "count": cnt})


@app.route("/api/trip/<path:trip_id>/prim-roads")
def api_trip_prim_roads(trip_id):
    """트립 주변 도로망 조회 — ruc.road_link 기준 (2026-08-26 최정우 수정 — roadnet DB
    참조 제거, 사용자 지시: 웹뷰어는 ruc 스키마 테이블만 참조)"""
    cfg = load_config()
    buffer_m = int(request.args.get("buffer", cfg["road_buffer_m"]))
    with get_conn_points() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT gps_lon, gps_lat FROM ruc.prim_rawgps
                WHERE trip_id = %s AND gps_lat IS NOT NULL AND gps_lon IS NOT NULL
                """,
                (trip_id,),
            )
            rows = cur.fetchall()
        lons = [float(r[0]) for r in rows]
        lats = [float(r[1]) for r in rows]
        features = _query_ruc_roads_near_points(conn, lons, lats, buffer_m)
    return jsonify({"type": "FeatureCollection", "features": features})


@app.route("/api/remote/trip/<path:trip_id>/prim-roads")
def api_remote_trip_prim_roads(trip_id):
    """트립 주변 도로망 조회 — 원격 ruc DB(실서비스 DB) 기준, 읽기전용 연결. 도로망도 같은
    원격 커넥션의 ruc.road_link 로 조회(2026-08-26 최정우 수정 — roadnet DB 참조 제거,
    기존엔 점만 원격이고 도로망은 로컬 roadnet DB를 섞어 썼음) (2026-08-19 최정우 추가)"""
    cfg = load_config()
    buffer_m = int(request.args.get("buffer", cfg["road_buffer_m"]))
    conn = get_conn_remote()
    if conn is None:
        return jsonify({"error": "remote_database 설정 없음 (config.ini [remote_database])"}), 404
    with conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT gps_lon, gps_lat FROM ruc.prim_rawgps
                WHERE trip_id = %s AND gps_lat IS NOT NULL AND gps_lon IS NOT NULL
                """,
                (trip_id,),
            )
            rows = cur.fetchall()
        lons = [float(r[0]) for r in rows]
        lats = [float(r[1]) for r in rows]
        features = _query_ruc_roads_near_points(conn, lons, lats, buffer_m)
    return jsonify({"type": "FeatureCollection", "features": features})


@app.route("/api/trips/prim-roads")
def api_trips_prim_roads():
    """여러 trip 의 주변 도로를 한 번에 조회 — 다중 차량 동시 지도 표시용 (2026-07-23 최정우 추가).
    ruc.road_link 기준으로 전환(2026-08-26 최정우 수정 — roadnet DB 참조 제거. 기존엔
    roadnet.prim_rawgps 를 참조해 2026-08-11 ruc DB 전환 이후 항상 0건이었음 — 같은 원인으로
    이미 고쳤던 /api/trip/<id>/prim-roads 와 동일 버그)."""
    trip_ids = [t for t in request.args.get("trip_ids", "").split(",") if t]
    if not trip_ids:
        return jsonify({"error": "trip_ids required"}), 400
    cfg = load_config()
    buffer_m = int(request.args.get("buffer", cfg["road_buffer_m"]))
    with get_conn_points() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT gps_lon, gps_lat FROM ruc.prim_rawgps
                WHERE trip_id = ANY(%s) AND gps_lat IS NOT NULL AND gps_lon IS NOT NULL
                """,
                (trip_ids,),
            )
            rows = cur.fetchall()
        lons = [float(r[0]) for r in rows]
        lats = [float(r[1]) for r in rows]
        features = _query_ruc_roads_near_points(conn, lons, lats, buffer_m)
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

    with get_conn_points() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT link_id, road_name, length_m, f_node, t_node, road_type, coords
                FROM ruc.road_link
                WHERE min_lon <= %s AND max_lon >= %s
                  AND min_lat <= %s AND max_lat >= %s
                LIMIT 8000
                """,
                (max_lon, min_lon, max_lat, min_lat),
            )
            features = [_road_link_feature(*row) for row in cur.fetchall()]
    return jsonify({"type": "FeatureCollection", "features": features})


@app.route("/api/remote/prim/roads")
def api_remote_prim_roads_bbox():
    """지도 pan/zoom 시 화면 bbox 도로망 — 원격 ruc DB 기준 (2026-08-26 최정우 추가,
    /api/prim/roads 의 원격 버전 — 뷰어가 지도를 이동할 때마다 그 영역 도로를 동적으로
    불러오기 위함, 사용자 지시)"""
    try:
        min_lon = float(request.args["min_lon"])
        min_lat = float(request.args["min_lat"])
        max_lon = float(request.args["max_lon"])
        max_lat = float(request.args["max_lat"])
    except (KeyError, ValueError):
        return jsonify({"error": "min_lon,min_lat,max_lon,max_lat required"}), 400

    conn = get_conn_remote()
    if conn is None:
        return jsonify({"error": "remote_database 설정 없음 (config.ini [remote_database])"}), 404
    with conn:
        with conn.cursor() as cur:
            cur.execute(
                """
                SELECT link_id, road_name, length_m, f_node, t_node, road_type, coords
                FROM ruc.road_link
                WHERE min_lon <= %s AND max_lon >= %s
                  AND min_lat <= %s AND max_lat >= %s
                LIMIT 8000
                """,
                (max_lon, min_lon, max_lat, min_lat),
            )
            features = [_road_link_feature(*row) for row in cur.fetchall()]
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
