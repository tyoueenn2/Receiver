"""Forecast steering directions from the user's real mouse movement."""
import math
import os

from worker_protocol import read_json, run_worker, send_json

HISTORY_LENGTH = 164
HORIZON_MS = 160
TRAJECTORY_MS = 320
MAX_PLAN_AGE_MS = 16
VELOCITY_DECAY = 0.5 ** (1 / 40)


class Movement:
    def __init__(self):
        try:
            import abcurves
        except ImportError as exc:
            raise RuntimeError('Install ABCurves 2 in the Python environment selected in Receiver') from exc
        if not abcurves.__version__.startswith('2.'):
            raise RuntimeError('Receiver requires ABCurves 2')
        self.prepare_history = abcurves.prepare_history
        self.planner = abcurves.load(prewarm=True)

    def advance(self, request):
        x, y = request['target']
        raw = request['history']
        if len(raw) != HISTORY_LENGTH or not all(
            len(delta) == 2 and all(type(v) is int and -32768 <= v <= 32767 for v in delta)
            for delta in raw
        ):
            raise ValueError('Invalid physical mouse history')
        if not math.isfinite(x) or not math.isfinite(y):
            raise ValueError('Non-finite ABCurves target')
        # Reset at the actual endpoint of the history. Each request forecasts
        # from a new observation, including the user's unassisted movement.
        start = self.prepare_history([(dx, -dy) for dx, dy in raw], current_xy=(0, 0))
        self.planner.reset(initial_xy=start.observed_xy, history=start.history)
        self.planner.update_target((x, -y), timestamp_us=0)
        trajectory = request.get('trajectory', False)
        horizon = TRAJECTORY_MS if trajectory else HORIZON_MS
        positions = self.planner.advance(horizon * 1000)['xy']
        if len(positions) != horizon:
            raise RuntimeError('Unexpected ABCurves forecast length')
        if trajectory:
            return {'status': 'result', 'positions': [[float(x), -float(y)] for x, y in positions]}
        directions = [None] * (MAX_PLAN_AGE_MS + 1)
        future_x = future_y = 0.0
        next_x, next_y = map(float, positions[-1])
        for i in range(HORIZON_MS - 1, -1, -1):
            previous_x, previous_y = map(float, positions[i - 1]) if i else (0.0, 0.0)
            dx, dy = next_x - previous_x, previous_y - next_y
            next_x, next_y = previous_x, previous_y
            # Weight nearer predicted velocities more heavily without discarding the far path.
            future_x = dx + VELOCITY_DECAY * future_x
            future_y = dy + VELOCITY_DECAY * future_y
            if i <= MAX_PLAN_AGE_MS:
                if x * future_x + y * future_y > 0:
                    directions[i] = [future_x, future_y]
                else:
                    directions[i] = [dx, dy]
        return {'status': 'result', 'directions': directions}


def serve(stream):
    motion = Movement()
    send_json(stream, {'status': 'ready', 'version': 4})
    while True:
        send_json(stream, motion.advance(read_json(stream)))


if __name__ == '__main__':
    # The planner's batch-one matrices are too small to benefit from BLAS worker teams.
    os.environ['OPENBLAS_NUM_THREADS'] = '1'
    run_worker(serve, __doc__)
