"""Run the real headless receiver against a pseudo-terminal MAKCU and UDP sender."""
import errno
import json
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import sys
import tempfile
import threading
import time

from integration_test import Sender, free_port, wait_for, wait_until_quiet


class Makcu:
    def __init__(self, framed):
        self.master, slave = pty.openpty()
        self.port = os.ttyname(slave)
        os.close(slave)
        self.buttons = 0
        self.framed = framed
        self.respond = True
        self.moves = []
        self.probes = []
        self.error = None
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        pending = b''
        negotiated = False
        monitoring = False
        last_buttons = None
        try:
            while not self.stopping.is_set():
                if monitoring and self.buttons != last_buttons:
                    last_buttons = self.buttons
                    os.write(self.master, (b'km.' if self.framed else b'') + bytes([last_buttons]))
                readable, _, _ = select.select([self.master], [], [], .002)
                if not readable:
                    continue
                try:
                    chunk = os.read(self.master, 4096)
                except OSError as error:
                    if error.errno == errno.EIO:  # The receiver has not opened the slave yet.
                        self.stopping.wait(.002)
                        continue
                    raise
                pending += chunk
                if not negotiated:
                    if len(pending) < 9:
                        continue
                    assert pending[:9] == bytes.fromhex('de ad 05 00 a5 00 09 3d 00'), pending[:9]
                    pending = pending[9:]
                    negotiated = True
                while b'\n' in pending:
                    command, pending = pending.split(b'\n', 1)
                    command = command.rstrip(b'\r')
                    if not command:
                        continue
                    if command == b'km.buttons(1)':
                        monitoring = True
                        last_buttons = None
                    elif command == b'km.buttons(0)':
                        monitoring = False
                    elif re.fullmatch(rb'km.version\(\)#[0-9]+', command):
                        self.probes.append(command)
                        if self.respond:
                            # Deliberately split echoed ASCII/CRLF across writes.
                            reply = command + b':km.MAKCU:3.9' if self.framed else command
                            os.write(self.master, b'>>> ' + reply + b'\r')
                            os.write(self.master, b'\nMAKCU test firmware\r\n>>> ')
                    else:
                        move = re.fullmatch(rb'km.move\((-?[0-9]+),(-?[0-9]+)\)', command)
                        assert move, command
                        dx, dy = map(int, move.groups())
                        assert -32767 <= dx <= 32767 and -32767 <= dy <= 32767
                        self.moves.append((dx, dy))
                        os.write(self.master, command + b'\r\n>>> ')
        except Exception as error:
            self.error = error

    def stop(self):
        self.stopping.set()
        self.thread.join(timeout=2)
        assert not self.thread.is_alive(), 'Fake MAKCU worker leaked'
        os.close(self.master)
        if self.error:
            raise self.error


def session(executable, scenario):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        device = Makcu(framed=scenario != 'disarmed')
        port = free_port()
        sender = Sender(port=port, width=160, height=160, fps=120).start()
        profile = root / 'settings.json'
        # MAKCU must never subscribe to or send mouse commands to a Pi.
        profile.write_text(json.dumps(dict(bind_ip='127.0.0.1', sender_ip='127.0.0.1',
                                           pi_ip='203.0.113.1', frame_port=port,
                                           activation_button=1, secondary_button=2,
                                           constant_tracking=scenario == 'continuous')))
        metrics = root / 'metrics.csv'
        command = [executable, '--profile', str(profile), '--simulate',
                   '--mouse-backend', 'makcu', '--makcu-port', device.port,
                   '--seconds', '0', '--metrics', str(metrics)]
        if scenario != 'disarmed':
            command.append('--arm')
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        device_open = True
        try:
            wait_for(lambda: len(device.probes) >= 2 or device.error,
                     'MAKCU handshake/response probes did not run')
            assert device.error is None, device.error
            device.buttons = 0 if scenario == 'continuous' else 2  # Alternate physical activation button.
            if scenario == 'disarmed':
                time.sleep(.3)
                assert process.poll() is None and not device.moves, 'Disarmed MAKCU moved the mouse'
                process.terminate()
                output = process.communicate(timeout=5)[0]
                assert process.returncode == 0, output
                print('Default-disarmed MAKCU received frames/button reports without movement.')
            else:
                wait_for(lambda: len(device.moves) >= 5, 'Armed MAKCU did not receive corrections')
                assert all(dx > 0 and dy == 0 for dx, dy in device.moves), device.moves
                if scenario == 'continuous':
                    assert device.buttons == 0, 'Continuous mode unexpectedly requires a physical press'
                    sender.paused = True
                    count = wait_until_quiet(device.moves, 'Continuous MAKCU moved without fresh frames')
                    sender.paused = False
                    wait_for(lambda: len(device.moves) > count + 5, 'Continuous MAKCU did not resume')
                    process.terminate()
                    output = process.communicate(timeout=5)[0]
                    assert process.returncode == 0, output
                    print('MAKCU continuous mode follows arming and fresh-frame gates without held buttons.')
                elif scenario == 'gates':
                    device.buttons = 0
                    count = wait_until_quiet(device.moves, 'MAKCU moved after physical release')
                    device.buttons = 2
                    wait_for(lambda: len(device.moves) > count + 5, 'MAKCU did not resume on activation')
                    sender.paused = True
                    count = wait_until_quiet(device.moves, 'MAKCU repeated movement after capture stopped')
                    sender.paused = False
                    wait_for(lambda: len(device.moves) > count + 5, 'MAKCU did not recover fresh frames')
                    device.respond = False
                    output = process.communicate(timeout=4)[0]
                    assert process.returncode == 1 and 'MAKCU is not responding' in output, output
                    count = len(device.moves)
                    time.sleep(.1)
                    assert len(device.moves) == count, 'MAKCU moved after link timeout'
                    print('MAKCU activation, release, frame stall/recovery and response timeout passed.')
                else:
                    device.stop()
                    device_open = False
                    output = process.communicate(timeout=4)[0]
                    assert process.returncode == 1 and 'MAKCU serial' in output, output
                    print('MAKCU disconnection stopped the running receiver.')
            report = metrics.read_text()
            assert '# click_commands_submitted_locally,0' in report
            assert '# synthetic_snapshots,0' in report  # No Pi packets on the serial path.
            assert 'command_submission' in report
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)
            sender.stop()
            if device_open:
                device.stop()


if __name__ == '__main__':
    for case in ('disarmed', 'gates', 'disconnect', 'continuous'):
        session(sys.argv[1], case)
