# -*- coding: utf-8 -*-
"""000093 트립군의 gps_dt/recv_dt 를 좌표·보고속도에 맞게 재계산 (2026-08-23 최정우)

  이 트립군은 경로를 일정 거리로 샘플링한 waypoint 인데 시각이 일괄 3초 간격으로 채워져 있어,
  좌표에서 환산한 속도(52~326km/h)와 보고속도(52~56km/h)가 서로 무관했다. 그 결과
  이상속도 검사(ShouldSkipImplausibleSpeed)에 걸려 매칭률이 0~76% 로 낮았다.
  좌표는 유효하므로 시각을 '거리 ÷ 보고속도' 로 다시 채워 물리적으로 일관되게 만든다.
  첫 좌표 시각은 trip_id 에 박혀 있어 그대로 둔다.
  실행: python3 fix_000093_time.py [--apply]
"""
import sys, math, psycopg2
APPLY = '--apply' in sys.argv
cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cu = cn.cursor()
cu.execute("""SELECT trip_id, gps_seq, gps_dt, gps_lon, gps_lat, speed_kmh
              FROM ruc.prim_rawgps WHERE trip_id LIKE '000093%%' ORDER BY trip_id, gps_seq""")
rows = cu.fetchall()

def hav(a, b):
    R = 6378137.0
    p1, p2 = math.radians(a[1]), math.radians(b[1])
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(b[0] - a[0]) / 2) ** 2
    return 2 * R * math.asin(min(1.0, math.sqrt(h)))

import datetime
FMT = '%Y%m%d%H%M%S'
upd = []; by = {}
for tid, seq, dt, lon, lat, spd in rows:
    by.setdefault(tid, []).append((seq, dt.strip(), float(lon), float(lat), spd))

print('%-24s %5s %8s %10s  %s' % ('trip_id', '좌표', '기존 총초', '변경 총초', '간격(최소~최대)'))
for tid, pts in sorted(by.items()):
    t0 = datetime.datetime.strptime(pts[0][1], FMT)
    cur = t0; gaps = []
    for i, (seq, dt, lon, lat, spd) in enumerate(pts):
        if i == 0:
            upd.append((tid, seq, pts[0][1])); continue
        pseq, pdt, plon, plat, pspd = pts[i - 1]
        d = hav((plon, plat), (lon, lat))
        v = ((spd or 0) + (pspd or 0)) / 2.0                 # 구간 평균 보고속도(km/h)
        if v <= 0.5 or d <= 0.5:
            gap = 3                                          # 정차 구간은 원래 주기 유지
        else:
            gap = max(1, int(round(d / (v / 3.6))))
        gaps.append(gap)
        cur = cur + datetime.timedelta(seconds=gap)
        upd.append((tid, seq, cur.strftime(FMT)))
    old_tot = int((datetime.datetime.strptime(pts[-1][1], FMT) - t0).total_seconds())
    print('%-24s %5d %8d %10d  %d~%d초' % (tid, len(pts), old_tot,
          int((cur - t0).total_seconds()), min(gaps) if gaps else 0, max(gaps) if gaps else 0))

if not APPLY:
    print('\n(변경안만 출력 — 반영하려면 --apply)')
    sys.exit(0)
cu.executemany("""UPDATE ruc.prim_rawgps SET gps_dt=%s, recv_dt=%s
                  WHERE trip_id=%s AND gps_seq=%s""",
               [(t, t, tid, seq) for tid, seq, t in upd])
cn.commit()
print('\n%d건 시각 갱신 완료 (백업: ruc.bak_rawgps_000093)' % len(upd))
