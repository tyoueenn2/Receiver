"""Verify class preference through real App workers, UVF1 frames and UDP output."""
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
    with tempfile.TemporaryDirectory(prefix='receiver-class-preference-') as directory:
        root = Path(directory)
        # YOLO centers/width/height, class ID, confidence. Player A is the normal target.
        player_a = [160, 190, 40, 100, 0, .9]
        player_b = [260, 150, 40, 100, 0, .95]
        head_a = [160, 130, 20, 20, 1, .8]
        head_b = [260, 90, 20, 20, 1, .99]

        def model(name, detections):
            path = root / (name + '.pt')
            path.write_text(json.dumps(dict(names=['player', 'head'], detections=detections)))
            return str(path)

        orphan = model('head of another player', [player_a, player_b, head_b])
        associated = model('associated heads', [player_a, player_b, head_a, head_b])
        missing = model('no heads', [player_a, player_b])
        frame_port, pi_port = free_port(), free_port()
        pi = MockPi(pi_port, physical=0).start()
        sender = Sender(port=frame_port, width=320, height=320, fps=60).start()
        profile = root / 'profile.json'
        profile.write_text(json.dumps(dict(
            bind_ip='127.0.0.1', sender_ip='127.0.0.1', pi_ip='127.0.0.1',
            frame_port=frame_port, pi_port=pi_port, model=orphan, classes=[0], max_age_ms=250,
            omni_python=sys.executable, omni_device='cpu',
            omni_worker=str(Path(__file__).with_name('fault_worker.py').resolve()),
            input_size=320, auto_size=False, constant_tracking=True)))
        control = None
        phases = []
        try:
            control = Control(sys.argv[1], profile)
            control.wait(lambda s: s['pi_ready'] and s['synchronized'] and s['inferred'] > 3,
                         'Preference worker and loopback peers did not become ready')
            assert not any(c['dx'] or c['dy'] for c in pi.commands), 'Disarmed preference test moved'

            def expect_movement(start, dx, dy, description):
                def matches(command):
                    return command['dx'] == dx and command['dy'] == dy and command['buttons'] == 0
                state = control.wait(
                    lambda s: s['active'] and sum(matches(c) for c in pi.commands[start:]) >= 3,
                    description + ': expected UDP movement ' + str((dx, dy)))
                assert not state['error'], state
                phases.append(description)

            assert control.request('arm', enabled=True)['ok']
            expect_movement(len(pi.commands), 0, 6, 'Normal filter targets player A')

            def preference(class_id=1, mode=0, distance=32):
                start = len(pi.commands)
                response = control.request('configure', settings=dict(class_preference=dict(
                    class_id=class_id, class_name='head' if class_id >= 0 else '',
                    mode=mode, max_distance=distance)))
                assert response['ok'], response
                assert response['stats']['revision'] == 1, 'Preference edit reloaded the model'
                return start

            expect_movement(preference(), 20, -14, 'Global preference admits head outside player filter')
            expect_movement(preference(mode=1), 0, 6, 'Near mode ignores another player\'s head')
            expect_movement(preference(mode=1, distance=100), 20, -14, 'Live association distance is applied')
            expect_movement(preference(mode=1), 0, 6, 'Reducing distance restores the same normal player')

            def load_scene(path, expected_revision):
                response = control.request('configure', settings=dict(model=path))
                assert response['ok'] and not response['stats']['armed'], response
                control.wait(lambda s: not s['loading'] and s['revision'] == expected_revision,
                             'Scene model did not load')
                assert control.request('arm', enabled=True)['ok']
                return len(pi.commands)

            expect_movement(load_scene(associated, 2), 0, -6, 'Near mode selects player A\'s own head')
            start = len(pi.commands)
            response = control.request('configure', settings=dict(
                class_preference=dict(class_id=-1, class_name='')))
            assert response['ok'], response
            expect_movement(start, 0, 6, 'Disabling preference restores normal selection')
            assert control.request('configure', settings=dict(
                class_preference=dict(class_id=1, class_name='head', mode=0)))['ok']
            expect_movement(load_scene(missing, 3), 0, 6, 'Missing preferred class falls back to player')

            sender.stop()
            control.wait(lambda s: not s['active'], 'Preference bypassed sender freshness gating')
            time.sleep(.05)  # Drain any UDP datagrams submitted before deactivation.
            start = len(pi.commands)
            time.sleep(.15)
            assert not any(c['dx'] or c['dy'] for c in pi.commands[start:]), 'Preference moved on stale video'
            print(str(len(phases)) + ' App/worker/UDP preference scenarios and sender-loss gating passed')
        finally:
            try:
                if control is not None:
                    control.close()
            finally:
                sender.stop()
                pi.stop()


if __name__ == '__main__':
    main()
