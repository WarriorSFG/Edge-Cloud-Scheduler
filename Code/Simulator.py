"""
Simulator.py — High-performance parallel simulation orchestrator for the Scheduler.

Runs fast, faithful event-driven simulations of the scheduling problem using
the multi-threaded C++ engine (Simulator.exe) with OpenMP parallelization.

Usage:
    python Simulator.py [testcases.jsonl] [--threads N] [--repeat N] [--profile NAME] [--verbose]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def get_compiler_cmd(cpp_file: Path, scheduler_cpp: Path, output_exe: Path) -> list[str] | None:
    """Find an available C++ compiler and return the compilation command."""
    # Check for g++ (MinGW/MSYS2)
    if shutil.which("g++"):
        return [
            "g++",
            "-O3",
            "-fopenmp",
            "-std=c++17",
            "-I", str(scheduler_cpp.parent),
            str(cpp_file),
            str(scheduler_cpp),
            "-o", str(output_exe),
        ]
    
    # Check for clang++
    if shutil.which("clang++"):
        return [
            "clang++",
            "-O3",
            "-fopenmp",
            "-std=c++17",
            "-I", str(scheduler_cpp.parent),
            str(cpp_file),
            str(scheduler_cpp),
            "-o", str(output_exe),
        ]

    # Check for MSVC cl.exe
    if shutil.which("cl"):
        return [
            "cl",
            "/O2",
            "/openmp",
            "/std:c++17",
            "/EHsc",
            f"/I{scheduler_cpp.parent}",
            str(cpp_file),
            str(scheduler_cpp),
            f"/Fe:{output_exe}",
        ]

    return None


def ensure_binary(rebuild: bool = False) -> Path:
    """Ensure Simulator.exe is compiled and up to date."""
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent
    cpp_file = script_dir / "Simulator.cpp"
    scheduler_cpp = project_root / "Schedulers" / "Scheduler.cpp"
    scheduler_h = project_root / "Schedulers" / "Scheduler.h"
    output_exe = script_dir / ("Simulator.exe" if os.name == "nt" else "Simulator")

    if not cpp_file.exists():
        raise FileNotFoundError(f"Simulator source not found: {cpp_file}")
    if not scheduler_cpp.exists():
        raise FileNotFoundError(f"Scheduler source not found: {scheduler_cpp}")

    needs_build = rebuild or not output_exe.exists()
    if not needs_build:
        exe_mtime = output_exe.stat().st_mtime
        if (
            cpp_file.stat().st_mtime > exe_mtime
            or scheduler_cpp.stat().st_mtime > exe_mtime
            or scheduler_h.stat().st_mtime > exe_mtime
        ):
            needs_build = True

    if needs_build:
        cmd = get_compiler_cmd(cpp_file, scheduler_cpp, output_exe)
        if not cmd:
            raise RuntimeError(
                "No supported C++ compiler (g++, clang++, cl) found in PATH. "
                "Please install a C++ compiler with OpenMP support."
            )
        print(f"Compiling Simulator engine: {' '.join(cmd)}")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(res.stdout, file=sys.stdout)
            print(res.stderr, file=sys.stderr)
            raise RuntimeError(f"Compilation failed with return code {res.returncode}")
        print("Compilation successful.")

    return output_exe


def run_simulation(
    testcases_path: str,
    threads: int = 0,
    repeat: int = 1,
    profile: str = "",
    verbose: bool = False,
    json_mode: bool = False,
    rebuild: bool = False,
) -> int:
    """Run the compiled Simulator on the given testcases."""
    exe_path = ensure_binary(rebuild=rebuild)

    cmd = [str(exe_path), "-i", testcases_path]
    if threads > 0:
        cmd.extend(["-t", str(threads)])
    if repeat > 1:
        cmd.extend(["-r", str(repeat)])
    if profile:
        cmd.extend(["-p", profile])
    if verbose:
        cmd.append("-v")
    if json_mode:
        cmd.append("--json")

    res = subprocess.run(cmd)
    return res.returncode


def main():
    parser = argparse.ArgumentParser(
        description="Parallel, high-throughput simulation runner for the Scheduler.",
    )
    parser.add_argument(
        "testcases",
        type=str,
        nargs="?",
        default="Testcases/Raw/testcases.jsonl",
        help="Path to JSONL testcases file (default: Testcases/Raw/testcases.jsonl).",
    )
    parser.add_argument(
        "--threads", "-t", "-j",
        type=int,
        default=0,
        help="Number of OpenMP worker threads (default: hardware max).",
    )
    parser.add_argument(
        "--repeat", "-r",
        type=int,
        default=1,
        help="Number of times to repeat test suite evaluation for benchmarking (default: 1).",
    )
    parser.add_argument(
        "--profile", "-p",
        type=str,
        default="",
        help="Filter testcases to evaluate only a specific profile.",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print per-testcase breakdown table (ID, Profile, Score, TP, TDR, TPOT).",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output summary metrics in JSON format.",
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Force recompilation of the Simulator C++ binary.",
    )

    args = parser.parse_args()

    # Resolve relative path if needed
    testcases_file = Path(args.testcases)
    if not testcases_file.exists():
        # Try from project root
        project_root = Path(__file__).resolve().parent.parent
        alt_path = project_root / args.testcases
        if alt_path.exists():
            testcases_file = alt_path
        else:
            print(f"Error: Testcases file not found: {args.testcases}", file=sys.stderr)
            sys.exit(1)

    code = run_simulation(
        testcases_path=str(testcases_file),
        threads=args.threads,
        repeat=args.repeat,
        profile=args.profile,
        verbose=args.verbose,
        json_mode=args.json,
        rebuild=args.rebuild,
    )
    sys.exit(code)


if __name__ == "__main__":
    main()
