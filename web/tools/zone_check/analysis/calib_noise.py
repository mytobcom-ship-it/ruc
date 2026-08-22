# -*- coding: utf-8 -*-
"""시뮬레이터 노이즈 모델 실측 보정용 측정 (2026-08-23 최정우)

  왜 필요한가
    시뮬레이터는 accuracy_m 을 `실제 오프셋 + N(0,2)` 로 만든다. 즉 단말이 자기 오차를
    거의 정확히 안다고 가정한다. 그런데 실측은 그렇지 않다 — raw_vld=false 구간에서
    accuracy_m 이 실제 이격의 4~11배로 과장돼 있다. 이대로 합성하면 "정확도 기반 판단이
    잘 통한다"는 낙관적 결론이 나온다.

    여기서 상태별 과장 배수 K = accuracy_m / 실제이격 의 분포를 뽑아, 시뮬레이터가
    accuracy_m = 오프셋 x K (K 는 실측 분포에서 추출) 로 만들도록 보정 근거를 만든다.

  실제 이격의 대용치
    최근접 링크까지의 거리를 쓴다. 차가 도로 위에 있을 때(DRIVE_STATUS=0 ON_ROAD)만
    유효한 대용치다 — 정차 중이면 주차장·부지 안이라 이격이 커도 GPS 오차가 아니다.
    그래서 K 분포는 ON_ROAD 로 적합하고, 서행/정차는 참고값으로만 출력한다.
    000093_* 합성 좌표열과 시뮬레이터 트립('9' 로 시작)은 제외한다 — 실주행만으로 보정해야 한다.
"""
import os as _os, json, statistics, collections, psycopg2

WORK = _os.environ.get('ZONE_CHECK_WORK',
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'work')) + '/'
_os.makedirs(WORK, exist_ok=True)

r = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
n = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='roadnet')
r.set_session(readonly=True); n.set_session(readonly=True)
cr, cn = r.cursor(), n.cursor()

cr.execute("""SELECT gps_lon, gps_lat, raw_vld, drive_status, accuracy_m, speed_kmh
              FROM ruc.prim_rawgps
              WHERE trip_id NOT LIKE '000093%%' AND trip_id NOT LIKE '9%%'
                AND gps_lon IS NOT NULL AND gps_lat IS NOT NULL AND accuracy_m IS NOT NULL""")
rows = cr.fetchall()

def nearest(lon, lat):
    cn.execute("""SELECT MIN(ST_Distance(ST_SetSRID(ST_MakePoint(%s,%s),4326)::geography,
                                ST_Transform(geom,4326)::geography))
                  FROM network.moct_link
                  WHERE ST_DWithin(ST_Transform(ST_SetSRID(ST_MakePoint(%s,%s),4326),5186), geom, 500)""",
               (lon, lat, lon, lat))
    v = cn.fetchone()[0]
    return None if v is None else float(v)

data = collections.defaultdict(list)          # drive_status -> [(이격, accuracy_m, speed)]
for lon, lat, vld, ds, acc, spd in rows:
    d = nearest(lon, lat)
    if d is None:
        continue
    data[ds].append((d, float(acc), spd))

NM = {0: 'ON_ROAD', 1: 'IDLE', 2: 'PARKED'}
def q(v, p):
    v = sorted(v); return v[min(int(len(v) * p), len(v) - 1)] if v else 0.0

print('── 상태별 (이격 대용치, accuracy_m) ──')
print('%-8s %6s %8s %8s %8s %8s' % ('상태', '건수', '이격중앙', '정확도중앙', 'K중앙', 'K 90분위'))
calib = {}
for ds in sorted(data):
    v = data[ds]
    dev = [x[0] for x in v]; acc = [x[1] for x in v]
    # K = accuracy_m / 이격 — 이격이 아주 작으면 발산하므로 하한 1m 로 클램프
    K = [a / max(d, 1.0) for d, a, _ in v]
    print('%-8s %6d %8.1f %8.1f %8.2f %8.2f'
          % (NM.get(ds, ds), len(v), statistics.median(dev), statistics.median(acc),
             statistics.median(K), q(K, 0.9)))
    calib[str(ds)] = {'n': len(v), 'dev_median': round(statistics.median(dev), 2),
                      'acc_median': round(statistics.median(acc), 2),
                      'K_p10': round(q(K, 0.10), 3), 'K_p25': round(q(K, 0.25), 3),
                      'K_median': round(statistics.median(K), 3),
                      'K_p75': round(q(K, 0.75), 3), 'K_p90': round(q(K, 0.90), 3)}

print('\n── ON_ROAD 만: 이격 분위수 (시뮬레이터 sigma 보정용) ──')
on = sorted(x[0] for x in data.get(0, []))
if on:
    for p in (0.5, 0.75, 0.9, 0.95, 0.99):
        print('  %4.0f%% : %6.1f m' % (p * 100, q(on, p)))
    print('  최대   : %6.1f m' % on[-1])
    # 반절정규분포 가정 시 sigma = 중앙값 / 0.6745
    calib['onroad_sigma_est'] = round(statistics.median(on) / 0.6745, 2)
    print('  → 반정규 가정 sigma 추정 %.2f m (현재 config sigma_m=4.0)' % calib['onroad_sigma_est'])

print('\n── accuracy_m 분포 (raw_vld 경계 재확인) ──')
allacc = sorted(float(x[1]) for v in data.values() for x in v)
print('  중앙 %.0f · 90분위 %.0f · 최대 %.0f · 15 이하 비율 %.1f%%'
      % (q(allacc, 0.5), q(allacc, 0.9), allacc[-1],
         100.0 * sum(1 for a in allacc if a <= 15) / len(allacc)))
calib['acc_le15_pct'] = round(100.0 * sum(1 for a in allacc if a <= 15) / len(allacc), 1)

# 이상치(멀티패스) 비율 — ON_ROAD 인데 이격 25m 초과
if on:
    out = sum(1 for d in on if d > 25.0)
    calib['onroad_outlier_pct'] = round(100.0 * out / len(on), 2)
    print('\n── ON_ROAD 이상치(이격 25m 초과) %d건 / %d건 = %.2f%% (현재 config outlier_prob=0.03)'
          % (out, len(on), 100.0 * out / len(on)))

json.dump(calib, open(WORK + 'noise_calib.json', 'w'), ensure_ascii=False, indent=1)
print('\nwork/noise_calib.json 저장')
