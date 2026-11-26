"""Run the ABCurves 2 Continuous Planner for Receiver's movement path."""
import math

from worker_protocol import read_json, run_worker, send_json


class Movement:
    def __init__(self):
        try:
            import abcurves
        except ImportError as exc:
            raise RuntimeError('Install ABCurves 2 in the Python environment selected in Receiver') from exc
        if not abcurves.__version__.startswith('2.'):
            raise RuntimeError('Receiver requires ABCurves 2')
        self.planner = abcurves.load(prewarm=True)
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
            self.planner.reset()
            self.generation = generation
            self.time_us = 0
        # Receiver gives relative output counts with Y down. The planner takes
        # absolute common angular counts with Y up.
        self.planner.update_target((actual_x + x, -(actual_y + y)), timestamp_us=self.time_us)
        self.time_us += step_ms * 1000
        plan_x, plan_y = self.planner.advance(self.time_us)['xy'][-1]
        return {'status': 'result', 'dx': float(plan_x) - actual_x, 'dy': -float(plan_y) - actual_y}


def serve(stream):
    motion = Movement()
    send_json(stream, {'status': 'ready', 'version': 1})
    while True:
        send_json(stream, motion.advance(read_json(stream)))


if __name__ == '__main__':
    run_worker(serve, __doc__)
