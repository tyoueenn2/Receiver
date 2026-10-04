"""Shared model inspection for native inference and optional ONNX export."""
from pathlib import Path
import sys

OMNI_MODULES = frozenset({
    'DeformableConv', 'DeformableAAttn', 'DeformableA2C2f', 'DomainAdaptiveLayer',
    'ViewEmbedding', 'DynamicScaleRouter', 'SphereAAttn', 'CircularConv',
    'ARMRouter', 'OffsetSTN',
})


def use_source(source):
    if not source:
        return
    root = Path(source).resolve()
    if not (root / 'ultralytics' / '__init__.py').is_file():
        raise ValueError('YOLO-Omni source must point to its repository root (containing ultralytics)')
    sys.path.insert(0, str(root))


def model_family(model):
    return 'yolo_omni' if any(type(m).__name__ in OMNI_MODULES for m in model.modules()) else 'yolo'


def class_names(names):
    if isinstance(names, dict):
        if set(names) != set(range(len(names))):
            raise ValueError('Model class IDs must be contiguous starting at zero')
        names = [names[i] for i in range(len(names))]
    if not isinstance(names, (list, tuple)) or not 1 <= len(names) <= 10000:
        raise ValueError('Model must provide 1..10000 class names')
    if any(not isinstance(n, str) for n in names):
        raise ValueError('Model class names must be strings')
    return list(names)
