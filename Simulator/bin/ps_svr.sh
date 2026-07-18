#!/bin/bash
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../../test_lib.sh
source "$ROOT/test_lib.sh"
engine_ps_line "$SIM_BIN"
