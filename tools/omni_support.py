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


def select_raw_detection(model):
    """Select the unfused Ultralytics one-to-many xywh head before inference/export.

    Older releases store end2end on both the network and head; newer releases
    expose a forwarding property. Never fuse here: fusion can discard a head.
    The caller must still validate the actual output tensor.
    """
    layers = getattr(model, 'model', ())
    head = layers[-1] if len(layers) else None
    if getattr(model, 'end2end', False) or getattr(head, 'end2end', False):
        if (head is None or type(head).__name__ != 'Detect' or
                getattr(head, 'cv2', None) is None or getattr(head, 'cv3', None) is None):
            raise ValueError('Raw detection requires an intact one-to-many Detect head; use the original checkpoint')
        head.end2end = False
        model.end2end = False
        if getattr(model, 'end2end', False) or getattr(head, 'end2end', False):
            raise ValueError('Model cannot select raw one-to-many detection')
    if getattr(head, 'xyxy', False):
        raise ValueError('Receiver requires raw xywh boxes, not xyxy')


def class_names(names):
    if isinstance(names, dict):
        if set(names) != set(range(len(names))):
            raise ValueError('Model class IDs must be contiguous starting at zero')
        names = [names[i] for i in range(len(names))]
    if not isinstance(names, (list, tuple)) or not 1 <= len(names) <= 10000:
        raise ValueError('Model must provide 1..10000 class names')
    if any(not isinstance(n, str) or not n for n in names):
        raise ValueError('Model class names must be strings')
    return list(names)
