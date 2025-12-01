# ChampSim Log Parser

A Python script to parse ChampSim simulation log files and extract meaningful performance metrics into a CSV file.

## Features

The script extracts the following metrics from ChampSim log files:

- **IPC** (Instructions Per Cycle) - overall performance metric
- **L1D Cache**:
  - Total access count
  - Miss rate (percentage)
  - Average miss latency (cycles)
  - Prefetcher accuracy (percentage of prefetches that were useful)
- **L2C Cache**:
  - Total access count
  - Miss rate (percentage)
  - Average miss latency (cycles)
  - Prefetcher accuracy (percentage of prefetches that were useful)
- **LLC Cache** (Last Level Cache):
  - Total access count
  - Miss rate (percentage)
  - Average miss latency (cycles)
  - Prefetcher accuracy (percentage of prefetches that were useful)

## Usage

```bash
./parse_logs.py <results_subdir> [options]
```

### Arguments

- `results_subdir`: Results subdirectory relative to the `results/` folder (required)
- `-o, --output`: Output directory for CSV files (default: `results_csv`)

### Examples

```bash
# Parse logs from results/Graph directory
./parse_logs.py Graph

# Parse logs and save to custom output directory
./parse_logs.py Graph -o my_output_folder

# Parse logs from results/dpc4 directory
./parse_logs.py dpc4
```

## Output

The script generates a CSV file with all extracted metrics. The filename follows the pattern:
```
results_csv/<subdir>/<subdir>_metrics.csv
```

For example, parsing `Graph` results produces:
```
results_csv/Graph/Graph_metrics.csv
```

### CSV Structure

Each row in the CSV contains metrics for one trace:

```csv
trace_name,IPC,L1D_total_access,L1D_miss_rate,L1D_avg_miss_latency,L1D_prefetch_accuracy,...
bc-0,0.2111,85676679,33.72,93.63,18.95,...
bfs-10,0.6415,66237501,19.61,66.54,50.49,...
```

## Performance Notes

- Handles multiple trace file extensions: `.trace.gz`, `.trace`, `.champsimtrace.gz`, `.champsimtrace`
- Processes all `.log` files in the specified directory
- Provides progress feedback during parsing
- Handles missing or malformed metrics gracefully (stores `None` for unavailable data)

## Requirements

- Python 3.6+
- No external dependencies (uses only standard library modules: `re`, `csv`, `pathlib`)

## Script Location

The script should be placed in the ChampSim root directory alongside `parallel_sim.py`.

```
ChampSim/
├── parallel_sim.py
├── parse_logs.py        ← This script
├── results/
│   └── Graph/           ← Log files here
└── results_csv/         ← Output CSV files here
```
