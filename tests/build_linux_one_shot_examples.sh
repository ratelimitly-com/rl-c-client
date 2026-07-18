#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX="$ROOT/tests/linux-one-shot-examples.txt"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "build_linux_one_shot_examples: SKIP (Linux-only matrix)"
  exit 0
fi

make -C "$ROOT" clean
make -C "$ROOT" \
  CFLAGS='-O2 -Wall -Wextra -Werror -std=c11' \
  librclient.a test-responder

while IFS='|' read -r name binary _; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  example_dir="$ROOT/$(dirname "$binary")"
  make -C "$example_dir" clean all
done <"$MATRIX"
