"""Production App + real worker processes + independent loopback Pi under faults."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender
from integration_test import free_port


class Control:
    def __init__(self, executable, profile):
        self.process = subprocess.Popen([str(Path(executable).resolve()), str(profile)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.responses = queue.Queue()
        def reader():
            for line in self.process.stdout:
                self.responses.put(json.loads(line))
            self.responses.put(None)
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()

    def request(self, operation='stats', **values):
        self.process.stdin.write(json.dumps(dict(operation=operation, **values)) + '\n')
        self.process.stdin.flush()
        try:
            response = self.responses.get(timeout=12)
        except queue.Empty:
            raise AssertionError('Scenario runner did not respond')
        assert response is not None, self.process.stderr.read()
        return response

    def wait(self, predicate, description, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            response = self.request()
            state = response['stats']
            if predicate(state):
                return state
            time.sleep(.01)
        raise AssertionError(description + ': ' + json.dumps(state))

    def close(self):
        if self.process.poll() is None:
            try:
                self.request('stop')
                self.reader.join(timeout=5)
                self.process.communicate(timeout=12)
            except Exception:
                self.process.kill()
                self.process.communicate(timeout=5)
                raise
        assert self.process.returncode == 0, self.process.stderr.read()


def alive(pid):
    if os.name != 'nt':
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    handle = kernel.OpenProcess(0x1000 | 0x100000, False, pid)
    if not handle:
        return False
    try:
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        return kernel.WaitForSingleObject(handle, 0) == 258
    finally:
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle(handle)


def movement(pi):
    return sum(bool(c['dx'] or c['dy'] or c['buttons']) for c in pi.commands)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    phases = []
    with tempfile.TemporaryDirectory(prefix='receiver-model-faults-') as directory:
        root = Path(directory)
        def model(name, **spec):
            path = root / (name + '.pt')
            path.write_text(json.dumps(spec))
            return str(path)
        good = model('good weights', mode='chunked')
        ports = free_port(), free_port()
        pi = MockPi(ports[1]).start()
        sender = Sender(port=ports[0], width=128, height=96, fps=60,
                        duplicate=.1, reorder=True).start()
        profile = root / 'profile.json'
        profile.write_text(json.dumps(dict(bind_ip='127.0.0.1', sender_ip='127.0.0.1', pi_ip='127.0.0.1',
            frame_port=ports[0], pi_port=ports[1], model=good, preview=True, max_age_ms=250,
            omni_device='cpu', omni_python=sys.executable,
            omni_worker=str(Path(__file__).with_name('fault_worker.py').resolve()),
            classes=[0], selected_class_names=['object'])))
        control = Control(args.executable, profile)
        try:
            control.wait(lambda s: s['revision'] == 1 and s['pi_ready'] and s['synchronized'] and s['inferred'] > 3,
                         'Initial worker did not become ready')
            assert not movement(pi), 'Startup emitted movement while disarmed'
            assert control.request('arm', enabled=True)['ok']
            control.wait(lambda s: s['sent'] > 3, 'Ready worker did not submit movement')
            assert control.request('button', button=3, enabled=True)['ok']
            control.wait(lambda s: s['persistent'] != 0, 'Synthetic hold missing')
            second = model('second', mode='ok', load_delay=.4, input_size=640, names=['other', 'object'])
            previous = control.request()['stats']
            changed = control.request('configure', settings=dict(model=second))
            assert changed['ok'] and changed['stats']['loading'] and not changed['stats']['armed']
            assert control.request('arm', enabled=True)['stats']['armed'] is False
            state = control.wait(lambda s: s['completed'] > previous['completed'] + 4 and s['loading'],
                                 'Pictures stopped arriving during loading')
            assert state['persistent'] == 0
            assert not control.request('click')['ok'], 'Click accepted while loading'
            state = control.wait(lambda s: s['revision'] == 2, 'Replacement not committed')
            assert state['running'] and not state['armed'] and state['settings']['input_size'] == 640
            assert state['settings']['classes'] == [1], 'Class labels did not remap'
            assert state['release_reason'] == 'configuration restart'
            saved = state['settings']
            subscription_sources = {event['source'] for event in pi.subscription_events}
            assert len(subscription_sources) == 1, 'Hot swap replaced the Pi connection'
            assert control.request('configure', settings=dict(model=model('bad-load', mode='load_error'),
                                                             confidence=.8))['ok']
            state = control.wait(lambda s: s['revision'] == 3, 'Failed replacement not rolled back')
            assert state['running'] and state['settings'] == saved and state['model_error']
            phases.append('hot swap, class remapping, release fence, live receive and load rollback')
            before = movement(pi)
            for _ in range(12):
                revision = control.request()['stats']['revision']
                assert control.request('configure', settings={}, reload=True)['ok']
                state = control.wait(lambda s: s['revision'] == revision + 1, 'Repeated reload hung')
                assert not state['armed'] and state['settings'] == saved
            assert movement(pi) == before, 'A reload re-enabled movement'
            phases.append('12 repeated worker reloads without rearming')
            inferred = state['inferred']
            control.wait(lambda s: s['inferred'] > inferred + 2, 'No fresh benchmark picture after reload')
            response = control.request('benchmark')
            assert response['ok'], response
            assert not control.request('arm', enabled=True)['stats']['armed']
            assert control.request('cancel')['ok']
            state = control.wait(lambda s: not s['benchmarking'], 'Benchmark cancellation hung')
            assert state['running'] and state['settings'] == saved
            phases.append('benchmark cancellation preserves the active model')
            assert {event['source'] for event in pi.subscription_events} == subscription_sources, \
                'Reload or benchmark replaced the Pi connection'
            for mode, extra in (('bad_version', {}), ('oversize', {}),
                                ('bad-size', dict(input_size=33)), ('empty-names', dict(names=[])),
                                ('empty-label', dict(names=[''])), ('wrong-name-type', dict(names=[3]))):
                revision = state['revision']
                assert control.request('configure', settings=dict(model=model(mode, mode=mode, **extra)))['ok']
                state = control.wait(lambda s: s['revision'] == revision + 1, 'Malformed ready reply not handled')
                assert state['running'] and state['settings'] == saved and state['model_error']
            for mode in ('crash_run', 'truncated', 'bad_classes', 'bad_timing', 'bad_shape'):
                assert control.request('configure', settings=dict(model=model(mode, mode=mode)))['ok']
                state = control.wait(lambda s: bool(s['error']), 'Broken inference did not stop the pipeline')
                assert not state['armed'] and state['persistent'] == 0
                assert control.request('restart', settings=dict(model=second))['ok']
                state = control.wait(lambda s: s['revision'] == 1 and s['pi_ready'] and s['inferred'] > 1,
                                     'Recovery after worker failure did not restart')
                assert not state['armed']
            phases.append('malformed handshakes, worker crashes, truncated payloads and bad outputs')
            stall = model('stall', mode='stall_load')
            assert control.request('configure', settings=dict(model=stall))['ok']
            deadline = time.monotonic() + 5
            while not Path(stall + '.pids').exists() and time.monotonic() < deadline:
                time.sleep(.01)
            assert Path(stall + '.pids').exists(), 'Stalled worker never launched'
            control.close()
            phases.append('shutdown cancels a stalled worker')
            pids = [int(line) for file in root.glob('*.pids') for line in file.read_text().splitlines()]
            deadline = time.monotonic() + 5
            while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
                time.sleep(.02)
            assert not any(alive(pid) for pid in pids), 'Worker process survived cleanup'
            print('\n'.join('Passed: ' + phase for phase in phases), flush=True)
        finally:
            control.close()
            sender.stop()
            pi.stop()
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(dict(phases=phases, workers=len(pids)), indent=2))


if __name__ == '__main__':
    main()
