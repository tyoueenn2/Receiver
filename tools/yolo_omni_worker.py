"""Persistent native YOLO-Omni/PyTorch worker, launched by Receiver over loopback TCP."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import time
import traceback

from omni_support import class_names, model_family, use_source

MAX_JSON = 65536
MAX_FLOATS = 16 * 1024 * 1024


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


def dimensions(request):
    width, height, fmt = (request[k] for k in ('width', 'height', 'format'))
    if any(type(v) is not int for v in (width, height, fmt)):
        raise ValueError('Frame dimensions and format must be integers')
    if not 1 <= width <= 1024 or not 1 <= height <= 1024 or fmt not in (1, 2):
        raise ValueError('Invalid frame dimensions or format')
    length = width * height * (3 if fmt == 1 else 4)
    if request['bytes'] != length:
        raise ValueError('Frame byte count mismatch')
    return width, height, fmt, length


def model_input_size(requested, automatic, saved, stride):
    if automatic:
        if isinstance(saved, (list, tuple)) and len(saved) == 2 and saved[0] == saved[1]:
            saved = saved[0]
        if type(saved) is int and 32 <= saved <= 1024:
            requested = saved
    if type(stride) is not int or stride < 1 or stride > 1024:
        raise ValueError('Unsupported model stride')
    alignment = max(32, stride)
    if alignment % 32:
        raise ValueError('Unsupported model stride')
    size = ((requested + alignment - 1) // alignment) * alignment
    if size > 1024:
        raise ValueError('Model input exceeds supported size after stride alignment')
    return size


class Model:
    def __init__(self, request):
        if request.get('version') != 1:
            raise ValueError('Unsupported Receiver worker protocol')
        weights = Path(request['model'])
        if weights.suffix.lower() != '.pt' or not weights.is_file():
            raise ValueError('Select an existing trained .pt detection checkpoint')
        self.size = request['input_size']
        if type(self.size) is not int or not 32 <= self.size <= 1024 or self.size % 32:
            raise ValueError('Model input must be 32..1024 in multiples of 32')
        if request['device'] not in ('cuda:0', 'cpu'):
            raise ValueError('Device must be cuda:0 or cpu')
        use_source(request.get('source', ''))
        # Dependencies belong to the selected environment; never install them or download weights here.
        os.environ['YOLO_AUTOINSTALL'] = 'false'
        # The fork uses full-module checkpoints and predates PyTorch 2.6's load default.
        os.environ.setdefault('TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD', '1')
        try:
            import numpy as np
            import torch
            from ultralytics import YOLO
        except ImportError as exc:
            raise RuntimeError('Install PyTorch and the YOLO-Omni fork in the selected Python environment') from exc
        self.np, self.torch = np, torch
        self.device = torch.device(request['device'])
        if self.device.type == 'cpu':
            torch.set_num_threads(min(4, torch.get_num_threads()))
        if self.device.type == 'cuda' and not torch.cuda.is_available():
            raise RuntimeError('CUDA is unavailable in this Python environment; install a CUDA-enabled '
                               'PyTorch build, or explicitly select cpu for testing')
        try:
            wrapper = YOLO(str(weights))
        except (AttributeError, ModuleNotFoundError) as exc:
            raise RuntimeError('Checkpoint modules are unavailable. Select the YOLO-Omni repository '
                               'in omni_source or install its fork into this Python environment') from exc
        if wrapper.task != 'detect':
            raise ValueError('Only detection checkpoints are supported (no pose, segmentation, OBB or classification)')
        self.names = class_names(wrapper.names)
        self.network = wrapper.model.float().to(self.device).eval()
        model_args = getattr(self.network, 'args', {})
        if not isinstance(model_args, dict):
            model_args = {}
        saved = model_args.get('imgsz', getattr(wrapper, 'overrides', {}).get('imgsz'))
        stride = getattr(self.network, 'stride', None)
        self.size = model_input_size(self.size, request.get('auto_size', False), saved,
                                     int(stride.max().item()) if stride is not None else 32)
        self.family = model_family(self.network)
        if getattr(self.network, 'end2end', False):
            raise ValueError('End-to-end/NMS checkpoints do not provide the required raw detection layout')
        self.synchronize()
        with torch.inference_mode():
            tensor = torch.zeros((1, 3, self.size, self.size), device=self.device)
            for _ in range(3):
                self.raw_output(self.network(tensor))
        self.synchronize()

    def synchronize(self):
        if self.device.type == 'cuda':
            self.torch.cuda.synchronize(self.device)

    def raw_output(self, output):
        if isinstance(output, tuple):
            output = output[0]
        if not isinstance(output, self.torch.Tensor) or output.ndim != 3:
            raise ValueError('Expected raw [1,4+classes,candidates] detection output')
        if output.shape[0] != 1 or output.shape[1] != 4 + len(self.names) or not 1 <= output.shape[2] <= 100000:
            raise ValueError('Unsupported model output: expected [1,4+classes,candidates]')
        if output.numel() > MAX_FLOATS:
            raise ValueError('Model output is too large')
        return output

    def information(self):
        free, total = self.torch.cuda.mem_get_info(self.device) if self.device.type == 'cuda' else (0, 0)
        family = 'YOLO-Omni' if self.family == 'yolo_omni' else 'YOLO'
        device = self.torch.cuda.get_device_name(self.device) if self.device.type == 'cuda' else 'CPU (explicit)'
        return dict(status='ready', version=1, names=self.names, model_family=self.family,
                    description=f'{family} PyTorch FP32 / {device} / {self.size}x{self.size}',
                    input_size=self.size, free_memory=free, total_memory=total)

    def preprocess(self, data, width, height, fmt):
        np, torch = self.np, self.torch
        image = np.frombuffer(data, dtype=np.uint8).reshape(height, width, 3 if fmt == 1 else 4)
        if fmt == 2:
            image = image[:, :, [2, 1, 0]]  # BGRA -> RGB
        if not image.flags.writeable:
            image = image.copy()
        tensor = torch.from_numpy(np.ascontiguousarray(image)).to(self.device, dtype=torch.float32)
        tensor = tensor.permute(2, 0, 1).unsqueeze(0) / 255.
        # Match Receiver's half-pixel bilinear resize and C++ round-to-nearest letterbox.
        scale = min(np.float32(self.size) / width, np.float32(self.size) / height)
        rw = max(1, int(np.floor(np.float32(width * scale) + np.float32(.5))))
        rh = max(1, int(np.floor(np.float32(height * scale) + np.float32(.5))))
        tensor = torch.nn.functional.interpolate(tensor, size=(rh, rw), mode='bilinear', align_corners=False)
        left, top = (self.size - rw) // 2, (self.size - rh) // 2
        return torch.nn.functional.pad(tensor, (left, self.size - rw - left, top,
                                                self.size - rh - top), value=114. / 255.)

    def run(self, data, width, height, fmt):
        with self.torch.inference_mode():
            tensor = self.preprocess(data, width, height, fmt)
            self.synchronize()
            start = time.perf_counter()
            output = self.raw_output(self.network(tensor))
            self.synchronize()
            inference_ms = (time.perf_counter() - start) * 1000
            array = output.detach().to('cpu', dtype=self.torch.float32).contiguous().numpy()
        return dict(status='result', classes=len(self.names), candidates=array.shape[2],
                    inference_ms=inference_ms), array.astype('<f4', copy=False).tobytes()


def serve(stream, factory=Model):
    model = factory(read_json(stream))
    send_json(stream, model.information())
    while True:
        request = read_json(stream)
        width, height, fmt, length = dimensions(request)
        reply, output = model.run(read_exact(stream, length), width, height, fmt)
        send_json(stream, reply)
        stream.sendall(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
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


if __name__ == '__main__':
    main()
