#!/bin/bash
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../../test_lib.sh
source "$ROOT/test_lib.sh"
engine_stop "$SIM_BIN" "$SIM_STOP_WAIT"
