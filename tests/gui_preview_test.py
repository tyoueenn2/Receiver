"""Exercise the actual second HWND, DirectX renderer, resize and close/reopen lifecycle."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender
from integration_test import free_port


user32 = ctypes.WinDLL('user32', use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [EnumProc, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                               ctypes.c_int, ctypes.c_int, wintypes.UINT]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]


def window(pid, name):
    matches = []
    @EnumProc
    def visit(handle, unused):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(handle, ctypes.byref(owner))
        label = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(handle, label, len(label))
        if owner.value == pid and label.value == name:
            matches.append(handle)
        return True
    user32.EnumWindows(visit, 0)
    return matches[0] if matches else None


def until(process, condition):
    end = time.monotonic() + 10
    while process.poll() is None and time.monotonic() < end:
        value = condition()
        if value:
            return value
        time.sleep(.01)
    raise AssertionError('GUI exited or did not create its preview window')


def run_case(executable, case):
    for name in ('gui-smoke.bmp', 'gui-smoke-preview.bmp', 'gui-smoke-state.json'):
        Path(name).unlink(missing_ok=True)
    pi = MockPi(port=free_port()).start()
    port = free_port()
    # A known solid image gives an independent oracle for the rendered bitmap.
    rgb = (255, 0, 0) if case == 'reopen' else (16, 80, 160)
    sender = Sender(port=port, width=128, height=96, pixels=bytes(rgb) * (128 * 96)).start()
    process = None
    try:
        process = subprocess.Popen([str(Path(executable).resolve()), '--smoke-test',
            '--page=connections', '--compact',
            '--test-frame-port=' + str(port), '--test-pi-port=' + str(pi.sock.getsockname()[1])]
            + (['--test-preview-reopen', '--test-color=only'] if case == 'reopen' else []),
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        preview = until(process, lambda: window(process.pid, 'ReceiverSeparatePreview'))
        assert user32.SetWindowPos(preview, None, 100, 100, 640, 480, 0x14), 'Preview resize failed'
        rect = wintypes.RECT()
        assert user32.GetClientRect(preview, ctypes.byref(rect))
        expected_size = rect.right, rect.bottom
        time.sleep(.7) # Allow several received frames to reach the second swap chain.
        assert process.poll() is None, 'Resizing preview closed the application'
        if case == 'standalone':
            main = until(process, lambda: window(process.pid, 'UdpVisionReceiver'))
            user32.ShowWindow(main, 6) # The independent preview must continue while main is minimized.
        else:
            assert user32.PostMessageW(preview, 0x10, 0, 0), 'Preview close failed'
            until(process, lambda: not window(process.pid, 'ReceiverSeparatePreview'))
            assert process.poll() is None, 'Closing preview closed Receiver'
            if case == 'reopen':
                until(process, lambda: window(process.pid, 'ReceiverSeparatePreview'))
        assert process.wait(timeout=15) == 0, 'GUI preview smoke check failed'
        state = json.loads(Path('gui-smoke-state.json').read_text())
        assert state['page'] == 'connections', state
        assert state['preview_frames'] >= 3 and state['main_context_restored'], state
        assert not state['armed'] and not state['gui_error'], state
        assert pi.commands and all(not c['dx'] and not c['dy'] and not c['buttons'] for c in pi.commands), \
            'Preview interaction emitted synthetic input'
        assert state['preview_enabled'] == (case != 'close'), state
        assert state['preview_window_open'] == state['preview_enabled'], state
        assert state['preview_openings'] == (2 if case == 'reopen' else 1), state
        if case != 'close':
            assert (state['preview_width'], state['preview_height']) == expected_size, state
            bitmap = Path('gui-smoke-preview.bmp').read_bytes()
            assert struct.unpack_from('<ii', bitmap, 18) == (expected_size[0], -expected_size[1])
            offset = struct.unpack_from('<I', bitmap, 10)[0]
            assert bytes([rgb[2], rgb[1], rgb[0], 255]) in bitmap[offset:], \
                'Second swap chain did not render the received image'
        print(case + ': preview video, native window lifecycle, overlays and disarmed output passed', flush=True)
    finally:
        for filename in ('gui-smoke.bmp', 'gui-smoke-preview.bmp', 'gui-smoke-state.json'):
            if Path(filename).exists():
                source = Path(filename)
                shutil.copyfile(source, source.with_name(source.stem + '-' + case + source.suffix))
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        sender.stop()
        pi.stop()


if __name__ == '__main__':
    for case in ('standalone', 'close', 'reopen'):
        run_case(sys.argv[1], case)
