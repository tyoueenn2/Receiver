"""Deterministic loopback Pi/proxy simulator. Never accesses USB or the OS mouse."""
import argparse
import json
import secrets
import socket
import struct
import threading
import time

from wire import CLICK_ACK, CLICK_REQUEST, MOVE, SUBSCRIBE, TELEMETRY, TELEMETRY3, newer


class MockPi:
    def __init__(self, port=12345, physical=2):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', port))
        self.sock.settimeout(.002)
        self.physical, self.ready = physical, True
        self.telemetry_enabled = True
        self.motion_enabled = True
        self.upt3_enabled = True
        self.endpoint_poll_us = 1000
        self.motion_x = self.motion_y = 0.0
        self.motion_generation = 1
        self.commands, self.transitions = [], []
        self.subscription_sources = []
        self.subscription_versions = []
        self.click_requests, self.release_requests, self.clicks = [], [], {}
        self.click_response_override = None
        self.release_cancel_count = 0
        self.interfere_next_click = False
        self.stop_event = threading.Event()
        self.session = secrets.randbits(64) or 1
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=2)
        self.sock.close()

    @staticmethod
    def ack(status, button, client, command, accepted, completed, server, depth=0):
        return CLICK_ACK.pack(b'UPA1', 2, status, button, 0, client, command, accepted,
                              completed, server, depth, 0, 0)

    def run(self):
        subscriber = None
        client = token = 0
        renewed = published = 0
        seq = 0
        previous = None
        owner = None
        lease = 0
        move_seq = None
        telemetry_version = 1
        total_x = total_y = 0.0
        persistent = scheduled = 0
        accepted_total = completed_total = 0
        motion_at = time.perf_counter()
        last_motion = 0
        while not self.stop_event.is_set():
            now = time.perf_counter()
            if owner and now - lease > .25:
                owner = None
                move_seq = None
            try:
                data, source = self.sock.recvfrom(2048)
                if len(data) == SUBSCRIBE.size and data[:4] in (b'UPS1', b'UPS2', b'UPS3'):
                    _, reserved, candidate, candidate_token = SUBSCRIBE.unpack(data)
                    self.subscription_versions.append(data[:4])
                    supported = data[:4] != b'UPS3' or self.upt3_enabled
                    if not supported:
                        self.sock.sendto(b'error unknown command', source)
                    elif (not reserved and candidate and candidate_token and
                          (subscriber is None or source == subscriber or now - renewed > .1)):
                        self.subscription_sources.append(source)
                        if candidate != client or now - renewed > .1 or candidate_token > token:
                            if candidate != client:
                                seq = 0
                            client, token = candidate, candidate_token
                            subscriber, renewed, published = source, now, 0
                            telemetry_version = int(chr(data[3]))
                            # Test the Receiver's button-only compatibility path even after
                            # it selected a cumulative telemetry version. Renewals restore
                            # the requested version as soon as motion data is available.
                            if telemetry_version >= 2 and not self.motion_enabled:
                                telemetry_version = 1
                elif len(data) == MOVE.size and data[:4] == b'UPX1':
                    _, move_sequence, dx, dy, wheel, pan, buttons, reserved = MOVE.unpack(data)
                    if owner is not None and owner != source:
                        self.sock.sendto(b'busy', source)
                    elif not self.ready:
                        self.sock.sendto(b'error not50', source)
                    elif not reserved and (move_seq is None or newer(move_sequence, move_seq)):
                        owner, lease, move_seq = source, now, move_sequence
                        if persistent != buttons:
                            self.transitions.append(dict(time=now, persistent=buttons,
                                                         scheduled=scheduled, source=source))
                        persistent = buttons
                        self.commands.append(dict(time=now, sequence=move_sequence, dx=dx, dy=dy,
                                                  wheel=wheel, pan=pan, buttons=buttons,
                                                  physical=self.physical, scheduled=scheduled,
                                                  combined=buttons | self.physical | scheduled,
                                                  source=source))
                elif len(data) == CLICK_REQUEST.size and data[:4] == b'UPC1':
                    (_, version, operation, button, reserved, click_client, command_id, count,
                     press_us, interval_us, tail) = CLICK_REQUEST.unpack(data)
                    status = self.click_response_override
                    key = (click_client, command_id)
                    valid_header = version == 2 and operation in (1, 2) and not reserved and not tail
                    if owner is not None and owner != source:
                        status = 4
                    elif not valid_header or not click_client or not command_id:
                        status = 7
                    elif key in self.clicks:
                        status = (self.clicks[key].get('terminal_status', 3)
                                  if self.clicks[key]['completed'] else 2)
                    elif operation == 2:
                        if button or count or press_us or interval_us:
                            status = 7
                        else:
                            for click in self.clicks.values():
                                if not click['completed'] and not click['release']:
                                    click['cancelled'] = True
                            scheduled = 0
                            self.release_requests.append(dict(time=now, client=click_client,
                                                              command=command_id, source=source))
                            terminal_status = 8 if self.release_cancel_count else 3
                            if self.release_cancel_count:
                                self.release_cancel_count -= 1
                            self.clicks[key] = dict(source=source, button=0, count=0,
                                                    complete_at=now, completed=True,
                                                    cancelled=False, release=True,
                                                    terminal_status=terminal_status,
                                                    completed_clicks=0)
                            owner, lease = source, now
                            status = terminal_status
                    elif not (1 <= button <= 8 and 1 <= count <= 10000 and
                              self.endpoint_poll_us <= press_us <= 5_000_000 and
                              interval_us <= 60_000_000):
                        status = 7
                    elif persistent & (1 << (button - 1)):
                        status = 9
                    elif status is None:
                        duration = (count * press_us + (count - 1) * interval_us) / 1_000_000
                        self.clicks[key] = dict(source=source, button=button, count=count,
                                                complete_at=now + duration, completed=False,
                                                cancelled=False, release=False,
                                                interfere=self.interfere_next_click,
                                                interfere_at=now + duration / 2,
                                                completed_clicks=0)
                        self.interfere_next_click = False
                        self.click_requests.append(dict(time=now, client=click_client,
                                                        command=command_id, button=button,
                                                        count=count, press_us=press_us,
                                                        interval_us=interval_us, source=source))
                        accepted_total += count
                        scheduled |= 1 << (button - 1)
                        owner, lease = source, now
                        status = 1
                    click = self.clicks.get(key)
                    accepted = count if click and not click['release'] else 0
                    completed = click.get('completed_clicks', 0) if click and not click['release'] else 0
                    depth = len([x for x in self.clicks.values() if not x['completed']])
                    self.sock.sendto(self.ack(status, button, click_client, command_id, accepted,
                                              completed, self.session, depth), source)
                elif len(data) == 32 and data[:4] == b'UPC1':
                    # The removed private draft is deliberately ignored.
                    pass
                elif data == b'+state':
                    reply = (f'state {self.physical} 0 {self.physical} -127 127 -127 127'.encode()
                             if self.ready else b'not_ready')
                    self.sock.sendto(reply, source)
            except (socket.timeout, ConnectionResetError):
                pass

            now = time.perf_counter()
            for key, click in list(self.clicks.items()):
                if click['completed'] or click['release']:
                    continue
                if click['cancelled']:
                    click['completed'] = True
                    click['terminal_status'] = 8
                    self.sock.sendto(self.ack(8, click['button'], key[0], key[1], click['count'],
                                              click['completed_clicks'], self.session), click['source'])
                elif click.get('interfere') and now >= click['interfere_at']:
                    click['completed'] = True
                    click['terminal_status'] = 9
                    click['completed_clicks'] = max(0, click['count'] - 1)
                    completed_total += click['completed_clicks']
                    scheduled &= ~(1 << (click['button'] - 1))
                    self.sock.sendto(self.ack(9, click['button'], key[0], key[1], click['count'],
                                              click['completed_clicks'], self.session), click['source'])
                elif now >= click['complete_at']:
                    click['completed'] = True
                    click['terminal_status'] = 3
                    click['completed_clicks'] = click['count']
                    completed_total += click['count']
                    scheduled &= ~(1 << (click['button'] - 1))
                    self.sock.sendto(self.ack(3, click['button'], key[0], key[1], click['count'],
                                              click['count'], self.session), click['source'])

            total_x += self.motion_x * (now - motion_at)
            total_y += self.motion_y * (now - motion_at)
            if self.motion_x or self.motion_y:
                last_motion = now
            motion_at = now
            state = (self.ready, self.physical, persistent, scheduled)
            if (subscriber and self.telemetry_enabled and now - renewed <= .1 and
                    (now - published >= .01 or state != previous)):
                age = (min(0xffffffff, int((now - last_motion) * 1e6))
                       if last_motion else 0xffffffff)
                if telemetry_version == 3:
                    active = len([x for x in self.clicks.values()
                                  if not x['completed'] and not x['release']])
                    packet = TELEMETRY3.pack(
                        b'UPT3', int(self.ready), self.physical, persistent, scheduled,
                        client, self.session, token, seq, -127, 127, -127, 127,
                        self.endpoint_poll_us, self.motion_generation, time.perf_counter_ns(),
                        int(total_x), int(total_y), age, accepted_total, completed_total,
                        active, 0, 0, 0, 0, 0, 0, 0, 0)
                else:
                    packet = TELEMETRY.pack(
                        b'UPT2' if telemetry_version == 2 else b'UPT1', int(self.ready),
                        self.physical, 0, client, self.session, token, seq,
                        -127, 127, -127, 127, 0)
                    if telemetry_version == 2:
                        packet += struct.pack('!QQIIII', self.motion_generation,
                                              time.perf_counter_ns(), int(total_x) & 0xffffffff,
                                              int(total_y) & 0xffffffff, age, 0)
                self.sock.sendto(packet, subscriber)
                seq = (seq + 1) & 0xffffffff
                published, previous = now, state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=12345)
    parser.add_argument('--buttons', type=int, default=2)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--log', default='mock_commands.json')
    parser.add_argument('--release-after', type=float, default=-1)
    parser.add_argument('--motion-x', type=float, default=0)
    parser.add_argument('--motion-y', type=float, default=0)
    args = parser.parse_args()
    if not 0 <= args.buttons <= 255:
        parser.error('Button mask must be 0..255')
    pi = MockPi(args.port, args.buttons).start()
    started = time.perf_counter()
    pi.motion_x, pi.motion_y = args.motion_x, args.motion_y
    try:
        while time.perf_counter() - started < args.seconds:
            if args.release_after >= 0 and time.perf_counter() - started >= args.release_after:
                pi.physical = 0
            time.sleep(.01)
    except KeyboardInterrupt:
        pass
    finally:
        pi.stop()
    with open(args.log, 'w') as output:
        json.dump(pi.commands, output, indent=2)
    print(f'Recorded {len(pi.commands)} commands; no real mouse output occurred.')


if __name__ == '__main__':
    main()
