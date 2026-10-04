import json
from pathlib import Path
import socket
import struct
import sys
import unittest
import tempfile
import hashlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from omni_support import class_names, model_family
from yolo_omni_worker import dimensions, read_json, send_json, Model, model_input_size
from prepare_engine import prepare


class WorkerTests(unittest.TestCase):
    def test_raw_engine_sidecar(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            engine, manifest = folder / 'renamed.engine', folder / 'source.onnx.json'
            engine.write_bytes(b'engine fixture')
            manifest.write_text(json.dumps(dict(version=1, task='detect', layout='NCHW',
                output='raw_yolo_omni', names=['person'], sha256='source hash')))
            result = json.loads(prepare(engine, manifest).read_text())
            self.assertEqual(result['names'], ['person'])
            self.assertEqual(result['sha256'], hashlib.sha256(engine.read_bytes()).hexdigest())
            self.assertEqual(result['output'], 'raw_yolo_omni')
            manifest.write_text(json.dumps(dict(version=1, task='segment', layout='NCHW', output='raw_yolo11')))
            with self.assertRaises(ValueError): prepare(engine, manifest)

    def test_model_size_and_stride(self):
        self.assertEqual(model_input_size(320, True, 640, 32), 640)
        self.assertEqual(model_input_size(320, True, [512, 512], 32), 512)
        self.assertEqual(model_input_size(320, True, [512, 640], 32), 320)
        self.assertEqual(model_input_size(160, False, 640, 64), 192)
        self.assertEqual(model_input_size(320, True, 9999, 32), 320)
        for stride in (0, 33, 2048):
            with self.assertRaises(ValueError):
                model_input_size(320, True, 640, stride)

    def test_frame_validation(self):
        self.assertEqual(dimensions(dict(width=320, height=160, format=2, bytes=204800)), (320, 160, 2, 204800))
        for update in (dict(width=0), dict(format=3), dict(bytes=1), dict(height=1025), dict(width=True)):
            with self.assertRaises(ValueError):
                dimensions(dict(dict(width=320, height=160, format=1, bytes=153600), **update))

    def test_family_uses_modules_not_filename(self):
        class Network:
            def __init__(self, module): self.module = module
            def modules(self): return [self.module]
        self.assertEqual(model_family(Network(type('DomainAdaptiveLayer', (), {})())), 'yolo_omni')
        self.assertEqual(model_family(Network(type('Conv', (), {})())), 'yolo')

    def test_class_ids(self):
        self.assertEqual(class_names({1: 'second', 0: 'first'}), ['first', 'second'])
        for names in ({1: 'wrong'}, [], ['ok', 3]):
            with self.assertRaises(ValueError): class_names(names)

    def test_protocol_and_bounds(self):
        left, right = socket.socketpair()
        with left, right:
            send_json(left, dict(names=['object'], status='ready'))
            self.assertEqual(read_json(right)['names'], ['object'])
            left.sendall(struct.pack('!I', 65537))
            with self.assertRaises(ValueError): read_json(right)

    def test_missing_weights_never_downloaded(self):
        with self.assertRaisesRegex(ValueError, 'existing trained'):
            Model(dict(version=1, model='nonexistent-yolo-omni-checkpoint.pt'))


if __name__ == '__main__':
    unittest.main()
