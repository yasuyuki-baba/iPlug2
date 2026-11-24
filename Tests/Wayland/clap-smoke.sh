#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || ! -f $1 || ! -f $2/include/clap/clap.h ]]; then
  echo "Usage: $0 /path/to/IPlugEffect.clap /path/to/CLAP_SDK" >&2
  exit 2
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
export WAYLAND_DISPLAY=iplug-clap-smoke
unset DISPLAY WAYLAND_SOCKET

c++ -std=c++17 -Wall -Wextra -Werror -I"$2/include" "$test_dir/clap-lifecycle.cpp" \
  -pthread -ldl -o "$runtime/clap-lifecycle"
timeout 10s "$runtime/clap-lifecycle" "$1" unavailable

# Use a software renderer so Weston actually consumes the editor's buffers.
weston_help=$(weston --help 2>&1)
pixman=(--use-pixman)
if [[ $weston_help == *--renderer* ]]; then
  pixman=(--renderer=pixman)
fi
weston --no-config --backend=headless-backend.so "${pixman[@]}" \
  --scale=2 --width=1600 --height=1600 --socket="$WAYLAND_DISPLAY" --log="$runtime/weston.log" &
weston_pid=$!
for ((attempt = 0; attempt < 50; ++attempt)); do
  [[ ! -S $runtime/$WAYLAND_DISPLAY ]] || break
  sleep .1
done
if [[ ! -S $runtime/$WAYLAND_DISPLAY ]]; then
  cat "$runtime/weston.log" >&2
  exit 1
fi

if ! WAYLAND_DEBUG=client timeout 20s "$runtime/clap-lifecycle" "$1" >"$runtime/client.log" 2>&1 ||
   [[ $(grep -Ec 'xdg_surface[@#][0-9]+\.get_toplevel' "$runtime/client.log") != 3 ]] ||
   [[ $(grep -Ec 'xdg_toplevel[@#][0-9]+\.destroy\(' "$runtime/client.log") != 3 ]] ||
   [[ $(grep -Ec 'wl_compositor[@#][0-9]+\.create_surface' "$runtime/client.log") != 1 ]] ||
   ! grep -Eq 'wl_surface[@#][0-9]+\.set_buffer_scale\(2\)' "$runtime/client.log" ||
   [[ $(grep -Ec 'wl_surface[@#][0-9]+\.attach\(nil' "$runtime/client.log") -lt 3 ]] ||
   [[ $(grep -Ec 'wl_surface[@#][0-9]+\.attach\(wl_buffer[@#]' "$runtime/client.log") -lt 3 ]]; then
  tail -100 "$runtime/client.log" >&2
  exit 1
fi
if ! awk '
  /CLAP editor hidden/ { hidden = 1; ++intervals }
  /CLAP hidden interval ended/ { hidden = 0 }
  hidden && /wl_surface[@#][0-9]+[.]attach[(]wl_buffer[@#]/ { invalid = 1 }
  END { exit invalid || intervals != 3 }
' "$runtime/client.log"; then
  tail -100 "$runtime/client.log" >&2
  exit 1
fi
grep 'CLAP Wayland smoke test:' "$runtime/client.log"

# A lost compositor connection while hidden must notify the host and release the GUI timer.
: >"$runtime/disconnect.log"
WAYLAND_DEBUG=client timeout 20s "$runtime/clap-lifecycle" "$1" interactive >"$runtime/disconnect.log" 2>&1 &
client_pid=$!
for ((attempt = 0; attempt < 50; ++attempt)); do
  grep -Fq 'CLAP editor hidden' "$runtime/disconnect.log" && break
  sleep .1
done
kill "$weston_pid"
wait "$weston_pid" || true
weston_pid=
if ! wait "$client_pid" || ! grep -Fq 'hidden editor reported closed(true)' "$runtime/disconnect.log"; then
  tail -100 "$runtime/disconnect.log" >&2
  exit 1
fi
grep 'smoke test:' "$runtime/disconnect.log"
