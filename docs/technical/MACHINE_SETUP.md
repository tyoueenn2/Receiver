# Machine setup file

The Windows GUI keeps one-time connection and Python installation settings in
`user-settings/setup.json`, relative to the app's working directory. Launch the
regular app once to create it. Existing installations copy these values from
their latest saved preferences; a new installation starts with the defaults below.
The demo and smoke tests do not read or write personal setup files.

Edit the file in a text editor, end any active session, then choose
**Connections > Reload setup file**, or restart Receiver. Connections shows the
file location and configured network endpoints. Invalid JSON, unknown keys and
invalid values display an error; a failed reload retains the last accepted setup.

```json
{
  "bind_ip": "0.0.0.0",
  "sender_ip": "127.0.0.1",
  "pi_ip": "127.0.0.1",
  "frame_port": 5000,
  "pi_port": 12345,
  "makcu_port": "",
  "omni_python": "python",
  "omni_source": "",
  "omni_device": "cuda:0",
  "omni_worker": "tools/yolo_omni_worker.py"
}
```

| Setting | Purpose |
| --- | --- |
| `bind_ip` | Local IPv4 interface for incoming video; `0.0.0.0` listens on all interfaces |
| `sender_ip` | IPv4 address of the video-sending PC |
| `pi_ip` | IPv4 address of the Raspberry Pi mouse proxy |
| `frame_port` | Video UDP port, 1–65535; match the sender |
| `pi_port` | Mouse-proxy UDP port, 1–65535; match the Pi |
| `makcu_port` | MAKCU control port, such as `COM3`; configure before selecting MAKCU output |
| `omni_python` | Python executable or interpreter path, such as `C:/Models/env/Scripts/python.exe` |
| `omni_source` | YOLO-Omni checkout root, or empty if installed in the Python environment |
| `omni_device` | `cuda:0` for the NVIDIA GPU, or `cpu` for testing |
| `omni_worker` | Worker script path; defaults to the bundled worker |

Use forward slashes in Windows JSON paths, or escape backslashes as `\\`.
Relative paths resolve from the working directory. Omitted keys use the defaults
above. JSON does not support comments.

The GUI still controls practice/live mode, video source and capture display, mouse
output, models, activation, detection, movement, preview and other session tuning.
Practice temporarily uses loopback addresses without changing the setup file.

**Connections > Find Raspberry Pi** can fill `pi_ip` and `pi_port` automatically.
Selecting a discovered proxy saves those fields to this file while preserving
other edits. The search uses the configured `pi_port`. See [LAN discovery](DISCOVERY.md).

`setup.json` takes precedence over machine values in GUI autosaves, per-model
settings and imported profiles. Preference autosaves never rewrite this file. Normal
profile exports and preference snapshots retain a full settings snapshot for
compatibility, but the GUI reapplies the setup file when loading them. Reloading
setup does not reset model selection or tuning.

Headless tools continue to use their profile and command-line options; they do
not load this GUI setup file. See [headless operation](HEADLESS.md).
