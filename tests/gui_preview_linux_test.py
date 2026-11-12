"""Exercise Linux's native preview window, shared GL texture, resize and close/reopen."""
import ctypes as c
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender
from integration_test import free_port

x11 = c.CDLL('libX11.so.6')
Window = c.c_ulong
x11.XOpenDisplay.argtypes = [c.c_char_p]
x11.XOpenDisplay.restype = c.c_void_p
x11.XDefaultRootWindow.argtypes = [c.c_void_p]
x11.XDefaultRootWindow.restype = Window
x11.XQueryTree.argtypes = [c.c_void_p, Window, c.POINTER(Window), c.POINTER(Window),
                           c.POINTER(c.POINTER(Window)), c.POINTER(c.c_uint)]
x11.XFetchName.argtypes = [c.c_void_p, Window, c.POINTER(c.c_char_p)]
x11.XFree.argtypes = [c.c_void_p]
x11.XResizeWindow.argtypes = [c.c_void_p, Window, c.c_uint, c.c_uint]
x11.XMapWindow.argtypes = [c.c_void_p, Window]
x11.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
x11.XInternAtom.restype = c.c_ulong
x11.XFlush.argtypes = [c.c_void_p]
x11.XCloseDisplay.argtypes = [c.c_void_p]


class ClientData(c.Union):
    _fields_ = [('longs', c.c_long * 5), ('bytes', c.c_char * 20)]


class ClientMessage(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                ('display', c.c_void_p), ('window', Window), ('message_type', c.c_ulong),
                ('format', c.c_int), ('data', ClientData)]


class Event(c.Union):
    _fields_ = [('client', ClientMessage), ('padding', c.c_long * 24)]


x11.XSendEvent.argtypes = [c.c_void_p, Window, c.c_int, c.c_long, c.POINTER(Event)]


def find_window(display, name, parent=None):
    parent = parent or x11.XDefaultRootWindow(display)
    root, owner, count = Window(), Window(), c.c_uint()
    children = c.POINTER(Window)()
    if not x11.XQueryTree(display, parent, c.byref(root), c.byref(owner), c.byref(children), c.byref(count)):
        return None
    try:
        for index in range(count.value):
            window = children[index]
            title = c.c_char_p()
            if x11.XFetchName(display, window, c.byref(title)) and title:
                matches = title.value.decode(errors='replace') == name
                x11.XFree(title)
                if matches:
                    return window
            found = find_window(display, name, window)
            if found:
                return found
    finally:
        if children:
            x11.XFree(children)
    return None


def until(process, condition):
    end = time.monotonic() + 8
    while process.poll() is None and time.monotonic() < end:
        value = condition()
        if value:
            return value
        time.sleep(.01)
    raise AssertionError('GUI exited or preview lifecycle did not finish')


def run_case(executable, case):
    pi = MockPi(port=free_port()).start()
    port = free_port()
    rgb = (255, 0, 0) if case == 'reopen' else (16, 80, 160)
    sender = Sender(port=port, width=128, height=96, pixels=bytes(rgb) * (128 * 96)).start()
    process = display = None
    try:
        display = x11.XOpenDisplay(None)
        assert display, 'Preview lifecycle tests require X11; run with xvfb-run -a'
        process = subprocess.Popen([str(Path(executable).resolve()), '--smoke-test', '--compact',
            '--test-frame-port=' + str(port), '--test-pi-port=' + str(pi.sock.getsockname()[1])]
            + (['--test-preview-reopen', '--test-color=only'] if case == 'reopen' else []),
            env=dict(os.environ, SDL_VIDEODRIVER='x11'))
        preview = until(process, lambda: find_window(display, 'Receiver Preview'))
        # Map the smoke window before resizing so GLX updates its drawable on Xvfb.
        x11.XMapWindow(display, preview)
        x11.XFlush(display)
        time.sleep(.1)
        x11.XResizeWindow(display, preview, 640, 480)
        x11.XFlush(display)
        time.sleep(.7)
        assert process.poll() is None, 'Resizing preview closed Receiver'
        if case != 'standalone':
            event = Event()
            event.client.type = 33  # ClientMessage / WM_DELETE_WINDOW
            event.client.display = display
            event.client.window = preview
            event.client.message_type = x11.XInternAtom(display, b'WM_PROTOCOLS', 0)
            event.client.format = 32
            event.client.data.longs[0] = x11.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
            assert x11.XSendEvent(display, preview, 0, 0, c.byref(event))
            x11.XFlush(display)
            until(process, lambda: not find_window(display, 'Receiver Preview'))
            if case == 'reopen':
                until(process, lambda: find_window(display, 'Receiver Preview'))
        assert process.wait(timeout=15) == 0, 'Linux GUI preview smoke check failed'
        state = json.loads(Path('gui-smoke-state.json').read_text())
        assert state['preview_frames'] >= 3 and state['main_context_restored'], state
        assert not state['armed'] and not state['gui_error'], state
        assert state['preview_enabled'] == (case != 'close'), state
        assert state['preview_window_open'] == state['preview_enabled'], state
        assert state['preview_openings'] == (2 if case == 'reopen' else 1), state
        assert pi.commands and all(not item['dx'] and not item['dy'] and not item['buttons']
                                   for item in pi.commands), 'Preview interaction emitted input'
        if case != 'close':
            assert (state['preview_width'], state['preview_height']) == (640, 480), state
            bitmap = Path('gui-smoke-preview.bmp').read_bytes()
            width, height = struct.unpack_from('<ii', bitmap, 18)
            assert (width, abs(height)) == (640, 480)
            offset = struct.unpack_from('<I', bitmap, 10)[0]
            assert bytes([rgb[2], rgb[1], rgb[0], 255]) in bitmap[offset:], \
                'Shared OpenGL texture did not render the received image'
        print(case + ': Linux preview video, resizing, close/reopen and disarmed output passed', flush=True)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if display:
            x11.XCloseDisplay(display)
        sender.stop()
        pi.stop()


if __name__ == '__main__':
    for name in ('standalone', 'close', 'reopen'):
        run_case(sys.argv[1], name)
