"""Check the ABCurves adapter without installing or loading its model."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, call, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from abcurves_worker import Movement


class AbcurvesWorkerTests(unittest.TestCase):
    def make_movement(self):
        pipeline = Mock()
        module = SimpleNamespace(__version__='2.0.0')
        with patch.dict(sys.modules, abcurves=module), patch('abcurves_worker.load_pipeline', return_value=pipeline):
            movement = Movement()
        return movement, pipeline

    def test_preserves_native_reports_and_target_clock(self):
        movement, pipeline = self.make_movement()
        pipeline.advance.side_effect = [
            {'reports': [(1, 2), (0, 0), (-2, 1)]},
            {'reports': [(3, -2), (1, 5)]},
        ]
        first = movement.advance(dict(generation=1, step_ms=3, target=(20, 12), executed=(3, 4)))
        second = movement.advance(dict(generation=1, step_ms=2, target=(-3, 6), executed=(6, 7)))

        pipeline.reset.assert_called_once_with()
        self.assertEqual(pipeline.update_target.call_args_list, [
            call((23, -16), timestamp_us=0),
            call((3, -13), timestamp_us=3000),
        ])
        self.assertEqual(pipeline.advance.call_args_list, [call(3000), call(5000)])
        self.assertEqual(first, dict(status='result', reports=[[1, 2], [0, 0], [-2, 1]]))
        self.assertEqual(second, dict(status='result', reports=[[3, -2], [1, 5]]))

    def test_new_generation_restarts_planner_and_renderer_clock(self):
        movement, pipeline = self.make_movement()
        pipeline.advance.side_effect = [{'reports': [(2, 3)] * 32}, {'reports': [(-4, 2)]}]
        movement.advance(dict(generation=4, step_ms=32, target=(10, 2), executed=(0, 0)))
        result = movement.advance(dict(generation=5, step_ms=1, target=(-10, -2), executed=(0, 0)))

        self.assertEqual(pipeline.reset.call_args_list, [call(), call()])
        self.assertEqual(pipeline.update_target.call_args_list, [
            call((10, -2), timestamp_us=0),
            call((-10, 2), timestamp_us=0),
        ])
        self.assertEqual(pipeline.advance.call_args_list, [call(32000), call(1000)])
        self.assertEqual(result, dict(status='result', reports=[[-4, 2]]))

    def test_invalid_output_length(self):
        movement, pipeline = self.make_movement()
        pipeline.advance.return_value = {'reports': [(0, 0)]}
        with self.assertRaisesRegex(RuntimeError, 'output length'):
            movement.advance(dict(generation=4, step_ms=8, target=(10, 2), executed=(0, 0)))

    def test_invalid_requests_leave_planner_untouched(self):
        movement, planner = self.make_movement()
        valid = dict(generation=1, step_ms=8, target=(20, 12), executed=(0, 0))
        for step_ms in (0, -1, 33, 1.5, True):
            with self.subTest(step_ms=step_ms):
                with self.assertRaisesRegex(ValueError, 'step duration'):
                    movement.advance(dict(valid, step_ms=step_ms))
        for update in (dict(target=(float('nan'), 0)), dict(executed=(0, float('inf')))):
            with self.subTest(update=update):
                with self.assertRaisesRegex(ValueError, 'Non-finite'):
                    movement.advance(dict(valid, **update))
        self.assertEqual(planner.mock_calls, [])

    def test_missing_or_unsupported_dependency_reports_setup_error(self):
        with patch.dict(sys.modules, abcurves=None):
            with self.assertRaisesRegex(RuntimeError, 'Install ABCurves 2'):
                Movement()
        with patch.dict(sys.modules, abcurves=SimpleNamespace(__version__='1.9.0')), \
                patch('abcurves_worker.load_pipeline') as load:
            with self.assertRaisesRegex(RuntimeError, 'requires ABCurves 2'):
                Movement()
        load.assert_not_called()


if __name__ == '__main__':
    unittest.main()
