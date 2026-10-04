"""Install, archive, and test exactly the archive produced by CI."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--configuration', default='Release')
    parser.add_argument('--name', required=True)
    parser.add_argument('--output', type=Path, default=Path('dist'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='receiver-ci-package-') as directory:
        stage = Path(directory) / 'stage'
        subprocess.run(['cmake', '--install', str(args.build), '--config', args.configuration,
                        '--prefix', str(stage)], check=True)
        archive = Path(shutil.make_archive(str(args.output.resolve() / args.name),
                         'zip' if os.name == 'nt' else 'gztar', root_dir=stage.parent, base_dir='stage'))
        extracted = Path(directory) / 'extracted'
        # This archive was just generated from our own install tree.
        shutil.unpack_archive(archive, extracted)
        subprocess.run([sys.executable, str(extracted / 'stage/tests/package_smoke_test.py'),
                        str(extracted / 'stage')], check=True, timeout=150)
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_name(archive.name + '.sha256').write_text(f'{checksum}  {archive.name}\n')
        print(f'Tested package: {archive.name} (SHA-256 {checksum})')


if __name__ == '__main__':
    main()
