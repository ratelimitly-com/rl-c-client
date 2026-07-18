#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX="$ROOT/tests/linux-http-examples.txt"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "build_linux_http_examples: SKIP (Linux-only matrix)"
  exit 0
fi
if [[ "$#" -ne 1 || ! "$1" =~ ^(a|b|c|all)$ ]]; then
  echo "usage: $0 <a|b|c|all>" >&2
  exit 2
fi

check_dependency() {
  local name=$1
  case "$name" in
    mongoose) [[ -f "${MONGOOSE_ROOT:-}/mongoose.c" ]] ;;
    civetweb) [[ -f "${CIVETWEB_ROOT:-}/src/civetweb.c" ]] ;;
    libmicrohttpd) pkg-config --exists libmicrohttpd ;;
    ulfius) pkg-config --exists libulfius liborcania ;;
    h2o) pkg-config --exists libh2o-evloop ;;
    lwan)
      [[ -f "${LWAN_ROOT:-}/src/lib/lwan.h" \
        && -f "${LWAN_BUILD:-}/src/lib/liblwan.a" \
        && -f "${LWAN_BUILD:-}/lwan.pc" ]]
      ;;
    libreactor) pkg-config --exists libreactor ;;
    facil_io) [[ -f "${FACIL_ROOT:-}/tmp/libfacil.so" ]] ;;
    onion)
      [[ -f "${ONION_ROOT:-}/include/onion/onion.h" \
        && -e "${ONION_ROOT:-}/lib/libonion.so" ]]
      ;;
    kore)
      [[ -x "${KORE_ROOT:-}/kore" \
        && -f "${KORE_ROOT:-}/include/kore/kore.h" ]]
      ;;
    *) return 1 ;;
  esac
}

shard=$1
make -C "$ROOT" clean
make -C "$ROOT" \
  CFLAGS='-O2 -Wall -Wextra -Werror -std=c11' \
  librclient.a test-responder

while IFS='|' read -r name row_shard binary _; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  if [[ "$shard" != "all" && "$row_shard" != "$shard" ]]; then
    continue
  fi
  check_dependency "$name" \
    || { echo "$name: required framework dependency is unavailable" >&2; exit 1; }
  make -C "$ROOT/$(dirname "$binary")" clean all
done <"$MATRIX"

echo "build_linux_http_examples: PASS ($shard)"
