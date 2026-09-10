"""Render each requested page into the app's own hidden DirectX framebuffer."""
import argparse
import socket
import shutil
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mock_pi import MockPi
from test_sender import Sender

def free_port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1',0))
        return sock.getsockname()[1]

def main():
    pages=['setup','detect','mouse','saved','human','hub']
    parser=argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--page',choices=pages,default='setup')
    parser.add_argument('--all-pages',action='store_true')
    args=parser.parse_args()
    for page in pages if args.all_pages else [args.page]:
        pi=MockPi(port=free_port()).start()
        port=free_port()
        sender=Sender(port=port).start()
        try:
            result=subprocess.run([str(Path(args.executable).resolve()),'--smoke-test','--page='+page,
                '--test-frame-port='+str(port),'--test-pi-port='+str(pi.sock.getsockname()[1])],timeout=20,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            assert result.returncode==0, f'{page}: GUI returned {result.returncode}'
            assert not pi.commands, 'GUI unexpectedly started armed'
            assert Path('gui-smoke.bmp').stat().st_size>10000
            shutil.copyfile('gui-smoke.bmp','gui-smoke-'+page+'.bmp')
            print(page+': rendering, preview, sync, telemetry and disarmed startup passed.',flush=True)
        finally:
            sender.stop(); pi.stop()
if __name__=='__main__': main()
