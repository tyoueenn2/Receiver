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

def CountTransform(scale, *, y_down):
    assert scale > 0 and y_down
    return scale

class ContinuousPipeline:
    def __init__(self, profile, *, transform, prewarm):
        assert transform > 0 and prewarm and len(profile) == 256
        self.reset()

    def reset(self):
        self.time = 0

    def update_target(self, target, timestamp_us):
        assert self.time == timestamp_us

    def advance(self, timestamp_us):
        count = (timestamp_us - self.time) // 1000
        assert 1 <= count <= 32
        if Path("fail").exists():
            raise RuntimeError("fixture planner failure")
        if Path("stall").exists():
            Path("entered").touch()
            while Path("stall").exists():
                time.sleep(.005)
        self.time = timestamp_us
        sample = [(2, 1), (0, 0), (1, -1)]
        return {"reports": [sample[i] if i < len(sample) else (0, 0) for i in range(count)]}
'''

NUMPY_FIXTURE = '''
from contextlib import contextmanager

@contextmanager
def load(path, *, allow_pickle):
    assert path.is_file() and not allow_pickle
    yield {"profile_hardware": [(0, 0)] * 256}
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
            package = root / 'abcurves'
            package.mkdir()
            (package / '__init__.py').write_text(PLANNER_FIXTURE)
            (package / 'continuous.py').write_text('COMMON_RADIANS_PER_COUNT = 0.001')
            (root / 'numpy.py').write_text(NUMPY_FIXTURE)
            data = root / 'examples' / 'data'
            data.mkdir(parents=True)
            (data / 'human_start.npz').touch()
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
            assert any(c['dx'] > 0 for c in motions()) and any(c['dy'] > 0 for c in motions())
            if not args.real_python:
                pair = [(c['dx'], c['dy']) for c in motions()[:2]]
                assert pair == [(2, 1), (1, -1)], pair
                # The mock timestamps before recvfrom, which can group arrivals
                # when the receiving thread is descheduled. Sample several blocks.
                wait_for(lambda: len(motions()) >= 12, 'Too few rendered blocks to check pacing')
                commands = motions()
                gaps = [right['time'] - left['time'] for left, right in zip(commands, commands[1:])
                        if (left['dx'], left['dy'], right['dx'], right['dy']) == (2, 1, 1, -1)]
                assert any(gap >= .001 for gap in gaps), gaps
                pi.endpoint_poll_us = 4000
                wait_for(lambda: any((c['dx'], c['dy']) == (3, 0) for c in motions()),
                         'Slower endpoint did not receive the sum of adjacent reports')
                pi.endpoint_poll_us = 1000
                pi.motion_x = 1500
                time.sleep(.08)
                during_motion = len(motions())
                time.sleep(.06)
                assert len(motions()) == during_motion, 'Renderer continued while physical motion was active'
                pi.motion_x = 0
                wait_for(lambda: len(motions()) > during_motion + 2, 'Movement did not resume after physical input')
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
