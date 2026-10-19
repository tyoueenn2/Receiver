"""Real process lifecycle checks and a deterministic PID-reuse regression."""
import subprocess
import sys
import unittest
from unittest.mock import patch

from process_identity import alive, identity


class ProcessIdentityTests(unittest.TestCase):
    def test_current_process_is_alive(self):
        self.assertTrue(alive(identity()))

    def test_reused_pid_is_not_the_original_worker(self):
        worker = identity()
        with patch('process_identity.running_process_start', return_value=worker['started'] + 1):
            self.assertFalse(alive(worker))
        self.assertTrue(alive(worker))  # A live worker with the original identity must still be detected.

    def test_missing_process_is_not_alive(self):
        worker = identity()
        with patch('process_identity.running_process_start', return_value=None):
            self.assertFalse(alive(worker))

    def test_worker_lifecycle_and_cleanup(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        try:
            worker = identity(child.pid)
            self.assertTrue(alive(worker), 'A real surviving worker must fail the cleanup check')
            child.terminate()
            child.wait(timeout=5)
            self.assertFalse(alive(worker), 'An exited worker must not be reported as alive')
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
