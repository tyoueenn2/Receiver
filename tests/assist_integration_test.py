"""Exercise actual App triggering, activation and collection through loopback peers."""
import json
from pathlib import Path
import sys
import tempfile
import time

from model_fault_integration_test import Control
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender
from integration_test import free_port


def main():
    with tempfile.TemporaryDirectory(prefix='receiver-assist-pipeline-') as directory:
        root = Path(directory)
        weights = root / 'target.pt'
        weights.write_text(json.dumps(dict(mode='ok')))
        frame_port, pi_port = free_port(), free_port()
        pi = MockPi(pi_port, physical=0).start()
        sender = Sender(port=frame_port, width=128, height=96, fps=60).start()
        profile = root / 'profile.json'
        profile.write_text(json.dumps(dict(
            bind_ip='127.0.0.1', sender_ip='127.0.0.1', pi_ip='127.0.0.1',
            frame_port=frame_port, pi_port=pi_port, model=str(weights), max_age_ms=250,
            omni_python=sys.executable, omni_device='cpu',
            omni_worker=str(Path(__file__).with_name('fault_worker.py').resolve()),
            reference_x=76.8, reference_y=48, gain_x=0, gain_y=0, constant_tracking=True,
            trigger=dict(enabled=True, delay_ms=60, interval_ms=180),
            collection=dict(enabled=True, auto_label=True, interval_ms=100, directory=str(root / 'dataset')))))
        control = Control(sys.argv[1], profile)
        try:
            control.wait(lambda s: s['pi_ready'] and s['synchronized'] and s['inferred'] > 3,
                         'Worker and peers did not become ready')
            state = control.wait(lambda s: s['collected_frames'] > 0, 'Collection did not write frames')
            assert state['auto_triggered'] == 0 and not pi.click_requests, 'Disarmed app triggered'
            assert list((root / 'dataset/images').glob('*.ppm'))
            assert list((root / 'dataset/labels').glob('*.txt'))
            assert not state['collection_error']
            assert control.request('arm', enabled=True)['ok']
            state = control.wait(lambda s: s['auto_triggered'] == 1 and s['click_completed'] >= 1,
                                 'Constant tracking did not trigger without held buttons')
            time.sleep(.5)
            assert control.request()['stats']['auto_triggered'] == 1, 'Single mode fired repeatedly'
            pi.telemetry_enabled = False
            control.wait(lambda s: not s['active'], 'Single mode did not pause on telemetry loss')
            pi.telemetry_enabled = True
            control.wait(lambda s: s['active'], 'Single mode did not recover')
            time.sleep(.2)
            assert control.request()['stats']['auto_triggered'] == 1, 'Telemetry recovery re-armed the same target'
            assert control.request('configure', settings=dict(trigger=dict(spray=True)))['ok']
            assert not control.request()['stats']['armed'], 'Trigger change must disarm'
            assert control.request('arm', enabled=True)['ok']
            control.wait(lambda s: s['auto_triggered'] >= 4, 'Spray did not repeat')
            assert control.request('configure', settings=dict(constant_tracking=False))['ok']
            assert control.request('arm', enabled=True)['ok']
            before = control.request()['stats']['auto_triggered']
            time.sleep(.3)
            assert control.request()['stats']['auto_triggered'] == before, 'Held mode ignored button gate'
            pi.physical = 2
            control.wait(lambda s: s['auto_triggered'] > before, 'Physical activation did not trigger')
            pi.physical = 0
            control.wait(lambda s: not s['active'], 'Button release did not deactivate')
            before = control.request()['stats']['auto_triggered']
            time.sleep(.3)
            assert control.request()['stats']['auto_triggered'] == before, 'Trigger continued after release'
            pi.physical = 2
            assert control.request('configure', settings=dict(trigger=dict(press_ms=800, interval_ms=900)))['ok']
            assert control.request('arm', enabled=True)['ok']
            control.wait(lambda s: any(not c['release'] and not c['completed'] for c in list(pi.clicks.values())),
                         'Long automatic click was not accepted')
            pi.telemetry_enabled = False
            control.wait(lambda s: not s['active'], 'Telemetry loss did not deactivate')
            time.sleep(.1)
            assert any(c['cancelled'] for c in list(pi.clicks.values()) if not c['release']), \
                'Telemetry loss did not cancel the outstanding automatic click'
            pi.telemetry_enabled = True
            control.wait(lambda s: s['pi_ready'], 'Telemetry did not recover')
            assert control.request('configure', settings=dict(trigger=dict(press_ms=10, interval_ms=180)))['ok']
            before = control.request()['stats']['auto_triggered']
            assert control.request('arm', enabled=True)['ok']
            control.wait(lambda s: s['auto_triggered'] > before, 'Reacquisition failed')
            sender.stop()
            control.wait(lambda s: not s['active'], 'Sender loss did not deactivate')
            before = control.request()['stats']['auto_triggered']
            time.sleep(.3)
            assert control.request()['stats']['auto_triggered'] == before, 'Trigger continued on stale frames'
            print('Collection, single/spray triggering, activation, telemetry cancellation and sender-loss gating passed')
        finally:
            control.close()
            sender.stop()
            pi.stop()


if __name__ == '__main__':
    main()
