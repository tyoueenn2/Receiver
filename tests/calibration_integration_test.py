"""Exercise the real Receiver calibration controller against deterministic peers."""
import json
from pathlib import Path
import random
import socket
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def run(binary, mode):
    pi = MockPi(free_port(), physical=0).start()
    rng = random.Random(61)
    texture = bytes(rng.randrange(256) for _ in range(256*256*3))
    def cursor():
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
            assert result.returncode == 0, result.stdout+result.stderr
            data = json.loads(report.read_text())
            assert not data['armed'], data
            if mode in ('desktop', 'view', 'manual', 'miss'):
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
            assert pi.transactions.position == (0, 0), (mode, pi.transactions.position, data)
            print(mode, data)
    finally:
        sender.stop()
        pi.stop()


if __name__ == '__main__':
    for scenario in ('desktop', 'view', 'manual', 'cancel', 'old_sender', 'nonlinear', 'miss'):
        run(sys.argv[1], scenario)
