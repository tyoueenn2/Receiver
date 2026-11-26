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

__version__ = "2.0.0"

class Planner:
    def reset(self):
        self.x = self.y = 0
        self.time = 0

    def update_target(self, target, timestamp_us):
        assert self.time == timestamp_us
        self.target = target

    def advance(self, timestamp_us):
        assert 1000 <= timestamp_us - self.time <= 32000
        if Path("fail").exists():
            raise RuntimeError("fixture planner failure")
        if Path("stall").exists():
            Path("entered").touch()
            while Path("stall").exists():
                time.sleep(.005)
        self.time = timestamp_us
        x, y = self.target
        self.x += (x - self.x) / 2
        self.y += (y - self.y) / 2
        return {"xy": [(self.x, self.y)]}

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
        max_step = 8
        profile.write_text(json.dumps(dict(frame_port=frame_port, pi_port=pi_port,
            gain_x=2, gain_y=2, max_step=max_step, offset_y=25, max_age_ms=100,
            omni_python=args.real_python or sys.executable, humanization=dict(enabled=True, path=5))))
        pi = MockPi(pi_port).start()
        sender = Sender(port=frame_port, fps=90).start()
        log = root / 'receiver.log'
        with log.open('wb') as output:
            process = subprocess.Popen([str(args.executable), '--profile', str(profile), '--simulate',
                '--arm', '--seconds', '0'], cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT)

        def motions():
            return [c for c in pi.commands.copy() if c['dx'] or c['dy']]

        try:
            wait_for(lambda: len(motions()) >= 5, 'No ABCurves movement', timeout=120 if args.real_python else 8)
            assert 'ABCurves ready' in log.read_text(), log.read_text()
            assert all(abs(c['dx']) <= max_step and abs(c['dy']) <= max_step for c in motions())
            assert any(c['dx'] > 0 and c['dy'] > 0 for c in motions())
            if not args.real_python:
                assert all((c['dx'], c['dy']) == (max_step, max_step) for c in motions())
                (root / 'stall').touch()
                wait_for((root / 'entered').exists, 'No outstanding planner request')
                pi.physical = 0
                wait_for(lambda: 'Armed - hold activation button' in log.read_text(),
                         'Receiver did not observe activation release')
                count = len(motions())
                (root / 'stall').unlink()
                time.sleep(.2)
                assert len(motions()) == count, 'Late reply produced movement after release'
                pi.physical = 2
                wait_for(lambda: len(motions()) > count + 5, 'Movement did not resume after activation')
                (root / 'entered').unlink()
                (root / 'stall').touch()
                wait_for((root / 'entered').exists, 'No outstanding request before the capture stall')
                sender.paused = True
                time.sleep(.15)
                count = len(motions())
                (root / 'stall').unlink()
                time.sleep(.15)
                assert len(motions()) == count, 'Late reply used an expired capture'
                sender.paused = False
                wait_for(lambda: len(motions()) > count + 5, 'Movement did not resume after fresh frames')
                (root / 'fail').touch()
                wait_for(lambda: 'fixture planner failure' in log.read_text(), 'Worker failure was hidden')
                count = len(motions())
                time.sleep(.2)
                assert len(motions()) == count, 'Receiver moved after the selected planner failed'
        finally:
            stop_logged_process(process)
            output = collect_logged_output(process, log, timeout=5)
            sender.stop()
            pi.stop()
            print(output)


if __name__ == '__main__':
    main()
