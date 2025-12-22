#!/usr/bin/env python3
import sys
import re
import csv
from pathlib import Path

TIMESTAMP_RE = re.compile(r"\d{8}_\d{6}")
TRACE_RE = re.compile(r"Core 0:\s+(.+)")
WALL_RE = re.compile(r"WALL_CLOCK_TIME:\s+(\d+:\d{2}:\d{2})")


def hms_to_seconds(hms: str) -> int:
    h, m, s = map(int, hms.split(":"))
    return h * 3600 + m * 60 + s


def extract_from_log(log_path: Path):
    trace_name = None
    wall_time = None

    with log_path.open("r", errors="ignore") as f:
        for line in f:
            if trace_name is None:
                m = TRACE_RE.search(line)
                if m:
                    trace_path = Path(m.group(1))
                    trace_name = trace_path.name

            if wall_time is None:
                m = WALL_RE.search(line)
                if m:
                    wall_time = m.group(1)

            if trace_name and wall_time:
                break

    if not trace_name or not wall_time:
        return None

    return {
        "trace_name": trace_name,
        "wall_clock_time": wall_time,
        "wall_clock_seconds": hms_to_seconds(wall_time),
        "log_file": log_path.name,
    }


def process_baseline_dir(baseline_dir: Path):
    timestamps = [
        d for d in baseline_dir.iterdir()
        if d.is_dir() and TIMESTAMP_RE.fullmatch(d.name)
    ]

    if not timestamps:
        return

    latest = sorted(timestamps)[-1]

    rows = []
    for log_file in sorted(latest.glob("*.log")):
        result = extract_from_log(log_file)
        if result:
            rows.append(result)

    if not rows:
        return

    csv_name = (
        f"{baseline_dir.parent.name}_"
        f"{baseline_dir.name}_"
        f"{latest.name}.csv"
    )
    csv_path = latest / csv_name

    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "trace_name",
                "wall_clock_time",
                "wall_clock_seconds",
                "log_file",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    print(f"Wrote CSV: {csv_path} ({len(rows)} traces)")


def main(root_dir: Path, baseline_name: str):
    for path in root_dir.rglob(baseline_name):
        if path.is_dir():
            process_baseline_dir(path)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <results_root> <baseline_dir_name>")
        sys.exit(1)

    root = Path(sys.argv[1]).resolve()
    baseline_name = sys.argv[2]

    if not root.exists():
        print(f"ERROR: {root} does not exist")
        sys.exit(1)

    main(root, baseline_name)