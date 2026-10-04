"""Export a YOLO11 or YOLO-Omni raw detection model and verified I/O manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
from omni_support import class_names, model_family, use_source


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--weights', default='yolo11n.pt')
    p.add_argument('--size', type=int, default=320)
    p.add_argument('--fixed', action='store_true')
    p.add_argument('--omni-source', default='', help='Optional path to the YOLO-Omni repository')
    a = p.parse_args()
    if a.size < 32 or a.size > 1024 or a.size % 32:
        p.error('Size must be 32..1024 in multiples of 32')
    use_source(a.omni_source)
    os.environ.setdefault('TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD', '1')
    from ultralytics import YOLO
    import onnx
    model = YOLO(a.weights)
    if model.task != 'detect':
        raise ValueError('Only raw detection weights are supported')
    family = model_family(model.model)
    output = Path(model.export(format='onnx', imgsz=a.size, batch=1, dynamic=not a.fixed,
                               simplify=False, opset=17, half=False, nms=False, device='cpu'))
    graph = onnx.load(output)
    inp, out = graph.graph.input, graph.graph.output
    if len(inp) != 1 or len(out) != 1 or len(inp[0].type.tensor_type.shape.dim) != 4 or len(out[0].type.tensor_type.shape.dim) != 3:
        raise ValueError('Unsupported exported graph layout')
    names = class_names(model.names)
    input_type, output_type = inp[0].type.tensor_type, out[0].type.tensor_type
    if input_type.elem_type != onnx.TensorProto.FLOAT or output_type.elem_type != onnx.TensorProto.FLOAT:
        raise ValueError('Export must have float32 input and output')
    in_dims, out_dims = input_type.shape.dim, output_type.shape.dim
    if in_dims[1].dim_value != 3 or out_dims[1].dim_value not in (0, 4 + len(names)):
        raise ValueError('Export must have RGB NCHW input and raw [1,4+classes,N] output')
    manifest = dict(version=1, task='detect', layout='NCHW',
                    output='raw_yolo_omni' if family == 'yolo_omni' else 'raw_yolo11',
                    model_family=family, names=names,
                    sha256=hashlib.sha256(output.read_bytes()).hexdigest(), input_size=a.size, dynamic=not a.fixed)
    Path(str(output) + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Exported {output} and {output}.json. TensorRT engines will be built on the receiver GPU.')


if __name__ == '__main__':
    main()
