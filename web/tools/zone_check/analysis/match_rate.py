# -*- coding: utf-8 -*-
"""매칭률 지표 — 실주행 트립 기준 (2026-08-23 최정우)

  000093_* 는 제외한다. 방위 편차 0.00~0.04도·간격 편차 0.00~0.06m 인 직선·등간격 합성
  좌표열이라 도로를 따라가지 않는다(미매칭 171건 중 122건이 radius=50 밖, 평균 107m).
  이걸 분모에 넣으면 엔진 성능이 실제보다 낮게 나온다. 근거는 check_accuracy.py 주석 참고.

  분모도 나눠서 본다 — "도로에서 멀리 떨어져 주차한 좌표"를 매칭 실패로 세면 지표가 왜곡된다.
"""
import psycopg2
# 시뮬레이터 합성 트립(device_key 가 '9' 로 시작)도 제외 — 자세한 이유는 check_accuracy.py 주석
EXCLUDE = "trip_id NOT LIKE '000093%%' AND trip_id NOT LIKE '9%%'"
cn = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
cn.set_session(readonly=True); cu = cn.cursor()

cu.execute("""
  WITH p AS (
    SELECT match_status, raw_vld, gps_lat, gps_lon,
      (SELECT ST_Distance(l.geom, ST_Transform(ST_SetSRID(ST_MakePoint(gps_lon,gps_lat),4326),5186))
       FROM network.moct_link l
       ORDER BY l.geom <-> ST_Transform(ST_SetSRID(ST_MakePoint(gps_lon,gps_lat),4326),5186) LIMIT 1) d
    FROM ruc.prim_rawgps WHERE """ + EXCLUDE + """ AND gps_lat IS NOT NULL)
  SELECT count(*),
         count(*) FILTER (WHERE match_status=1),
         count(*) FILTER (WHERE NOT raw_vld),
         count(*) FILTER (WHERE raw_vld),
         count(*) FILTER (WHERE raw_vld AND d <= 50),
         count(*) FILTER (WHERE raw_vld AND d <= 50 AND match_status=1)
  FROM p""")
tot, m, novld, vld, inr, inr_m = cu.fetchone()
print('실주행 트립(000093·합성 제외) 매칭률')
print('  전체 좌표                  %5d' % tot)
print('  매칭 성공                  %5d   (%.1f%%)' % (m, 100.0*m/max(1,tot)))
print('  raw_vld=false (측위 실패)  %5d' % novld)
print('  판정 가능(raw_vld=true)    %5d   → 매칭률 %.1f%%' % (vld, 100.0*m/max(1,vld)))
print('  그중 반경 50m 이내         %5d   → 매칭률 %.1f%%' % (inr, 100.0*inr_m/max(1,inr)))
print()
cu.execute("SELECT count(DISTINCT trip_id) FROM ruc.prim_rawgps WHERE " + EXCLUDE)
print('  대상 트립 %d개' % cu.fetchone()[0])
