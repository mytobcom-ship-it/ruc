#!/bin/bash
# maxstep 값을 바꿔가며 재매칭하고 지표를 뽑는다
MS=$1
cd /home/mytobcom/ruc
python3 - "$MS" <<'PY'
import io,sys
p='MapMatchSvr/bin/config.ini'; s=io.open(p,encoding='utf-8').read()
import re
s=re.sub(r'\nmaxstep=\d+', '\nmaxstep=%s'%sys.argv[1], s, count=1)
io.open(p,'w',encoding='utf-8').write(s)
PY
PID=$(ps -eo pid,args | awk '$NF=="./MapMatchSvr" {print $1; exit}'); [ -n "$PID" ] && kill "$PID"
sleep 3
PGPASSWORD=my664761 psql -h 127.0.0.1 -U mytobcom -d ruc -q -c "
DELETE FROM ruc.prim_chargehand;
UPDATE ruc.prim_rawgps SET match_status=0, match_lat=NULL, match_lon=NULL, match_link_id=NULL, intersect_len=0;" >/dev/null
cd /home/mytobcom/ruc/MapMatchSvr/bin && nohup ./MapMatchSvr > /dev/null 2>&1 &
until [ "$(PGOPTIONS='-c default_transaction_read_only=on' PGPASSWORD=my664761 psql -h 127.0.0.1 -U mytobcom -d ruc -Atc 'SELECT count(*) FROM ruc.prim_rawgps WHERE match_status=0')" = "0" ]; do sleep 3; done
echo "maxstep=$MS 재매칭 완료"
