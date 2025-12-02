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


class SimulationManager:
    def __init__(self, champsim_root, config_file, traces_dir, num_parallel, warmup_instrs, sim_instrs):
        self.champsim_root = Path(champsim_root).resolve()
        self.config_file = Path(config_file).resolve()
        self.num_parallel = num_parallel
        self.warmup_instrs = warmup_instrs
        self.sim_instrs = sim_instrs

        # Traces directory
        self.traces_base = self.champsim_root / "traces"
        self.traces_dir = self.traces_base / traces_dir

        # Load executable name from JSON
        self.executable_name = self._get_executable_name()

        # Generate single timestamp for entire run
        self.run_timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")

        # Output directory structure:
        # results/<suite>/<executable>/<timestamp>/
        self.results_base = (
            self.champsim_root /
            "results" /
            traces_dir /
            self.executable_name /
            self.run_timestamp
        )

        # Process tracking
        self.active = {}
        self.completed = []
        self.failed = []

    def _get_executable_name(self):
        try:
            with open(self.config_file, "r") as f:
                cfg = json.load(f)
            return cfg.get("executable_name", "champsim")
        except:
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
            "*.champsimtrace",
            "*.trace.xz",
            "*.trace",
            "*.champsim"
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
            ".champsimtrace",
            ".trace.xz",
            ".trace",
            ".champsim"
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
            trace_file
        ]

        try:
            outfile = open(log_path, "w")
            proc = subprocess.Popen(cmd, stdout=outfile, stderr=subprocess.STDOUT)

            print(f"  [PID {proc.pid}] Launched: {Path(trace_file).name}")
            print(f"             Output: {log_path}")

            self.active[proc.pid] = (proc, trace_file, log_path, outfile)

        except Exception as e:
            print(f"✗ Launch failed for {trace_file}: {e}")
            self.failed.append(trace_file)

    def run(self):
        traces = self.get_trace_files()
        queue = list(traces)

        print(f"Total traces: {len(traces)}")
        print(f"Parallel sims: {self.num_parallel}")
        print(f"Binary: {self.executable_name}")
        print(f"Traces directory: {self.traces_dir}")
        print(f"Results directory: {self.results_base}\n")

        print("Launching initial batch of simulations...")
        for _ in range(min(self.num_parallel, len(queue))):
            trace_file = queue.pop(0)
            self.launch(trace_file)

        while self.active or queue:
            time.sleep(1)

            done = []
            for pid, (proc, trace, log_path, outfile) in list(self.active.items()):
                if proc.poll() is not None:  # completed
                    outfile.close()

                    if proc.returncode == 0:
                        print(f"✓ [PID {pid}] Completed: {Path(trace).name}")
                        self.completed.append(trace)
                    else:
                        print(f"✗ [PID {pid}] Failed ({proc.returncode}): {Path(trace).name}")
                        self.failed.append(trace)

                    done.append(pid)

            for pid in done:
                del self.active[pid]

            while queue and len(self.active) < self.num_parallel:
                trace_file = queue.pop(0)
                self.launch(trace_file)

        print("\nSummary:")
        print(f"  Completed: {len(self.completed)}")
        print(f"  Failed:    {len(self.failed)}")

        return len(self.failed) == 0


def main():
    parser = argparse.ArgumentParser(description="ChampSim Parallel Simulation Launcher")

    parser.add_argument("config_file", help="JSON config file containing executable_name")
    parser.add_argument("traces_dir", help="subdirectory under traces/")
    parser.add_argument("-n", "--num-parallel", type=int, default=8)
    parser.add_argument("--warmup", type=int, default=20_000_000)
    parser.add_argument("--sim", type=int, default=50_000_000)

    args = parser.parse_args()

    root = Path(__file__).parent

    if Path(args.config_file).is_absolute():
        config = Path(args.config_file)
    else:
        config = root / args.config_file

    manager = SimulationManager(
        champsim_root=root,
        config_file=config,
        traces_dir=args.traces_dir,
        num_parallel=args.num_parallel,
        warmup_instrs=args.warmup,
        sim_instrs=args.sim
    )

    if not manager.validate_inputs():
        return 1

    ok = manager.run()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())