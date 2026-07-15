#!/usr/bin/env python3
"""
roadnet 데이터 import (통합)
  - MOCT_NODE.shp / MOCT_LINK.shp → network.moct_node / moct_link
  - MULTILINK.dbf → network.multilink
  - TURNINFO.dbf  → network.turn_info

의존: psycopg2-binary (pip install psycopg2-binary)
"""

from __future__ import annotations

import argparse
import io
import struct
import sys
from pathlib import Path

# 원본 Shapefile 좌표계: EPSG:5186 (신규 표준노드링크)
# DB 저장: EPSG:4326 (WGS84GEO). 원본 5186 → import 시 ST_Transform 변환 적재
SRID_SRC = 5186
SRID_DST = 4326
COORD_GRID = 0.000001  # WGS84 변환 시 소수 6자리


def geom_expr() -> str:
    if SRID_SRC == SRID_DST:
        return "ST_SetSRID(ST_GeomFromText(geom_wkt), %s)"
    return "ST_ReducePrecision(ST_Transform(ST_SetSRID(ST_GeomFromText(geom_wkt), %s), %s), %s)"


def geom_params() -> tuple:
    if SRID_SRC == SRID_DST:
        return (SRID_SRC,)
    return (SRID_SRC, SRID_DST, COORD_GRID)


def read_dbf(path: Path) -> list[dict[str, str]]:
    with open(path, "rb") as f:
        header = f.read(32)
        num_records = struct.unpack("<I", header[4:8])[0]
        header_len = struct.unpack("<H", header[8:10])[0]
        record_len = struct.unpack("<H", header[10:12])[0]
        f.seek(32)
        fields: list[tuple[str, str, int]] = []
        for _ in range((header_len - 33) // 32):
            fd = f.read(32)
            name = fd[:11].split(b"\x00")[0].decode("ascii", errors="replace")
            ftype = chr(fd[11])
            flen = fd[16]
            fields.append((name, ftype, flen))
        rows: list[dict[str, str]] = []
        f.seek(header_len)
        for _ in range(num_records):
            rec = f.read(record_len)
            if not rec or rec[0:1] == b"\x1a":
                break
            pos = 1
            row: dict[str, str] = {}
            for name, ftype, flen in fields:
                raw = rec[pos : pos + flen]
                pos += flen
                if ftype in ("N", "F"):
                    val = raw.decode("ascii", errors="replace").strip()
                else:
                    try:
                        val = raw.decode("euc-kr").strip()
                    except UnicodeDecodeError:
                        val = raw.decode("ascii", errors="replace").strip()
                row[name.lower()] = val
            rows.append(row)
    return rows


def read_shapes(path: Path) -> list[object | None]:
    with open(path, "rb") as f:
        f.seek(100)
        geoms: list[object | None] = []
        while True:
            rec_header = f.read(8)
            if len(rec_header) < 8:
                break
            _, content_len = struct.unpack(">ii", rec_header)
            if content_len <= 0:
                break
            content = f.read(content_len * 2)
            if len(content) < 4:
                break
            shp_type = struct.unpack("<i", content[:4])[0]
            if shp_type == 0:
                geoms.append(None)
            elif shp_type == 1:
                x, y = struct.unpack("<dd", content[4:20])
                geoms.append((x, y))
            elif shp_type == 3:
                num_parts, num_points = struct.unpack("<ii", content[36:44])
                parts = list(struct.unpack(f"<{num_parts}i", content[44 : 44 + 4 * num_parts]))
                pts_off = 44 + 4 * num_parts
                coords = struct.unpack(f"<{num_points * 2}d", content[pts_off : pts_off + num_points * 16])
                points = list(zip(coords[0::2], coords[1::2]))
                if num_parts == 1:
                    geoms.append(points)
                else:
                    lines = []
                    for i, start in enumerate(parts):
                        end = parts[i + 1] if i + 1 < num_parts else num_points
                        lines.append(points[start:end])
                    geoms.append(max(lines, key=len))
            else:
                geoms.append(None)
    return geoms


def wkt_point(x: float, y: float) -> str:
    return f"POINT({x} {y})"


def wkt_line(points: list[tuple[float, float]]) -> str:
    coords = ", ".join(f"{x} {y}" for x, y in points)
    return f"LINESTRING({coords})"


def to_int(val: str | None) -> int | None:
    if val in (None, ""):
        return None
    return int(float(val))


def to_num(val: str | None) -> str | None:
    if val in (None, ""):
        return None
    return val


def pg_connect(host: str, port: str, dbname: str, user: str, password: str):
    import psycopg2

    kwargs = {"dbname": dbname, "user": user, "password": password or None}
    if host:
        kwargs["host"] = host
        kwargs["port"] = port
    return psycopg2.connect(**kwargs)


def copy_stage(cur, table: str, columns: list[str], rows: list[tuple]) -> None:
    buf = io.StringIO()
    for row in rows:
        buf.write("\t".join("" if v is None else str(v).replace("\t", " ").replace("\n", " ") for v in row))
        buf.write("\n")
    buf.seek(0)
    cur.copy_from(buf, table, columns=columns, null="")


def import_node(conn, shp: Path) -> int:
    rows = read_dbf(shp.with_suffix(".dbf"))
    geoms = read_shapes(shp)
    if len(rows) != len(geoms):
        raise RuntimeError(f"Record count mismatch: dbf={len(rows)} shp={len(geoms)}")

    cur = conn.cursor()
    cur.execute("SET search_path TO network, public")
    cur.execute("TRUNCATE network.moct_node CASCADE")
    cur.execute("""
        CREATE TEMP TABLE stg_moct_node (
            node_id varchar(10), node_type varchar(3), node_name varchar(50),
            turn_p varchar(1), updatedate varchar(8), remark varchar(30),
            hist_type varchar(8), histremark varchar(30), geom_wkt text
        ) ON COMMIT DROP
    """)
    stage_rows = [
        (
            row.get("node_id"),
            row.get("node_type") or None,
            row.get("node_name") or None,
            row.get("turn_p") or None,
            row.get("updatedate") or None,
            row.get("remark") or None,
            row.get("hist_type") or None,
            row.get("histremark") or None,
            wkt_point(*geom) if geom else None,
        )
        for row, geom in zip(rows, geoms)
    ]
    copy_stage(
        cur, "stg_moct_node",
        ["node_id", "node_type", "node_name", "turn_p", "updatedate", "remark", "hist_type", "histremark", "geom_wkt"],
        stage_rows,
    )
    cur.execute(f"""
        INSERT INTO network.moct_node
            (node_id, node_type, node_name, turn_p, updatedate, remark, hist_type, histremark, geom)
        SELECT node_id, node_type, node_name, turn_p, updatedate, remark, hist_type, histremark,
               {geom_expr()}
        FROM stg_moct_node WHERE geom_wkt IS NOT NULL
    """, geom_params())
    conn.commit()
    cur.close()
    return len(stage_rows)


def import_link(conn, shp: Path) -> int:
    rows = read_dbf(shp.with_suffix(".dbf"))
    geoms = read_shapes(shp)
    if len(rows) != len(geoms):
        raise RuntimeError(f"Record count mismatch: dbf={len(rows)} shp={len(geoms)}")

    cur = conn.cursor()
    cur.execute("SET search_path TO network, public")
    cur.execute("TRUNCATE network.moct_link CASCADE")
    cur.execute("""
        CREATE TEMP TABLE stg_moct_link (
            link_id varchar(10), f_node varchar(10), t_node varchar(10),
            lanes integer, road_rank varchar(3), road_type varchar(3),
            road_no varchar(5), road_name varchar(30), road_use varchar(1),
            multi_link varchar(1), connect varchar(3), max_spd integer,
            rest_veh varchar(3), rest_w integer, rest_h integer, c_its varchar(1),
            length numeric(18,12), updatedate varchar(8), remark varchar(30),
            hist_type varchar(8), histremark varchar(30), geom_wkt text
        ) ON COMMIT DROP
    """)
    stage_rows = [
        (
            row.get("link_id"), row.get("f_node") or None, row.get("t_node") or None,
            to_int(row.get("lanes")), row.get("road_rank") or None, row.get("road_type") or None,
            row.get("road_no") or None, row.get("road_name") or None, row.get("road_use") or None,
            row.get("multi_link") or None, row.get("connect") or None, to_int(row.get("max_spd")),
            row.get("rest_veh") or None, to_int(row.get("rest_w")), to_int(row.get("rest_h")),
            row.get("c-its") or None, to_num(row.get("length")), row.get("updatedate") or None,
            row.get("remark") or None, row.get("hist_type") or None, row.get("histremark") or None,
            wkt_line(geom) if geom else None,
        )
        for row, geom in zip(rows, geoms)
    ]
    copy_stage(
        cur, "stg_moct_link",
        [
            "link_id", "f_node", "t_node", "lanes", "road_rank", "road_type", "road_no", "road_name",
            "road_use", "multi_link", "connect", "max_spd", "rest_veh", "rest_w", "rest_h", "c_its",
            "length", "updatedate", "remark", "hist_type", "histremark", "geom_wkt",
        ],
        stage_rows,
    )
    cur.execute(f"""
        INSERT INTO network.moct_link
            (link_id, f_node, t_node, lanes, road_rank, road_type, road_no, road_name,
             road_use, multi_link, connect, max_spd, rest_veh, rest_w, rest_h, c_its,
             length, updatedate, remark, hist_type, histremark, geom)
        SELECT link_id, f_node, t_node, lanes, road_rank, road_type, road_no, road_name,
               road_use, multi_link, connect, max_spd, rest_veh, rest_w, rest_h, c_its,
               length, updatedate, remark, hist_type, histremark,
               {geom_expr()}
        FROM stg_moct_link WHERE geom_wkt IS NOT NULL
    """, geom_params())
    conn.commit()
    cur.close()
    return len(stage_rows)


def import_multilink(conn, dbf: Path) -> int:
    rows = read_dbf(dbf)
    cur = conn.cursor()
    cur.execute("SET search_path TO network, public")
    cur.execute("TRUNCATE network.multilink")
    batch = [
        (
            r.get("link_id"),
            int(r["multi_id"]) if r.get("multi_id") else None,
            r.get("road_rank") or None, r.get("road_type") or None,
            r.get("road_no") or None, r.get("road_name") or None, r.get("remark") or None,
        )
        for r in rows
    ]
    cur.executemany("""
        INSERT INTO network.multilink
            (link_id, multi_id, road_rank, road_type, road_no, road_name, remark)
        VALUES (%s, %s, %s, %s, %s, %s, %s)
    """, batch)
    conn.commit()
    cur.close()
    return len(batch)


def import_turninfo(conn, dbf: Path) -> int:
    rows = read_dbf(dbf)
    cur = conn.cursor()
    cur.execute("SET search_path TO network, public")
    cur.execute("TRUNCATE network.turn_info")
    batch = [
        (
            r.get("node_id"),
            int(r["turn_id"]) if r.get("turn_id") else None,
            r.get("st_link"), r.get("ed_link"),
            r.get("turn_type") or None, r.get("turn_oper") or None, r.get("remark") or None,
        )
        for r in rows
    ]
    cur.executemany("""
        INSERT INTO network.turn_info
            (node_id, turn_id, st_link, ed_link, turn_type, turn_oper, remark)
        VALUES (%s, %s, %s, %s, %s, %s, %s)
    """, batch)
    conn.commit()
    cur.close()
    return len(batch)


def print_counts(conn) -> None:
    cur = conn.cursor()
    cur.execute("""
        SELECT 'moct_node' AS tbl, count(*)::text FROM network.moct_node
        UNION ALL SELECT 'moct_link', count(*)::text FROM network.moct_link
        UNION ALL SELECT 'multilink', count(*)::text FROM network.multilink
        UNION ALL SELECT 'turn_info', count(*)::text FROM network.turn_info
    """)
    print("\n=== import 결과 ===")
    for tbl, cnt in cur.fetchall():
        print(f"  {tbl}: {int(cnt):,}")
    cur.close()


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="roadnet MOCT 데이터 import")
    parser.add_argument("--host", default="")
    parser.add_argument("--port", default="5432")
    parser.add_argument("--dbname", default="roadnet")
    parser.add_argument("--user", default="postgres")
    parser.add_argument("--password", default="")
    parser.add_argument("--data-dir", default=str(root / "CreateData" / "data"))
    parser.add_argument("--turninfo", default=str(root / "CreateData" / "data" / "TURNINFO.dbf"))
    parser.add_argument("--dbf-only", action="store_true", help="multilink/turn_info 만 import (ogr2ogr 후)")
    args = parser.parse_args()

    try:
        import psycopg2  # noqa: F401
    except ImportError:
        print("psycopg2 required: pip install psycopg2-binary", file=sys.stderr)
        return 1

    data = Path(args.data_dir)
    turninfo = Path(args.turninfo)
    conn = pg_connect(args.host, args.port, args.dbname, args.user, args.password)

    if not args.dbf_only:
        print("Importing moct_node ...")
        n = import_node(conn, data / "MOCT_NODE.shp")
        print(f"  {n:,} rows")
        print("Importing moct_link ...")
        n = import_link(conn, data / "MOCT_LINK.shp")
        print(f"  {n:,} rows")

    print("Importing multilink ...")
    n = import_multilink(conn, data / "MULTILINK.dbf")
    print(f"  {n:,} rows")

    print("Importing turn_info ...")
    n = import_turninfo(conn, turninfo)
    print(f"  {n:,} rows")

    print_counts(conn)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
