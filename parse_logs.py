#!/usr/bin/env python3
"""
ChampSim Log Parser (Recursive Version)

Automatically walks the entire results directory and processes every:
    results/<suite>/<executable>/<timestamp>/

For each timestamp folder containing .log files:

  - If a CSV already exists, skip it.
  - If not, parse all .log files using existing semantics
    and generate a CSV named:

      <suite>_<executable>_<timestamp>.csv

Dots are replaced with underscores.
"""

import os
import sys
import re
import csv
from pathlib import Path


# ============================================================
# =============== ORIGINAL PARSER LOGIC (UNCHANGED) ==========
# ============================================================

class LogParser:
    def __init__(self, results_dir):
        self.results_dir = Path(results_dir)

    def parse_log_file(self, log_file):
        try:
            with open(log_file, 'r') as f:
                content = f.read()
        except Exception as e:
            print(f"Error reading {log_file}: {e}")
            return None

        metrics = {}

        filename = Path(log_file).stem
        for ext in ['.trace.gz', '.trace', '.champsimtrace.gz', '.champsimtrace']:
            if filename.endswith(ext):
                filename = filename[:-len(ext)]
                break

        metrics['trace_name'] = filename

        ipc = re.search(r'CPU 0 cumulative IPC: ([\d.]+)', content)
        metrics['IPC'] = float(ipc.group(1)) if ipc else None

        # ---------- L1D ----------
        l1d_total = re.search(r'cpu0->cpu0_L1D TOTAL.*?ACCESS:\s+(\d+)', content)
        metrics['L1D_total_access'] = int(l1d_total.group(1)) if l1d_total else None

        l1d_miss = re.search(
            r'cpu0->cpu0_L1D TOTAL.*?ACCESS:\s+\d+.*?HIT:\s+\d+.*?MISS:\s+(\d+)',
            content, re.DOTALL
        )
        if l1d_total and l1d_miss:
            total = int(l1d_total.group(1))
            miss = int(l1d_miss.group(1))
            metrics['L1D_miss_rate'] = (miss / total * 100) if total > 0 else 0
        else:
            metrics['L1D_miss_rate'] = None

        l1d_lat = re.search(r'cpu0->cpu0_L1D AVERAGE MISS LATENCY: ([\d.]+|-)', content)
        metrics['L1D_avg_miss_latency'] = (
            float(l1d_lat.group(1)) if l1d_lat and l1d_lat.group(1) != '-' else None
        )

        # ---------- L2C ----------
        l2c_total = re.search(r'cpu0->cpu0_L2C TOTAL.*?ACCESS:\s+(\d+)', content)
        metrics['L2C_total_access'] = int(l2c_total.group(1)) if l2c_total else None

        l2c_miss = re.search(
            r'cpu0->cpu0_L2C TOTAL.*?ACCESS:\s+\d+.*?HIT:\s+\d+.*?MISS:\s+(\d+)',
            content, re.DOTALL
        )
        if l2c_total and l2c_miss:
            total = int(l2c_total.group(1))
            miss = int(l2c_miss.group(1))
            metrics['L2C_miss_rate'] = (miss / total * 100) if total > 0 else 0
        else:
            metrics['L2C_miss_rate'] = None

        l2c_lat = re.search(r'cpu0->cpu0_L2C AVERAGE MISS LATENCY: ([\d.]+|-)', content)
        metrics['L2C_avg_miss_latency'] = (
            float(l2c_lat.group(1)) if l2c_lat and l2c_lat.group(1) != '-' else None
        )

        # ---------- LLC ----------
        llc_total = re.search(r'cpu0->LLC TOTAL.*?ACCESS:\s+(\d+)', content)
        metrics['LLC_total_access'] = int(llc_total.group(1)) if llc_total else None

        llc_miss = re.search(
            r'cpu0->LLC TOTAL.*?ACCESS:\s+\d+.*?HIT:\s+\d+.*?MISS:\s+(\d+)',
            content, re.DOTALL
        )
        if llc_total and llc_miss:
            total = int(llc_total.group(1))
            miss = int(llc_miss.group(1))
            metrics['LLC_miss_rate'] = (miss / total * 100) if total > 0 else 0
        else:
            metrics['LLC_miss_rate'] = None

        llc_lat = re.search(r'cpu0->LLC AVERAGE MISS LATENCY: ([\d.]+|-)', content)
        metrics['LLC_avg_miss_latency'] = (
            float(llc_lat.group(1)) if llc_lat and llc_lat.group(1) != '-' else None
        )

        # ---------- Prefetch ----------
        def pref_acc(pattern):
            m = re.search(pattern, content, re.DOTALL)
            if not m:
                return None
            requested = int(m.group(1))
            useful = int(m.group(2))
            return (useful / requested * 100) if requested > 0 else 0

        metrics['L1D_prefetch_accuracy'] = pref_acc(
            r'cpu0->cpu0_L1D PREFETCH REQUESTED:\s+(\d+).*?USEFUL:\s+(\d+)'
        )
        metrics['L2C_prefetch_accuracy'] = pref_acc(
            r'cpu0->cpu0_L2C PREFETCH REQUESTED:\s+(\d+).*?USEFUL:\s+(\d+)'
        )

        llc_pref = re.search(
            r'cpu0->LLC PREFETCH REQUESTED:\s+(\d+).*?ISSUED:\s+\d+.*?USEFUL:\s+(\d+)',
            content, re.DOTALL
        )
        if llc_pref:
            req = int(llc_pref.group(1))
            use = int(llc_pref.group(2))
            metrics['LLC_prefetch_accuracy'] = (use / req * 100) if req > 0 else 0
        else:
            metrics['LLC_prefetch_accuracy'] = None

        return metrics

    def process_directory(self):
        log_files = list(self.results_dir.glob("*.log"))
        metrics = []

        if not log_files:
            print(f"No logs found in {self.results_dir}")
            return metrics

        print(f"Found {len(log_files)} logs in {self.results_dir}")

        for log_file in sorted(log_files):
            m = self.parse_log_file(log_file)
            if m:
                metrics.append(m)

        return metrics

    def write_csv(self, metrics_list, csv_path):
        headers = [
            'trace_name',
            'IPC',
            'L1D_total_access', 'L1D_miss_rate', 'L1D_avg_miss_latency', 'L1D_prefetch_accuracy',
            'L2C_total_access', 'L2C_miss_rate', 'L2C_avg_miss_latency', 'L2C_prefetch_accuracy',
            'LLC_total_access', 'LLC_miss_rate', 'LLC_avg_miss_latency', 'LLC_prefetch_accuracy',
        ]

        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=headers)
            writer.writeheader()
            for row in metrics_list:
                writer.writerow(row)

        print(f"✓ Wrote CSV: {csv_path}")


# ============================================================
# =============== NEW RECURSIVE WALKING LOGIC ================
# ============================================================

def main():
    root = Path(__file__).parent
    results_root = root / "results"

    if not results_root.exists():
        print("ERROR: results/ directory not found")
        return 1

    print("Walking results/ ...\n")

    # Iterate over all subdirectories of results/
    for timestamp_dir in results_root.rglob("*"):
        if not timestamp_dir.is_dir():
            continue

        # Must contain .log files
        logs = list(timestamp_dir.glob("*.log"))
        if not logs:
            continue

        # Skip if CSV already exists
        existing_csvs = list(timestamp_dir.glob("*.csv"))
        if existing_csvs:
            print(f"Skipping {timestamp_dir} (already has CSV)")
            continue

        # Build CSV name based on relative path: suite/exe/timestamp
        rel = timestamp_dir.relative_to(results_root)
        clean_parts = [p.replace(".", "_") for p in rel.parts]
        csv_name = "_".join(clean_parts) + ".csv"

        csv_path = timestamp_dir / csv_name

        print(f"\n=== Processing {timestamp_dir} ===")
        parser = LogParser(timestamp_dir)
        metrics = parser.process_directory()
        parser.write_csv(metrics, csv_path)

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())