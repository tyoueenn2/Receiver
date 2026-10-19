"""HSV-only and hybrid targeting through real App workers, frames, and UDP output."""
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
    with tempfile.TemporaryDirectory(prefix='receiver-color-') as directory:
        root = Path(directory)
        model = root / 'players.pt'
        model.write_text(json.dumps(dict(names=['player'], detections=[
            [160, 170, 40, 100, 0, .9], [205, 65, 30, 30, 0, .99]])))
        pixels = bytearray(320 * 320 * 3)
        for x, y in [(200, 60), (150, 130), (160, 140)]:
            index = (y * 320 + x) * 3
            pixels[index:index + 3] = bytes([255, 0, 0])
        frame_port, pi_port = free_port(), free_port()
        pi = MockPi(pi_port, physical=0).start()
        sender = Sender(port=frame_port, width=320, height=320, fps=60, pixels=bytes(pixels)).start()
        profile = root / 'profile.json'
        profile.write_text(json.dumps(dict(
            bind_ip='127.0.0.1', sender_ip='127.0.0.1', pi_ip='127.0.0.1',
            frame_port=frame_port, pi_port=pi_port, max_age_ms=250,
            model='', omni_python='this-python-does-not-exist',
            omni_worker='this-worker-does-not-exist.py',
            color=dict(mode=1, offset_x=-.5, offset_y=-.5),
            preview=True, constant_tracking=True)))
        control = None
        phases = []
        try:
            control = Control(sys.argv[1], profile)
            ready = control.wait(lambda s: s['revision'] == 1 and s['pi_ready'] and s['synchronized']
                                 and s['inferred'] > 3, 'HSV-only session did not become ready')
            assert ready['classes'] == [] and 'HSV' in ready['backend'], ready
            assert not any(c['dx'] or c['dy'] for c in pi.commands), 'HSV startup moved while disarmed'
            assert not control.request('benchmark')['ok'], 'Color-only accepted a model benchmark'

            def expect(start, dx, dy, description):
                state = control.wait(lambda s: s['active'] and sum(
                    c['dx'] == dx and c['dy'] == dy and not c['buttons']
                    for c in pi.commands[start:]) >= 3, description)
                assert not state['error'] and not state['model_error'], state
                phases.append(description)

            assert control.request('arm', enabled=True)['ok']
            expect(len(pi.commands), 8, -20, 'HSV targets the highest red pixel without any model/runtime')
            start = len(pi.commands)
            response = control.request('configure', settings=dict(color=dict(offset_x=4.5, offset_y=9.5)))
            assert response['ok'] and response['stats']['armed'] and response['stats']['revision'] == 1
            expect(start, 9, -18, 'Live color offsets change the output without a model reload')

            def switch(mode, revision, **settings):
                response = control.request('configure', settings=dict(color=dict(
                    mode=mode, min_h=330, max_h=30, offset_x=-.5, offset_y=-.5), **settings))
                assert response['ok'] and not response['stats']['armed'], response
                control.wait(lambda s: s['revision'] == revision and not s['loading'], 'Mode switch failed')
                assert control.request('arm', enabled=True)['ok']
                return len(pi.commands)

            expect(switch(2, 2, model=str(model), input_size=320, auto_size=False,
                          omni_python=sys.executable, omni_device='cpu',
                          omni_worker=str(Path(__file__).with_name('fault_worker.py').resolve())),
                   -2, -6, 'Hybrid refines player A and ignores higher red outside that box')
            start = len(pi.commands)
            response = control.request('configure', settings=dict(color=dict(min_h=100, max_h=140)))
            assert response['ok'] and response['stats']['revision'] == 2 and response['stats']['armed']
            expect(start, 0, 2, 'Missing green falls back to the normal box aim point')
            expect(switch(0, 2), 0, 2, 'Model mode keeps normal aiming without reloading the model')
            expect(switch(2, 2), -2, -6, 'Enabling hybrid refinement reuses the loaded model')
            expect(switch(1, 3, model='', omni_python='missing-python'), 8, -20,
                   'Switching back to HSV releases the model and needs no Python')

            # A color miss must not retain the previous target or movement.
            sender.pixels = bytes(len(pixels))
            inferred = control.request()['stats']['inferred']
            control.wait(lambda s: s['inferred'] > inferred + 5, 'Blank color frames were not processed')
            time.sleep(.05)
            start = len(pi.commands)
            time.sleep(.1)
            assert not any(c['dx'] or c['dy'] for c in pi.commands[start:]), 'HSV retained a missing color'
            sender.pixels = bytes(pixels)
            expect(len(pi.commands), 8, -20, 'New matching pixels immediately restore color targeting')

            sender.stop()
            control.wait(lambda s: not s['active'], 'Color mode bypassed sender freshness')
            time.sleep(.05)
            start = len(pi.commands)
            time.sleep(.15)
            assert not any(c['dx'] or c['dy'] for c in pi.commands[start:]), 'HSV moved on stale video'
            print(str(len(phases)) + ' HSV/hybrid App and UDP scenarios plus color-loss/freshness gates passed')
        finally:
            try:
                if control is not None:
                    control.close()
            finally:
                sender.stop()
                pi.stop()


if __name__ == '__main__':
    main()
