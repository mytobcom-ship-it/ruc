#!/bin/bash
# link.psf 안전 배포 — chattr +i(불변 속성)로 오삭제 방지 걸린 link.psf를 교체할 때 이 스크립트만 사용.
#   맨손으로 rm/cp 하면 chattr -i 없이 rm이 "명령을 허용하지 않음"으로 실패하거나,
#   -i 를 풀고 나서 재설정을 깜빡해 보호가 다시 풀린 채로 남을 수 있음 (2026-08-21 최정우 추가 —
#   link.psf 반복 소실 사고[수동 rm/rm *.psf, ~/.bash_history 확인됨] 재발방지책)
# 사용법: ./update_link_psf.sh [교체할 원본 psf 경로]  (기본값: ../../CreateData/bin/link.psf)
set -e

BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="$BIN_DIR/link.psf"
SOURCE="${1:-$BIN_DIR/../../CreateData/bin/link.psf}"

if [ ! -f "$SOURCE" ]; then
	echo "오류: 원본 파일 없음 - $SOURCE"
	exit 1
fi

echo "1) 불변 속성 해제 ($TARGET)..."
sudo chattr -i "$TARGET" 2>/dev/null || true

echo "2) 교체: $SOURCE -> $TARGET"
if ! cp "$SOURCE" "$TARGET"; then
	echo "오류: 복사 실패 — 불변 속성 재설정 후 중단"
	sudo chattr +i "$TARGET" 2>/dev/null || true
	exit 1
fi

echo "3) 불변 속성 재설정..."
sudo chattr +i "$TARGET"

lsattr "$TARGET"
echo "완료 — MapMatchSvr에 반영하려면 ./test_svr.sh mm-restart 실행"
