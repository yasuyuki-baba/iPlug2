#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 || ! -f $1 || ! -f $2/pluginterfaces/gui/iwaylandframe.h || ( $# == 3 && $3 != --concurrent && $3 != --host-reads ) ]]; then
  echo "Usage: $0 /path/to/IPlugEffect.so /path/to/VST3_SDK [--concurrent|--host-reads]" >&2
  exit 2
fi
cycles=3
args=()
if [[ ${3:-} == --concurrent ]]; then
  cycles=50
  args=(--concurrent)
elif [[ ${3:-} == --host-reads ]]; then
  export IPLUG_VST3_WAYLAND_HOST_READS=1
  args=(--host-reads)
fi

test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
runtime=$(mktemp -d)
weston_pid=
cleanup()
{
  if [[ -n $weston_pid ]]; then
    kill "$weston_pid" 2>/dev/null || true
    wait "$weston_pid" 2>/dev/null || true
  fi
  rm -rf -- "$runtime"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY=iplug-vst3-smoke
unset DISPLAY

c++ -std=c++17 -pthread -I"$2" "$test_dir/vst3-lifecycle.cpp" \
  "$2/pluginterfaces/base/funknown.cpp" "$2/pluginterfaces/base/coreiids.cpp" \
  "$2/public.sdk/source/common/commoniids.cpp" "$2/public.sdk/source/vst/vstinitiids.cpp" \
  $(pkg-config --cflags --libs wayland-client) -ldl -o "$runtime/vst3-lifecycle"

weston --no-config --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" --log="$runtime/weston.log" &
weston_pid=$!
for ((attempt = 0; attempt < 50; ++attempt)); do
  [[ ! -S $runtime/$WAYLAND_DISPLAY ]] || break
  sleep .1
done
if [[ ! -S $runtime/$WAYLAND_DISPLAY ]]; then
  cat "$runtime/weston.log" >&2
  exit 1
fi

if ! WAYLAND_DEBUG=client timeout 60s "$runtime/vst3-lifecycle" "$1" "${args[@]}" >"$runtime/client.log" 2>&1 ||
   [[ $(grep -Ec 'wl_subcompositor[@#][0-9]+\.get_subsurface' "$runtime/client.log") != "$cycles" ]] ||
   [[ $(grep -Ec 'wl_subsurface[@#][0-9]+\.destroy\(' "$runtime/client.log") != "$cycles" ]] ||
   [[ $(grep -Ec 'wl_surface[@#][0-9]+\.attach\(wl_buffer[@#]' "$runtime/client.log") -lt $cycles ]]; then
  tail -100 "$runtime/client.log" >&2
  exit 1
fi
if [[ ${3:-} != --concurrent ]]; then
  grep -Eq 'wl_surface[@#][0-9]+\.set_buffer_scale\(2\)' "$runtime/client.log"
fi
grep 'smoke test:' "$runtime/client.log"
