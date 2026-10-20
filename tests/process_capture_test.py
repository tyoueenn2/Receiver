"""Check log backpressure, timeout diagnostics and child cleanup with real peers."""
from pathlib import Path
import sys
import tempfile
import time
import unittest

from process_capture import collect_logged_output, start_logged_process, stop_logged_process


class ProcessCaptureTests(unittest.TestCase):
    def wait_for_file(self, path):
        deadline = time.perf_counter() + 5
        while not path.exists():
            self.assertLess(time.perf_counter(), deadline, 'Child blocked before publishing readiness')
            time.sleep(.01)

    def test_logs_cannot_block_a_peer_while_driver_waits_for_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ready, log = root / 'ready', root / 'peer.log'
            child = start_logged_process([sys.executable, '-c',
                'import pathlib,sys,time; '
                'sys.stdout.write("x" * 1048576); sys.stdout.flush(); '
                'sys.stderr.write("y" * 1048576); sys.stderr.flush(); '
                'pathlib.Path(sys.argv[1]).touch(); time.sleep(60)', str(ready)], log)
            try:
                self.wait_for_file(ready)  # Do not read logs before the peer becomes ready.
                self.assertIsNone(child.poll())
                self.assertEqual(log.stat().st_size, 2097152)
            finally:
                stop_logged_process(child)
            self.assertIsNotNone(child.poll())

    def test_timeout_preserves_diagnostics_and_reaps_child(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ready, log = root / 'ready', root / 'peer.log'
            child = start_logged_process([sys.executable, '-c',
                'import pathlib,sys,time; print("TRACE before stall",flush=True); '
                'pathlib.Path(sys.argv[1]).touch(); time.sleep(60)', str(ready)], log)
            try:
                self.wait_for_file(ready)
                with self.assertRaisesRegex(AssertionError, 'TRACE before stall') as error:
                    collect_logged_output(child, log, timeout=.1)
                self.assertIn(str(log.resolve()), str(error.exception))
                self.assertIsNotNone(child.poll())
                self.assertIn('TRACE before stall', log.read_text())
            finally:
                stop_logged_process(child)

    def test_nonzero_exit_and_both_streams_remain_visible(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'peer.log'
            child = start_logged_process([sys.executable, '-c',
                'import sys; print("stdout",flush=True); '
                'print("stderr",file=sys.stderr,flush=True); sys.exit(7)'], log)
            try:
                output = collect_logged_output(child, log, timeout=5)
                self.assertEqual(child.returncode, 7)
                self.assertIn('stdout', output)
                self.assertIn('stderr', output)
            finally:
                stop_logged_process(child)


if __name__ == '__main__':
    unittest.main()
