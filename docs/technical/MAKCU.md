# MAKCU mouse output

Receiver supports MAKCU USB serial output on Windows and Linux. Connect the
MAKCU control USB port to the Receiver computer and the mouse through MAKCU using
the device's normal wiring. Close other applications using its control port.

On Windows, end the session, open **Connections > Mouse output**, select
**MAKCU (USB serial)**, and enter the control port shown in Device Manager,
such as `COM3` or `COM12`. Start the session and wait for MAKCU to connect. Select
**Arm MAKCU assistance**, then hold a physical activation button after startup. The
left, right, middle and two side buttons are supported, including alternate
activation and dynamic search-area buttons.

For headless operation, supply the serial device explicitly:

```bash
./build/linux-tests/receiver_headless --profile profiles/yolo11n.json \
  --mouse-backend makcu --makcu-port /dev/ttyACM0 --seconds 0
```

On Windows use `receiver_headless.exe` and a port such as `COM3`. Linux users need
read/write permission for the selected serial device. Add `--arm` only when
movement is intended. Activation follows the selected held-button or continuous
mode; held-button activation is the default.

Profiles store `"mouse_backend": 2` and `"makcu_port": "COM3"` (or the Linux
device path). Existing profiles retain their previous connection. Changing the
backend or port requires stopping the session first. An attempted live change
disarms movement and is rejected; restart to use the new connection. Selecting a
different model preserves the serial-port preference. The Activation page selects
held-button or continuous
assistance while armed, with fresh pictures and a responding MAKCU required in
both modes.

## Protocol and behavior

The connection opens at 115200 baud, sends the MAKCU baud-change packet and
switches to 4,000,000 baud with 8 data bits, no parity, one stop bit, and no flow
control. Movement uses `km.move(dx,dy)` followed by CRLF, with signed movement
limited to -32767..32767. No third-party serial runtime is required.

`km.buttons(1)` enables physical button monitoring. The parser supports legacy
raw five-bit reports and newer `km.`-prefixed reports, including reports split
between reads. It separates ASCII command replies, prompts and line endings
from button state. Tagged `km.version()` probes check that the device responds;
legacy untagged `km.MAKCU` version replies are also accepted. An initial response
must arrive within 500 ms; subsequent response gaps over 250 ms stop the session.
Serial write failure or disconnection also stops movement. Reconnection requires
an explicit session restart and rearming. Write waits are bounded to 10 ms.

The protocol follows the public
[MAKCU Python client](https://github.com/SleepyTotem/makcu-py-lib) and
[MAKCU C++ client](https://github.com/K4HVH/makcu-cpp) conventions.

Only movement is injected through MAKCU. Pi-scheduled clicks and synthetic holds
require the Pi UDP backend. MAKCU's button stream does not supply cumulative
physical motion, so direction-based assistance is unavailable. Sensitivity,
smoothing, prediction and dynamic search area remain available. Existing timing
metrics measure local command submission rather than physical USB delivery.

Practice pictures can be used with MAKCU, but **Arm MAKCU assistance controls
the real connected mouse**, including when `--simulate` supplies example
detections. Default practice with **Raspberry Pi / simulator** remains hardware
free.

## Tests

Dedicated Linux and Windows GitHub Actions jobs run these tests on every push,
pull request and manual workflow run. Results are saved as `makcu-<runner>`
artifacts. The full native and Linux sanitizer jobs run the same tests as part
of their complete suites.

```bash
cmake --preset linux-tests -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build --preset linux-tests --parallel 3
ctest --preset linux-tests -R makcu --output-on-failure --no-tests=error
```

`receiver_makcu` covers command bounds, ASCII/binary parsing, all five-button
combinations, response correlation, saved settings, model preference changes and
serial initialization. On Linux it uses pseudo-terminals to verify actual raw
serial configuration, port exclusivity, startup timeouts and disconnection.

`receiver_makcu_pipeline` runs the actual headless application with a UDP frame
sender and pseudo-terminal MAKCU firmware. It checks disarmed startup, physical
activation/release, continuous-mode arming and fresh-frame gates, loss of device
responses,
unplugging, and absence of Pi synthetic packets. Both legacy and newer report
formats are exercised. This test runs on Linux; the portable protocol/profile
checks also run on Windows through CTest.

These checks require no MAKCU hardware and do not establish physical cursor
delivery or device latency. Actual firmware/USB behavior still needs a device
test.
