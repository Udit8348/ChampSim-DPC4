#!/usr/bin/env python3
"""
ChampSim Parallel Simulation Manager

This script manages parallel ChampSim simulations:
1. Configures and builds ChampSim with a given configuration file
2. Launches multiple simulations in parallel from a traces directory
3. Monitors simulation completion and launches remaining traces
4. Saves results to organized output directories
"""

import os
import sys
import subprocess
import argparse
import time
import glob
import json
from pathlib import Path
from collections import defaultdict
import signal

class SimulationManager:
    def __init__(self, champsim_root, config_file, traces_dir, num_parallel, warmup_instrs, sim_instrs):
        """
        Initialize the simulation manager.
        
        Args:
            champsim_root: Root directory of ChampSim
            config_file: JSON configuration file path
            traces_dir: Directory containing trace files (relative to traces/ folder)
            num_parallel: Number of parallel simulations
            warmup_instrs: Number of warmup instructions
            sim_instrs: Number of simulation instructions
        """
        self.champsim_root = Path(champsim_root).resolve()
        self.config_file = Path(config_file).resolve()
        self.num_parallel = num_parallel
        self.warmup_instrs = warmup_instrs
        self.sim_instrs = sim_instrs
        
        # Handle traces directory path
        self.traces_base = self.champsim_root / "traces"
        self.traces_dir = self.traces_base / traces_dir
        
        # Results directory
        self.results_base = self.champsim_root / "results" / traces_dir
        
        # Extract executable name from config file
        self.executable_name = self._get_executable_name()
        
        # Active simulations tracking
        self.active_processes = {}  # {process_id: (process_obj, trace_file, result_path)}
        self.completed_traces = []
        self.failed_traces = []
        
    def _get_executable_name(self):
        """Extract the executable name from the config JSON file."""
        try:
            with open(self.config_file, 'r') as f:
                config = json.load(f)
                exe_name = config.get('executable_name', 'champsim')
                return exe_name
        except Exception as e:
            print(f"Warning: Could not read executable_name from config: {e}")
            print(f"Using default: champsim")
            return 'champsim'
        
    def validate_inputs(self):
        """Validate that all input files and directories exist."""
        if not self.champsim_root.exists():
            print(f"ERROR: ChampSim root not found: {self.champsim_root}")
            return False
            
        if not self.config_file.exists():
            print(f"ERROR: Configuration file not found: {self.config_file}")
            return False
            
        if not self.traces_dir.exists():
            print(f"ERROR: Traces directory not found: {self.traces_dir}")
            print(f"       Looked in: {self.traces_dir}")
            return False
            
        trace_files = self.get_trace_files()
        if not trace_files:
            print(f"ERROR: No trace files found in: {self.traces_dir}")
            return False
            
        print(f"Found {len(trace_files)} trace files")
        return True
        
    def configure_champsim(self):
        """Run config.sh with the specified configuration file."""
        print(f"\n{'='*60}")
        print(f"Configuring ChampSim with: {self.config_file.name}")
        print(f"{'='*60}")
        
        try:
            # Clean previous configuration
            csconfig_dir = self.champsim_root / ".csconfig"
            if csconfig_dir.exists():
                print("Cleaning previous configuration...")
                import shutil
                shutil.rmtree(csconfig_dir)
            
            config_script = self.champsim_root / "config.sh"
            
            # Use python3 explicitly to ensure proper execution
            # Convert absolute config_file path to relative path from champsim_root
            if self.config_file.is_absolute():
                try:
                    config_rel = self.config_file.relative_to(self.champsim_root)
                except ValueError:
                    # If config_file is outside champsim_root, use absolute path
                    config_rel = self.config_file
            else:
                config_rel = self.config_file
            
            result = subprocess.run(
                ["python3", str(config_script), str(config_rel)],
                cwd=self.champsim_root,
                check=True,
                capture_output=True,
                text=True
            )
            
            # Print output for visibility
            if result.stdout:
                print(result.stdout)
            if result.stderr:
                print("STDERR:", result.stderr)
                
            print("✓ Configuration completed successfully")
            return True
        except subprocess.CalledProcessError as e:
            print(f"✗ Configuration failed with return code {e.returncode}")
            if e.stdout:
                print("STDOUT:", e.stdout)
            if e.stderr:
                print("STDERR:", e.stderr)
            return False
        except Exception as e:
            print(f"✗ Configuration error: {e}")
            return False
            
    def build_champsim(self):
        """Run make to build ChampSim."""
        print(f"\n{'='*60}")
        print(f"Building ChampSim")
        print(f"{'='*60}")
        
        try:
            result = subprocess.run(
                ["make"],
                cwd=self.champsim_root,
                check=True,
                capture_output=True,
                text=True
            )
            
            # Print output for visibility
            if result.stdout:
                print(result.stdout)
            if result.stderr:
                print("STDERR:", result.stderr)
                
            print("✓ Build completed successfully")
            return True
        except subprocess.CalledProcessError as e:
            print(f"✗ Build failed with return code {e.returncode}")
            if e.stdout:
                print("STDOUT:", e.stdout)
            if e.stderr:
                print("STDERR:", e.stderr)
            return False
        except Exception as e:
            print(f"✗ Build error: {e}")
            return False
            
    def get_trace_files(self):
        """Get all trace files from the traces directory."""
        # Look for common trace file extensions
        trace_patterns = ["*.champsimtrace.xz", "*.champsimtrace", "*.trace.xz", "*.trace"]
        trace_files = []
        
        for pattern in trace_patterns:
            trace_files.extend(glob.glob(str(self.traces_dir / pattern)))
            
        return sorted(trace_files)
        
    def create_result_directory(self, trace_file):
        """Create results directory for a trace file."""
        self.results_base.mkdir(parents=True, exist_ok=True)
        return self.results_base
        
    def get_result_filename(self, trace_file):
        """Generate result filename based on trace filename."""
        trace_name = Path(trace_file).name
        # Remove common trace extensions
        for ext in [".champsimtrace.xz", ".champsimtrace", ".trace.xz", ".trace"]:
            if trace_name.endswith(ext):
                trace_name = trace_name[:-len(ext)]
                break
        return f"{trace_name}.log"
        
    def launch_simulation(self, trace_file):
        """
        Launch a single simulation.
        
        Returns:
            process_id or None if failed to launch
        """
        result_dir = self.create_result_directory(trace_file)
        result_file = result_dir / self.get_result_filename(trace_file)
        
        # Use the executable name from the config file
        champsim_binary = self.champsim_root / "bin" / self.executable_name
        
        if not champsim_binary.exists():
            print(f"✗ ChampSim binary not found: {champsim_binary}")
            print(f"  Available binaries in bin/:")
            bin_dir = self.champsim_root / "bin"
            if bin_dir.exists():
                for exe in sorted(bin_dir.glob("*")):
                    if exe.is_file() and exe.stat().st_mode & 0o111:
                        print(f"    - {exe.name}")
            return None
            
        cmd = [
            str(champsim_binary),
            "--warmup-instructions", str(self.warmup_instrs),
            "--simulation-instructions", str(self.sim_instrs),
            str(trace_file)
        ]
        
        try:
            # Open result file for writing
            with open(result_file, 'w') as outfile:
                proc = subprocess.Popen(
                    cmd,
                    stdout=outfile,
                    stderr=subprocess.STDOUT,
                    cwd=self.champsim_root
                )
                
            proc_id = proc.pid
            self.active_processes[proc_id] = (proc, trace_file, result_file)
            
            print(f"  [PID {proc_id}] Launched: {Path(trace_file).name}")
            print(f"             Output: {result_file}")
            
            return proc_id
            
        except Exception as e:
            print(f"✗ Failed to launch simulation for {Path(trace_file).name}: {e}")
            self.failed_traces.append(trace_file)
            return None
            
    def run_simulations(self):
        """Main simulation loop."""
        trace_files = self.get_trace_files()
        
        if not trace_files:
            print("ERROR: No trace files found!")
            return False
            
        total_traces = len(trace_files)
        trace_queue = list(trace_files)
        
        print(f"\n{'='*60}")
        print(f"Simulation Management")
        print(f"{'='*60}")
        print(f"Total traces: {total_traces}")
        print(f"Parallel simulations: {self.num_parallel}")
        print(f"Binary: {self.executable_name}")
        print(f"Traces directory: {self.traces_dir}")
        print(f"Results directory: {self.results_base}")
        print(f"{'='*60}\n")
        
        # Launch initial batch
        print("Launching initial batch of simulations...")
        for i in range(min(self.num_parallel, len(trace_queue))):
            trace_file = trace_queue.pop(0)
            self.launch_simulation(trace_file)
            
        # Monitor and manage running simulations
        while self.active_processes or trace_queue:
            time.sleep(1)  # Check every second
            
            # Check for completed processes
            completed_pids = []
            for proc_id, (proc, trace_file, result_file) in list(self.active_processes.items()):
                if proc.poll() is not None:  # Process has completed
                    return_code = proc.returncode
                    trace_name = Path(trace_file).name
                    
                    if return_code == 0:
                        print(f"✓ [PID {proc_id}] Completed: {trace_name}")
                        self.completed_traces.append(trace_file)
                    else:
                        print(f"✗ [PID {proc_id}] Failed (code {return_code}): {trace_name}")
                        self.failed_traces.append(trace_file)
                        
                    completed_pids.append(proc_id)
                    
            # Remove completed processes
            for proc_id in completed_pids:
                del self.active_processes[proc_id]
                
            # Launch new simulations if queue has items and slots are available
            while trace_queue and len(self.active_processes) < self.num_parallel:
                trace_file = trace_queue.pop(0)
                self.launch_simulation(trace_file)
                
        # Summary
        print(f"\n{'='*60}")
        print(f"Simulation Summary")
        print(f"{'='*60}")
        print(f"Completed: {len(self.completed_traces)}/{total_traces}")
        print(f"Failed: {len(self.failed_traces)}/{total_traces}")
        
        if self.failed_traces:
            print(f"\nFailed traces:")
            for trace in self.failed_traces:
                print(f"  - {Path(trace).name}")
                
        print(f"{'='*60}\n")
        
        return len(self.failed_traces) == 0
        
    def run(self):
        """Run the complete workflow."""
        print(f"\n{'='*60}")
        print(f"ChampSim Parallel Simulation Manager")
        print(f"{'='*60}\n")
        
        # Validate inputs
        if not self.validate_inputs():
            return 1
            
        # Configure
        # if not self.configure_champsim():
        #     return 1
            
        # Build
        if not self.build_champsim():
            return 1
            
        # Run simulations
        success = self.run_simulations()
        
        return 0 if success else 1


def main():
    parser = argparse.ArgumentParser(
        description='ChampSim Parallel Simulation Manager',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run 4 parallel simulations with default instructions
  ./parallel_sim.py dpc4/1C.baseline.json ai-ml -n 4
  
  # Run 8 parallel simulations with custom instruction counts
  ./parallel_sim.py dpc4/4C.baseline.json ai-ml -n 8 --warmup 100000000 --sim 250000000
        """
    )
    
    parser.add_argument('config_file',
        help='Configuration JSON file (relative to ChampSim root or absolute path)')
    parser.add_argument('traces_dir',
        help='Directory containing trace files (relative to traces/ folder)')
    parser.add_argument('-n', '--num-parallel', type=int, default=8,
        help='Number of parallel simulations to run (default: 16)')
    parser.add_argument('--warmup', type=int, default=20000000,
        help='Number of warmup instructions (default: 20000000)')
    parser.add_argument('--sim', type=int, default=50000000,
        help='Number of simulation instructions (default: 50000000)')
    
    args = parser.parse_args()
    
    # Determine ChampSim root
    champsim_root = Path(__file__).parent
    
    # Determine config file path
    if Path(args.config_file).is_absolute():
        config_file = args.config_file
    else:
        config_file = champsim_root / args.config_file
        
    # Create manager and run
    manager = SimulationManager(
        champsim_root=champsim_root,
        config_file=config_file,
        traces_dir=args.traces_dir,
        num_parallel=args.num_parallel,
        warmup_instrs=args.warmup,
        sim_instrs=args.sim
    )
    
    exit_code = manager.run()
    sys.exit(exit_code)


if __name__ == '__main__':
    main()
