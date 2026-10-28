"""Opt-in real YOLO11/26 CPU regression: worker, ONNX, metadata and C++ NMS.

Requires local trained weights; never substitutes random weights or mocks.
"""
import argparse
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

# Opt out before any dependency can initialize ONNX Runtime, including children.
os.environ["ORT_DISABLE_TELEMETRY"] = "1"

import numpy as np
from PIL import Image
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from yolo_omni_worker import Model
from omni_support import select_raw_detection
from model_reference_test import reference
from validate_gpu import preprocess


def command(args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True, timeout=180).stdout


def decode(decoder, folder, output, size, width, height):
    profile, raw = folder / 'decode.json', folder / 'output.f32'
    profile.write_text(json.dumps(dict(version=1, input_size=size, confidence=.45, nms_iou=.45)))
    output.astype('<f4').tofile(raw)
    return np.array(json.loads(command([decoder, profile, raw, width, height, output.shape[1] - 4, output.shape[2]])))


def check_decode(decoder, folder, output, size, width, height):
    expected = reference(output, width, height, size, [])
    actual = decode(decoder, folder, output, size, width, height)
    assert len(expected) > 0, 'Real image must produce detections'
    np.testing.assert_allclose(actual, expected, atol=1e-4, rtol=1e-5)
    return len(actual)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--weights', action='append', required=True)
    p.add_argument('--image', required=True)
    p.add_argument('--decoder', required=True)
    p.add_argument('--inspector', required=True)
    p.add_argument('--verify-exe', required=True)
    p.add_argument('--native-only', action='store_true', help='Do not import or execute ONNX Runtime/export')
    a = p.parse_args()
    if not a.native_only:
        import onnxruntime as ort
        ort.disable_telemetry_events()
    torch.set_num_threads(4)
    for weights in a.weights:
        weights = Path(weights).resolve()
        assert weights.is_file(), weights
        print(f'WEIGHTS {weights.name} sha256={hashlib.sha256(weights.read_bytes()).hexdigest()}', flush=True)
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            copy = folder / weights.name
            shutil.copy2(weights, copy)
            for size, candidates in ((320, 2100), (640, 8400)):
                worker = Model(dict(version=1, model=str(copy), input_size=size, auto_size=False, device='cpu'))
                nc = len(worker.names)
                width, height = 319, 241  # odd, non-square: exercise letterbox rounding
                image = Image.open(a.image).convert('RGB').resize((width, height))
                data = image.tobytes()
                tensor = worker.preprocess(data, width, height, 1).numpy()
                assert tensor.shape == (1, 3, size, size)
                np.testing.assert_allclose(tensor, preprocess(data, width, height, size), atol=1e-4, rtol=1e-5)
                reply, payload = worker.run(data, width, height, 1)
                assert reply['classes'] == nc and reply['candidates'] == candidates
                raw = np.frombuffer(payload, '<f4').reshape(1, 4 + nc, candidates)
                assert np.isfinite(raw).all()
                for invalid in (torch.zeros(1, 300, 6), torch.zeros(1, nc + 5, candidates)):
                    try:
                        worker.raw_output(invalid)
                    except ValueError:
                        pass
                    else:
                        raise AssertionError('Worker accepted a non-raw/class-mismatched tensor')
                if getattr(worker.network.model[-1], 'one2one_cv2', None) is not None:
                    worker.network.end2end = True
                    select_raw_detection(worker.network)
                    assert not worker.network.end2end
                    with torch.inference_mode():
                        selected = worker.raw_output(worker.network(torch.from_numpy(tensor))).numpy()
                    np.testing.assert_allclose(selected, raw, atol=.003, rtol=.0002)
                    fused = deepcopy(worker.network)
                    fused.end2end = True
                    fused.fuse(verbose=False)
                    try:
                        select_raw_detection(fused)
                    except ValueError:
                        pass
                    else:
                        raise AssertionError('A one-to-one-only fused checkpoint must be rejected')
                count = check_decode(a.decoder, folder, raw, size, width, height)
                # Actual C++ -> TCP worker process, including reported input size/classes.
                frame, profile, result = folder / 'frame.rgb', folder / 'profile.json', folder / 'result.json'
                frame.write_bytes(data)
                profile.write_text(json.dumps(dict(version=1, model=str(copy), input_size=size, auto_size=False,
                    inference_backend='auto', omni_python=sys.executable, omni_device='cpu',
                    omni_worker=str(ROOT / 'tools/yolo_omni_worker.py'))))
                command([a.verify_exe, profile, frame, width, height, result])
                process = json.loads(result.read_text())
                assert process['shape'] == list(raw.shape)
                # CPU kernels can differ slightly between independent processes.
                process_raw = np.array(process['raw'], dtype=np.float32).reshape(raw.shape)
                np.testing.assert_allclose(process_raw, raw, atol=.003, rtol=.0002)
                check_decode(a.decoder, folder, process_raw, size, width, height)
                # Deterministic geometry/NMS fixture: same-class duplicate suppressed,
                # overlapping different class retained, low confidence removed.
                fixture = np.zeros((1, 6, candidates), np.float32)
                scale = np.float32(size) / width
                top = (size - int(np.floor(height * scale + .5))) // 2
                fixture[0, :4, :4] = np.array([100 * scale, 90 * scale + top, 80 * scale, 60 * scale])[:, None]
                fixture[0, 4, :2] = [.9, .8]
                fixture[0, 5, 2:] = 0  # keep the full raw candidate layout
                fixture[0, 5, 2] = .85
                fixture[0, 4, 3] = .1
                decoded = decode(a.decoder, folder, fixture, size, width, height)
                np.testing.assert_allclose(decoded, [[60, 60, 80, 60, .9, 0], [60, 60, 80, 60, .85, 1]], atol=1e-4)
                if a.native_only:
                    print(f'PASS {weights.name} size={size} raw={raw.shape} classes={nc} detections={count}; native worker, coordinates, NMS; ONNX NOT RUN', flush=True)
                    continue
                for fixed in (False, True):
                    # Disable ORT telemetry before the exporter's own verification session too.
                    export = ('import onnxruntime as ort; ort.disable_telemetry_events(); '
                              'import runpy, sys; sys.path.insert(0, sys.argv[1]); '
                              'script=sys.argv.pop(2); sys.argv.pop(1); '
                              'runpy.run_path(script, run_name="__main__")')
                    command([sys.executable, '-c', export, ROOT / 'tools', ROOT / 'tools/export_model.py',
                             '--weights', copy, '--size', size] + (['--fixed'] if fixed else []))
                    onnx = copy.with_suffix('.onnx')
                    manifest = json.loads(Path(str(onnx) + '.json').read_text())
                    assert manifest['output'] == 'raw_yolo11' and manifest['names'] == worker.names
                    assert manifest['input_size'] == size and manifest['dynamic'] == (not fixed)
                    assert manifest['sha256'] == hashlib.sha256(onnx.read_bytes()).hexdigest()
                    info = json.loads(command([a.inspector, onnx]))
                    assert info['names'] == worker.names and info['fixed_size'] == (size if fixed else 0)
                    options = ort.SessionOptions(); options.intra_op_num_threads = 4
                    session = ort.InferenceSession(str(onnx), sess_options=options, providers=['CPUExecutionProvider'])
                    actual = session.run(None, {session.get_inputs()[0].name: tensor})[0]
                    assert actual.shape == raw.shape and np.isfinite(actual).all()
                    np.testing.assert_allclose(actual, raw, atol=.003, rtol=.0002)
                    check_decode(a.decoder, folder, actual, size, width, height)
                    if not fixed:
                        other = 640 if size == 320 else 320
                        other_tensor = preprocess(data, width, height, other)
                        other_raw = session.run(None, {session.get_inputs()[0].name: other_tensor})[0]
                        assert other_raw.shape == (1, 4 + nc, 8400 if other == 640 else 2100)
                        check_decode(a.decoder, folder, other_raw, other, width, height)
                print(f'PASS {weights.name} size={size} raw={raw.shape} classes={nc} detections={count}; native worker, fixed/dynamic ONNX, metadata, coordinates, NMS', flush=True)


if __name__ == '__main__':
    main()
