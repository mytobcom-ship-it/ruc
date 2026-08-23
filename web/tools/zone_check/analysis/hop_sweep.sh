#!/bin/bash
# hoppenalty_lenratio 스윕 — 지선 누락과 우회 매칭을 동시에 본다 (2026-08-23 최정우)
#   hoppenalty 는 우회 매칭을 막으려고 넣은 장치라, 지선 누락이 줄어도 우회가 늘면 탈락이다.
#   실행: bash hop_sweep.sh "0.0 0.3 0.5 0.8"
cd /home/mytobcom/ruc
for R in ${1:-"0.0 0.3 0.5 0.8"}; do
  python3 - "$R" <<'PY'
import io,re,sys
p='MapMatchSvr/bin/config.ini'; s=io.open(p,encoding='utf-8').read()
io.open(p,'w',encoding='utf-8').write(re.sub(r'\nhoppenalty_lenratio=[\d.]+','\nhoppenalty_lenratio='+sys.argv[1],s))
PY
  PID=$(ps -eo pid,args | awk '$NF=="./MapMatchSvr" {print $1;exit}'); [ -n "$PID" ] && kill "$PID"; sleep 3
  PGPASSWORD=my664761 psql -h 127.0.0.1 -U mytobcom -d ruc -q -c "
    DELETE FROM ruc.prim_chargehand;
    UPDATE ruc.prim_rawgps SET match_status=0, match_lat=NULL, match_lon=NULL,
           match_link_id=NULL, intersect_len=0;" >/dev/null
  (cd MapMatchSvr/bin && setsid ./MapMatchSvr >/dev/null 2>&1 </dev/null &)
  until [ "$(PGOPTIONS='-c default_transaction_read_only=on' PGPASSWORD=my664761 psql -h 127.0.0.1 -U mytobcom -d ruc -Atc 'SELECT count(*) FROM ruc.prim_rawgps WHERE match_status IN (0,2)')" = "0" ]; do sleep 3; done
  echo "═══ hoppenalty_lenratio=$R ═══"
  python3 web/tools/zone_check/analysis/crossover_metric.py
  python3 web/tools/zone_check/analysis/check_accuracy.py 2>&1 | tail -1
  PGOPTIONS='-c default_transaction_read_only=on' PGPASSWORD=my664761 psql -h 127.0.0.1 -U mytobcom -d ruc -Atc \
    "SELECT '  실주행 매칭 '||count(*) FILTER (WHERE match_status=1)||' · 과금 '||
     (SELECT count(*) FROM ruc.prim_chargehand WHERE device_key NOT LIKE '9%')||'건'
     FROM ruc.prim_rawgps WHERE trip_id NOT LIKE '000093%' AND trip_id NOT LIKE '9%'"
  echo
done
