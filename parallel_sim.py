#!/usr/bin/env python3
import os
import sys
import subprocess
import argparse
import time
import glob
import json
import datetime
from pathlib import Path

sys.stdout.reconfigure(line_buffering=True)


def load_skip_list(skip_file: Path) -> set[str]:
    if skip_file is None or not skip_file.exists():
        return set()

    patterns = set()
    for line in skip_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        patterns.add(line)
    return patterns


class SimulationManager:
    def __init__(
        self,
        champsim_root,
        config_file,
        traces_dir,
        num_parallel,
        warmup_instrs,
        sim_instrs,
        skip_patterns,
        results_base=None,
    ):
        self.champsim_root = Path(champsim_root).resolve()
        self.config_file = Path(config_file).resolve()
        self.num_parallel = num_parallel
        self.warmup_instrs = warmup_instrs
        self.sim_instrs = sim_instrs
        self.skip_patterns = skip_patterns

        # Resolve traces directory (absolute or under champsim_root/traces)
        traces_dir = Path(traces_dir)
        if traces_dir.is_absolute():
            self.traces_dir = traces_dir
        else:
            self.traces_dir = self.champsim_root / "traces" / traces_dir

        self.trace_set_name = self.traces_dir.name

        # Load executable name from JSON
        self.executable_name = self._get_executable_name()

        # Timestamp
        self.run_timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")

        # Resolve results base
        if results_base is not None:
            results_root = Path(results_base).resolve()
        else:
            results_root = self.champsim_root / "results"

        self.results_base = (
            results_root /
            self.trace_set_name /
            self.executable_name /
            self.run_timestamp
        )

        # Process tracking
        self.active = {}      # pid -> (proc, trace, log_path, outfile, start_time)
        self.completed = []   # (trace, elapsed_seconds)
        self.failed = []      # (trace, elapsed_seconds)

    def _get_executable_name(self):
        try:
            with open(self.config_file, "r") as f:
                cfg = json.load(f)
            return cfg.get("executable_name", "champsim")
        except Exception:
            return "champsim"

    def validate_inputs(self):
        if not self.traces_dir.exists():
            print(f"ERROR: traces directory not found: {self.traces_dir}")
            return False

        if not self.get_trace_files():
            print(f"ERROR: no trace files found in {self.traces_dir}")
            return False

        binary = self.champsim_root / "bin" / self.executable_name
        if not binary.exists():
            print(f"ERROR: ChampSim binary not found: {binary}")
            return False

        return True

    def get_trace_files(self):
        patterns = [
            "*.champsimtrace.xz",
            "*.champsimtrace.gz",
            "*.champsimtrace",
            "*.trace.xz",
            "*.trace.gz",
            "*.trace",
            "*.champsim",
            "*.champsim.gz"
        ]

        traces = []
        for pat in patterns:
            traces.extend(glob.glob(str(self.traces_dir / pat)))
        return sorted(traces)

    def create_result_dir(self):
        self.results_base.mkdir(parents=True, exist_ok=True)

    def get_result_filename(self, trace_file):
        trace_name = Path(trace_file).name

        for ext in [
            ".champsimtrace.xz",
            ".champsimtrace.gz",
            ".champsimtrace",
            ".trace.xz",
            ".trace.gz",
            ".trace",
            ".champsim",
            ".champsim.gz"
        ]:
            if trace_name.endswith(ext):
                trace_name = trace_name[:-len(ext)]
                break

        return f"{trace_name}.log"

    def launch(self, trace_file):
        exe = self.champsim_root / "bin" / self.executable_name
        self.create_result_dir()

        log_filename = self.get_result_filename(trace_file)
        log_path = self.results_base / log_filename

        cmd = [
            str(exe),
            "--warmup-instructions", str(self.warmup_instrs),
            "--simulation-instructions", str(self.sim_instrs),
            trace_file,
        ]

        try:
            outfile = open(log_path, "w")
            start_time = time.time()
            proc = subprocess.Popen(
                cmd,
                stdout=outfile,
                stderr=subprocess.STDOUT,
            )

            print(f"  [PID {proc.pid}] Launched: {Path(trace_file).name}")
            print(f"             Output: {log_path}")

            self.active[proc.pid] = (
                proc, trace_file, log_path, outfile, start_time
            )

        except Exception as e:
            print(f"✗ Launch failed for {trace_file}: {e}")
            self.failed.append((trace_file, 0.0))

    def run(self):
        all_traces = self.get_trace_files()

        filtered = []
        skipped = []

        for t in all_traces:
            name = Path(t).name
            if any(pat in name for pat in self.skip_patterns):
                skipped.append(name)
            else:
                filtered.append(t)

        if skipped:
            print("Skipping traces:")
            for s in skipped:
                print(f"  - {s}")
            print()

        queue = list(filtered)
        self.run_start_time = time.time()

        print(f"Total traces found: {len(all_traces)}")
        print(f"Traces after filtering: {len(queue)}")
        print(f"Parallel sims: {self.num_parallel}")
        print(f"Binary: {self.executable_name}")
        print(f"Traces directory: {self.traces_dir}")
        print(f"Results directory: {self.results_base}\n")

        print("Launching initial batch of simulations...")
        for _ in range(min(self.num_parallel, len(queue))):
            self.launch(queue.pop(0))

        while self.active or queue:
            time.sleep(1)

            done = []
            for pid, (proc, trace, log_path, outfile, start_time) in list(self.active.items()):
                if proc.poll() is not None:
                    outfile.close()

                    elapsed = time.time() - start_time
                    elapsed_str = str(datetime.timedelta(seconds=int(elapsed)))

                    with open(log_path, "a") as f:
                        f.write("\n")
                        f.write("========================================\n")
                        f.write(f"WALL_CLOCK_TIME: {elapsed_str}\n")
                        f.write("========================================\n")

                    if proc.returncode == 0:
                        print(f"✓ [PID {pid}] Completed: {Path(trace).name} ({elapsed_str})")
                        self.completed.append((trace, elapsed))
                    else:
                        print(
                            f"✗ [PID {pid}] Failed ({proc.returncode}): "
                            f"{Path(trace).name} ({elapsed_str})"
                        )
                        self.failed.append((trace, elapsed))

                    done.append(pid)

            for pid in done:
                del self.active[pid]

            while queue and len(self.active) < self.num_parallel:
                self.launch(queue.pop(0))

        total_elapsed = time.time() - self.run_start_time
        total_str = str(datetime.timedelta(seconds=int(total_elapsed)))

        print("\nSummary:")
        print(f"  Completed: {len(self.completed)}")
        print(f"  Failed:    {len(self.failed)}")
        print(f"  Total wall time: {total_str}")

        return len(self.failed) == 0


def main():
    parser = argparse.ArgumentParser(
        description="ChampSim Parallel Simulation Launcher"
    )

    parser.add_argument("config_file")
    parser.add_argument("traces_dir")
    parser.add_argument("-n", "--num-parallel", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=20_000_000)
    parser.add_argument("--sim", type=int, default=50_000_000)
    parser.add_argument("--skip-file", type=Path, default=None)
    parser.add_argument(
        "--results-base",
        type=Path,
        default=None,
        help="Optional base directory for results (e.g. /fast_data/...)",
    )

    args = parser.parse_args()

    root = Path(__file__).parent.resolve()

    config = Path(args.config_file)
    if not config.is_absolute():
        config = root / config

    skip_file = args.skip_file if args.skip_file else root / "skip_traces.txt"
    skip_patterns = load_skip_list(skip_file)

    manager = SimulationManager(
        champsim_root=root,
        config_file=config,
        traces_dir=args.traces_dir,
        num_parallel=args.num_parallel,
        warmup_instrs=args.warmup,
        sim_instrs=args.sim,
        skip_patterns=skip_patterns,
        results_base=args.results_base,
    )

    if not manager.validate_inputs():
        return 1

    return 0 if manager.run() else 1


if __name__ == "__main__":
    sys.exit(main())