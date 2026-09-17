"""Exercise UPT3 startup and recovery through the real C++ Receiver process."""
import argparse
import json
from pathlib import Path
import secrets
import subprocess
import tempfile
import time

from integration_test import MockPi, Sender, free_port, wait_for, wait_until_quiet


def profile_at(root, frame_port, pi_port, direction):
    path = root / 'settings.json'
    path.write_text(json.dumps(dict(frame_port=frame_port, pi_port=pi_port,
                                    direction=dict(enabled=direction, strength=.5,
                                                   window_ms=10, slow_speed=40))))
    return path


def launch(executable, profile, metrics, seconds=3, click=False):
    command = [executable, '--profile', str(profile), '--simulate', '--arm',
               '--seconds', str(seconds), '--metrics', str(metrics)]
    if click:
        command += ['--click', '3', '2', '10', '15']
    return subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)


def finish(process):
    output = process.communicate(timeout=7)[0]
    assert process.returncode == 0, output
    return output


def stop(process, sender, pi=None):
    if process.poll() is None:
        process.terminate()
        process.communicate(timeout=5)
    sender.stop()
    if pi is not None:
        pi.stop()


def late_pi(executable):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        frame_port, pi_port = free_port(), free_port()
        profile = profile_at(root, frame_port, pi_port, True)
        sender = Sender(port=frame_port).start()
        process = launch(executable, profile, root / 'metrics.csv', seconds=3, click=True)
        pi = None
        try:
            time.sleep(.35)
            assert process.poll() is None, 'Receiver exited before the Pi started'
            pi = MockPi(pi_port)
            pi.motion_x = 200
            pi.start()
            wait_for(lambda: len(pi.commands) > 5,
                     'Direction assistance did not recover after late Pi startup')
            wait_for(lambda: len(pi.click_requests) == 1,
                     'Click capability did not recover after late Pi startup')
            finish(process)
            versions = pi.subscription_versions
            assert b'UPS3' in versions and b'UPS2' not in versions
            report = (root / 'metrics.csv').read_text()
            assert '# click_commands_accepted_by_pi,1' in report
            assert '# click_commands_completed_by_usb_writer,1' in report
            print('Late Pi startup recovered UPT3 direction and click capability.')
        finally:
            stop(process, sender, pi)


def lost_initial_upt3(executable):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        frame_port, pi_port = free_port(), free_port()
        profile = profile_at(root, frame_port, pi_port, True)
        pi = MockPi(pi_port)
        pi.motion_x = 200
        pi.upt3_packets_to_drop = 100
        pi.start()
        sender = Sender(port=frame_port).start()
        process = launch(executable, profile, root / 'metrics.csv', seconds=3, click=True)
        try:
            wait_for(lambda: b'UPS1' in pi.subscription_versions,
                     'Receiver did not fall back after lost initial UPT3 responses')
            assert not pi.commands and not pi.click_requests
            pi.upt3_packets_to_drop = 0
            wait_for(lambda: len(pi.commands) > 5,
                     'Direction assistance did not recover after UPT3 response loss')
            wait_for(lambda: len(pi.click_requests) == 1,
                     'Click capability did not recover after UPT3 response loss')
            finish(process)
            assert any(event['dropped'] for event in pi.telemetry_events)
            print('Lost initial UPT3 responses recovered without restarting Receiver.')
        finally:
            stop(process, sender, pi)


def late_upt3(executable):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        frame_port, pi_port = free_port(), free_port()
        profile = profile_at(root, frame_port, pi_port, True)
        pi = MockPi(pi_port)
        pi.motion_x = 200
        pi.upt3_enabled = False
        pi.start()
        sender = Sender(port=frame_port).start()
        process = launch(executable, profile, root / 'metrics.csv', seconds=3, click=True)
        try:
            wait_for(lambda: b'UPS1' in pi.subscription_versions,
                     'Receiver did not establish UPT1 fallback')
            time.sleep(.25)
            assert not pi.commands and not pi.click_requests
            pi.upt3_enabled = True
            wait_for(lambda: len(pi.commands) > 5,
                     'Direction assistance did not upgrade when UPT3 became available')
            wait_for(lambda: len(pi.click_requests) == 1,
                     'Click capability did not upgrade when UPT3 became available')
            finish(process)
            print('UPT3 becoming available after UPT1 fallback upgraded in place.')
        finally:
            stop(process, sender, pi)


def permanent_upt1(executable):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        frame_port, pi_port = free_port(), free_port()
        profile = profile_at(root, frame_port, pi_port, False)
        pi = MockPi(pi_port)
        pi.upt3_enabled = False
        pi.start()
        sender = Sender(port=frame_port).start()
        process = launch(executable, profile, root / 'metrics.csv', seconds=3)
        try:
            wait_for(lambda: len(pi.commands) > 10, 'UPT1-only peer did not provide movement')
            start = len(pi.commands)
            time.sleep(1.25)
            assert len(pi.commands) > start + 10, 'UPT3 reprobes disrupted useful UPT1 service'
            finish(process)
            v3_attempts = sum(event['version'] == b'UPS3' for event in pi.subscription_events)
            assert 2 <= v3_attempts <= 12, v3_attempts
            assert b'UPS2' not in pi.subscription_versions
            first_v1 = min(event['time'] for event in pi.subscription_events
                           if event['version'] == b'UPS1')
            background = [event['time'] for event in pi.subscription_events
                          if event['version'] == b'UPS3' and event['time'] >= first_v1]
            retry_gaps = [right - left for left, right in zip(background, background[1:])]
            assert 2 <= len(background) <= 5, background
            assert all(gap >= .20 for gap in retry_gaps), retry_gaps
            assert all(right + .05 >= left for left, right in
                       zip(retry_gaps, retry_gaps[1:])), retry_gaps
            sources = {event['source'] for event in pi.subscription_events}
            assert len(sources) == 1, sources
            gaps = [right['time'] - left['time'] for left, right in
                    zip(pi.commands[10:], pi.commands[11:])]
            assert gaps and max(gaps) < .25, max(gaps) if gaps else None
            print(f'Permanent UPT1 service stayed active with {v3_attempts} bounded UPT3 probes.')
        finally:
            stop(process, sender, pi)


def delayed_restart_and_loss(executable):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        frame_port, pi_port = free_port(), free_port()
        profile = profile_at(root, frame_port, pi_port, True)
        pi = MockPi(pi_port)
        pi.motion_x = 200
        pi.upt3_enabled = False
        pi.start()
        sender = Sender(port=frame_port).start()
        process = launch(executable, profile, root / 'metrics.csv', seconds=5, click=True)
        try:
            wait_for(lambda: b'UPS1' in pi.subscription_versions, 'UPT1 fallback was not established')
            pi.upt3_delay_s = .12
            pi.upt3_enabled = True
            wait_for(lambda: bool(pi.delayed_telemetry), 'No delayed UPT3 response was captured')
            assert not pi.commands and not pi.click_requests

            # Restart the peer after it formed the delayed old-epoch response. New
            # telemetry must establish the new epoch; the old response must not upgrade it.
            pi.session = secrets.randbits(64) or 1
            pi.motion_generation += 1
            pi.upt3_delay_s = 0
            wait_for(lambda: any(event['delayed'] for event in pi.telemetry_events),
                     'Delayed old-epoch UPT3 response was not delivered')
            wait_for(lambda: len(pi.commands) > 5,
                     'Direction did not recover after the server epoch changed')
            wait_for(lambda: len(pi.click_requests) == 1,
                     'Click capability did not recover after the server epoch changed')

            pi.telemetry_enabled = False
            stopped_at = wait_until_quiet(
                pi.commands, 'Direction output continued after telemetry peer loss')
            time.sleep(.12)
            pi.telemetry_enabled = True
            wait_for(lambda: len(pi.commands) > stopped_at + 5,
                     'Direction did not recover after telemetry peer recovery')
            finish(process)
            report = (root / 'metrics.csv').read_text()
            assert '# click_server_epoch_resets,1' in report.splitlines()
            print('Delayed reply, server-epoch restart, peer loss and recovery passed.')
        finally:
            stop(process, sender, pi)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable')
    args = parser.parse_args()
    late_pi(args.executable)
    lost_initial_upt3(args.executable)
    late_upt3(args.executable)
    permanent_upt1(args.executable)
    delayed_restart_and_loss(args.executable)


if __name__ == '__main__':
    main()
