"""Generate models with the official ONNX schema and check the native inspector."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix='receiver-onnx-oracle-') as directory:
        root = Path(directory)
        def run(shape, output=(1, 6, 1), names=None, preferred=None, valid=True, external=False):
            nonlocal count
            count += 1
            constant = numpy_helper.from_array(np.zeros((1, 6, 1), np.float32), name='fixture')
            graph = helper.make_graph([helper.make_node('Identity', ['fixture'], ['out'])], 'metadata',
                [helper.make_tensor_value_info('images', TensorProto.FLOAT, shape)],
                [helper.make_tensor_value_info('out', TensorProto.FLOAT, output)], [constant])
            model = helper.make_model(graph, opset_imports=[helper.make_opsetid('', 17)])
            model.ir_version = 10
            props = dict(task='detect', names=names or json.dumps(['object', 'other']))
            if preferred:
                props['imgsz'] = json.dumps([preferred, preferred])
            helper.set_model_props(model, props)
            path = root / f'fixture-{count}.onnx'
            if external:
                onnx.save_model(model, path, save_as_external_data=True, all_tensors_to_one_file=True,
                                location=f'fixture-{count}.bin', size_threshold=0)
            else:
                onnx.save_model(model, path)
            result = subprocess.run([str(Path(args.executable).resolve()), str(path)], capture_output=True, text=True, timeout=15)
            assert (result.returncode == 0) == valid, (shape, output, result.stdout, result.stderr)
            if valid:
                info = json.loads(result.stdout)
                assert info['fixed_size'] == (shape[2] if isinstance(shape[2], int) else 0), info
                assert len(info['names']) == 2
                if preferred and isinstance(shape[2], str):
                    assert info['preferred_size'] == preferred
        for size in (32, 160, 256, 320, 416, 512, 640, 1024):
            run((1, 3, size, size))
        run(('batch', 3, 'height', 'width'), preferred=512)
        run((1, 3, 320, 320), names="{0: 'o\\'bj', 1: 'other'}")
        run((1, 3, 320, 320), names=json.dumps({'0': '対象', '1': "o' bj"}, ensure_ascii=False))
        for shape in ((2, 3, 320, 320), (1, 4, 320, 320), (1, 3, 320, 640), (1, 3, 33, 33)):
            run(shape, valid=False)
        run((1, 3, 320, 320), output=(1, 7, 1), valid=False)
        run((1, 3, 320, 320), output=(1, 1, 6), valid=False)
        run((1, 3, 320, 320), names=json.dumps({'1': 'missing zero'}), valid=False)
        run((1, 3, 320, 320), external=True, valid=False)
    print(f'{count} official-schema ONNX metadata cases passed; no GPU required.')


if __name__ == '__main__':
    main()
