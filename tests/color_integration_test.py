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


def targeting():
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


def triggering():
    with tempfile.TemporaryDirectory(prefix='receiver-color-trigger-') as directory:
        frame_port, pi_port = free_port(), free_port()
        pi = MockPi(pi_port, physical=0).start()
        left, right, above = (56, 48), (72, 48), (64, 40)

        def pixels(points):
            data = bytearray(128 * 96 * 3)
            for x, y in points:
                offset = (y * 128 + x) * 3
                data[offset:offset + 3] = bytes([255, 0, 0])
            return bytes(data)

        sender = Sender(port=frame_port, width=128, height=96, fps=60,
                        pixels=pixels([left, right])).start()
        profile = Path(directory) / 'profile.json'
        profile.write_text(json.dumps(dict(
            bind_ip='127.0.0.1', sender_ip='127.0.0.1', pi_ip='127.0.0.1',
            frame_port=frame_port, pi_port=pi_port, max_age_ms=250,
            model='', omni_python='missing-python', omni_worker='missing-worker.py',
            gain_x=0, gain_y=0, constant_tracking=True,
            color=dict(mode=1, offset_x=7, offset_y=10),
            trigger=dict(enabled=True, delay_ms=60, interval_ms=100,
                         color_radius=8, color_tolerance=0))))
        control = None
        try:
            control = Control(sys.argv[1], profile)
            control.wait(lambda s: s['pi_ready'] and s['synchronized'] and s['inferred'] > 3,
                         'Color trigger session did not become ready')
            assert not pi.click_requests, 'Disarmed color session clicked'
            assert control.request('arm', enabled=True)['ok']

            def set_frame(points):
                sender.pixels = pixels(points)
                inferred = control.request()['stats']['inferred']
                control.wait(lambda s: s['inferred'] > inferred + 5, 'New color frame not processed')

            def no_clicks():
                before = control.request()['stats']['auto_triggered']
                time.sleep(.18)
                assert control.request()['stats']['auto_triggered'] == before, \
                    'Color trigger fired without the required geometry or repeated a single click'
                return before

            for points in ([left, right], [left, above], [right, above],
                           [(64, 48)], [left, right, (64, 56)]):
                set_frame(points)
                assert no_clicks() == 0
            set_frame([left, right, above])
            control.wait(lambda s: s['auto_triggered'] == 1 and s['click_completed'] >= 1,
                         'Left/right/above mask did not trigger with nonzero aim offsets')
            assert len(pi.click_requests) == 1
            set_frame([left, right, above, (50, 20)])
            assert no_clicks() == 1, 'Changing the topmost aim pixel re-armed single click'

            set_frame([left, right])
            assert no_clicks() == 1
            set_frame([left, right, above])
            control.wait(lambda s: s['auto_triggered'] == 2, 'Missing above did not allow reacquisition')

            # Scan settings persist and changing them disarms, just like existing click settings.
            response = control.request('configure', settings=dict(trigger=dict(color_radius=4)))
            assert response['ok'] and not response['stats']['armed'], response
            assert control.request('arm', enabled=True)['ok']
            assert no_clicks() == 2, 'Scan distance did not exclude distant matches'
            response = control.request('configure', settings=dict(trigger=dict(color_radius=8)))
            assert response['ok'] and not response['stats']['armed'], response
            set_frame([(56, 50), (72, 50), above])
            assert control.request('arm', enabled=True)['ok']
            assert no_clicks() == 2, 'Zero tolerance accepted off-axis side matches'
            response = control.request('configure', settings=dict(trigger=dict(color_tolerance=2)))
            assert response['ok'] and not response['stats']['armed'], response
            assert control.request('arm', enabled=True)['ok']
            control.wait(lambda s: s['auto_triggered'] == 3, 'Tolerance did not accept nearby side matches')

            pi.telemetry_enabled = False
            control.wait(lambda s: not s['active'], 'Color trigger ignored telemetry loss')
            assert no_clicks() == 3
            pi.telemetry_enabled = True
            control.wait(lambda s: s['active'], 'Color trigger did not recover telemetry')
            assert no_clicks() == 3, 'Telemetry recovery re-armed continuous occupancy'

            assert control.request('configure', settings=dict(trigger=dict(spray=True)))['ok']
            assert control.request('arm', enabled=True)['ok']
            control.wait(lambda s: s['auto_triggered'] >= 6, 'Color repeat mode did not repeat')
            sender.paused = True
            # Allow the current 250 ms frame deadline to expire while clock/hello traffic continues.
            time.sleep(.35)
            no_clicks()
            print('Color directional geometry, aim jitter, offsets, scan settings, single/repeat, '
                  'telemetry and stale-frame gates passed')
        finally:
            try:
                if control is not None:
                    control.close()
            finally:
                sender.stop()
                pi.stop()


if __name__ == '__main__':
    targeting()
    triggering()
