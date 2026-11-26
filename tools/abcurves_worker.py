"""Run the ABCurves 2 Continuous Planner and native Renderer."""
import math
from pathlib import Path

from worker_protocol import read_json, run_worker, send_json


def load_pipeline(abcurves):
    import numpy as np
    from abcurves.continuous import COMMON_RADIANS_PER_COUNT

    profile = Path(abcurves.__file__).resolve().parent.parent / 'examples' / 'data' / 'human_start.npz'
    if not profile.is_file():
        raise RuntimeError('Install ABCurves 2 from its checkout to use the bundled movement profile')
    with np.load(profile, allow_pickle=False) as data:
        # Preserve the existing mapping from one configured output count to
        # one common count. The pipeline converts native Y-down reports.
        return abcurves.ContinuousPipeline(
            data['profile_hardware'],
            transform=abcurves.CountTransform(COMMON_RADIANS_PER_COUNT, y_down=True),
            prewarm=True,
        )


class Movement:
    def __init__(self):
        try:
            import abcurves
        except ImportError as exc:
            raise RuntimeError('Install ABCurves 2 in the Python environment selected in Receiver') from exc
        if not abcurves.__version__.startswith('2.'):
            raise RuntimeError('Receiver requires ABCurves 2')
        self.pipeline = load_pipeline(abcurves)
        self.generation = None
        self.time_us = 0

    def advance(self, request):
        generation, step_ms = request['generation'], request['step_ms']
        if type(step_ms) is not int or not 1 <= step_ms <= 32:
            raise ValueError('Invalid ABCurves step duration')
        x, y = request['target']
        actual_x, actual_y = request['executed']
        if not all(math.isfinite(v) for v in (x, y, actual_x, actual_y)):
            raise ValueError('Non-finite ABCurves input')
        if generation != self.generation:
            self.pipeline.reset()
            self.generation = generation
            self.time_us = 0
        self.pipeline.update_target((actual_x + x, -(actual_y + y)), timestamp_us=self.time_us)
        self.time_us += step_ms * 1000
        rendered = self.pipeline.advance(self.time_us)['reports']
        if len(rendered) != step_ms:
            raise RuntimeError('Unexpected ABCurves output length')
        return {'status': 'result', 'reports': [[int(dx), int(dy)] for dx, dy in rendered]}


def serve(stream):
    motion = Movement()
    send_json(stream, {'status': 'ready', 'version': 2})
    while True:
        send_json(stream, motion.advance(read_json(stream)))


if __name__ == '__main__':
    run_worker(serve, __doc__)
