#!/bin/bash
# ./test_svr.sh ps 와 동일 (하위 호환)
exec "$(cd "$(dirname "$0")" && pwd)/test_svr.sh" ps "$@"
