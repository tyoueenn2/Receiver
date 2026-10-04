# Model switching and remembered settings

The **Models** page accepts trained `.pt` checkpoints, raw detection `.onnx`
exports and TensorRT `.engine` files. Auto chooses the matching backend.
Native checkpoints need the selected Python environment; ONNX and engines need
the TensorRT build.

## Switch during a session

Browse for a model, choose one from the searchable library, or enter a path and
press **Load selected model**. Loading runs on the inference worker while the
picture and mouse connections stay open. The replacement is validated and warmed
before the previous backend is released. A failed load restores the previous
model and its settings, and displays the error without ending the session.
Selecting the current model again reloads it, including weights replaced at
the same path.

Changing processing size or model runtime options follows the same reload path.
Both successful and failed reloads leave mouse control off, discard pending
detections and request synthetic-button release. Turn mouse control on again
after checking the preview. Connection addresses, ports and mouse connection
type still require stopping the session.

Two backends can temporarily occupy memory during a swap. A model that cannot
fit beside the active backend fails safely; stop the session to load it with
only one backend allocated. TensorRT building itself cannot be interrupted
mid-call. Native worker loading checks cancellation and terminates its process
when stopped.

## Automatic model information

**Choose size from model** is enabled for a newly selected model:

- ONNX input dimensions determine a fixed square resolution. Dynamic exports
  use their embedded `imgsz` or companion `input_size` recommendation when
  available, otherwise 320.
- TensorRT engines supply their actual fixed input dimensions or the optimum
  dimensions of optimization profile zero. Their actual profile bounds control
  the performance helper's size choices.
- Native checkpoints supply their saved training image size when available.
  Sizes are rounded up to the model's stride; the effective size is returned to
  Receiver and used for decoding and the UI.

Fixed dimensions always take precedence over a manually selected size. Supported
images remain square, 32 through 1024 pixels in multiples of 32, batch one,
NCHW, with RGB/BGRA pictures letterboxed to the chosen input size. Models with
segmentation, pose, transposed detection outputs or embedded NMS remain
unsupported.

Object names are read from native checkpoints, engine metadata, or ONNX
`metadata_props`. ONNX names may be JSON arrays/dictionaries or the common
Python-style integer-key dictionary; metadata is parsed without evaluation.
An explicit companion metadata file still works. ONNX exports without an
external manifest must identify a detection task, class names and the supported
raw output layout; TensorRT also validates the parsed network and output shape.
External-data ONNX files are rejected.

## Saved preferences and libraries

Normal GUI use automatically saves accepted changes after a short pause to
`user-settings/preferences.json`. Closing also saves the current accepted
setup. Writes replace the file atomically. The next launch restores settings
and the current model selection, with the session stopped and movement off.
Practice demo and interface smoke tests do not overwrite personal preferences.

Each model has its own processing size, FPS limit, detection/class selection
and control settings. Switching back restores those choices while preserving
the application's connection addresses, activation buttons and preview setting.
Class selections are remembered by name so changes in class ordering can be
mapped to the new IDs. Missing or out-of-range IDs are removed.

**Save settings as** exports a normal profile. **Open saved settings** and the
profile library can apply a profile during a session when its connections match;
loading always turns movement off. Profiles from earlier versions keep an
explicitly chosen input size unless automatic sizing is enabled.

The model and profile libraries scan `models/`, `profiles/` and folders chosen
through browsing. Searches match filenames without case sensitivity. Lists
refresh once per second and remember added folders. Scans are nonrecursive and
bounded; opening a file performs validation rather than trusting its extension.

## Performance helper

Start a real model, receive a picture with preview enabled, choose **Fastest**,
**Balanced** or **Lower GPU load**, and press **Measure model performance**.
Movement stays off throughout. Supported dynamic model sizes are tested at
160, 256, 320, 416, 512 and 640 pixels; a fixed model uses only its required size
and a serialized engine uses only sizes inside its actual optimization profile.
Each candidate warms for 0.5 seconds and samples for 1.5 seconds after loading.

The table reports completed frames per second and average/95th-percentile
processing time, including preprocessing, inference and detection decoding.
Failed candidates display their reason. Fastest chooses maximum throughput;
Balanced prefers more detail within 70% of peak throughput and the timing budget;
Lower GPU load selects the smallest successful size and limits the recommendation
to at most 60 FPS. Recommended caps reserve approximately 20% headroom, with a
240 FPS maximum. **Apply recommendation** sets size and the processing FPS cap,
uses the normal reload path and remembers the choice for that model.

The FPS cap consumes the latest available frame instead of queuing old pictures.
Cancel keeps the active model and settings. This helper measures a repeated
received image and cannot establish detection accuracy, LAN latency or power
consumption. Model loading is excluded from the timed samples.
