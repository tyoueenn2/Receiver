"""Benchmark the real receiver with synthetic loopback frames and simulated mouse output."""
import argparse,json,socket,subprocess,tempfile
from pathlib import Path
from mock_pi import MockPi
from test_sender import Sender

def free_port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('executable');p.add_argument('--output',default='loopback-benchmark.json');a=p.parse_args()
    results=[]
    for fps in (120,240):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder); fp,pp=free_port(),free_port()
            profile=folder/'settings.json';metrics=folder/'metrics.csv'
            profile.write_text(json.dumps(dict(frame_port=fp,pi_port=pp,sender_ip='127.0.0.1',pi_ip='127.0.0.1')))
            pi=MockPi(pp).start();sender=Sender(port=fp,fps=fps).start()
            try:
                run=subprocess.run([str(Path(a.executable).resolve()),'--simulate','--arm','--profile',str(profile),'--seconds','5','--metrics',str(metrics)],capture_output=True,text=True,check=True)
                report=dict(requested_fps=fps,metrics=metrics.read_text(),scope='Synthetic UDP loopback; fixed detection; simulated Pi; no CUDA or real cursor output')
                results.append(report);print(json.dumps(report),flush=True)
            finally: sender.stop();pi.stop()
    Path(a.output).write_text(json.dumps(results,indent=2))
if __name__=='__main__':main()
