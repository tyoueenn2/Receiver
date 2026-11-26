"""Loopback protocol shared by Receiver's Python workers."""
import argparse
import json
import socket
import struct
import traceback

MAX_JSON = 65536


def read_exact(stream, length):
    data = bytearray(length)
    view = memoryview(data)
    offset = 0
    while offset < length:
        count = stream.recv_into(view[offset:])
        if not count:
            raise EOFError('Receiver disconnected')
        offset += count
    return data


def read_json(stream):
    length, = struct.unpack('!I', read_exact(stream, 4))
    if not 1 <= length <= MAX_JSON:
        raise ValueError('Invalid request size')
    value = json.loads(read_exact(stream, length))
    if not isinstance(value, dict):
        raise ValueError('Request must be an object')
    return value


def send_json(stream, value):
    data = json.dumps(value, allow_nan=False).encode('utf-8')
    if len(data) > MAX_JSON:
        raise ValueError('Response is too large')
    stream.sendall(struct.pack('!I', len(data)) + data)


def run_worker(serve, description):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--token', type=int, required=True)
    args = parser.parse_args()
    with socket.create_connection(('127.0.0.1', args.port), timeout=10) as stream:
        stream.settimeout(None)
        stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        stream.sendall(struct.pack('!Q', args.token))
        try:
            serve(stream)
        except (EOFError, ConnectionError):
            return
        except Exception as exc:
            traceback.print_exc()
            try:
                send_json(stream, dict(status='error', message=str(exc)))
            except (OSError, ValueError):
                pass
            raise SystemExit(1)
