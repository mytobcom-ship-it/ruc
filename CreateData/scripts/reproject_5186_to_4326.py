#!/usr/bin/env python3
# MOCT shapefile geometry 5186 -> 4326 재투영 (geometry 만, DBF 는 원본 그대로 사용)
#   목적: 엔진 바이너리(link.psf)를 웹(prim_link_info)과 동일한 proj 변환 좌표로 생성
#         → CreateData coordtype=1(WGS84GEO)로 재변환 없이 사용, 변환 구현 차이 제거
#   주의: DBF(CP949, 도로명 등)는 디코딩하지 않고 원본 .dbf 를 그대로 복사해 인덱스 정렬 유지
# 사용: python reproject_5186_to_4326.py <in.shp> <out.shp>
import sys
import shapefile
from pyproj import Transformer

SRC_EPSG = 5186   # Korea 2000 / Central Belt 2010 (MOCT 원본)
DST_EPSG = 4326   # WGS84 경위도 (웹/prim_link_info 와 동일)


def main():
    in_path, out_path = sys.argv[1], sys.argv[2]
    tr = Transformer.from_crs(SRC_EPSG, DST_EPSG, always_xy=True)

    r = shapefile.Reader(in_path)
    shp_type = r.shapeType
    # .shp/.shx 만 기록(.dbf 미생성) → 원본 .dbf 를 그대로 복사해 사용(인덱스 정렬 유지)
    out_base = out_path[:-4] if out_path.lower().endswith(".shp") else out_path
    w = shapefile.Writer(shp=out_base + ".shp", shx=out_base + ".shx", shapeType=shp_type)

    is_point = shp_type in (1, 11, 21)  # Point / PointZ / PointM
    n = r.numShapes if hasattr(r, "numShapes") else r.numRecords
    print("reproject %s (%d shapes, type=%d) -> %s" % (in_path, n, shp_type, out_path), flush=True)

    cnt = 0
    for shape in r.iterShapes():
        pts = shape.points
        if pts:
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            lon, lat = tr.transform(xs, ys)
            new_pts = list(zip(lon, lat))
        else:
            new_pts = []

        if is_point:
            if new_pts:
                w.point(new_pts[0][0], new_pts[0][1])
            else:
                w.null()
        else:
            parts = list(shape.parts) if shape.parts else [0]
            segs = []
            for k in range(len(parts)):
                s = parts[k]
                e = parts[k + 1] if k + 1 < len(parts) else len(new_pts)
                if e > s:
                    segs.append(new_pts[s:e])
            if segs:
                w.line(segs)
            else:
                w.null()

        cnt += 1
        if cnt % 200000 == 0:
            print("  ... %d / %d" % (cnt, n), flush=True)

    w.close()
    r.close()
    print("done: %s (%d shapes)" % (out_path, cnt), flush=True)


if __name__ == "__main__":
    main()
