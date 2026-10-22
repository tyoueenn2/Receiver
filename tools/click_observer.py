#!/usr/bin/env python3
"""Windows test application: log clicks handled by this window, not USB ACKs.
Run one manually tagged trial at a time. Does not move or click the mouse.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--tag', required=True, help='unique manual trial label')
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('Run this observer on the Windows target')
    # Must precede Tk window creation: coordinates are physical desktop pixels.
    if not ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
        parser.error('Cannot enable per-monitor DPI awareness; use a fresh Python process')
    import tkinter as tk
    root = tk.Tk()
    root.title('Move / click / return observer — ' + args.tag)
    root.geometry('640x480')
    canvas = tk.Canvas(root, bg='white')
    canvas.pack(fill='both', expand=True)
    canvas.create_rectangle(240, 160, 400, 320, fill='#cce9ff', tags='target')
    caption = canvas.create_text(320, 60, text='Move-only first. Target coordinates appear below.')
    count = 0
    with args.log.open('a', encoding='utf-8') as log:
        def emit(event, **fields):
            log.write(json.dumps(dict(schema=1, component='click_observer', tag=args.tag,
                event=event, windows_monotonic_ns=time.perf_counter_ns(),
                utc=datetime.now(timezone.utc).isoformat(), **fields))+'\n')
            log.flush()
        def geometry():
            left, top = canvas.winfo_rootx(), canvas.winfo_rooty()
            return [left+240, top+160, left+400, top+320]
        def positioned(_event):
            box = geometry()
            canvas.itemconfigure(caption, text=f'Tag: {args.tag}\nTarget center (physical px): '
                f'{(box[0]+box[2])//2}, {(box[1]+box[3])//2}\nHandled target clicks: {count}')
        def clicked(event):
            nonlocal count
            hit = 240 <= event.x < 400 and 160 <= event.y < 320
            if hit: count += 1
            emit('button_press', button=event.num, position=[event.x_root, event.y_root],
                 target=geometry(), accepted_by_test_target=hit, count=count)
            positioned(None)
        canvas.bind('<Configure>', positioned)
        for button in (1, 2, 3):
            canvas.bind(f'<ButtonPress-{button}>', clicked)
        emit('observer_start')
        root.mainloop()
        emit('observer_end', accepted_count=count)


if __name__ == '__main__': main()
