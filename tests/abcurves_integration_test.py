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
        assert timestamp_us == 16000
        if Path("fail").exists():
            raise RuntimeError("fixture planner failure")
        if Path("stall").exists():
            Path("entered").touch()
            while Path("stall").exists():
                time.sleep(.005)
        return {"xy": [(i * 2 * self.vx, i * (2 * self.vy - .1)) for i in range(1, 17)]}

def load(*, prewarm):
    assert prewarm
    return Planner()
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=lambda s: Path(s).resolve())
    parser.add_argument('--real-python', help='Use an interpreter with ABCurves installed')
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
            gain_x=2, gain_y=2, max_age_ms=100,
            omni_python=args.real_python or sys.executable, humanization=dict(enabled=True, path=5))))
        pi = MockPi(pi_port)
        pi.history_enabled = True
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
            time.sleep(.12)
            assert not advice(), 'ABCurves engaged with a stationary mouse'
            pi.motion_x = 2000
            time.sleep(.14)
            assert not advice(), 'A target 64 pixels away exceeded the default range'
            sender.stop()
            sender = Sender(port=frame_port, width=160, height=160, fps=90).start()
            wait_for(lambda: len(advice()) >= (1 if args.real_python else 5), 'No near-target steering advice')
            assert all(e['angle'] == 3000 and 0 < e['expires'] - e['sample'] <= 20 for e in advice())
            assert all(e['aim'][0] > 0 and abs(e['aim'][1]) <= 1 for e in advice())

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
