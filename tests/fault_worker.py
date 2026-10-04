"""Independent worker protocol fixture. No ML packages, GPU, USB or OS input."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import time


def exact(stream, length):
    parts = bytearray()
    while len(parts) < length:
        data = stream.recv(length - len(parts))
        if not data:
            raise EOFError
        parts.extend(data)
    return bytes(parts)


def read_json(stream):
    length, = struct.unpack('!I', exact(stream, 4))
    if not 0 < length <= 65536:
        raise ValueError('fixture request length')
    return json.loads(exact(stream, length))


def send_json(stream, value, chunked=False):
    data = json.dumps(value, separators=(',', ':')).encode()
    packet = struct.pack('!I', len(data)) + data
    if chunked:
        for start in range(0, len(packet), 7):
            stream.sendall(packet[start:start + 7])
    else:
        stream.sendall(packet)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--token', type=int, required=True)
    args = parser.parse_args()
    with socket.create_connection(('127.0.0.1', args.port), timeout=10) as stream:
        stream.settimeout(None)
        stream.sendall(struct.pack('!Q', args.token))
        request = read_json(stream)
        weights = Path(request['model'])
        spec = json.loads(weights.read_text())
        with Path(str(weights) + '.pids').open('a') as output:
            output.write(str(os.getpid()) + '\n')
        mode = spec.get('mode', 'ok')
        time.sleep(spec.get('load_delay', 0))
        if mode == 'stall_load':
            time.sleep(60)
        if mode == 'load_error':
            send_json(stream, dict(status='error', message='deliberate fixture load failure'))
            return
        if mode == 'oversize':
            stream.sendall(struct.pack('!I', 65537))
            return
        names = spec.get('names', ['object', 'other'])
        size = spec.get('input_size', request['input_size'])
        info = dict(status='ready', version=2 if mode == 'bad_version' else 1,
                    names=names, input_size=size, description='independent fixture',
                    free_memory=0, total_memory=0)
        send_json(stream, info, mode == 'chunked')
        while True:
            frame = read_json(stream)
            exact(stream, frame['bytes'])
            if mode == 'crash_run':
                os._exit(17)
            if mode == 'stall_run':
                time.sleep(60)
            reply = dict(status='result', candidates=100001 if mode == 'bad_shape' else 1,
                         classes=len(names) + (mode == 'bad_classes'),
                         inference_ms=-1 if mode == 'bad_timing' else .2)
            send_json(stream, reply, mode == 'chunked')
            values = [.6 * size, .5 * size, .15 * size, .1 * size]
            values += [.9 if name == 'object' else .1 for name in names]
            data = struct.pack('<' + 'f' * len(values), *values)
            if mode == 'truncated':
                stream.sendall(data[:4])
                return
            if mode == 'chunked':
                for start in range(0, len(data), 3):
                    stream.sendall(data[start:start + 3])
            else:
                stream.sendall(data)


if __name__ == '__main__':
    try:
        main()
    except (EOFError, ConnectionError):
        pass
