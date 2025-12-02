#!/usr/bin/env python3
"""
ChampSim Configuration + Build Script

This script:
1. Reads the JSON config file to determine the executable name
2. Runs config.sh with the given config
3. Builds ChampSim with make
4. Verifies that the expected binary exists

This script intentionally does *not* run any simulations.
"""

import subprocess
import argparse
from pathlib import Path
import json
import shutil
import sys


def load_executable_name(config_file):
    """Extract `executable_name` from the JSON config (default: champsim)."""
    try:
        with open(config_file, 'r') as f:
            cfg = json.load(f)
            return cfg.get("executable_name", "champsim")
    except Exception as e:
        print(f"Warning: could not read executable_name: {e}")
        return "champsim"


def configure_champsim(root, config_file):
    """
    Run config.sh <config_file> and wipe previous .csconfig directory.
    """
    csconfig_dir = root / ".csconfig"
    if csconfig_dir.exists():
        print("Cleaning previous configuration...")
        shutil.rmtree(csconfig_dir)

    config_script = root / "config.sh"

    print(f"Running config.sh with: {config_file}")
    result = subprocess.run(
        ["python3", str(config_script), str(config_file)],
        cwd=root,
        check=True,
        capture_output=True,
        text=True
    )

    if result.stdout:
        print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr)

    print("✓ Configuration completed successfully")


def build_champsim(root):
    """
    Run `make` in the ChampSim root directory.
    """
    print("Building ChampSim...")

    result = subprocess.run(
        ["make"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True
    )

    if result.stdout:
        print(result.stdout)
    if result.stderr:
        print("STDERR:", result.stderr)

    print("✓ Build completed successfully")


def main():
    parser = argparse.ArgumentParser(
        description="ChampSim Configuration + Build Script"
    )

    parser.add_argument(
        "config_file",
        help="JSON configuration file for ChampSim (relative to root or absolute)"
    )

    args = parser.parse_args()

    champsim_root = Path(__file__).parent.resolve()

    # Determine absolute JSON path
    if Path(args.config_file).is_absolute():
        config_path = Path(args.config_file)
    else:
        config_path = champsim_root / args.config_file

    if not config_path.exists():
        print(f"ERROR: config file not found: {config_path}")
        return 1

    exe_name = load_executable_name(config_path)

    print(f"ChampSim root:     {champsim_root}")
    print(f"Config file:       {config_path}")
    print(f"Executable name:   {exe_name}")
    print()

    # Run configuration
    try:
        configure_champsim(champsim_root, config_path)
    except subprocess.CalledProcessError as e:
        print("✗ Configuration failed")
        print("STDOUT:", e.stdout)
        print("STDERR:", e.stderr)
        return 1

    # Build
    try:
        build_champsim(champsim_root)
    except subprocess.CalledProcessError as e:
        print("✗ Build failed")
        print("STDOUT:", e.stdout)
        print("STDERR:", e.stderr)
        return 1

    # Verify the binary
    binary_path = champsim_root / "bin" / exe_name
    if not binary_path.exists():
        print(f"ERROR: Expected binary not found: {binary_path}")
        print("Available binaries in bin/:")
        for p in sorted((champsim_root / "bin").glob("*")):
            if p.is_file():
                print(f"  - {p.name}")
        return 1

    print(f"✓ Verified binary exists: {binary_path}")
    print("Build process completed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())