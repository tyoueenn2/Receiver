"""Open a complete local UI demo; closing the app stops both test peers."""
import argparse
import os
import sys
from collections import deque
from pathlib import Path
import subprocess
import threading
from mock_pi import MockPi
from test_sender import Sender


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, help='Location of receiver.exe')
    parser.add_argument('--page', choices=['setup', 'detect', 'mouse', 'human', 'saved', 'hub'], default='setup')
    parser.add_argument("--cycle-motion", action="store_true", help="Cycle simulated direction every four seconds; off by default")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    candidates = [root / 'receiver.exe', root / 'build/tests/Release/receiver.exe',
                  root / 'build/local/receiver.exe']
    executable = args.exe or next((path for path in candidates if path.is_file()), None)
    if not executable or not executable.is_file():
        parser.error('Receiver is not built yet. Build it first, or use --exe to select receiver.exe.')
    pi = MockPi()
    pi.commands = deque(maxlen=10000)
    sender = Sender()
    finished = threading.Event()
    def cycle_motion():
        while not finished.is_set():
            for speed in (120, -120, 4, 0):
                pi.motion_x = speed
                if finished.wait(4):
                    return
    motion_thread = threading.Thread(target=cycle_motion, daemon=True)
    try:
        pi.start()
        sender.start()
        if args.cycle_motion:
            motion_thread.start()
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 1
        result = subprocess.run([str(executable.resolve()), '--demo', '--page=' + args.page], cwd=root,
                                startupinfo=startup, env=dict(os.environ, RECEIVER_PYTHON=sys.executable))
        return result.returncode
    finally:
        finished.set()
        if motion_thread.is_alive():
            motion_thread.join(timeout=1)
        sender.stop()
        pi.stop()


if __name__ == '__main__':
    raise SystemExit(main())
