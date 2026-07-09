#!/usr/bin/env bash
# RawGpsSimSvr 데몬 상태 확인
ps -ef | grep RawGpsSimSvr | grep -v grep
