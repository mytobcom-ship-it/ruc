#!/usr/bin/env bash
# RawGpsSimSvr 데몬 종료 (SIGTERM → 잔여 버퍼 flush 후 정상 종료)
pkill -TERM -f RawGpsSimSvr

sleep 2

ps -ef | grep RawGpsSimSvr | grep -v grep
