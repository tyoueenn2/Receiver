"""Exercise ABCurves selection, commands, freshness and worker errors over loopback."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from integration_test import MockPi, Sender, free_port, wait_for
from process_capture import collect_logged_output, stop_logged_process



PLANNER_FIXTURE = '''
from pathlib import Path
import time
from types import SimpleNamespace

__version__ = "2.0.0"

def prepare_history(history, *, current_xy):
    assert len(history) == 164 and current_xy == (0, 0)
    return SimpleNamespace(observed_xy=current_xy, history=history[-160:])

class Planner:
    def reset(self, *, initial_xy, history):
        assert initial_xy == (0, 0) and len(history) == 160
        self.vx, self.vy = history[-1]

    def update_target(self, target, timestamp_us):
        assert timestamp_us == 0

    def advance(self, timestamp_us):
        assert timestamp_us in (160000, 320000)
        if Path("fail").exists():
            raise RuntimeError("fixture planner failure")
        if Path("stall").exists():
            Path("entered").touch()
            while Path("stall").exists():
                time.sleep(.005)
        if timestamp_us == 320000:
            if Path("flat").exists():
                Path("flat-entered").touch()
                return {"xy": [(0, 0)] * 320}
            if not self.vx and not self.vy:
                # The first positive motion is too small to use before the forecast is refreshed.
                return {"xy": [(.001 * min(i, 100) + .5 * max(i - 100, 0), -.3 * max(i - 100, 0))
                               for i in range(1, 321)]}
            # A reversed hand first goes left, then turns toward the target late in the forecast.
            turn = 80 if self.vx < 0 else 0
            vertical = 0.7 if self.vy < 0 else -0.7
            return {"xy": [(-min(i, turn) + max(i - turn, 0), vertical * max(i - turn, 0))
                           for i in range(1, 321)]}
        return {"xy": [(i * 2 * self.vx, i * 2 * self.vy - max(0, i - 20))
                       for i in range(1, 161)]}

def load(*, prewarm):
    assert prewarm
    return Planner()
'''

def check_additive(pi, sender, root, log, real_python):
    def moves():
        return [c for c in pi.commands.copy() if c['dx'] or c['dy']]

    wait_for(lambda: any(c['dx'] > 0 for c in moves()),
             'An away-going hand did not receive a targetward correction')
    assert not any(e['angle'] for e in pi.steering_events), 'Additive mode sent a steering advisory'
    assert all(abs(c['dx']) <= 5 and abs(c['dy']) <= 5 for c in moves()), 'Exceeded the configured step limit'
    if not real_python:
        wait_for(lambda: any(c['dy'] > 0 for c in moves()), 'The forecast curve produced no vertical movement')
        assert all(c['dx'] > 0 and c['dy'] >= 0 for c in moves()), 'Played the away-going forecast prefix'
        count = len(moves())
        pi.motion_y = 2000
        wait_for(lambda: any(c['dy'] < 0 for c in moves()[count:]),
                 'Recent physical movement did not change the generated path')

    pi.motion_x = pi.motion_y = 0
    time.sleep(.18)
    count = len(moves())
    wait_for(lambda: any(c['dx'] > 0 for c in moves()[count:]), 'Steering stopped when the hand stopped')

    pi.history_enabled = False
    time.sleep(.08)
    count = len(moves())
    time.sleep(.08)
    assert len(moves()) == count, 'Additive motion continued without physical history'
    pi.history_enabled = True
    wait_for(lambda: len(moves()) > count, 'Additive movement did not resume with history')

    sender.paused = True
    time.sleep(.15)
    count = len(moves())
    time.sleep(.08)
    assert len(moves()) == count, 'Additive movement used stale capture'
    sender.paused = False
    wait_for(lambda: len(moves()) > count, 'Additive movement did not resume with capture')

    if not real_python:
        (root / 'stall').touch()
        wait_for((root / 'entered').exists, 'No outstanding additive forecast')
    pi.physical = 0
    wait_for(lambda: 'Armed - hold activation button' in log.read_text(), 'Activation release was ignored')
    count = len(moves())
    if not real_python:
        (root / 'stall').unlink()
    time.sleep(.16)
    assert len(moves()) == count, 'Additive movement continued after activation release'


def check_idle(pi, root, log, real_python):
    def moves():
        return [c for c in pi.commands.copy() if c['dx'] or c['dy']]

    wait_for(lambda: sum(c['dx'] > 0 and (real_python is not None or c['dy'] > 0) for c in moves()) >= 3,
             'Stationary activation did not sustain targetward movement')
    assert not any(e['angle'] for e in pi.steering_events), 'Idle additive mode sent a steering advisory'
    if not real_python:
        (root / 'flat').touch()
        wait_for((root / 'flat-entered').exists, 'The flat fixture forecast did not run')
        time.sleep(.08)
        count = len(moves())
        wait_for(lambda: any(c['dx'] == 1 and c['dy'] == 0 for c in moves()[count:]),
                 'The flat forecast prevented stationary movement')

    pi.physical = 0
    wait_for(lambda: 'Armed - hold activation button' in log.read_text(), 'Activation release was ignored')
    count = len(moves())
    time.sleep(.12)
    assert len(moves()) == count, 'Idle additive movement continued after activation release'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=lambda s: Path(s).resolve())
    parser.add_argument('--real-python', help='Use an interpreter with ABCurves installed')
    scenarios = parser.add_mutually_exclusive_group()
    scenarios.add_argument('--additive', action='store_true', help='Exercise additive trajectory playback')
    scenarios.add_argument('--idle', action='store_true', help='Exercise additive playback from a still mouse')
    args = parser.parse_args()
    source_tools = Path(__file__).resolve().parents[1] / 'tools'
    with tempfile.TemporaryDirectory(prefix='receiver-abcurves-') as directory:
        root = Path(directory)
        (root / 'tools').mkdir()
        for file in ('abcurves_worker.py', 'worker_protocol.py'):
            shutil.copyfile(source_tools / file, root / 'tools' / file)
        env = os.environ.copy()
        if not args.real_python:
            (root / 'abcurves.py').write_text(PLANNER_FIXTURE)
            env['PYTHONPATH'] = str(root)
        frame_port, pi_port = free_port(), free_port()
        while frame_port == pi_port:
            pi_port = free_port()
        profile = root / 'profile.json'
        profile.write_text(json.dumps(dict(frame_port=frame_port, pi_port=pi_port,
            gain_x=.2 if args.idle else 2, gain_y=.2 if args.idle else 2,
            reference_x=209 if args.idle else -1, max_age_ms=50 if args.idle else 100,
            max_step=5 if args.additive or args.idle else 32,
            omni_python=args.real_python or sys.executable,
            humanization=dict(enabled=True, path=6 if args.additive or args.idle else 5),
            direction=dict(enabled=args.idle, slow_strength=0))))
        pi = MockPi(pi_port)
        pi.history_enabled = True
        if args.additive:
            pi.motion_x = -2000
        pi.start()
        sender = Sender(port=frame_port, fps=90).start()
        log = root / 'receiver.log'
        with log.open('wb') as output:
            process = subprocess.Popen([str(args.executable), '--profile', str(profile), '--simulate',
                '--arm', '--seconds', '0'], cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT)

        def advice():
            return [event for event in pi.steering_events.copy() if event['angle']]

        try:
            wait_for(lambda: 'ABCurves ready' in log.read_text(), 'Hand history did not become ready',
                     timeout=120 if args.real_python else 8)
            if args.idle:
                check_idle(pi, root, log, args.real_python)
                return
            if args.additive:
                check_additive(pi, sender, root, log, args.real_python)
                return
            time.sleep(.12)
            assert not advice(), 'ABCurves engaged with a stationary mouse'
            pi.motion_x = 2000
            time.sleep(.14)
            assert not advice(), 'A target 64 pixels away exceeded the default range'
            sender.stop()
            sender = Sender(port=frame_port, width=160, height=160, fps=90).start()
            wait_for(lambda: len(advice()) >= (1 if args.real_python else 5), 'No near-target steering advice')
            assert all(e['angle'] == 3000 and e['mode'] == 0 and
                       0 < e['expires'] - e['sample'] <= 20 for e in advice())
            assert all(e['aim'][0] > 0 and abs(e['aim'][1]) <= 1 for e in advice())
            if not args.real_python:
                assert all(e['tangent'][1] > 500 for e in advice()), 'The turn after 20 ms did not affect steering'

            pi.motion_x = 4000
            pi.motion_y = 1000
            time.sleep(.18)
            count = len(advice())
            wait_for(lambda: len(advice()) > count, 'ABCurves did not steer a gesture inside the 30 degree cone')

            pi.motion_y = 3000
            time.sleep(.08)
            count = len(advice())
            time.sleep(.08)
            assert len(advice()) == count, 'ABCurves steered outside the direction cone'
            assert pi.steering_events[-1]['angle'] == 0, 'Receiver did not cancel steering'
            pi.motion_y = 0
            pi.motion_x = -2000
            time.sleep(.08)
            count = len(advice())
            time.sleep(.08)
            assert len(advice()) == count, 'ABCurves pushed against a reversed gesture'
            sender.stop()
            sender = Sender(port=frame_port, width=64, height=64, fps=90).start()
            wait_for(lambda: any(e['mode'] == 1 for e in advice()), 'Reversal blocked inner steering')
            inner_advice = [e for e in advice() if e['mode'] == 1]
            assert all(e['aim'][0] > 0 and e['tangent'][0] > 0 and
                       0 < e['expires'] - e['sample'] <= 20 for e in inner_advice)
            sender.stop()
            sender = Sender(port=frame_port, width=160, height=160, fps=90).start()
            wait_for(lambda: pi.steering_events[-1]['angle'] == 0, 'Leaving the inner radius did not cancel steering')
            time.sleep(.08)
            count = len(advice())
            time.sleep(.08)
            assert len(advice()) == count, 'Reversed input still steered in the outer ring'
            pi.motion_x = 0
            time.sleep(.08)
            assert len(advice()) == count, 'ABCurves moved after a stop'
            if not args.real_python:
                pi.history_enabled = False
                pi.motion_x = 2000
                time.sleep(.15)
                assert len(advice()) == count, 'ABCurves used unavailable physical history'
                pi.history_enabled = True
                wait_for(lambda: len(advice()) > count + 5, 'Steering did not resume with physical history')
                (root / 'stall').touch()
                wait_for((root / 'entered').exists, 'No outstanding planner request')
                pi.physical = 0
                wait_for(lambda: 'Armed - hold activation button' in log.read_text(),
                         'Receiver did not observe activation release')
                count = len(advice())
                (root / 'stall').unlink()
                time.sleep(.2)
                assert len(advice()) == count, 'Late reply produced movement after release'
                pi.physical = 2
                wait_for(lambda: len(advice()) > count + 5, 'Steering did not resume after activation')
                (root / 'entered').unlink()
                (root / 'stall').touch()
                wait_for((root / 'entered').exists, 'No outstanding request before the capture stall')
                sender.paused = True
                time.sleep(.15)
                count = len(advice())
                (root / 'stall').unlink()
                time.sleep(.15)
                assert len(advice()) == count, 'Late reply used an expired capture'
                sender.paused = False
                wait_for(lambda: len(advice()) > count + 5, 'Steering did not resume after fresh frames')
                (root / 'fail').touch()
                wait_for(lambda: 'fixture planner failure' in log.read_text(), 'Worker failure was hidden')
                count = len(advice())
                time.sleep(.2)
                assert len(advice()) == count, 'Receiver moved after the selected planner failed'
            assert all(not c['dx'] and not c['dy'] for c in pi.commands), 'ABCurves sent extra movement'
        finally:
            stop_logged_process(process)
            output = collect_logged_output(process, log, timeout=5)
            sender.stop()
            pi.stop()
            print(output)


if __name__ == '__main__':
    main()
