#!/usr/bin/env bash
# Mirror this tree from wherever it lives into a Linux-native filesystem and
# run a make target there.
#
# Why a copy at all?  The tests exercise flock(), file ownership, socket
# permissions and /proc accounting.  /mnt/c is drvfs: it has no real Unix
# ownership or locking semantics, so a build and test run there would be
# meaningless.  Everything is therefore mirrored to $HOME first.
#
# Usage:
#   bash scripts/wsl-run.sh [make-target ...]      # default target: all
#
# Environment:
#   SRC   source tree (default: the parent directory of this script)
#   DEST  mirror destination (default: $HOME/unix-odmain-ipc)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$(cd "$HERE/.." && pwd)}"
DEST="${DEST:-$HOME/unix-odmain-ipc}"
TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=(all)
fi

mkdir -p "$DEST"
rsync -a --delete \
      --exclude '.git' --exclude 'build' --exclude 'build-asan' \
      --exclude '.workbuddy' \
      "$SRC"/ "$DEST"/

cd "$DEST"
echo "== source : $SRC"
echo "== mirror : $DEST"
echo "== target : ${TARGETS[*]}"
echo "== cc     : $(cc --version | head -1)"
make "${TARGETS[@]}"
