"""Capture long-running test peers without blocking them on an unread log pipe."""
from pathlib import Path
import subprocess


def start_logged_process(command, log_path):
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open('wb') as output:
        return subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)


def stop_logged_process(process):
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def collect_logged_output(process, log_path, timeout):
    log_path = Path(log_path)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        stop_logged_process(process)
        output = log_path.read_text(encoding='utf-8', errors='replace')
        raise AssertionError(
            f'Process did not exit within {timeout}s; command={process.args!r}; '
            f'log={log_path.resolve()}\n{output}') from error
    return log_path.read_text(encoding='utf-8', errors='replace')
