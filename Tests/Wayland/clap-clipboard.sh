#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || ! -f $1 || ! -f $2/include/clap/clap.h ]]; then
  echo "Usage: $0 /path/to/WaylandClipboardTest.clap /path/to/CLAP_SDK" >&2
  exit 2
fi

test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
runtime=$(mktemp -d)
xvfb_pid=
weston_pid=
rdp_pid=
cleanup()
{
  for pid in "$rdp_pid" "$weston_pid" "$xvfb_pid"; do
    if [[ -n $pid ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -rf -- "$runtime"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR="$runtime"
export XDG_CONFIG_HOME="$runtime/config"
export WAYLAND_DISPLAY=iplug-clap-clipboard
unset WAYLAND_SOCKET

c++ -std=c++17 -Wall -Wextra -Werror -I"$2/include" "$test_dir/clap-lifecycle.cpp" \
  -pthread -ldl -o "$runtime/clap-lifecycle"

Xvfb -displayfd 3 -screen 0 1280x900x24 -nolisten tcp 3>"$runtime/display" &
xvfb_pid=$!
for ((attempt = 0; attempt < 50; ++attempt)); do
  [[ ! -s $runtime/display ]] || break
  sleep .1
done
[[ -s $runtime/display ]]
export DISPLAY=:$(cat "$runtime/display")

# Headless Weston lacks a seat. Use an RDP client inside Xvfb to supply input.
openssl req -new -newkey rsa:2048 -x509 -nodes -days 1 -subj /CN=127.0.0.1 \
  -keyout "$runtime/key.pem" -out "$runtime/cert.pem" >"$runtime/cert.log" 2>&1
fingerprint=$(openssl x509 -in "$runtime/cert.pem" -noout -sha256 -fingerprint | cut -d= -f2)
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
env -u DISPLAY weston --no-config --backend=rdp-backend.so --width=1280 --height=900 \
  --address=127.0.0.1 --port="$port" --rdp-tls-cert="$runtime/cert.pem" \
  --rdp-tls-key="$runtime/key.pem" --socket="$WAYLAND_DISPLAY" --log="$runtime/weston.log" &
weston_pid=$!
for ((attempt = 0; attempt < 50; ++attempt)); do
  [[ ! -S $runtime/$WAYLAND_DISPLAY ]] || break
  sleep .1
done
if [[ ! -S $runtime/$WAYLAND_DISPLAY ]]; then
  cat "$runtime/weston.log" >&2
  exit 1
fi

rdp_command=(
  "${FREERDP:-xfreerdp}" "/v:127.0.0.1:$port" /u:wayland-test /p:unused /sec:tls
  "/cert:fingerprint:sha256:$fingerprint" /size:1280x900 /bpp:24 /f -clipboard -grab-keyboard
)
# Start the editor before RDP creates the first seat, then replace that seat.
(
  trap 'exit 1' TERM
  for ((attempt = 0; attempt < 50; ++attempt)); do
    if [[ -f $runtime/host-timer/client.log ]] &&
       grep -Fq 'CLAP editor shown' "$runtime/host-timer/client.log"; then
      break
    fi
    sleep .1
  done
  if ((attempt == 50)); then
    echo "The editor did not open before connecting RDP" >&2
    exit 1
  fi
  "${rdp_command[@]}" /client-hostname:iplug-seat1 &
  first_rdp=$!
  trap 'kill "$first_rdp" 2>/dev/null || true' EXIT
  for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ ! -f $runtime/reconnect-input ]] || break
    sleep .1
  done
  if ((attempt == 100)); then
    echo "The editor did not request an RDP reconnect" >&2
    exit 1
  fi
  kill "$first_rdp"
  wait "$first_rdp" || true
  exec "${rdp_command[@]}" /client-hostname:iplug-seat2
) >"$runtime/rdp.log" 2>&1 &
rdp_pid=$!
if ! python3 "$test_dir/clap-clipboard.py" "$runtime" "$1"; then
  tail -40 "$runtime/rdp.log" >&2
  exit 1
fi
