"""Exercise the real GUI startup path with video only and no device."""
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

from gui_smoke import free_port

SESSION = 0xAABB1122
PIXEL = bytes((12, 132, 240, 255))


def fragment(seq, i):
    w, h, stride = 24, 16, 1024
    frame = PIXEL * (w * h)
    offset = i * stride
    header = struct.pack("!4sBBHQIQHHIHHIHH", b"UVF1", 1, 2, 48, SESSION,
                         seq, 1, w, h, len(frame), i, 2, offset, stride, 0)
    return header + frame[offset:offset + stride]


def run(executable, compact=False):
    with tempfile.TemporaryDirectory(prefix="receiver-passive-") as temporary, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as forbidden_output:
        port = free_port()
        sender.bind(("127.0.0.1", 0))
        sender.settimeout(.06)
        forbidden_output.bind(("127.0.0.1", 0))
        forbidden_output.setblocking(False)
        receiver = ("127.0.0.1", port)
        hello = struct.pack("!4sIQ", b"UVH1", 0, SESSION)
        command = [str(Path(executable).resolve()), "--smoke-test", "--test-passive-preview",
                   "--test-frame-port=" + str(port),
                   "--test-pi-port=" + str(forbidden_output.getsockname()[1])]
        if compact:
            command.append("--compact")
        wrong_ip_supported = False
        with subprocess.Popen(command, cwd=temporary) as process:
            try:
                deadline = time.monotonic() + 3
                while True:
                    assert process.poll() is None, f"Receiver exited: {process.returncode}"
                    assert time.monotonic() < deadline, "No video-only crop negotiation"
                    sender.sendto(hello, receiver)
                    try:
                        packet, from_address = sender.recvfrom(256)
                        break
                    except TimeoutError:
                        continue
                    except ConnectionResetError as error:
                        # Winsock reports ICMP Port Unreachable if our hello reaches the
                        # UDP port before Receiver binds it. Keep the existing startup
                        # deadline and retry only that Windows error.
                        if getattr(error, "winerror", None) != 10054:
                            raise
                        continue
                expected = struct.pack("!4sBBHQHHI", b"UVN1", 1, 2, 0, SESSION, 320, 320, 0)
                assert packet == expected and from_address == receiver, (packet, from_address)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other_ip:
                    try:
                        other_ip.bind(("127.0.0.2", 0))
                        wrong_ip_supported = True
                    except OSError:
                        pass  # Some Windows loopback stacks expose 127.0.0.1 only.
                    if wrong_ip_supported:
                        other_ip.settimeout(.08)
                        other_ip.sendto(hello, receiver)
                        try:
                            other_ip.recvfrom(256)
                        except TimeoutError:
                            pass
                        else:
                            raise AssertionError("Preview replied to the mismatched sender address")
                # A heartbeat and one fragment must not promote Waiting to Live.
                sender.sendto(fragment(1, 0), receiver)
                # Another socket on the same source IP cannot impersonate the accepted video peer.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                    other.sendto(fragment(3, 0), receiver)
                    other.sendto(fragment(3, 1), receiver)
                time.sleep(.2)
                for i in (1, 0):  # Complete out of order.
                    sender.sendto(fragment(2, i), receiver)
                # Delayed incomplete fragments and hello traffic do not refresh video.
                sender.sendto(fragment(1, 1), receiver)
                sender.sendto(hello, receiver)
                assert process.wait(timeout=8) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        state = json.loads((Path(temporary) / "gui-smoke-state.json").read_text())
        for flag in ("passive_only", "passive_waiting", "passive_live", "passive_stale", "passive_inline"):
            assert state[flag], state
        assert state["page"] == "connections" and not state["passive_error"] and not state["gui_error"], state
        assert state["passive_complete_frames"] == 1, state
        assert state["passive_wide_layout"] == (not compact), state
        if wrong_ip_supported:
            assert state["passive_sender_mismatch"], state
        assert not any(state[k] for k in ("active_app_running", "active_app_inferred", "active_app_sent")), state
        assert not state["armed"] and not state["preview_window_open"], state
        # No mouse endpoint traffic at all; no clocks/cursors back to sender.
        try:
            forbidden_output.recvfrom(256)
        except BlockingIOError:
            pass
        else:
            raise AssertionError("Preview contacted the configured mouse device")
        sender.setblocking(False)
        while True:
            try:
                packet, _ = sender.recvfrom(256)
            except BlockingIOError:
                break
            assert packet == expected, packet
        image = (Path(temporary) / "gui-smoke.bmp").read_bytes()
        assert image.startswith(b"BM") and len(image) > 10000, "Missing rendered GUI frame"
        assert image.count(PIXEL[:3]) > 200, "Raw test frame color was not visible"
        print(f"{int(state['width'])} px: preview waited, displayed the complete frame, became stale, and never ran output.")


if __name__ == "__main__":
    run(sys.argv[1])
    run(sys.argv[1], compact=True)
