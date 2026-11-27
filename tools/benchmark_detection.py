"""Plan and summarize a physical Windows capture-to-detection benchmark."""
import argparse
import csv
import hashlib
import itertools
import json
from pathlib import Path

SIZES = (320, 640)
LINKS_GBPS = (1, 2.5, 10)
CAPTURE_METHODS = ("dxgi", "wgc", "gdi")
PIXEL_FORMATS = {"bgra": 4, "rgb24": 3}


def power_shell_command(*args):
    return "& " + " ".join("'" + str(arg).replace("'", "''") + "'" for arg in args)


def wire_limits(size, pixel_format, link):
    payload = size * size * PIXEL_FORMATS[pixel_format]
    packets = (payload + 1351) // 1352
    # UVF1 (48), IPv4/UDP (28), untagged Ethernet/FCS (18), preamble and IFG (20).
    bits = (payload + packets * 114) * 8
    return bits / (link * 1e6), link * 1e9 / bits


def plan(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / "plan.json"
    if manifest.exists():
        raise ValueError(f"{manifest} already exists; choose a new output directory")
    models = {320: args.model_320.resolve(), 640: args.model_640.resolve()}
    hashes = {}
    for size, model in models.items():
        with model.open("rb") as stream:
            hashes[size] = hashlib.file_digest(stream, "sha256").hexdigest()

    scenarios = list(itertools.product(
        SIZES, LINKS_GBPS, CAPTURE_METHODS, PIXEL_FORMATS, ("sync", "pipeline"), args.sender_fps))
    if args.local_dxgi:
        scenarios += [(size, None, "local-dxgi", "bgra", "sync", fps)
                      for size, fps in itertools.product(SIZES, args.sender_fps)]
    rows = []
    for size, link, method, pixel_format, runtime, fps in scenarios:
        local = link is None
        network_id = "local" if local else f"{link:g}g"
        run = f"{size}-{network_id}-{method}-{pixel_format}-{runtime}-{fps:g}fps"
        profile = output / f"{run}.json"
        profile.write_text(json.dumps({
            "version": 1, "sender_ip": args.sender_ip, "frame_port": args.port,
            "pi_ip": "127.0.0.1", "input_size": size, "model": str(models[size]),
            "frame_source": "local" if local else "udp", "capture_output": args.display,
            "auto_size": False, "tensorrt_pipeline": runtime == "pipeline", "inference_fps": fps if local else 0,
            "preview": False,
        }, indent=2) + "\n", encoding="utf-8")
        metrics = output / f"{run}.csv"
        receiver = power_shell_command(
            args.receiver_exe, "--profile", profile, "--backend", "tensorrt", "--target-mode", "model",
            "--detection-warmup", args.warmup, "--seconds", args.seconds, "--log-interval-ms", 0,
            "--metrics", metrics)
        sender = "" if local else power_shell_command(
            args.sender_exe, "--host", args.receiver_ip, "--port", args.port, "--output", args.display,
            "--capture", method, "--pixel-format", pixel_format, "--fps", fps)
        floor, ceiling = (None, None) if local else wire_limits(size, pixel_format, link)
        rows.append(dict(run=run, size=size, link_gbps=link, capture_method=method, pixel_format=pixel_format,
                         runtime=runtime, fps_cap=fps, model_sha256=hashes[size], seconds=args.seconds,
                         wire_floor_ms=floor, wire_ceiling_fps=ceiling, capture_command=sender,
                         receiver_command=receiver, metrics=metrics.name))
    manifest.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    write_csv(output / "commands.csv", rows)
    print(f"Wrote {len(rows)} scenarios to {output / 'commands.csv'}")


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def summarize(args):
    output = args.output
    rows = json.loads((output / "plan.json").read_text(encoding="utf-8"))
    results = []
    for row in rows:
        result = {key: value for key, value in row.items()
                  if key not in ("capture_command", "receiver_command", "metrics")}
        path = output / row["metrics"]
        result.update(status="missing", backend="", detection_fps="", detection_frames="",
                      latency_samples="", latency_coverage_pct="", capture_to_detection_upper_p50_ms="",
                      capture_to_detection_upper_p95_ms="", capture_to_detection_upper_p99_ms="")
        if path.exists():
            try:
                with path.open(newline="", encoding="utf-8") as stream:
                    metrics = {values[0]: values[1:] for values in csv.reader(stream) if values}
                backend = metrics["# backend"][0]
                frames = int(metrics["# detection_frames"][0])
                fps = float(metrics["# detection_fps"][0])
                seconds = float(metrics["# detection_seconds"][0])
                samples, *latencies = metrics["capture_to_detection_upper"]
                samples = int(samples)
                shape = (int(metrics["# capture_width"][0]), int(metrics["# capture_height"][0]))
                result["backend"] = backend
                if (not backend.startswith("TensorRT") or shape != (row["size"], row["size"]) or
                        (" / overlap" in backend) != (row["runtime"] == "pipeline") or
                        fps <= 0 or frames < 2 or not 0 < samples <= frames or seconds < row["seconds"] - 1):
                    result["status"] = "incomplete or wrong backend/shape"
                else:
                    p50, p95, p99 = (float(value) for value in latencies)
                    result.update(status="measured", detection_fps=fps, detection_frames=frames,
                                  latency_samples=samples, latency_coverage_pct=100 * samples / frames,
                                  capture_to_detection_upper_p50_ms=p50,
                                  capture_to_detection_upper_p95_ms=p95,
                                  capture_to_detection_upper_p99_ms=p99)
            except (IndexError, KeyError, ValueError):
                result["status"] = "invalid metrics file"
        results.append(result)
    write_csv(output / "summary.csv", results)
    count = sum(row["status"] == "measured" for row in results)
    print(f"{count}/{len(results)} scenarios measured. Report written to {output / 'summary.csv'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("plan")
    generate.add_argument("--output", type=Path, required=True)
    generate.add_argument("--receiver-ip", required=True)
    generate.add_argument("--sender-ip", required=True)
    generate.add_argument("--model-320", type=Path, required=True)
    generate.add_argument("--model-640", type=Path, required=True)
    generate.add_argument("--receiver-exe", default=r".\build\cuda\Release\receiver_headless.exe")
    generate.add_argument("--sender-exe", default=r".\frame_sender.exe")
    generate.add_argument("--sender-fps", type=int, nargs="+", default=[0])
    generate.add_argument("--port", type=int, default=5000)
    generate.add_argument("--display", type=int, default=0)
    generate.add_argument("--seconds", type=int, default=30)
    generate.add_argument("--warmup", type=int, default=5)
    generate.add_argument("--local-dxgi", action="store_true")
    load = commands.add_parser("summarize")
    load.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "plan":
            if (not 2 <= args.seconds <= 86400 or not 0 <= args.warmup <= 300 or
                    not 1 <= args.port <= 65535 or args.display < 0 or
                    any(not 0 <= fps <= 1000 for fps in args.sender_fps) or
                    len(set(args.sender_fps)) != len(args.sender_fps)):
                raise ValueError("Use a valid port and display, distinct FPS caps from 0 to 1000, and a finite measurement window")
            plan(args)
        else:
            summarize(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
