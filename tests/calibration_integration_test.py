"""Exercise the real Receiver calibration controller against deterministic peers."""
from contextlib import contextmanager
import json
from pathlib import Path
import random
import socket
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender
from wire import HELLO


@contextmanager
def peer_timer_resolution():
    if sys.platform != 'win32':
        yield
        return
    import ctypes
    winmm = ctypes.WinDLL('winmm')
    # Receiver's timeBeginPeriod call affects only its own process on current
    # Windows. The Python peers also need fine waits to meet the 50 ms lease.
    if winmm.timeBeginPeriod(1):
        raise RuntimeError('Cannot request 1 ms timer resolution for calibration peers')
    try:
        yield
    finally:
        winmm.timeEndPeriod(1)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class PeerTimingError(RuntimeError):
    """The safety gate cancelled a probe after a loopback peer missed its lease."""


def run_once(binary, mode):
    pi = MockPi(port=0, physical=0)
    if mode == 'telemetry_loss':
        # Keep the first probe outbound until the lease expires, so the test
        # checks an interrupted return rather than a pause between probes.
        pi.transactions.minimum_settle_s = .25
    pi.start()
    rng = random.Random(61)
    texture = bytes(rng.randrange(256) for _ in range(256*256*3))
    def cursor():
        if mode == 'telemetry_loss' and pi.transactions.requests:
            pi.telemetry_enabled = False
        x, y = pi.transactions.position
        extra = .2*x*abs(x) if mode == 'nonlinear' else 0
        if mode == 'miss' and pi.transactions.requests and pi.transactions.requests[-1]['button']:
            extra = 4
        return 960+2*x+extra, 540+1.5*y
    def image():
        x, y = pi.transactions.position
        left, top = 64+round(2*x), 64+round(1.5*y)
        return b''.join(texture[((top+row)*256+left)*3:((top+row)*256+left+128)*3]
                        for row in range(128))
    port = free_port()
    size = 128 if mode == 'view' else 64
    sender = Sender(port=port, width=size, height=size, fps=60,
        cursor_provider=None if mode == 'old_sender' else cursor,
        frame_provider=image if mode == 'view' else None).start()
    if mode == 'desktop':
        # Race heartbeat publication against the controller's freshness snapshot
        # while real calibration probes and a verified click are in flight.
        def heartbeat_loop():
            packet = HELLO.pack(b'UVH1', 0, sender.session)
            while not sender.stop_event.wait(.0001):
                sender.sock.sendto(packet, sender.target)
        heartbeat = threading.Thread(target=heartbeat_loop, daemon=True)
        sender.threads.append(heartbeat)
        heartbeat.start()
    try:
        with tempfile.TemporaryDirectory(prefix='receiver-calibration-') as directory:
            directory = Path(directory)
            config = dict(frame_port=port, pi_port=pi.sock.getsockname()[1], input_size=size,
                sender_ip='127.0.0.1', pi_ip='127.0.0.1', model='',
                return_action=dict(verified_target=True))
            if mode in ('manual', 'nonlinear'):
                config['calibration'] = dict(xx=2, yy=1.5, max_counts=8,
                    verified=mode == 'nonlinear', layout=1, poll_us=1000, display=1,
                    display_width=1920, display_height=1080, frame_width=128, frame_height=128)
            path, report = directory/'config.json', directory/'report.json'
            path.write_text(json.dumps(config))
            result = subprocess.run([binary, str(path), str(report), mode], capture_output=True,
                                    text=True, timeout=35)
            data = json.loads(report.read_text())
            assert result.returncode == 0, (data, result.returncode, result.stdout, result.stderr)
            assert not data['armed'], data
            peer_timeout = (data.get('error') in ('Pi operation: cancelled',
                                                'Requires a ready Pi with move and return support') and
                            data.get('release_reason') in ('stale telemetry', 'sender loss'))
            if mode == 'telemetry_loss':
                assert peer_timeout and data['release_reason'] == 'stale telemetry', data
                assert not data['verified'] and not pi.transactions.clicks, data
                assert pi.transactions.position != (0, 0) and data['probe_flags'] & 1, data
            elif peer_timeout:
                # Hosted runners may stall longer than the real 50 ms lease.
                # A transport abort stops movement; it cannot promise a return.
                # Never retry a click, an incorrectly verified return, another
                # error, or a persistent timeout.
                assert not pi.transactions.clicks, (mode, data, pi.transactions.clicks)
                assert pi.transactions.position == (0, 0) or data['probe_flags'] & 1, (mode, data)
                raise PeerTimingError(str(data))
            elif mode in ('desktop', 'view', 'manual', 'miss'):
                assert data['verified'] and not data['error'], (data, result.stdout, result.stderr)
                assert abs(data['xx']-2) <= .1 and abs(data['yy']-1.5) <= .1, data
                if mode == 'desktop':
                    assert data['action_status'] == 3 and data['action_flags'] & 6 == 6, data
                    assert 'return verified' in data['operation'], data
                    assert len(pi.transactions.clicks) == 1, pi.transactions.clicks
                    assert pi.transactions.clicks[0][1] == (4, 6), pi.transactions.clicks
                elif mode == 'miss':
                    assert data['action_status'] == 11 and data['action_flags'] & 2 == 0, data
                    assert not pi.transactions.clicks
                    assert 'click skipped' in data['operation_error'], data
                else:
                    assert not pi.transactions.clicks
            else:
                assert data['error'], data
                assert not pi.transactions.clicks
                if mode == 'nonlinear':
                    assert data['verified'] and data['xx'] == 2, data  # Previous calibration retained.
            if mode != 'telemetry_loss':
                assert pi.transactions.position == (0, 0), (mode, pi.transactions.position, data)
            print(mode, data)
    except Exception as error:
        def max_gap_ms(events):
            times = [event['time'] for event in events]
            return round(1000 * max((b - a for a, b in zip(times, times[1:])), default=0), 3)
        error.add_note(json.dumps(dict(
            scenario=mode,
            max_telemetry_gap_ms=max_gap_ms(pi.telemetry_events),
            max_subscription_gap_ms=max_gap_ms(pi.subscription_events),
            recent_telemetry=pi.telemetry_events[-8:],
            recent_subscriptions=pi.subscription_events[-8:]), default=str))
        raise
    finally:
        sender.stop()
        pi.stop()


@peer_timer_resolution()
def run(binary, mode):
    for attempt in range(3):
        try:
            return run_once(binary, mode)
        except PeerTimingError as error:
            if attempt == 2:
                raise
            print(f'{mode}: safe peer-timeout cancellation; retry {attempt + 1}/2',
                  error, *error.__notes__, sep='\n', flush=True)


if __name__ == '__main__':
    for scenario in ('desktop', 'view', 'manual', 'cancel', 'old_sender', 'nonlinear', 'miss',
                     'telemetry_loss'):
        run(sys.argv[1], scenario)
