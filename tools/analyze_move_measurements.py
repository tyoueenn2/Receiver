#!/usr/bin/env python3
"""Analyze schema-1 Receiver/Pi JSONL without mixing clock domains or units."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import sys


def read_events(paths):
    events = []
    for path in paths:
        with open(path, encoding='utf-8') as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
                    if not isinstance(item, dict) or item.get('schema') != 1:
                        raise ValueError('expected schema 1 object')
                    if item.get('component') not in ('receiver', 'pi') or not item.get('run') or not item.get('event'):
                        raise ValueError('missing/invalid component, run or event')
                    events.append(item)
                except (ValueError, TypeError) as exc:
                    raise ValueError(f'{path}:{line_number}: {exc}') from exc
    return events


def stats(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return dict(n=0, p50=None, p95=None, maximum=None)
    return dict(n=len(values), p50=values[math.ceil(.50*len(values))-1],
                p95=values[math.ceil(.95*len(values))-1], maximum=values[-1])


def wire_key(e):
    return tuple(str(e[k]) for k in ('session', 'command', 'generation'))


def analyze(events):
    issues, trials, writes = [], {}, defaultdict(list)
    if not events:
        issues.append('no measurement events')
    runs = defaultdict(list)
    for e in events:
        runs[(e['component'], e['run'])].append(e)
    for key, records in runs.items():
        ends = [e for e in records if e['event'] == 'log_end']
        if not ends or not any(e['event'] == 'log_start' for e in records):
            issues.append(f'{key}: missing lifecycle marker; log may be truncated')
        if any(e.get('dropped', 0) for e in ends):
            issues.append(f'{key}: logger dropped events')
    for e in events:
        if e['component'] == 'pi' and e['event'] == 'usb_write':
            duration = e['end_ns'] - e['start_ns']
            if duration < 0 or duration != e['duration_ns']:
                raise ValueError('invalid USB duration')
            writes[wire_key(e)].append(e)
        if e['component'] != 'receiver' or 'command' not in e:
            continue
        key = (e['run'], *wire_key(e))
        trials.setdefault(key, []).append(e)
    owners = Counter(key[1:] for key, es in trials.items() if any(e['event'] == 'submit' for e in es))
    rows = []
    for key, es in trials.items():
        submits = [e for e in es if e['event'] == 'submit']
        if not submits:
            continue
        if len(submits) != 1:
            raise ValueError(f'duplicate submit for {key}')
        start = dict(submits[0])
        for e in es:
            if e['event'] == 'probe_config':
                start['context'] = e['context']
        acks = [e for e in es if e['event'] == 'ack' and e['terminal']]
        terminal = acks[-1] if acks else None
        stops = [e for e in es if e['event'] == 'local_stop']
        landing = [e for e in es if e['event'] == 'landing']
        returned = [e for e in es if e['event'] == 'return']
        reasons = [e['reason'] for e in es if e['event'] in ('local_stop', 'return_missing', 'landing')
                   and 'reason' in e and e['reason'] != 'within_tolerance']
        status = terminal['status'] if terminal else 'local_stop' if stops else 'incomplete'
        if terminal and status != 'completed':
            reasons.append(status)
        if status == 'incomplete':
            issues.append(f'{key}: no terminal ACK or local stop')
        end = terminal or (stops[-1] if stops else None)
        elapsed = (end['receiver_ns'] - start['receiver_ns']) / 1e6 if end else None
        if elapsed is not None and elapsed < 0:
            raise ValueError('negative Receiver duration')
        def error_record(records):
            if not records:
                return None, None
            record = records[-1]
            value = record['error_px']
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError('invalid pixel error')
            return value, value <= record['tolerance_px']
        le, lp = error_record(landing)
        re, rp = error_record(returned)
        usb = writes.get(key[1:], []) if owners[key[1:]] == 1 else []
        if owners[key[1:]] > 1:
            issues.append(f'{key}: ambiguous Pi correlation; USB metrics omitted')
        flags = terminal['flags'] if terminal else 0
        if lp is False:
            reasons.append('landing_outside_tolerance')
        if rp is False:
            reasons.append('return_outside_tolerance')
        rows.append(dict(run=key[0], session=key[1], command=key[2], generation=key[3],
            calibration=start['calibration'], context=start['context'], status=status,
            landing_error_px=le, landing_pass=lp, return_error_px=re, return_pass=rp,
            receiver_elapsed_ms=elapsed, residual_counts=terminal.get('residual_counts') if terminal else None,
            usb_return_uncertain=bool(flags & 1) if terminal else True,
            usb_press_written=bool(flags & 2) if terminal else None,
            usb_release_written=bool(flags & 4) if terminal else None,
            application_click_result='unobserved', usb_write_count=len(usb),
            usb_write_failures=sum(not e['success'] for e in usb),
            usb_write_ms=[e['duration_ns']/1e6 for e in usb], reasons=sorted(set(reasons))))
    groups = {}
    for row in rows:
        group = ('calibration' if row['calibration'] else 'action') + ':' + str(row['context'])
        groups.setdefault(group, []).append(row)
    summaries = {}
    for name, rs in groups.items():
        summaries[name] = dict(trials=len(rs), statuses=dict(Counter(r['status'] for r in rs)),
            reasons=dict(Counter(x for r in rs for x in r['reasons'])),
            landing_error_px=stats([r['landing_error_px'] for r in rs]),
            return_error_px=stats([r['return_error_px'] for r in rs]),
            receiver_elapsed_ms=stats([r['receiver_elapsed_ms'] for r in rs]),
            usb_write_ms=stats([v for r in rs for v in r['usb_write_ms']]),
            landing_unobserved=sum(r['landing_error_px'] is None for r in rs),
            return_unobserved=sum(r['return_error_px'] is None for r in rs))
    unmatched = sum(len(es) for key, es in writes.items() if owners[key] != 1)
    if unmatched:
        issues.append(f'{unmatched} USB writes have no unique Receiver trial')
    rejected = [e for e in events if e['event'] == 'rejected']
    return dict(schema=1, integrity_issues=sorted(set(issues)), summaries=summaries, trials=rows,
                rejected_attempts=rejected,
                calibration_failures=[e for e in events if e['event'] == 'calibration_failed'],
                hardware_validation='not inferred: identify real hardware runs in your run manifest')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--json', type=Path, help='write full report (otherwise stdout)')
    parser.add_argument('--csv', type=Path, help='write one row per submitted operation')
    args = parser.parse_args()
    try:
        report = analyze(read_events(args.logs))
        text = json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + '\n'
        if args.json:
            args.json.write_text(text, encoding='utf-8')
        else:
            print(text, end='')
        if args.csv:
            with args.csv.open('w', newline='', encoding='utf-8') as f:
                rows = report['trials']
                if rows:
                    writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                    writer.writeheader()
                    for row in rows:
                        writer.writerow({k: json.dumps(v) if isinstance(v, (list, dict)) else v for k, v in row.items()})
        return 2 if report['integrity_issues'] else 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f'Invalid measurement input: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
