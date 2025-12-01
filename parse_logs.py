#!/usr/bin/env python3
"""
ChampSim Log Parser

Parses ChampSim simulation log files and extracts meaningful metrics into a CSV file.
Metrics extracted:
  - IPC (Instructions Per Cycle)
  - L1D: total access, miss rate, avg miss latency
  - L2C: total access, miss rate, avg miss latency
  - LLC: total access, miss rate, avg miss latency
  - Prefetcher accuracy at L1D, L2C, and LLC
"""

import os
import sys
import re
import csv
from pathlib import Path


class LogParser:
    def __init__(self, results_dir, output_dir):
        """
        Initialize the log parser.
        
        Args:
            results_dir: Directory containing log files (relative to results/)
            output_dir: Output directory for CSV files
        """
        self.results_dir = Path(results_dir)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
    def parse_log_file(self, log_file):
        """
        Parse a single log file and extract metrics.
        
        Returns:
            dict with extracted metrics or None if parsing fails
        """
        try:
            with open(log_file, 'r') as f:
                content = f.read()
        except Exception as e:
            print(f"Error reading {log_file}: {e}")
            return None
            
        metrics = {}
        
        # Extract filename without extension
        filename = Path(log_file).stem
        # Remove .trace.gz or similar extensions
        for ext in ['.trace.gz', '.trace', '.champsimtrace.gz', '.champsimtrace']:
            if filename.endswith(ext):
                filename = filename[:-len(ext)]
                break
        metrics['trace_name'] = filename
        
        # ===== IPC =====
        ipc_match = re.search(r'CPU 0 cumulative IPC: ([\d.]+)', content)
        metrics['IPC'] = float(ipc_match.group(1)) if ipc_match else None
        
        # ===== L1D Metrics =====
        # L1D Total Access
        l1d_total_match = re.search(r'cpu0->cpu0_L1D TOTAL.*?ACCESS:\s+(\d+)', content)
        metrics['L1D_total_access'] = int(l1d_total_match.group(1)) if l1d_total_match else None
        
        # L1D Miss Rate
        l1d_miss_match = re.search(
            r'cpu0->cpu0_L1D TOTAL.*?ACCESS:\s+\d+.*?HIT:\s+\d+.*?MISS:\s+(\d+)',
            content,
            re.DOTALL
        )
        if l1d_total_match and l1d_miss_match:
            total = int(l1d_total_match.group(1))
            misses = int(l1d_miss_match.group(1))
            metrics['L1D_miss_rate'] = (misses / total * 100) if total > 0 else 0
        else:
            metrics['L1D_miss_rate'] = None
            
        # L1D Avg Miss Latency
        l1d_latency_match = re.search(r'cpu0->cpu0_L1D AVERAGE MISS LATENCY: ([\d.]+|-)', content)
        if l1d_latency_match:
            latency_str = l1d_latency_match.group(1)
            metrics['L1D_avg_miss_latency'] = float(latency_str) if latency_str != '-' else None
        else:
            metrics['L1D_avg_miss_latency'] = None
            
        # ===== L2C Metrics =====
        # L2C Total Access
        l2c_total_match = re.search(r'cpu0->cpu0_L2C TOTAL.*?ACCESS:\s+(\d+)', content)
        metrics['L2C_total_access'] = int(l2c_total_match.group(1)) if l2c_total_match else None
        
        # L2C Miss Rate
        l2c_miss_match = re.search(
            r'cpu0->cpu0_L2C TOTAL.*?ACCESS:\s+\d+.*?HIT:\s+\d+.*?MISS:\s+(\d+)',
            content,
            re.DOTALL
        )
        if l2c_total_match and l2c_miss_match:
            total = int(l2c_total_match.group(1))
            misses = int(l2c_miss_match.group(1))
            metrics['L2C_miss_rate'] = (misses / total * 100) if total > 0 else 0
        else:
            metrics['L2C_miss_rate'] = None
            
        # L2C Avg Miss Latency
        l2c_latency_match = re.search(r'cpu0->cpu0_L2C AVERAGE MISS LATENCY: ([\d.]+|-)', content)
        if l2c_latency_match:
            latency_str = l2c_latency_match.group(1)
            metrics['L2C_avg_miss_latency'] = float(latency_str) if latency_str != '-' else None
        else:
            metrics['L2C_avg_miss_latency'] = None
            
        # ===== LLC Metrics =====
        # LLC Total Access
        llc_total_match = re.search(r'cpu0->LLC TOTAL.*?ACCESS:\s+(\d+)', content)
        metrics['LLC_total_access'] = int(llc_total_match.group(1)) if llc_total_match else None
        
        # LLC Miss Rate
        llc_miss_match = re.search(
            r'cpu0->LLC TOTAL.*?ACCESS:\s+\d+.*?HIT:\s+\d+.*?MISS:\s+(\d+)',
            content,
            re.DOTALL
        )
        if llc_total_match and llc_miss_match:
            total = int(llc_total_match.group(1))
            misses = int(llc_miss_match.group(1))
            metrics['LLC_miss_rate'] = (misses / total * 100) if total > 0 else 0
        else:
            metrics['LLC_miss_rate'] = None
            
        # LLC Avg Miss Latency
        llc_latency_match = re.search(r'cpu0->LLC AVERAGE MISS LATENCY: ([\d.]+|-)', content)
        if llc_latency_match:
            latency_str = llc_latency_match.group(1)
            metrics['LLC_avg_miss_latency'] = float(latency_str) if latency_str != '-' else None
        else:
            metrics['LLC_avg_miss_latency'] = None
            
        # ===== Prefetcher Accuracy =====
        # L1D Prefetcher Accuracy
        l1d_pref_match = re.search(
            r'cpu0->cpu0_L1D PREFETCH REQUESTED:\s+(\d+).*?USEFUL:\s+(\d+).*?USELESS:\s+(\d+)',
            content,
            re.DOTALL
        )
        if l1d_pref_match:
            requested = int(l1d_pref_match.group(1))
            useful = int(l1d_pref_match.group(2))
            metrics['L1D_prefetch_accuracy'] = (useful / requested * 100) if requested > 0 else 0
        else:
            metrics['L1D_prefetch_accuracy'] = None
            
        # L2C Prefetcher Accuracy
        l2c_pref_match = re.search(
            r'cpu0->cpu0_L2C PREFETCH REQUESTED:\s+(\d+).*?USEFUL:\s+(\d+).*?USELESS:\s+(\d+)',
            content,
            re.DOTALL
        )
        if l2c_pref_match:
            requested = int(l2c_pref_match.group(1))
            useful = int(l2c_pref_match.group(2))
            metrics['L2C_prefetch_accuracy'] = (useful / requested * 100) if requested > 0 else 0
        else:
            metrics['L2C_prefetch_accuracy'] = None
            
        # LLC Prefetcher Accuracy
        llc_pref_match = re.search(
            r'cpu0->LLC PREFETCH REQUESTED:\s+(\d+).*?ISSUED:\s+(\d+).*?USEFUL:\s+(\d+).*?USELESS:\s+(\d+)',
            content,
            re.DOTALL
        )
        if llc_pref_match:
            requested = int(llc_pref_match.group(1))
            useful = int(llc_pref_match.group(3))
            metrics['LLC_prefetch_accuracy'] = (useful / requested * 100) if requested > 0 else 0
        else:
            metrics['LLC_prefetch_accuracy'] = None
            
        return metrics
        
    def process_directory(self):
        """
        Process all log files in the results directory.
        
        Returns:
            list of parsed metrics dicts
        """
        all_metrics = []
        
        # Find all .log files
        log_files = list(self.results_dir.glob('*.log'))
        
        if not log_files:
            print(f"No log files found in {self.results_dir}")
            return all_metrics
            
        print(f"Found {len(log_files)} log files")
        
        for log_file in sorted(log_files):
            print(f"Parsing {log_file.name}...", end=" ")
            metrics = self.parse_log_file(log_file)
            if metrics:
                all_metrics.append(metrics)
                print("✓")
            else:
                print("✗")
                
        return all_metrics
        
    def write_csv(self, metrics_list, output_file):
        """
        Write metrics to CSV file.
        
        Args:
            metrics_list: list of metrics dicts
            output_file: output CSV file path
        """
        if not metrics_list:
            print("No metrics to write")
            return
            
        # Define column headers in order
        headers = [
            'trace_name',
            'IPC',
            'L1D_total_access', 'L1D_miss_rate', 'L1D_avg_miss_latency', 'L1D_prefetch_accuracy',
            'L2C_total_access', 'L2C_miss_rate', 'L2C_avg_miss_latency', 'L2C_prefetch_accuracy',
            'LLC_total_access', 'LLC_miss_rate', 'LLC_avg_miss_latency', 'LLC_prefetch_accuracy',
        ]
        
        try:
            with open(output_file, 'w', newline='') as csvfile:
                writer = csv.DictWriter(csvfile, fieldnames=headers)
                writer.writeheader()
                for metrics in metrics_list:
                    writer.writerow(metrics)
                    
            print(f"✓ CSV written to {output_file}")
        except Exception as e:
            print(f"✗ Error writing CSV: {e}")
            
    def run(self):
        """Run the complete parsing workflow."""
        print(f"\n{'='*60}")
        print(f"ChampSim Log Parser")
        print(f"{'='*60}\n")
        
        print(f"Processing logs from: {self.results_dir}")
        
        # Process all log files
        metrics_list = self.process_directory()
        
        if not metrics_list:
            print("No metrics extracted!")
            return 1
            
        print(f"\n✓ Extracted metrics from {len(metrics_list)} log files\n")
        
        # Generate output filename
        results_subdir = self.results_dir.name
        output_file = self.output_dir / f"{results_subdir}_metrics.csv"
        
        # Write CSV
        self.write_csv(metrics_list, output_file)
        
        print(f"{'='*60}\n")
        return 0


def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description='ChampSim Log Parser',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Parse Graph results
  ./parse_logs.py Graph
  
  # Parse with custom output directory
  ./parse_logs.py Graph -o custom_output/
        """
    )
    
    parser.add_argument('results_subdir',
        help='Results subdirectory (relative to results/ folder)')
    parser.add_argument('-o', '--output', default='results_csv',
        help='Output directory for CSV files (default: results_csv)')
    
    args = parser.parse_args()
    
    # Resolve paths relative to script location
    script_dir = Path(__file__).parent
    results_base = script_dir / 'results'
    results_dir = results_base / args.results_subdir
    output_dir = script_dir / args.output / args.results_subdir
    
    # Validate input directory
    if not results_dir.exists():
        print(f"ERROR: Results directory not found: {results_dir}")
        return 1
        
    # Create parser and run
    parser_obj = LogParser(results_dir, output_dir)
    exit_code = parser_obj.run()
    
    return exit_code


if __name__ == '__main__':
    sys.exit(main())
