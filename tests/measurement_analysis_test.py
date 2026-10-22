import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_move_measurements import analyze, read_events, stats


def event(name, **fields):
    return dict(schema=1, component='receiver', run='r1', event=name,
                session='18446744073709551615', command='7', generation='2', **fields)


def fixture():
    return [event('log_start'), event('submit', receiver_ns=100, calibration=False, context=0),
            event('landing', error_px=3, tolerance_px=2, receiver_ns=200,
                  reason='target_miss_click_skipped'),
            event('ack', terminal=True, status='cancelled', flags=0,
                  residual_counts=[0, 0], receiver_ns=1000100, pi_stage_ns=99999999999999),
            event('return', error_px=0, tolerance_px=2, receiver_ns=2000000),
            event('log_end', dropped=0)]


class AnalysisTests(unittest.TestCase):
    def test_separate_clocks_and_skipped_click(self):
        report = analyze(fixture())
        row = report['trials'][0]
        self.assertEqual(row['receiver_elapsed_ms'], 1)
        self.assertFalse(row['landing_pass'])
        self.assertTrue(row['return_pass'])
        self.assertFalse(row['usb_press_written'])
        self.assertEqual(row['application_click_result'], 'unobserved')
        self.assertEqual(row['session'], '18446744073709551615')
        self.assertFalse(report['integrity_issues'])

    def test_missing_is_not_zero(self):
        data = [e for e in fixture() if e['event'] not in ('landing', 'return')]
        row = analyze(data)['trials'][0]
        self.assertIsNone(row['landing_error_px'])
        self.assertIsNone(row['return_pass'])
        self.assertEqual(stats([])['n'], 0)

    def test_partial_drop_and_incomplete(self):
        data = [e for e in fixture() if e['event'] not in ('ack', 'log_end')]
        report = analyze(data)
        self.assertEqual(report['trials'][0]['status'], 'incomplete')
        self.assertEqual(len(report['integrity_issues']), 2)
        data = fixture()
        data[-1]['dropped'] = 3
        self.assertIn('dropped', analyze(data)['integrity_issues'][0])

    def test_usb_correlation_and_failure(self):
        data = fixture() + [dict(schema=1, component='pi', run='p', event='log_start'),
            dict(schema=1, component='pi', run='p', event='log_end', dropped=0)]
        data.append(dict(schema=1, component='pi', run='p', event='usb_write',
            session='18446744073709551615', command='7', generation='2',
            start_ns=9000000, end_ns=9500000, duration_ns=500000, success=False))
        row = analyze(data)['trials'][0]
        self.assertEqual(row['usb_write_ms'], [.5])
        self.assertEqual(row['usb_write_failures'], 1)
        data[-1]['duration_ns'] = -1
        with self.assertRaises(ValueError): analyze(data)

    def test_duplicate_and_invalid_json(self):
        data = fixture()
        data.append(data[1])
        with self.assertRaises(ValueError): analyze(data)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'bad.jsonl'
            path.write_text('{broken')
            with self.assertRaisesRegex(ValueError, ':1:'): read_events([path])
            path.write_text(json.dumps(event('landing', error_px=float('nan'))))
            with self.assertRaises(ValueError): read_events([path])

    def test_local_stop_and_endpoint_failure(self):
        data = [e for e in fixture() if e['event'] != 'ack']
        data.append(event('local_stop', receiver_ns=300, reason='sender_loss'))
        row = analyze(data)['trials'][0]
        self.assertEqual(row['status'], 'local_stop')
        self.assertTrue(row['usb_return_uncertain'])
        self.assertIn('sender_loss', row['reasons'])


if __name__ == '__main__': unittest.main()
