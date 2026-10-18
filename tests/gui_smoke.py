"""Render each requested page into the app's own hidden DirectX framebuffer."""
import argparse
import json
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
    pages=['connections','models','activation','targeting','movement','auto-click',
           'preview','data-capture','profiles','app','diagnostics']
    aliases=dict(setup='connections',detect='targeting',mouse='activation',human='movement',
                 saved='profiles',settings='app',hub='diagnostics')
    parser=argparse.ArgumentParser()
    parser.add_argument('executable')
    parser.add_argument('--page',choices=pages+list(aliases),default='connections')
    parser.add_argument('--all-pages',action='store_true')
    parser.add_argument('--compact',action='store_true',help='Render at the minimum supported window size')
    parser.add_argument('--search-checks',action='store_true',help='Check settings search routes to the right category')
    args=parser.parse_args()
    cases=[(page,None,None) for page in (pages if args.all_pages else [args.page])]
    if args.search_checks:
        cases += [('connections',query,target) for query,target in
                  [('prediction','targeting'),('smoothing','movement'),('spray','auto-click'),
                   ('dataset','data-capture'),('mouse sensitivity','movement'),
                   ('maximum frame age','activation'),('class preference','targeting'),
                   ('proximity','targeting'),('does-not-exist',None)]]
    for page,query,expected in cases:
        pi=MockPi(port=free_port()).start()
        port=free_port()
        sender=Sender(port=port).start()
        try:
            result=subprocess.run([str(Path(args.executable).resolve()),'--smoke-test','--page='+page,
                '--test-frame-port='+str(port),'--test-pi-port='+str(pi.sock.getsockname()[1])]
                + (['--compact'] if args.compact else []) + (['--find='+query.replace(' ','+')] if query else []),timeout=20,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            assert result.returncode==0, f'{page}: GUI returned {result.returncode}'
            assert pi.commands and all(not c['dx'] and not c['dy'] and not c['buttons'] for c in pi.commands), \
                'Disarmed GUI may emit only fail-safe shutdown release snapshots'
            assert Path('gui-smoke.bmp').stat().st_size>10000
            state=json.loads(Path('gui-smoke-state.json').read_text())
            assert not state['armed'], 'Browsing settings must keep assistance disarmed'
            assert state['navigation_overflow'] < 1, 'Every category must fit in the sidebar: '+str(state)
            if query:
                assert state['visible'] == ([expected] if expected else []), state
                if expected:
                    assert state['page'] == expected, state
            else:
                assert state['page'] == aliases.get(page,page) and state['visible'] == pages, state
            suffix = '-compact' if args.compact else ''
            if query:
                suffix += '-find-'+query
            shutil.copyfile('gui-smoke.bmp','gui-smoke-'+page+suffix+'.bmp')
            print(page+suffix+': category, search, rendering, sync and disarmed startup passed.',flush=True)
        finally:
            sender.stop(); pi.stop()
if __name__=='__main__': main()
