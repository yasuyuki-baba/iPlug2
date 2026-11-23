#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ! -x $1 || ( $# == 2 && $2 != --egl ) ]]; then
  echo "Usage: $0 /path/to/IPlugEffect [--egl]" >&2
  exit 2
fi

renderer=${2:-}
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
export XDG_CONFIG_HOME="$runtime/config"
export WAYLAND_DISPLAY=iplug2-smoke
unset DISPLAY WAYLAND_SOCKET
export GDK_BACKEND=wayland
export GDK_SCALE=${IPLUG_WAYLAND_TEST_SCALE:-1}
case "$GDK_SCALE" in
  1|2) ;;
  *) echo "IPLUG_WAYLAND_TEST_SCALE must be 1 or 2" >&2; exit 2 ;;
esac

if [[ $renderer != --egl ]]; then
  cc -std=c11 -Wall -Wextra -Werror -shared -fPIC "$test_dir/check-frame.c" \
    $(pkg-config --cflags --libs gtk+-3.0) -ldl -o "$runtime/check-frame.so"
fi

# Weston 9 uses --use-pixman and newer packages offer --renderer=pixman.
weston_help=$(weston --help 2>&1)
pixman=(--use-pixman)
if [[ $weston_help == *--renderer* ]]; then
  pixman=(--renderer=pixman)
fi
weston --no-config --backend=headless-backend.so "${pixman[@]}" \
  --width=1600 --height=1600 --scale="$GDK_SCALE" --socket="$WAYLAND_DISPLAY" --log="$runtime/weston.log" &
weston_pid=$!
for ((attempt = 0; attempt < 50; ++attempt)); do
  [[ ! -S $runtime/$WAYLAND_DISPLAY ]] || break
  sleep .1
done
if [[ ! -S $runtime/$WAYLAND_DISPLAY ]]; then
  cat "$runtime/weston.log" >&2
  exit 1
fi

if [[ $renderer == --egl ]]; then
  WAYLAND_DEBUG=client timeout 20s "$1" --wayland --no-io >"$runtime/client.log" 2>&1 &
  client_pid=$!
  attached=false
  for ((attempt = 0; attempt < 100; ++attempt)); do
    if grep -Eq 'wl_surface[@#][0-9]+\.attach\(wl_buffer[@#]' "$runtime/client.log"; then
      attached=true
      break
    fi
    kill -0 "$client_pid" 2>/dev/null || break
    sleep .1
  done
  kill "$weston_pid"
  wait "$weston_pid" || true
  weston_pid=
  if ! wait "$client_pid" || [[ $attached != true ]] ||
     ! grep -Eq 'xdg_surface[@#][0-9]+\.ack_configure\(' "$runtime/client.log" ||
     grep -qi 'xwayland' "$runtime/weston.log"; then
    tail -100 "$runtime/client.log" >&2
    exit 1
  fi
  echo "Wayland EGL smoke test: app submitted a native buffer and exited after compositor shutdown"
  exit 0
fi

if ! timeout 20s env LD_PRELOAD="$runtime/check-frame.so" WAYLAND_DEBUG=client \
    "$1" --wayland --no-io >"$runtime/client.log" 2>&1; then
  tail -100 "$runtime/client.log" >&2
  exit 1
fi
if ! grep -Fq "painted $((600 * GDK_SCALE))x$((600 * GDK_SCALE)) at ${GDK_SCALE}x" "$runtime/client.log" ||
   ! grep -Fq "painted $((720 * GDK_SCALE))x$((720 * GDK_SCALE)) at ${GDK_SCALE}x" "$runtime/client.log" ||
   ! grep -q 'scroll changed the rendered knob' "$runtime/client.log" ||
   ! grep -Eq 'wl_surface[@#][0-9]+\.attach\(wl_buffer[@#]' "$runtime/client.log" ||
   ! grep -Eq 'wl_buffer[@#][0-9]+\.release\(' "$runtime/client.log" ||
   grep -qi 'xwayland' "$runtime/weston.log"; then
  tail -100 "$runtime/client.log" >&2
  exit 1
fi
grep 'Wayland smoke test:' "$runtime/client.log"
echo "Wayland smoke test: compositor received a native buffer and the app exited normally"
