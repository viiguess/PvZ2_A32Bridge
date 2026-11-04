#!/bin/bash
# Build and run jiagu_run (Linux / WSL).
#
#   jr.sh build [--trace]                      configure and build (build trees: ~/jr_build, ~/jr_build_trace)
#   jr.sh run [--trace] <lib.so> <apk> [workdir] [-- extra jiagu_run options]
#
# From Windows:  wsl -e bash /mnt/c/.../tools/jiagu_run/jr.sh run ./libjiagu.so game.apk
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
trace=0
args=()
for a in "$@"; do
  if [ "$a" = "--trace" ]; then trace=1; else args+=("$a"); fi
done
set -- "${args[@]}"

if [ "$trace" = 1 ]; then
  build_dir="$HOME/jr_build_trace"; cmake_opts=(-DJR_TRACE=ON)
else
  build_dir="$HOME/jr_build"; cmake_opts=(-DJR_TRACE=OFF)
fi

build() {
  cmake -S "$here" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release "${cmake_opts[@]}" > /dev/null
  cmake --build "$build_dir" 2>&1 | grep -E "error|FAILED|undefined" || true
  test -x "$build_dir/jiagu_run"
}

case "${1:-}" in
  build)
    build
    echo "built $build_dir/jiagu_run"
    ;;
  run)
    shift
    [ $# -ge 2 ] || { echo "usage: $0 run [--trace] <lib.so> <apk> [workdir] [-- options]"; exit 2; }
    lib="$1"; apk="$2"; shift 2
    work="${1:-$HOME/jr_work}"; [ $# -gt 0 ] && shift
    [ "${1:-}" = "--" ] && shift
    mkdir -p "$work"
    build
    "$build_dir/jiagu_run" "$lib" "$apk" "$work" "$@" 2>&1
    ;;
  *)
    sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
