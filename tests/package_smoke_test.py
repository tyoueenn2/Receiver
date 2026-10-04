"""Check an installed/extracted package from a different working directory, without hardware."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=Path)
    args = parser.parse_args()
    root = args.package.resolve()
    suffix = '.exe' if os.name == 'nt' else ''
    for filename in ('receiver_headless' + suffix, 'receiver_verify' + suffix,
                     'receiver_scenario_runner' + suffix, 'tools/yolo_omni_worker.py',
                     'tools/omni_support.py', 'tools/mock_pi.py', 'tools/test_sender.py',
                     'docs/MODEL_WORKFLOWS.md', 'docs/licenses/json.txt',
                     'docs/licenses/ultralytics.txt', 'profiles/default.json',
                     'models/yolo11n.onnx', 'models/yolo11n.onnx.json',
                     'tests/fault_worker.py', 'src/preprocess_kernel.cuh',
                     'src/engine_precision_kernel.cuh'):
        assert (root / filename).is_file(), f'Package missing {filename}'
    if os.name == 'nt':
        assert (root / 'receiver.exe').is_file(), 'Windows GUI missing'
        runtimes = ('libc++.dll', 'libunwind.dll', 'libwinpthread-1.dll') if (root / 'libc++.dll').exists() \
            else ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
        for filename in runtimes:
            assert (root / filename).is_file(), f'Windows runtime missing {filename}'
    with tempfile.TemporaryDirectory(prefix='receiver-package-smoke-') as directory:
        working = Path(directory)
        profile = working / 'profile.json'
        profile.write_text(json.dumps(dict(bind_ip='127.0.0.1', sender_ip='127.0.0.1',
                                          pi_ip='127.0.0.1', mouse_backend=0)))
        result = subprocess.run([str(root / ('receiver_headless' + suffix)),
                                 '--profile', str(profile), '--simulate', '--seconds', '1',
                                 '--metrics', str(working / 'metrics.csv')], cwd=working,
                                check=True, capture_output=True, text=True, timeout=20)
        assert 'armed=0' in result.stdout, result.stdout
        assert (working / 'metrics.csv').stat().st_size > 0
        # All worker paths now originate in the extracted package, not the checkout.
        subprocess.run([sys.executable, str(root / 'tests/model_fault_integration_test.py'),
                        str(root / ('receiver_scenario_runner' + suffix))],
                       cwd=working, check=True, timeout=120)
    print('Extracted package starts disarmed and passes real worker reload/fault checks.')


if __name__ == '__main__':
    main()
