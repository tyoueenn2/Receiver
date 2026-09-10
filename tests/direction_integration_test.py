"""Check direction-dependent output through the real UDP receiver and simulated physical input."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time
from integration_test import MockPi, Sender, free_port, wait_for


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    for mode in (0, 1):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            frame_port, pi_port = free_port(), free_port()
            profile = root / 'settings.json'
            profile.write_text(json.dumps(dict(frame_port=frame_port, pi_port=pi_port,
                direction=dict(enabled=True, mode=mode, strength=.5, window_ms=10, slow_speed=40))))
            pi = MockPi(pi_port).start()
            sender = Sender(port=frame_port).start()
            pi.motion_x = 200
            process = subprocess.Popen([args.executable, '--profile', str(profile), '--simulate', '--arm',
                '--seconds', '8', '--metrics', str(root / 'metrics.csv')], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            def mean_at(speed):
                pi.motion_x = speed
                time.sleep(.15)
                start = len(pi.commands)
                wait_for(lambda: len(pi.commands) >= start + 20, 'No directional corrections')
                return statistics.mean(c['dx'] for c in pi.commands[start:])
            try:
                wait_for(lambda: len(pi.commands) > 5, 'No direction-enabled output')
                toward, away, still = mean_at(200), mean_at(-200), mean_at(0)
                weak, strong = (toward, away) if mode == 0 else (away, toward)
                assert strong > weak * 2.5, (mode, toward, away)
                assert weak < still < strong, (mode, toward, away, still)
                # A button-only Pi remains usable for legacy clients, but cannot drive this feature.
                pi.motion_enabled = False
                time.sleep(.12); count = len(pi.commands); time.sleep(.15)
                assert len(pi.commands) == count, 'Missing physical motion data did not stop output'
                pi.motion_enabled = True
                wait_for(lambda: len(pi.commands) > count + 5, 'Motion capability did not recover')
                pi.telemetry_enabled = False
                time.sleep(.12); count = len(pi.commands); time.sleep(.15)
                assert len(pi.commands) == count, 'Stale motion telemetry did not stop output'
                pi.telemetry_enabled = True; pi.motion_generation += 1
                wait_for(lambda: len(pi.commands) > count + 5, 'Motion generation restart did not recover')
                pi.physical = 0
                time.sleep(.12); count = len(pi.commands); time.sleep(.15)
                assert len(pi.commands) == count, 'Button release did not stop direction assistance'
                print(f'Mode {mode}: toward={toward:.2f}, away={away:.2f}, stationary={still:.2f}; missing/stale data, restart, release passed.')
            finally:
                process.terminate()
                output = process.communicate(timeout=5)[0]
                sender.stop(); pi.stop()
                if 'Pi:' in output or 'Failed' in output:
                    raise AssertionError(output)


if __name__ == '__main__':
    main()
