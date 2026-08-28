"""
Patcher.py — Injects tuned knob values from .env into C++ submission / scheduler files.

Reads key-value pairs (e.g. V4_SPT=1, V4_CHUNK_SMULT=182.5) from a .env file
(such as Artifacts/champion.env) and patches the fallback/default values in
C++ source files (such as Patcher/Submission.cpp or Schedulers/Scheduler.h).

Usage:
    python Patcher/Patcher.py [--env Artifacts/champion.env] [--cpp Patcher/Submission.cpp]
"""

import argparse
import difflib
import os
from pathlib import Path
import re
import sys
from typing import Any


def parse_env_file(env_path: Path) -> dict[str, str]:
    """Parse a .env or shell export file into a dict of {KNOB_NAME: VALUE}."""
    if not env_path.exists():
        raise FileNotFoundError(f"Environment file not found: {env_path}")

    knobs: dict[str, str] = {}
    with open(env_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            # Strip optional 'export ' prefix
            if line.startswith("export "):
                line = line[7:].strip()

            if "=" not in line:
                continue

            key, val = line.split("=", 1)
            key = key.strip()
            val = val.strip().strip("'\"")

            # Strip V4_ prefix if present to standardize knob name
            if key.startswith("V4_"):
                key = key[3:]

            knobs[key] = val

    return knobs


def patch_cpp_content(content: str, knobs: dict[str, str]) -> tuple[str, list[dict[str, Any]]]:
    """
    Patch C++ source content with knob values.
    Supports:
      1. envi("V4_NAME", <default>) / envd("V4_NAME", <default>)
      2. struct initializers: type NAME = <default>;
      3. direct constant assignments: KN_NAME = <default>;
    """
    patched = content
    changes: list[dict[str, Any]] = []

    for name, new_val in knobs.items():
        # Format new value (avoid .0 for integer-like values if applicable)
        try:
            float_val = float(new_val)
            if float_val.is_integer() and "." not in new_val and "e" not in new_val.lower():
                formatted_val = str(int(float_val))
            else:
                formatted_val = new_val
        except ValueError:
            formatted_val = new_val

<<<<<<< Updated upstream
        # Pattern 1: env fallback calls (e.g. envd("V4_W1", <val>), envi("V4_SPT", <val>))
=======
        # Pattern 1: envi("V4_NAME", <old_val>) or envd("V4_NAME", <old_val>)
        # Matches: envi("V4_SPT", 1) -> envi("V4_SPT", <new_val>)
>>>>>>> Stashed changes
        p1 = re.compile(
            rf'(env[id]\s*\(\s*"V4_{re.escape(name)}"\s*,\s*)([^)]+?)(\s*\))'
        )

<<<<<<< Updated upstream
        # Pattern 2: Struct initializers (e.g. double W1 = 0.7774; int SPT = 1;)
=======
        # Pattern 2: Struct initializers (e.g. in Scheduler.h)
        # Matches: int SPT = 1; -> int SPT = <new_val>;
>>>>>>> Stashed changes
        p2 = re.compile(
            rf'((?:int|double|float|int32_t|int64_t)\s+{re.escape(name)}\s*=\s*)([^;]+?)(;)'
        )

        # Pattern 3: Static assignment (e.g. KN_SPT = 1;)
        p3 = re.compile(
            rf'(KN_{re.escape(name)}\s*=\s*)([^;]+?)(;)'
        )

        matched = False
        old_val_str = ""

<<<<<<< Updated upstream
        def repl_func(m: re.Match) -> str:
            nonlocal matched, old_val_str
            matched = True
            if not old_val_str:
                old_val_str = m.group(2).strip()
            return f"{m.group(1)}{formatted_val}{m.group(3)}"

        # Patch all occurrences in the content
        patched, c2 = p2.subn(repl_func, patched)
        patched, c1 = p1.subn(repl_func, patched)
        patched, c3 = p3.subn(repl_func, patched)
=======
        def repl1(m: re.Match) -> str:
            nonlocal matched, old_val_str
            matched = True
            old_val_str = m.group(2).strip()
            return f"{m.group(1)}{formatted_val}{m.group(3)}"

        def repl2(m: re.Match) -> str:
            nonlocal matched, old_val_str
            matched = True
            old_val_str = m.group(2).strip()
            return f"{m.group(1)}{formatted_val}{m.group(3)}"

        def repl3(m: re.Match) -> str:
            nonlocal matched, old_val_str
            matched = True
            old_val_str = m.group(2).strip()
            return f"{m.group(1)}{formatted_val}{m.group(3)}"

        patched, count1 = p1.subn(repl1, patched)
        patched, count2 = p2.subn(repl2, patched)
        patched, count3 = p3.subn(repl3, patched)
>>>>>>> Stashed changes

        if matched:
            changes.append({
                "knob": name,
                "old": old_val_str,
                "new": formatted_val,
                "status": "UPDATED" if old_val_str != formatted_val else "UNCHANGED",
            })
<<<<<<< Updated upstream

=======
>>>>>>> Stashed changes
        else:
            changes.append({
                "knob": name,
                "old": "N/A",
                "new": formatted_val,
                "status": "NOT FOUND",
            })

    return patched, changes


def main():
    parser = argparse.ArgumentParser(
        description="Inject tuned knob values from .env into C++ submission / scheduler files.",
    )
    parser.add_argument(
        "--env", "-e",
        type=str,
        default="Artifacts/champion.env",
        help="Path to .env file containing knob definitions (default: Artifacts/champion.env).",
    )
    parser.add_argument(
        "--cpp", "-c",
        type=str,
        default="Patcher/Submission.cpp",
        help="Path to C++ source file to patch (default: Patcher/Submission.cpp).",
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default=None,
        help="Path to write patched file (default: overwrite target C++ file in-place).",
    )
    parser.add_argument(
        "--backup", "-b",
        action="store_true",
        help="Create a .bak backup file before modifying target file in-place.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Perform a trial run and print the diff without writing any files.",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print detailed per-knob replacement breakdown.",
    )

    args = parser.parse_args()

    project_root = Path(__file__).resolve().parent.parent

    # Resolve paths
    env_file = Path(args.env)
    if not env_file.is_absolute():
        env_file = (project_root / env_file).resolve()

    cpp_file = Path(args.cpp)
    if not cpp_file.is_absolute():
        cpp_file = (project_root / cpp_file).resolve()

    out_file = Path(args.output) if args.output else cpp_file
    if not out_file.is_absolute():
        out_file = (project_root / out_file).resolve()

    if not env_file.exists():
        print(f"Error: Environment file not found: {env_file}", file=sys.stderr)
        sys.exit(1)

    if not cpp_file.exists():
        print(f"Error: Target C++ file not found: {cpp_file}", file=sys.stderr)
        sys.exit(1)

    # 1. Parse .env
    knobs = parse_env_file(env_file)
    print(f"Loaded {len(knobs)} knob(s) from {env_file}")

    # 2. Read C++ content
    with open(cpp_file, "r", encoding="utf-8") as f:
        original_content = f.read()

    # 3. Patch content
    patched_content, changes = patch_cpp_content(original_content, knobs)

    # 4. Report changes
    updated_count = sum(1 for c in changes if c["status"] == "UPDATED")
    unchanged_count = sum(1 for c in changes if c["status"] == "UNCHANGED")
    not_found_count = sum(1 for c in changes if c["status"] == "NOT FOUND")

    print("\n" + "=" * 70)
    print(f"                        KNOB PATCH REPORT")
    print("=" * 70)
    print(f"  Target File    : {cpp_file}")
    print(f"  Source .env    : {env_file}")
    print(f"  Total Knobs    : {len(knobs)}")
    print(f"  Updated Knobs  : {updated_count}")
    print(f"  Unchanged Knobs: {unchanged_count}")
    if not_found_count > 0:
        print(f"  Not Found      : {not_found_count}")
    print("-" * 70)

    if args.verbose or updated_count > 0:
        print(f"  {'Knob Name':<20} {'Old Value':<20} {'New Value':<20} Status")
        print("  " + "-" * 66)
        for c in changes:
            if args.verbose or c["status"] != "UNCHANGED":
                print(f"  {c['knob']:<20} {c['old']:<20} {c['new']:<20} {c['status']}")
        print("=" * 70 + "\n")

    # 5. Dry-run diff
    if args.dry_run:
        print("--- Dry-Run Mode: Unified Diff ---")
        diff = list(difflib.unified_diff(
            original_content.splitlines(keepends=True),
            patched_content.splitlines(keepends=True),
            fromfile=str(cpp_file),
            tofile=str(out_file),
            n=2,
        ))
        if diff:
            sys.stdout.writelines(diff)
        else:
            print("No changes detected.")
        print("\nDry-run complete. No files modified.")
        return

    # 6. Backup if requested
    if args.backup and out_file.exists():
        backup_file = out_file.with_suffix(out_file.suffix + ".bak")
        with open(backup_file, "w", encoding="utf-8") as f:
            f.write(original_content)
        print(f"Created backup file: {backup_file}")

    # 7. Write output
    out_file.parent.mkdir(parents=True, exist_ok=True)
    with open(out_file, "w", encoding="utf-8") as f:
        f.write(patched_content)

    print(f"Successfully patched {updated_count} knob(s) -> {out_file}")


if __name__ == "__main__":
    main()
