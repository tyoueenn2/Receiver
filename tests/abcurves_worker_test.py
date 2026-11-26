"""Check the ABCurves hand-history adapter with a deterministic planner."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, call, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from abcurves_worker import Movement


class AbcurvesWorkerTests(unittest.TestCase):
    def make_movement(self):
        planner = Mock()
        prepare = Mock(return_value=SimpleNamespace(observed_xy=(0, 0), history='filtered'))
        load = Mock(return_value=planner)
        module = SimpleNamespace(__version__='2.0.0', prepare_history=prepare, load=load)
        with patch.dict(sys.modules, abcurves=module):
            movement = Movement()
        load.assert_called_once_with(prewarm=True)
        return movement, planner, prepare

    def test_forecast_starts_from_current_hand_history(self):
        movement, planner, prepare = self.make_movement()
        planner.advance.return_value = {'xy': [(i, -2 * i) for i in range(1, 17)]}
        first = [(1, 2)] * 164
        second = [(-1, 3)] * 164
        for history in (first, second):
            result = movement.advance(dict(target=(19, -3), history=history))
            self.assertEqual(result['positions'][0], [1, 2])
            self.assertEqual(result['positions'][-1], [16, 32])
        self.assertEqual(prepare.call_args_list, [
            call([(1, -2)] * 164, current_xy=(0, 0)),
            call([(-1, -3)] * 164, current_xy=(0, 0)),
        ])
        self.assertEqual(planner.reset.call_args_list, [call(initial_xy=(0, 0), history='filtered')] * 2)
        self.assertEqual(planner.update_target.call_args_list, [call((19, 3), timestamp_us=0)] * 2)
        self.assertEqual(planner.advance.call_args_list, [call(16000)] * 2)

    def test_invalid_history_does_not_drive_planner(self):
        movement, planner, prepare = self.make_movement()
        for history in ([], [(0, 0)] * 163, [(40000, 1)] * 164, [(0, .2)] * 164):
            with self.assertRaisesRegex(ValueError, 'Invalid physical mouse history'):
                movement.advance(dict(target=(3, 4), history=history))
        with self.assertRaisesRegex(ValueError, 'Non-finite'):
            movement.advance(dict(target=(float('inf'), 1), history=[(0, 0)] * 164))
        self.assertFalse(planner.mock_calls)
        prepare.assert_not_called()

    def test_missing_or_unsupported_dependency_reports_setup_error(self):
        with patch.dict(sys.modules, abcurves=None):
            with self.assertRaisesRegex(RuntimeError, 'Install ABCurves 2'):
                Movement()
        load = Mock()
        with patch.dict(sys.modules, abcurves=SimpleNamespace(__version__='1.9.0', load=load)):
            with self.assertRaisesRegex(RuntimeError, 'requires ABCurves 2'):
                Movement()
        load.assert_not_called()


if __name__ == '__main__':
    unittest.main()
