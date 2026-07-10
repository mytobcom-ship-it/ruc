#!/bin/bash
exec "$(cd "$(dirname "$0")/../.." && pwd)/test_kill.sh" "$@"
