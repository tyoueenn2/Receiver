"""Compare TensorRT detections for an RGB capture against ONNX Runtime or a baseline build."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def preprocess(raw, width, height, size):
    import numpy as np
    image = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 3).astype(np.float32)
    scale = min(size / width, size / height)
    rw, rh = max(1, int(np.floor(width * scale + .5))), max(1, int(np.floor(height * scale + .5)))
    left, top = (size - rw) // 2, (size - rh) // 2
    x = np.clip((np.arange(rw, dtype=np.float32) + .5) * width / rw - .5, 0, width - 1)
    y = np.clip((np.arange(rh, dtype=np.float32) + .5) * height / rh - .5, 0, height - 1)
    x0, y0 = x.astype(int), y.astype(int)
    x1, y1 = np.minimum(x0 + 1, width - 1), np.minimum(y0 + 1, height - 1)
    fx, fy = (x - x0)[None, :, None], (y - y0)[:, None, None]
    resized = ((image[y0[:, None], x0] * (1 - fx) + image[y0[:, None], x1] * fx) * (1 - fy)
               + (image[y1[:, None], x0] * (1 - fx) + image[y1[:, None], x1] * fx) * fy)
    padded = np.full((size, size, 3), 114, dtype=np.float32)
    padded[top:top + rh, left:left + rw] = resized
    return np.ascontiguousarray(padded.transpose(2, 0, 1)[None] / 255)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--verify-exe', required=True)
    p.add_argument('--baseline-verify-exe', help='Require identical raw output and detections from both builds')
    p.add_argument('--profile', required=True)
    p.add_argument('--raw', required=True)
    p.add_argument('--width', type=int, required=True)
    p.add_argument('--height', type=int, required=True)
    a = p.parse_args()
    import numpy as np

    def verify(executable, output):
        subprocess.run([executable, a.profile, a.raw, str(a.width), str(a.height), str(output)], check=True)
        result = json.loads(output.read_text())
        tensor = np.array(result['raw'], dtype=np.float32).reshape(result['shape'])
        if not np.isfinite(tensor).all():
            raise AssertionError(f'Nonfinite model output from {executable}')
        return result, tensor

    with tempfile.TemporaryDirectory() as temp:
        output = Path(temp) / 'gpu.json'
        if a.baseline_verify_exe:
            baseline, previous = verify(a.baseline_verify_exe, Path(temp) / 'baseline.json')
        result, actual = verify(a.verify_exe, output)
    if a.baseline_verify_exe:
        if not np.array_equal(actual.view(np.uint32), previous.view(np.uint32)):
            raise AssertionError('Raw TensorRT outputs differ between builds')
        if result['detections'] != baseline['detections']:
            raise AssertionError('Decoded detections differ between builds')
        print(f'Raw TensorRT output and {len(result["detections"])} decoded detections match the baseline build.')
        return

    import onnxruntime as ort
    config = json.loads(Path(a.profile).read_text())
    size = config.get('input_size', 320)
    tensor = preprocess(Path(a.raw).read_bytes(), a.width, a.height, size)
    session = ort.InferenceSession(config['model'], providers=['CPUExecutionProvider'])
    expected = session.run(None, {session.get_inputs()[0].name: tensor})[0]
    if expected.shape != actual.shape:
        raise AssertionError('Output shape mismatch')
    delta = np.abs(actual - expected)
    print(json.dumps(dict(coordinate_max_abs=float(delta[:, :4].max()), score_max_abs=float(delta[:, 4:].max()),
                          mean_abs=float(delta.mean()), gpu_detections=result['detections']), indent=2))
    # FP16 has reduced precision. Check meaningful detections, not arbitrary low-score cells.
    selected = np.max(expected[:, 4:], axis=1) >= config.get('confidence', .45)
    if selected.any():
        if delta[:, :4].transpose(0, 2, 1)[selected].max() > 2 or delta[:, 4:].transpose(0, 2, 1)[selected].max() > .05:
            raise AssertionError('FP16 deviations exceed initial acceptance thresholds (2 input pixels / 0.05 confidence)')
    print('FP32 reference comparison passed; visually review detection agreement on representative images.')


if __name__ == '__main__':
    main()
