"""Move/return loopback fixture. No USB, OS input, or hardware timing claims."""
import struct
import time

REQUEST = struct.Struct('!4sBBBBQQQQiiIIIII12x')


class MoveSimulator:
    def __init__(self, sock):
        self.sock = sock
        self.records = {}
        self.position = (0, 0)
        self.requests = []
        self.clicks = []

    def reply(self, record):
        r = record
        packet = struct.pack('!4sBBBBQQQQiiiiiiQ24x', b'UPR1', 1, r['status'],
            r['stage'], r['flags'], r['session'], r['command'], r['epoch'], r['generation'],
            r['ox'], r['oy'], r['rx'], r['ry'], r['ox']+r['rx'], r['oy']+r['ry'],
            time.perf_counter_ns())
        self.sock.sendto(packet, r['source'])

    def release(self):
        for record in self.records.values():
            if record['status'] <= 2:
                record.update(status=11, stage=9, flags=record['flags'] | 1)
                self.reply(record)

    def handle(self, data, source, epoch, generation, physical, persistent, ready):
        if len(data) == 24 and data[:4] == b'UPQ1':
            if data[4:8] != bytes((1, 0, 0, 0)):
                return True
            session, token = struct.unpack_from('!QQ', data, 8)
            if session and token:
                packet = struct.pack('!4sBBBBQQQQIiiii4xQI4x', b'UPI1', 1, int(ready),
                    31, 0, session, token, epoch, generation, 1000, -127, 127, -127, 127, 1, 32)
                self.sock.sendto(packet, source)
            return True
        if len(data) != REQUEST.size or data[:4] != b'UPM1':
            return False
        (_, version, operation, flags, button, session, command, expected_epoch,
         expected_generation, dx, dy, settle, press, delay, budget, wait) = REQUEST.unpack(data)
        if version != 1 or operation not in (1, 2, 3, 4) or flags & ~1 or button > 8 or any(data[68:]):
            return True
        key = (session, command)
        fingerprint = data[:5] + b'\x01' + data[6:]
        record = self.records.get(key)
        if record:
            if record['fingerprint'] != fingerprint:
                invalid = dict(record, status=6)
                self.reply(invalid)
                return True
            if operation == 2 and record['stage'] == 3:
                record['continue'] = True
                record['due'] = time.perf_counter()
            elif operation == 3 and record['status'] <= 2:
                record['cancel'] = True
            self.reply(record)
            return True
        conflict = physical & (1 << (button-1)) if button else physical
        status = (8 if operation != 1 else 14 if expected_epoch != epoch or expected_generation != generation
                  else 7 if not ready else 9 if conflict or persistent else
                  4 if any(r['status'] <= 2 for r in self.records.values()) else 2)
        record = dict(status=status, stage=0, flags=0, session=session, command=command,
                      epoch=epoch, generation=generation, ox=0, oy=0, rx=0, ry=0,
                      dx=dx, dy=dy, button=button, source=source, settle=settle/1e6,
                      press=press/1e6, delay=delay/1e6, wait=wait/1e6, midpoint=bool(flags & 1),
                      deadline=time.perf_counter()+budget/1e6, due=time.perf_counter(),
                      fingerprint=fingerprint, cancel=False, **{'continue': False})
        if status == 2:
            record['stage'] = 1
            self.records[key] = record
            self.requests.append(dict(session=session, command=command, dx=dx, dy=dy, button=button))
        self.reply(record)
        return True

    def tick(self):
        now = time.perf_counter()
        for r in self.records.values():
            if r['status'] > 2:
                continue
            if now >= r['deadline']:
                r.update(status=12, stage=9, flags=r['flags'] | 1)
            elif r['cancel'] and r['stage'] != 8:
                r.update(stage=8, due=now, flags=(r['flags'] | 4) if r['flags'] & 2 else r['flags'])
            elif now < r['due']:
                continue
            elif r['stage'] == 1:
                r['ox'], r['oy'] = r['dx'], r['dy']
                self.position = (self.position[0]+r['dx'], self.position[1]+r['dy'])
                r.update(stage=2, due=now+r['settle'])
            elif r['stage'] == 2:
                r.update(stage=3 if r['midpoint'] else 4 if r['button'] else 8,
                         due=now+r['wait'] if r['midpoint'] else now)
            elif r['stage'] == 3:
                if not r['continue'] and now < r['due']:
                    continue
                if not r['continue']:
                    r['cancel'] = True
                r.update(stage=4 if r['button'] and not r['cancel'] else 8, due=now)
            elif r['stage'] == 4:
                r['flags'] |= 2
                self.clicks.append((r['command'], self.position))
                r.update(stage=5, due=now+r['press'])
            elif r['stage'] == 5:
                r['flags'] |= 4
                r.update(stage=8, due=now+r['delay'])
            elif r['stage'] == 8:
                r['rx'], r['ry'] = -r['ox'], -r['oy']
                self.position = (self.position[0]+r['rx'], self.position[1]+r['ry'])
                r.update(stage=9, status=11 if r['cancel'] else 3)
            self.reply(r)
