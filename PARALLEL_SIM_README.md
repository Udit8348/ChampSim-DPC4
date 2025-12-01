# ChampSim Parallel Simulation Manager

A Python script that manages parallel ChampSim simulations with automatic trace management and result organization.

## Features

- **Configuration & Build**: Automatically runs `config.sh` and `make` with your specified configuration
- **Parallel Execution**: Launches multiple simulations simultaneously using process management
- **Smart Queue Management**: Monitors completion and automatically launches remaining traces
- **Organized Results**: Saves simulation logs in `results/<trace_dir>/` with descriptive filenames
- **Progress Tracking**: Real-time monitoring of simulation status (PID, trace name, output location)
- **Summary Report**: Detailed report of completed/failed simulations

## Usage

### Basic Command Structure

```bash
./parallel_sim.py <config_file> <traces_dir> [options]
```

### Arguments

- **config_file**: JSON configuration file (relative to ChampSim root or absolute path)
  - Examples: `dpc4/1C.baseline.json`, `dpc4/4C.baseline.json`
- **traces_dir**: Directory containing trace files (relative to `traces/` folder)
  - Example: `ai-ml` → reads traces from `traces/ai-ml/`

### Options

- `-n, --num-parallel N`: Number of parallel simulations (default: 4)
- `--warmup INSTRUCTIONS`: Number of warmup instructions (default: 200,000,000)
- `--sim INSTRUCTIONS`: Number of simulation instructions (default: 500,000,000)

## Examples

### Run 4 parallel simulations with default settings

```bash
./parallel_sim.py dpc4/1C.baseline.json ai-ml -n 4
```

### Run 8 parallel simulations

```bash
./parallel_sim.py dpc4/1C.baseline.json ai-ml -n 8
```

### Run with custom instruction counts

```bash
./parallel_sim.py dpc4/4C.baseline.json ai-ml -n 6 --warmup 100000000 --sim 250000000
```

### Use absolute path for configuration

```bash
./parallel_sim.py /home/user/config.json ai-ml -n 4
```

## How It Works

### Workflow

1. **Validation**: Checks that ChampSim root, config file, and traces directory exist
2. **Configuration**: Runs `./config.sh <config_file>` to generate ChampSim configuration
3. **Build**: Runs `make` to compile ChampSim with the specified configuration
4. **Initial Launch**: Starts N simulations in parallel (where N = `--num-parallel`)
5. **Monitoring Loop**:
   - Checks every second for completed simulations
   - For each completed simulation:
     - Saves the log file to `results/<traces_dir>/<trace_name>.log`
     - Launches the next trace from the queue
   - Continues until all traces are processed

### Output Organization

Results are saved in the following structure:
```
results/
└── <traces_dir>/
    ├── 600.perlbench_s-210B.log
    ├── 602.gcc_s-1850B.log
    ├── 605.mcf_s-484B.log
    └── ...
```

Each log file is automatically named based on the trace filename with `.log` extension.

## Process Details

### Simulation Command

Each simulation is launched with:
```bash
bin/champsim --warmup-instructions <warmup> --simulation-instructions <sim> <trace_file>
```

### Output Capture

- Stdout and stderr are combined and redirected to result files
- Results are saved as simulations complete (not at the end)
- Directory structure is created automatically if it doesn't exist

### Process Management

- Uses Python's `subprocess.Popen()` for non-blocking process management
- Monitors process completion via `process.poll()`
- Maintains up to N active processes simultaneously

## Example Output

```
============================================================
ChampSim Parallel Simulation Manager
============================================================

Found 12 trace files

============================================================
Configuring ChampSim with: 1C.baseline.json
============================================================
✓ Configuration completed successfully

============================================================
Building ChampSim
============================================================
✓ Build completed successfully

============================================================
Simulation Management
============================================================
Total traces: 12
Parallel simulations: 4
Traces directory: /home/user/ChampSim/traces/ai-ml
Results directory: /home/user/ChampSim/results/ai-ml
============================================================

Launching initial batch of simulations...
  [PID 12345] Launched: 600.perlbench_s-210B.champsimtrace.xz
             Output: /home/user/ChampSim/results/ai-ml/600.perlbench_s-210B.log
  [PID 12346] Launched: 602.gcc_s-1850B.champsimtrace.xz
             Output: /home/user/ChampSim/results/ai-ml/602.gcc_s-1850B.log
  ...

✓ [PID 12345] Completed: 600.perlbench_s-210B.champsimtrace.xz
  [PID 12349] Launched: 605.mcf_s-484B.champsimtrace.xz
             Output: /home/user/ChampSim/results/ai-ml/605.mcf_s-484B.log

✓ [PID 12346] Completed: 602.gcc_s-1850B.champsimtrace.xz
  ...

============================================================
Simulation Summary
============================================================
Completed: 12/12
Failed: 0/12
============================================================
```

## Troubleshooting

### "No trace files found in: ..."

- Check that the traces directory exists
- Verify the path is relative to the `traces/` folder
- Ensure trace files have supported extensions: `.champsimtrace.xz`, `.champsimtrace`, `.trace.xz`, `.trace`

### "ChampSim binary not found"

- Ensure the build completed successfully
- Check that `bin/champsim` exists after the build

### "Configuration file not found"

- Use a path relative to the ChampSim root or provide an absolute path
- Available configs: `dpc4/1C.baseline.json`, `dpc4/4C.baseline.json`, etc.

### Simulations failing

- Check the log files in `results/<traces_dir>/` for error details
- Verify trace files are not corrupted
- Ensure sufficient disk space and memory

## Tips

- Start with `-n 2` or `-n 4` to test setup before scaling up
- Monitor system resources (CPU, memory, I/O) when choosing parallel count
- For best performance, choose N equal to number of CPU cores available
- Use `tail -f results/traces_dir/trace_name.log` to monitor a running simulation

## Requirements

- Python 3.6+
- ChampSim repository with valid `config.sh` and `Makefile`
- Trace files in the specified directory
- Sufficient disk space for result logs

## License

This script is part of the ChampSim project and follows the same license terms.
