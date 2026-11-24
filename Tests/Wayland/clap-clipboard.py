"""Exercise a native CLAP clipboard source with a separate Wayland client."""

import os
from pathlib import Path
import re
import select
import subprocess
import sys
import time


PAYLOAD = b"".join(f"{i:08x}0123456789abcdef01234567".encode() for i in range(8192))
SEND = re.compile(r'wl_data_source[@#]\d+\.send\(.*?, fd (\d+)\)')


def wait_for(check, message, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.01)
    raise RuntimeError(message)


def editor_position():
    screenshot = subprocess.check_output(["import", "-window", "root", "png:-"])
    pixels = subprocess.check_output(
        ["convert", "png:-", "-alpha", "off", "-depth", "8", "rgb:-"], input=screenshot
    )
    # Find the test plug-in's solid green panel and click inside it.
    panel = bytes([17, 218, 45]) * 100
    start = pixels.find(panel)
    end = pixels.rfind(panel)
    if start < 0:
        return None
    top, left = divmod(start // 3, 1280)
    bottom, right = divmod(end // 3, 1280)
    return (left + right + 100) // 2, (top + bottom) // 2


def pipe_at(pid, fd):
    try:
        return os.readlink(f"/proc/{pid}/fd/{fd}")
    except FileNotFoundError:
        return None


def stalled_paste(pid, log):
    requests = len(SEND.findall(log()))
    reader = subprocess.Popen(["wl-paste", "--no-newline"], stdout=subprocess.PIPE)
    try:
        if not select.select([reader.stdout], [], [], 2)[0]:
            raise RuntimeError("the clipboard receiver did not get a first chunk")
        chunk = os.read(reader.stdout.fileno(), 4096)
        if not chunk or not PAYLOAD.startswith(chunk):
            raise RuntimeError("unexpected clipboard data from the stalled receiver")
        matches = SEND.findall(log())
        if len(matches) != requests + 1:
            raise RuntimeError("expected one clipboard send for the stalled receiver")
        # Use the pipe inode because the host can reuse the descriptor number.
        fd = int(matches[-1])
        pipe = pipe_at(pid, fd)
        if not pipe or not pipe.startswith("pipe:["):
            raise RuntimeError("the plug-in did not retain an active transfer pipe")
        return reader, fd, pipe
    except BaseException:
        reader.kill()
        reader.wait()
        reader.stdout.close()
        raise


def exercise(runtime, plugin, mode):
    directory = runtime / mode
    directory.mkdir()
    path = directory / "client.log"
    commands = {action: directory / action.lower() for action in ("HIDE", "DESTROY", "EXIT")}
    env = dict(os.environ, WAYLAND_DEBUG="client",
               **{f"IPLUG_TEST_{action}": str(target) for action, target in commands.items()})

    def log():
        return path.read_text(errors="replace")

    with path.open("wb") as output:
        host = subprocess.Popen(
            [str(runtime / "clap-lifecycle"), plugin, "clipboard", mode],
            env=env, stdout=output, stderr=output,
        )
        reader = None
        try:
            x, y = wait_for(editor_position, "the clipboard test editor did not appear", timeout=5)
            subprocess.run(
                ["xdotool", "mousemove", str(x), str(y), "sleep", ".2",
                 "click", "1", "sleep", ".2", "key", "F2"], check=True
            )
            wait_for(lambda: "CLAP clipboard copied" in log(), "the editor did not claim the clipboard")
            commands["HIDE"].touch()
            wait_for(lambda: "CLAP editor hidden" in log(), "the editor did not hide")

            data = subprocess.check_output(["wl-paste", "--no-newline"], timeout=3)
            if data != PAYLOAD:
                raise RuntimeError(f"wrong clipboard payload after hide ({len(data)} bytes)")

            reader, fd, pipe = stalled_paste(host.pid, log)
            reader.kill()
            reader.wait(timeout=2)
            reader.stdout.close()
            reader = None
            wait_for(lambda: pipe_at(host.pid, fd) != pipe, "cancelled transfer left its FD open")

            reader, fd, pipe = stalled_paste(host.pid, log)
            commands["DESTROY"].touch()
            wait_for(lambda: "CLAP editor destroyed" in log(), "the GUI did not destroy during transfer")
            if host.poll() is not None or pipe_at(host.pid, fd) == pipe:
                raise RuntimeError("GUI destroy did not release the pending clipboard FD")
            remainder, _ = reader.communicate(timeout=2)
            if reader.returncode != 0 or len(remainder) >= len(PAYLOAD) - 4096:
                raise RuntimeError("the receiver did not observe an interrupted transfer")
            commands["EXIT"].touch()
            if host.wait(timeout=2) != 0:
                raise RuntimeError("the test host failed after GUI destroy")
        except BaseException:
            print(log()[-12000:], file=sys.stderr)
            raise
        finally:
            if reader is not None:
                reader.kill()
                reader.wait()
                reader.stdout.close()
            if host.poll() is None:
                host.kill()
                host.wait()
    print(f"CLAP Wayland clipboard: {mode} passed (256 KiB, cancellation, destroy)")


if __name__ == "__main__":
    for timer_mode in ("host-timer", "no-timer", "reject-timer"):
        exercise(Path(sys.argv[1]), sys.argv[2], timer_mode)
