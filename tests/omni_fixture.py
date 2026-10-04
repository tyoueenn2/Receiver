"""Deterministic worker fixture; never loads weights or touches USB/GPU."""
import argparse
from pathlib import Path
import socket
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from yolo_omni_worker import serve, send_json


class Fixture:
    def __init__(self, request):
        self.scenario = Path(request['model']).read_text()
        if self.scenario == 'load_error':
            raise ValueError('fixture checkpoint load failed')

    def information(self):
        if self.scenario == 'stall':
            time.sleep(30)
        return dict(status='ready', version=1, names=['object', 'other'],
                    description='YOLO-Omni test fixture', free_memory=0, total_memory=0)

    def run(self, data, width, height, fmt):
        assert (width, height) == (320, 160)
        assert list(data[:3]) == ([12, 34, 56] if fmt == 1 else [56, 34, 12])
        count = 100001 if self.scenario == 'bad_shape' else 1
        return dict(status='result', candidates=count, classes=2, inference_ms=.25), struct.pack('<6f', 160, 160, 40, 80, .9, .1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--token', type=int, required=True)
    args = parser.parse_args()
    with socket.create_connection(('127.0.0.1', args.port)) as stream:
        stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        stream.sendall(struct.pack('!Q', args.token))
        try:
            serve(stream, Fixture)
        except EOFError:
            pass
        except Exception as exc:
            send_json(stream, dict(status='error', message=str(exc)))


if __name__ == '__main__':
    main()
