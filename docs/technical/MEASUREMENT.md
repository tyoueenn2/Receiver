# Move / click / return measurement (schema 1)

## Scope and source audit

Inspected main at Receiver `0cf206c`, Sender `555cf13`, usb-proxy-udp `3b285e3`.
Receiver already includes the changes after PR #13 (PR #14 and #15). No protocol
change or Sender rebuild is required by this diagnostic addition.

| Existing source | Available evidence | Limitation / addition |
|---|---|---|
| Receiver `calibration_app.cpp`, `CalibrationStatus` | Per-probe displacement, independent verification error, maximum return error, failure text | Previously mainly transient/UI state. Emit JSONL per probe and action. Fit probes record raw displacement, not a claimed target error before a mapping exists. |
| Receiver `app.cpp` | Validated UPR1 stages/status/flags, pre-click and return cursor checks | Log accepted changes, local stop reasons, measured errors and freshness timestamps. Busy/QueueFull responses are retried by existing code and are not terminal failures; this is not a packet trace. |
| Sender `src/main.cpp`, `src/cursor.hpp`, `docs/PROTOCOL.md` | UVP1/UVG1 physical cursor position, sample time, display/crop generation and frame association | Reuse existing ownership/freshness checks. No additional Sender IO/logging. A cursor position is not application click acceptance. |
| Proxy `move_executor.h`, `mouse_mixer.cpp`, UPR1 | Successfully written outbound/return/residual counts, press/release flags, endpoint/timeout/interruption failures | Counts are not pixels. `written_ns` also changes on state transitions: it is not exclusively a USB write timestamp. |
| Proxy UPT3 / existing capture | Physical movement counters, physical queue residence statistics, USB descriptor/input recording | Not transaction-specific ioctl latency or proof of Windows consumption. Add opt-in timing immediately around each transaction USB write in live proxy and replay. |
| Existing calibration loopback, protocol, mixer and client tests | Deterministic response fitting, partial writes, cancellation, stale peers | Software evidence only. Cannot establish hardware precision or latency. |

## Enable recording

Use unique files per process; do not have multiple processes append to one file.
The directories must already exist. Environment variables must be set **before**
launching the program. With no variable, logging is disabled.

Windows PowerShell (Receiver build containing these changes):

```powershell
New-Item -ItemType Directory -Force measurements
$env:RECEIVER_MEASUREMENT_LOG = "$PWD\measurements\receiver.jsonl"
.\receiver.exe
```

Pi, prepend the variable to the command you already use for the correct device,
UDC and peer. When using sudo, set it after sudo so it is preserved:

```sh
sudo env USB_PROXY_MEASUREMENT_LOG=/absolute/path/pi.jsonl ./usb-proxy <your-existing-arguments>
# Same variable is supported by usb-replay.
```

Logging uses a background writer and an 8192-event bounded queue. The control/USB
threads serialize and enqueue records but never write the file. This still adds
CPU/locking overhead: compare enabled and disabled runs for timing-sensitive use.
The log grows until process exit; keep sessions short and monitor disk space.
Open/write errors are printed on stderr. Do not use a run with those errors.
Normal program exit flushes the queue and adds `log_end`; forced termination may
lose records. Missing end markers or dropped records invalidate complete-run
statistics and cause analyzer exit code 2. A process that exits abnormally from
inside a failed USB ioctl might have no `usb_write` completion: absence is unknown,
not successful output.

## Run procedure

1. Create a run manifest with UTC date, all three commit IDs, real hardware vs
   simulation, OS/build, Pi kernel/Raw Gadget, USB VID/PID/firmware/endpoint interval,
   display geometry/DPI, Windows pointer speed/acceleration, Receiver profile,
   calibration mapping/range/tolerance, timings, verification mode and network.
   Keep the original JSONL beside the manifest. Never present the loopback data as
   hardware measurements.
2. Use a stationary desktop and release physical buttons. Run automatic calibration
   or manual verification. Calibration never clicks. Retain failed runs too.
3. Use **Test movement only**, with **Verify desktop position before clicking**
   enabled, at several distances and directions inside the verified range. Start
   near display center. Repeat at least 20 times per condition. Change only one
   condition per run (distance, timing, pointer settings, etc.).
4. Start the observer below on the controlled Windows target. After movement-only
   results are acceptable, use **Test click and return** with the left button on
   its blue target. One tagged trial at a time; do not physically move the mouse.
   Record each Receiver session/command and observer tag in the manifest.
5. Explicitly test cancel, physical motion/button interruption, no feedback and
   disconnect on a harmless test screen. Document which cases were actually run.
   A local stop does not promise a return movement; missing feedback remains unknown.
6. Close Receiver normally. Stop the proxy with its normal shutdown path. Copy the
   Pi log to the analysis machine and run:

```sh
python tools/analyze_move_measurements.py measurements/receiver.jsonl measurements/pi.jsonl \
  --json measurements/report.json --csv measurements/trials.csv
```

Receiver-only analysis works: omit the Pi path. No Pi samples means USB latency is
unmeasured. Exit 0 means structurally complete logs, **not** a hardware pass; inspect
statuses, failure reasons, tolerance comparisons and unobserved counts. Exit 1
means malformed input/IO error; exit 2 means partial/incomplete log evidence.

## Application click observation

On Windows with Python/Tk:

```powershell
python tools/click_observer.py --log measurements\clicks.jsonl --tag run01-trial01
```

The window shows a physical-pixel target center. Its event handler records button,
position, target bounds and whether the **test application** accepted the press.
A USB press/release ACK alone never sets `application_click_result` to success.
Observer JSONL has a separate component/schema and is **not** passed to the
Receiver/Pi analyzer. Match it manually through the manifest, with one trial per
tag. Physical clicks can also reach this window; exclude setup clicks and keep
hands off during trials. No event in an incomplete observer log proves nothing.
This demonstrates this test window's event handling only; another application's
processing requires its own observation. Cross-machine UTC labels assist human
correlation, not latency measurement. The observer has been syntax-checked but has
not been exercised on a real Windows desktop in this work.

## Data format and interpretation

UTF-8 JSONL: one JSON object per event. Common fields: `schema:1`, `component`
(`receiver` or `pi`), `run` (unique per process logger), `event`. IDs `session`,
`command`, `generation`, `epoch`, `layout` are decimal strings to preserve 64-bit
values in spreadsheet/JavaScript workflows. Do not convert them to floating point.

Receiver trial key is `(run, session, command, generation)`. Pi writes join via
`(session, command, generation)` only when it uniquely identifies one Receiver
trial. Repeated Pi segments remain separate observations; repeated Receiver ACKs
with identical content are suppressed. Endpoint epoch is retained in Receiver
records. Use logs from the same experiment; ambiguous matches are reported.

| Event / fields | Meaning |
|---|---|
| `submit` | Requested HID counts, button (0 = movement-only), timings, calibration/action context, tolerance, layout and poll interval. Receiver monotonic start, before UDP send. |
| `probe_config`, `probe_landing` | Calibration overrides (including 20/60ms settle), raw measured displacement and context (0 desktop, 1 view registration). |
| `target` | Manual action target, requested pixel delta and origin; context determines coordinate meaning. |
| `landing` | Euclidean pixel error and tolerance; verification probes or fresh pre-click desktop measurement. Action record includes reason, cursor position and Sender sample/Receiver feedback timestamps. |
| `return` | Euclidean return pixel error, independently observed after terminal completion. View context uses registration displacement. |
| `ack` | Validated status, stage, flags, outbound/return/residual HID counts, Pi stage timestamp. `terminal` marks final result. Press/release flag bits are USB-write evidence only. |
| `local_stop`, `return_missing`, `rejected`, `calibration_failed` | Reasons: Receiver timeout, sender loss, stale telemetry, local stop, superseded/no fresh return feedback, pre-submit rejection or calibration failure. A rejected manual attempt has no command ID. |
| `calibration_complete` | Maximum verification/return errors and verified count radius. |
| Pi `usb_write` | Start/end/duration in Pi steady-clock ns, report kind (6 out, 7 press, 8 release, 9 return), result/expected byte count, errno on failure, success and attempted segment counts. Failed writes' counts are not committed movement. |
| `log_start`, `log_end` | Run boundaries; `dropped` counts queue overflow. |

The analyzer produces per-operation CSV and JSON with status/reasons, landing and
return errors/pass comparisons, residual counts, USB press/release evidence,
application result (`unobserved`), Receiver elapsed time and USB write durations.
Summaries separate calibration/action and desktop/view. p50/p95 use nearest-rank;
all metrics include sample counts. Unobserved fields are JSON null / empty CSV,
never zero. Fit-only probes do not have a landing-error sample. Unverified actions
without a midpoint wait cannot provide an intermediate landing sample; enable the
verification option for that measurement. This affects timing and must be recorded.

**Clock domains and limits:** Receiver elapsed is submission to receipt of terminal
ACK (or local stop), including network, configured waits and verification latency.
It is not pure USB latency. Pi USB duration is the userspace elapsed time around
`USB_RAW_IOCTL_EP_WRITE`, including kernel scheduling/blocking, not wire-bus
latency or Windows input latency. Do not subtract Pi/Sender timestamps from
Receiver timestamps. `sender_sample_ns` is retained only as provenance. Feedback
request/receipt use the Receiver clock. No metric measures visual invisibility.

## Regression checks and remaining hardware work

```sh
cmake --preset linux-tests -DRECEIVER_INTEGRATION_TESTS=ON
cmake --build --preset linux-tests
ctest --preset linux-tests -R 'receiver_(calibration|measurement_analysis|injection|proxy_codec)' --output-on-failure
# In usb-proxy-udp:
make test
```

The real Receiver loopback controller now records/analyzes all seven existing
calibration scenarios, including a misplaced click. In the miss scenario, residual
HID counts are zero **while observed return error is 4 px**; tests preserve this
distinction. Python regressions cover missing observations, truncated/dropped
logs, duplicate submissions, invalid numbers/JSON, local stops, Pi correlation,
failed USB writes and separate clocks. Proxy serializer tests run without USB.

**Not performed:** real Windows cursor accuracy/DPI/monitor behavior, physical
Raspberry Pi/Raw Gadget/USB write latency, return precision under real pointer
acceleration or physical interference, target application click consumption, and
Windows GUI observer execution. These remain required deployment checks.

### Checks executed for this change (2026-10-22)

- Linux Release Receiver build: passed.
- Targeted CTest: 5/5 passed; includes the seven calibration loopback scenarios and
  six analyzer regression cases.
- Linux live USB proxy and replay build: passed, using locally built JsonCpp and
  libusb headers with the installed libusb runtime (development packages were not
  available in this environment).
- Proxy `make test`: passed, including seven Python cases and the new no-device
  writer-log serialization/flush check.
- Windows observer: Python syntax check only. No hardware results were collected.

## GitHub Actions coverage

The calibration job runs on Ubuntu and Windows and explicitly runs the analyzer
regressions, real-controller loopback measurements and JSON/CSV CLI export checks.
It retains each scenario's raw JSONL, exported analysis, runner report and stdout/
stderr under the `calibration-<os>` artifact, including failed/repeated scenarios.
`RECEIVER_MEASUREMENT_TEST_DIR` enables this retention locally too; unset uses
cleaned temporary directories. The complete native and sanitizer jobs also execute
the registered CTest tests. The Windows click observer is syntax-checked only.

The proxy GCC/Clang jobs build both binaries and run `make test`, including the
measurement serializer test with the selected compiler. `USB_PROXY_MEASUREMENT_TEST_DIR`
retains its synthetic JSONL fixture, uploaded with the test output. These CI
artifacts are simulation/test-fixture evidence, never real-device measurements.
