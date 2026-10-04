"""Optional actual-model check; requires PyTorch and the fork's Python dependencies."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from omni_support import model_family, use_source
from validate_gpu import preprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-exe', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--weights', help='Optional trained checkpoint; otherwise use an untrained Game2Real model')
    args = parser.parse_args()
    use_source(args.source)
    os.environ.setdefault('YOLO_CONFIG_DIR', str(Path('build/test-results/omni-config').resolve()))
    os.environ['YOLO_AUTOINSTALL'] = 'false'
    os.environ.setdefault('TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD', '1')
    import numpy as np
    import torch
    from ultralytics import YOLO
    from yolo_omni_worker import Model
    torch.set_num_threads(4)
    torch.manual_seed(32)
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        if args.weights:
            weights = Path(args.weights).resolve()
        else:
            wrapper = YOLO(str(Path(args.source).resolve() / 'ultralytics/cfg/models/v14/yolov14-game2real.yaml'), verbose=False)
            weights = folder / 'custom weights.pt'
            # The upstream save() deep-copies non-leaf domain caches; save the full model directly.
            torch.save(dict(model=wrapper.model.eval().float(), train_args=dict(task='detect', imgsz=256)), weights)
        model = Model(dict(version=1, model=str(weights), source=args.source, input_size=320, device='cpu'))
        assert model.size == 320, 'Explicit image size was overridden'
        assert model_family(model.network) == 'yolo_omni', 'Checkpoint contains no Omni custom modules'
        rng = np.random.default_rng(32)
        if not args.weights:
            automatic = Model(dict(version=1, model=str(weights), source=args.source, input_size=320,
                                   auto_size=True, device='cpu'))
            assert automatic.size == 256, f'Checkpoint image-size metadata ignored: {automatic.size}'
            del automatic
        for width, height, fmt, size in [(127, 91, 1, 320), (320, 160, 2, 160), (1, 1, 2, 32),
                                       (31, 97, 1, 256), (1024, 1, 2, 64), (1, 1024, 1, 64),
                                       (257, 257, 2, 192), (512, 321, 1, 416)]:
            image = rng.integers(0, 256, (height, width, 3 if fmt == 1 else 4), dtype=np.uint8)
            rgb = image if fmt == 1 else np.ascontiguousarray(image[:, :, 2::-1])
            model.size = size
            actual = model.preprocess(image.tobytes(), width, height, fmt).numpy()
            expected = preprocess(rgb.tobytes(), width, height, size)
            # FP32 interpolation evaluates coordinates in a different order in PyTorch.
            # 1e-4 normalized intensity is less than 0.026 of one uint8 level.
            np.testing.assert_allclose(actual, expected, atol=1e-4, rtol=1e-5,
                                       err_msg=f'{width}x{height}, format={fmt}, input={size}')
        model.size = 320
        width, height = 320, 160
        image = rng.integers(0, 256, (height, width, 3), dtype=np.uint8)
        raw = folder / 'sample.rgb'
        raw.write_bytes(image.tobytes())
        profile = folder / 'profile.json'
        profile.write_text(json.dumps(dict(version=1, model=str(weights), input_size=320,
            inference_backend='auto', omni_python=sys.executable, omni_source=str(Path(args.source).resolve()),
            omni_device='cpu', omni_worker=str(Path(__file__).resolve().parents[1] / 'tools/yolo_omni_worker.py'))))
        output = folder / 'result.json'
        subprocess.run([str(Path(args.verify_exe).resolve()), str(profile), str(raw),
                        str(width), str(height), str(output)], check=True, timeout=120)
        result = json.loads(output.read_text())
        actual = np.array(result['raw'], dtype=np.float32).reshape(result['shape'])
        with torch.inference_mode():
            expected = model.raw_output(model.network(torch.from_numpy(preprocess(image.tobytes(), width, height, 320)))).numpy()
        assert np.isfinite(actual).all()
        np.testing.assert_allclose(actual, expected, atol=2e-4, rtol=1e-5)
        print(f'Actual YOLO-Omni native checkpoint: {actual.shape}, process output matches direct PyTorch inference.')
        print('RGB/BGRA preprocessing matches the independent bilinear reference. This is a functional check, not an accuracy or GPU benchmark.')


if __name__ == '__main__':
    main()
