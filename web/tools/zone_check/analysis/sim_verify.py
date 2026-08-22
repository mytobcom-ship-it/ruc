# -*- coding: utf-8 -*-
"""시뮬레이터 정답(ruc.sim_truth) 기반 독립 검증 (2026-08-23 최정우)

  종전 기준선(groundtruth_draft.json)은 매칭 결과 중 위상적으로 깨끗한 구간을 그대로
  정답으로 채택한 자기 스냅샷이라, 만든 시점의 엔진으로 재면 정의상 100% 가 나온다.
  여기 쓰는 ruc.sim_truth 는 시뮬레이터가 좌표를 만들 때 확정한 값이라 엔진 출력과
  완전히 독립이다 — 절대 정확도를 말할 수 있다.

  출력
    1) 분포 대조   합성이 실주행과 닮았는지. 이게 어긋나면 아래 결론이 전부 무의미하다
    2) 매칭 정확도 참 링크 대비. RAW_VLD 와 무관하게 전량을 대상으로 한다
    3) RAW_VLD 검증  버려진 좌표의 진짜 평면오차와, 강제로 매칭했을 때 맞았는지
    4) ACCURACY_M 신뢰도  단말 보고값이 진짜 오차를 얼마나 예측하는가

  실행: python3 sim_verify.py
"""
import os as _os, statistics, collections, psycopg2

r = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='ruc')
n = psycopg2.connect(host='127.0.0.1', user='mytobcom', password='my664761', dbname='roadnet')
r.set_session(readonly=True); n.set_session(readonly=True)
cr, cn = r.cursor(), n.cursor()


def q(v, p):
    v = sorted(v)
    return v[min(int(len(v) * p), len(v) - 1)] if v else 0.0


cr.execute("""SELECT t.trip_id, t.gps_seq, t.true_link_id, t.offset_m, t.accuracy_m, t.raw_vld,
                     t.drive_status, t.speed_kmh, p.match_status, p.match_link_id, p.accuracy_m
              FROM ruc.sim_truth t JOIN ruc.prim_rawgps p USING (trip_id, gps_seq)""")
rows = cr.fetchall()
if not rows:
    raise SystemExit('ruc.sim_truth 가 비어 있습니다 — 시뮬레이터를 먼저 돌리세요')

print('합성 표본 %d건 (트립 %d개)\n' % (len(rows), len({x[0] for x in rows})))

# ── 1) 분포 대조 ───────────────────────────────────────────────────────────
print('── 1. 실주행 대비 분포 대조 ──')
sim_on = [float(x[3]) for x in rows if x[6] == 0]                # ON_ROAD 진짜 오차
sim_acc = [int(x[4]) for x in rows if x[4] is not None]
sim_vld = sum(1 for x in rows if x[5])

# 실주행 쪽 기준값 — calib_noise.py 가 측정한 값(최근접 링크 거리 대용치)
REAL = {'on_p50': 2.7, 'on_p75': 4.6, 'on_p90': 7.8, 'on_p95': 9.9,
        'acc_p50': 6.0, 'vld_pct': 84.9, 'outlier_pct': 0.83}
print('%-22s %10s %10s' % ('항목', '합성', '실주행'))
if sim_on:
    for lbl, p, key in (('ON_ROAD 오차 중앙', 0.50, 'on_p50'), ('  75분위', 0.75, 'on_p75'),
                        ('  90분위', 0.90, 'on_p90'), ('  95분위', 0.95, 'on_p95')):
        print('%-22s %9.1fm %9.1fm' % (lbl, q(sim_on, p), REAL[key]))
    out = sum(1 for d in sim_on if d > 25.0)
    print('%-22s %9.2f%% %9.2f%%' % ('ON_ROAD 이상치(25m 초과)',
                                     100.0 * out / len(sim_on), REAL['outlier_pct']))
print('%-22s %9.1f  %9.1f' % ('accuracy_m 중앙', q(sim_acc, 0.5) if sim_acc else 0, REAL['acc_p50']))
print('%-22s %9.1f%% %9.1f%%' % ('raw_vld=true 비율', 100.0 * sim_vld / len(rows), REAL['vld_pct']))

# ── 2) 참 링크 대비 매칭 정확도 (RAW_VLD 무관 전량) ────────────────────────
print('\n── 2. 참 링크 대비 매칭 정확도 (raw_vld 무관 전량) ──')
print('%-10s %7s %7s %7s %7s  %s' % ('구분', '전체', '매칭', '일치', '불일치', '정확도'))


def acc_row(label, sel):
    sub = [x for x in rows if sel(x)]
    if not sub:
        return
    m = [x for x in sub if x[8] == 1 and x[9]]
    hit = sum(1 for x in m if x[9] == x[2])
    pct = (100.0 * hit / len(m)) if m else 0.0
    print('%-10s %7d %7d %7d %7d  %5.1f%%' % (label, len(sub), len(m), hit, len(m) - hit, pct))


acc_row('전체', lambda x: True)
acc_row('raw_vld=T', lambda x: x[5])
acc_row('raw_vld=F', lambda x: not x[5])
for ds, nm in ((0, 'ON_ROAD'), (1, 'IDLE'), (2, 'PARKED')):
    acc_row('  ' + nm, lambda x, d=ds: x[6] == d)

# ── 3) RAW_VLD 로 버려진 좌표의 실체 ───────────────────────────────────────
print('\n── 3. raw_vld=false 좌표의 진짜 평면오차 ──')
print('%-10s %7s %8s %8s %8s %8s' % ('상태', '건수', '오차중앙', '90분위', '최대', '20m이내'))
for ds, nm in ((0, 'ON_ROAD'), (1, 'IDLE'), (2, 'PARKED')):
    sub = [float(x[3]) for x in rows if not x[5] and x[6] == ds]
    if not sub:
        continue
    print('%-10s %7d %8.1f %8.1f %8.1f %7.1f%%'
          % (nm, len(sub), statistics.median(sub), q(sub, 0.9), max(sub),
             100.0 * sum(1 for d in sub if d <= 20.0) / len(sub)))

# ── 4) ACCURACY_M 이 진짜 오차를 얼마나 예측하는가 ─────────────────────────
print('\n── 4. accuracy_m 신뢰도 (K = accuracy / 진짜오차) ──')
print('%-10s %7s %8s %8s %8s' % ('상태', '건수', 'K중앙', 'K90분위', '상관'))
for ds, nm in ((0, 'ON_ROAD'), (1, 'IDLE'), (2, 'PARKED')):
    sub = [(float(x[3]), int(x[4])) for x in rows if x[6] == ds and x[4] is not None]
    if len(sub) < 3:
        continue
    K = [a / max(d, 1.0) for d, a in sub]
    dv = [d for d, _ in sub]; av = [float(a) for _, a in sub]
    try:
        corr = statistics.correlation(dv, av)
    except Exception:
        corr = float('nan')
    print('%-10s %7d %8.2f %8.2f %8.2f' % (nm, len(sub), statistics.median(K), q(K, 0.9), corr))

# ── 5) 불일치 상세 (상위 15건) ─────────────────────────────────────────────
bad = [x for x in rows if x[8] == 1 and x[9] and x[9] != x[2]]
if bad:
    print('\n── 5. 오매칭 %d건 (상위 15) ──' % len(bad))
    print('%-32s %5s %-11s %-11s %7s %6s %5s' %
          ('trip_id', 'seq', '참링크', '매칭링크', '진짜오차', '정확도', 'vld'))
    for x in sorted(bad, key=lambda y: -float(y[3]))[:15]:
        print('%-32s %5d %-11s %-11s %6.1fm %6s %5s'
              % (x[0], x[1], x[2], x[9], float(x[3]), x[4], 'T' if x[5] else 'F'))
