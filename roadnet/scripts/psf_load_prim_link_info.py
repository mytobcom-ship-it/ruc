#!/usr/bin/env python3
"""
link.psf → ROADNET.PRIM_LINK_INFO 적재

- 노드 ID·노드 좌표: PSF LINK_INFO (dwStNode*/dwEdNode* / 360000.0)
- LEN: PSF dfLen (m)
- GEOM: PSF LINK_SGMT_INFO 세그먼트 시작점 + 마지막 세그먼트 끝점 (WGS84)
- 노드명: 적재 후 network.moct_node.node_name 조인

사용:
  # 1) DDL (postgres)
  sudo -u postgres psql -d roadnet -f roadnet/sql/prim_map_create.sql
  # 2) 적재
  PYTHONPATH=roadnet/scripts/.pydeps python3 roadnet/scripts/psf_load_prim_link_info.py
"""

from __future__ import annotations

import argparse
import configparser
import math
import struct
import sys
import time
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
_PYDEPS = _SCRIPT_DIR / ".pydeps"
if _PYDEPS.is_dir():
    sys.path.insert(0, str(_PYDEPS))

LINK_INFO_FMT = "<Q I H I B B d B B B B 46s Q I I B Q I I B"
LINK_SGMT_FMT = "<I I I H H Q H"
LINK_INFO_SIZE = struct.calcsize(LINK_INFO_FMT)
LINK_SGMT_SIZE = struct.calcsize(LINK_SGMT_FMT)
HEAD_FMT = "<15I"
HEAD_SIZE = struct.calcsize(HEAD_FMT)
SCALE = 360000.0


def parse_head(blob: bytes) -> dict:
    vals = struct.unpack(HEAD_FMT, blob[:HEAD_SIZE])
    keys = [
        "grid_cnt", "grid_sgmt_cnt", "link_sgmt_cnt", "link_cnt", "turn_cnt",
        "grid_off", "grid_sz", "grid_sgmt_off", "grid_sgmt_sz",
        "link_sgmt_off", "link_sgmt_sz", "link_off", "link_sz",
        "turn_off", "turn_sz",
    ]
    return dict(zip(keys, vals))


def fmt_link_id(link_id: int) -> str:
    return str(link_id).zfill(10)


def fmt_node_id(node_id: int) -> str:
    return str(node_id).zfill(10)


def decode_name(raw: bytes) -> str | None:
    text = raw.split(b"\x00", 1)[0].decode("utf-8", "replace").strip()
    return text or None


def seg_end(seg: tuple) -> tuple[float, float]:
    _, dw_x, dw_y, w_dir, w_len, _, _ = seg
    lon = dw_x / SCALE
    lat = dw_y / SCALE
    slen = w_len / SCALE
    ang = math.radians(w_dir)
    return lon + slen * math.sin(ang), lat + slen * math.cos(ang)


def build_wkt_from_nodes(st_lon: float, st_lat: float, ed_lon: float, ed_lat: float) -> str:
    return (
        f"SRID=4326;LINESTRING({st_lon:.6f} {st_lat:.6f}, "
        f"{ed_lon:.6f} {ed_lat:.6f})"
    )


def normalize_len(df_len: float) -> float:
    return 0 if df_len <= 0 else round(df_len, 3)


def build_wkt(segs: list[tuple]) -> str | None:
    if not segs:
        return None
    ordered = sorted(segs, key=lambda s: s[6])  # wLenFromLink
    pts: list[tuple[float, float]] = []
    for seg in ordered:
        lon = seg[1] / SCALE
        lat = seg[2] / SCALE
        if not pts or abs(pts[-1][0] - lon) > 1e-9 or abs(pts[-1][1] - lat) > 1e-9:
            pts.append((lon, lat))
    elon, elat = seg_end(ordered[-1])
    if not pts or abs(pts[-1][0] - elon) > 1e-9 or abs(pts[-1][1] - elat) > 1e-9:
        pts.append((elon, elat))
    if len(pts) < 2:
        pts.append((elon, elat))
    coord_txt = ", ".join(f"{lon:.6f} {lat:.6f}" for lon, lat in pts)
    return f"SRID=4326;LINESTRING({coord_txt})"


def load_psf(psf_path: Path) -> tuple[dict, list[tuple], bytes]:
    data = psf_path.read_bytes()
    head = parse_head(data)
    link_sgmt_off = head["link_sgmt_off"]
    link_sgmt_sz = head["link_sgmt_sz"]
    link_off = head["link_off"]
    link_sz = head["link_sz"]

    sgmt_blob = data[link_sgmt_off:link_sgmt_off + link_sgmt_sz]
    link_blob = data[link_off:link_off + link_sz]

    if len(sgmt_blob) % LINK_SGMT_SIZE != 0:
        raise ValueError("link segment blob size mismatch")
    if len(link_blob) % LINK_INFO_SIZE != 0:
        raise ValueError("link info blob size mismatch")

    sgmt_count = len(sgmt_blob) // LINK_SGMT_SIZE
    segs = [
        struct.unpack(LINK_SGMT_FMT, sgmt_blob[i * LINK_SGMT_SIZE:(i + 1) * LINK_SGMT_SIZE])
        for i in range(sgmt_count)
    ]
    return head, segs, link_blob


def iter_rows(link_blob: bytes, segs: list[tuple]):
    link_count = len(link_blob) // LINK_INFO_SIZE
    for i in range(link_count):
        off = i * LINK_INFO_SIZE
        row = struct.unpack(LINK_INFO_FMT, link_blob[off:off + LINK_INFO_SIZE])
        (
            qw_link_id, dw_sgmt_off, w_sgmt_cnt, _dw_turn_off, _n_turn_cnt,
            n_max_speed, df_len, n_road_rank, n_connect, n_road_type, n_lanes,
            sz_road_name, qw_st_node, dw_st_x, dw_st_y, _n_st_type,
            qw_ed_node, dw_ed_x, dw_ed_y, _n_ed_type,
        ) = row

        st_lon = round(dw_st_x / SCALE, 6)
        st_lat = round(dw_st_y / SCALE, 6)
        ed_lon = round(dw_ed_x / SCALE, 6)
        ed_lat = round(dw_ed_y / SCALE, 6)

        link_segs = segs[dw_sgmt_off:dw_sgmt_off + w_sgmt_cnt] if w_sgmt_cnt else []
        wkt = build_wkt(link_segs)
        if wkt is None:
            wkt = build_wkt_from_nodes(st_lon, st_lat, ed_lon, ed_lat)

        yield (
            fmt_link_id(qw_link_id),
            normalize_len(df_len),
            wkt,
            int(n_max_speed) if n_max_speed else None,
            int(n_road_rank),
            int(n_connect),
            int(n_road_type),
            int(n_lanes),
            decode_name(sz_road_name),
            fmt_node_id(qw_st_node),
            st_lon,
            st_lat,
            fmt_node_id(qw_ed_node),
            ed_lon,
            ed_lat,
        )


def read_db_config(ini_path: Path) -> dict:
    cfg = configparser.ConfigParser()
    with ini_path.open(encoding="utf-8-sig") as fp:
        cfg.read_file(fp)
    db = cfg["database"]
    return {
        "host": db.get("host", "127.0.0.1"),
        "port": int(db.get("port", "5432")),
        "dbname": db.get("name", "roadnet"),
        "user": db.get("userid", "postgres"),
        "password": db.get("password", ""),
    }


def connect_db(args):
    try:
        import psycopg2
    except ImportError:
        print("psycopg2 not found. install: pip install psycopg2-binary", file=sys.stderr)
        sys.exit(1)

    if args.dsn:
        return psycopg2.connect(args.dsn)
    if args.user:
        return psycopg2.connect(
            host=args.host,
            port=args.port,
            dbname=args.dbname,
            user=args.user,
            password=args.password,
        )
    if args.config:
        return psycopg2.connect(**read_db_config(args.config))
    return psycopg2.connect(
        host=args.host,
        port=args.port,
        dbname=args.dbname,
        user=args.user,
        password=args.password,
    )


def apply_node_names(cur) -> int:
    cur.execute(
        """
        UPDATE roadnet.prim_link_info AS l
        SET st_nd_name = NULLIF(TRIM(fn.node_name), ''),
            ed_nd_name = NULLIF(TRIM(tn.node_name), '')
        FROM network.moct_node AS fn,
             network.moct_node AS tn
        WHERE fn.node_id = l.st_nd_id
          AND tn.node_id = l.ed_nd_id
        """
    )
    return cur.rowcount


def main() -> int:
    parser = argparse.ArgumentParser(description="Load link.psf into ROADNET.PRIM_LINK_INFO")
    parser.add_argument(
        "--psf",
        default="/home/mytobcom/ruc/MapMatchSvr/bin/link.psf",
        help="path to link.psf",
    )
    parser.add_argument(
        "--config",
        default="/home/mytobcom/ruc/MapMatchSvr/bin/config.ini",
        help="MapMatchSvr config.ini for DB credentials",
    )
    parser.add_argument("--dsn", default="", help="PostgreSQL DSN override")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--dbname", default="roadnet")
    parser.add_argument("--user", default="", help="DB user (e.g. mytobcom)")
    parser.add_argument("--password", default="")
    parser.add_argument("--batch-size", type=int, default=5000)
    parser.add_argument("--truncate", action="store_true", default=True)
    parser.add_argument("--no-truncate", dest="truncate", action="store_false")
    parser.add_argument("--dry-run", action="store_true", help="parse only, no DB write")
    args = parser.parse_args()

    psf_path = Path(args.psf)
    if not psf_path.is_file():
        print(f"PSF not found: {psf_path}", file=sys.stderr)
        return 1

    t0 = time.time()
    print(f"reading {psf_path} ...")
    head, segs, link_blob = load_psf(psf_path)
    print(
        f"links={head['link_cnt']} segments={head['link_sgmt_cnt']} "
        f"parsed segs={len(segs)}"
    )

    rows = list(iter_rows(link_blob, segs))
    print(f"exportable rows={len(rows)} elapsed={time.time()-t0:.1f}s")
    if args.dry_run:
        print("sample:", rows[0])
        return 0

    import psycopg2.extras

    conn = connect_db(args)
    conn.autocommit = False
    try:
        with conn.cursor() as cur:
            if args.truncate:
                cur.execute("TRUNCATE TABLE roadnet.prim_link_info")
            insert_sql = """
                INSERT INTO roadnet.prim_link_info (
                    link_id, len, geom,
                    speed_limit, road_rank, road_connect, road_type, lanes, name,
                    st_nd_id, st_nd_lon, st_nd_lat,
                    ed_nd_id, ed_nd_lon, ed_nd_lat
                ) VALUES %s
            """
            template = "(%s, %s, ST_GeomFromEWKT(%s), %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)"
            total = 0
            for i in range(0, len(rows), args.batch_size):
                batch = rows[i:i + args.batch_size]
                psycopg2.extras.execute_values(
                    cur, insert_sql, batch, template=template, page_size=len(batch)
                )
                total += len(batch)
                if total % 50000 == 0 or total == len(rows):
                    print(f"inserted {total}/{len(rows)}")
            updated = apply_node_names(cur)
            conn.commit()
            print(f"done: inserted={total}, node_name_updated={updated}, elapsed={time.time()-t0:.1f}s")
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
