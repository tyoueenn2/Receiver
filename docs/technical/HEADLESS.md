# Headless operation

Run commands from the repository root or extracted app folder unless noted.

## Headless hardware-free test

Install Python 3.12 or newer. From the receiver directory, open three terminals:

```powershell
py tools/mock_pi.py --seconds 60
```

```powershell
py tools/test_sender.py --width 320 --height 320 --fps 120 --seconds 60
```

```powershell
.\receiver_headless.exe --simulate --arm --seconds 30 --metrics simulation.csv
```

For a source build, executables are under `build/tests/Release/` instead. The mock Pi reports a held right button by default and records received commands in `mock_commands.json`. It never accesses USB or moves the local mouse. Simulation refuses any sender/Pi IP other than `127.0.0.1`.

For manual GUI startup, run `receiver.exe`, choose **Practice on this computer**, then **Start practice** while the two test peers are running. Builds without TensorRT default to practice mode and also allow a two-computer setup using native `.pt` detection.

## Headless operation and logging

The headless executable logs lifecycle changes and a status summary every second by default. It writes normal logs to standard output and errors to standard error, making it suitable for a terminal, systemd, or another service supervisor.

- `--log-level quiet|error|info|debug|trace` controls detail. `info` is the default; `debug` adds network, output, retry, and drop counters; `trace` also adds latency percentiles, clock uncertainty, control strength, and GPU memory.
- `--log-interval-ms N` changes the periodic summary interval from its 1000 ms default. Use `0` to disable periodic summaries while retaining lifecycle and action messages.
- `--quiet`, `--verbose`, and `--trace` are shortcuts for quiet, debug, and trace logging.
- `--seconds 0` runs until SIGINT or SIGTERM. A graceful stop releases synthetic input and writes the metrics file selected by `--metrics`.

For example:

```bash
./build/linux-cuda/receiver_headless --profile profiles/yolo11n.json --arm --seconds 0 \
  --log-level debug --log-interval-ms 500 --metrics receiver.csv
```
