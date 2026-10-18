"""Identify the original worker even if its PID has since been reused."""
import ctypes
import os
from pathlib import Path


def running_process_start(pid):
    if os.name != 'nt':
        try:
            # Field 22 is the kernel start tick. The parenthesized command can contain spaces.
            fields = Path('/proc', str(pid), 'stat').read_text().rsplit(')', 1)[1].split()
        except (FileNotFoundError, ProcessLookupError):
            return None
        return int(fields[19])

    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x1000 | 0x100000, False, pid)  # Query identity and wait for termination.
    if not handle:
        return None
    try:
        state = kernel.WaitForSingleObject(handle, 0)
        if state == 0:
            return None
        if state != 258:  # WAIT_TIMEOUT means the process is still running.
            raise ctypes.WinError(ctypes.get_last_error())
        created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                      ctypes.byref(system), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        return created.dwHighDateTime << 32 | created.dwLowDateTime
    finally:
        kernel.CloseHandle(handle)


def identity(pid=None):
    pid = os.getpid() if pid is None else pid
    started = running_process_start(pid)
    if started is None:
        raise RuntimeError('Cannot record a running worker identity: ' + str(pid))
    return dict(pid=pid, started=started)


def alive(worker):
    started = running_process_start(worker['pid'])
    return started is not None and started == worker['started']
