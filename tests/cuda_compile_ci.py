"""Compile production CUDA kernels using pip's NVIDIA compiler libraries, without a GPU."""
import argparse
from pathlib import Path
import subprocess
import sys
import sysconfig


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=Path('build/cuda-check'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    nvidia = Path(sysconfig.get_paths()['purelib']) / 'nvidia'
    libraries = list((nvidia / 'cuda_nvrtc').rglob('libnvrtc.so.*'))
    libraries += list((nvidia / 'cuda_nvrtc').rglob('nvrtc64*.dll'))
    assert libraries, 'NVRTC package missing'
    includes = [nvidia / 'cuda_runtime/include', nvidia / 'cuda_nvcc/include']
    assert all(path.is_dir() for path in includes), 'CUDA header packages missing'
    args.output.mkdir(parents=True, exist_ok=True)
    for source in ('preprocess_kernel.cuh', 'engine_precision_kernel.cuh'):
        for architecture in ('compute_75', 'compute_86'):
            command = [sys.executable, str(root / 'tools/check_cuda_kernel.py'),
                       '--nvrtc', str(libraries[0]), '--source', str(root / 'src' / source),
                       '--arch', architecture, '--output', str(args.output / f'{source}-{architecture}.ptx')]
            for path in includes:
                command += ['--include', str(path)]
            subprocess.run(command, check=True, timeout=60)


if __name__ == '__main__':
    main()
