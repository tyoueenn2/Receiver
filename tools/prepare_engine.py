"""Attach raw-detection class metadata to a TensorRT engine built from an ONNX export."""
import argparse
import hashlib
import json
from pathlib import Path

from omni_support import class_names


def prepare(engine, manifest):
    engine, manifest = Path(engine), Path(manifest)
    if engine.suffix.lower() != '.engine' or not engine.is_file():
        raise ValueError('Select an existing .engine file')
    if not 0 < engine.stat().st_size <= 1024 ** 3:
        raise ValueError('Engine must be nonempty and no larger than 1 GiB')
    meta = json.loads(manifest.read_text(encoding='utf-8'))
    if meta.get('version') != 1 or meta.get('task') != 'detect' or meta.get('layout') != 'NCHW' or meta.get('output') not in ('raw_yolo11', 'raw_yolo_omni', 'raw_yolo'):
        raise ValueError('Use the receiver manifest for the raw detection ONNX used to build this engine')
    names = class_names(meta['names'])
    with engine.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    sidecar = dict(version=1, task='detect', layout='NCHW', output=meta['output'], names=names,
                   sha256=digest, format='tensorrt_engine', model_family=meta.get('model_family', 'yolo'))
    target = Path(str(engine) + '.json')
    target.write_text(json.dumps(sidecar, indent=2) + '\n', encoding='utf-8')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True)
    parser.add_argument('--manifest', required=True, help='Companion manifest for the source ONNX model')
    args = parser.parse_args()
    print(f'Prepared {prepare(args.engine, args.manifest)}. Runtime will verify the engine and raw I/O shape.')


if __name__ == '__main__':
    main()
