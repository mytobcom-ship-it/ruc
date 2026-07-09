#!/usr/bin/env bash
# RawGpsSimSvr 데몬 실행 (실행 디렉토리 = bin)
cd "$(dirname "$0")" || exit 1
path=$(pwd -P)
nohup "$path/RawGpsSimSvr" >/dev/null 2>&1 &
echo "RawGpsSimSvr started (pid=$!)"
