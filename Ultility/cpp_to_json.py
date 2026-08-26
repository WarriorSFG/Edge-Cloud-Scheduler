#!/usr/bin/env python3
"""
cpp_to_json.py / extract_knobs.py
==================================
Utility script to extract knobs/hyperparameters from C++ scheduler code
(or snippets, .env files, or direct pasted text) and export them into a 
standardized JSON format with "score": 0.

Output Format:
{
  "score": 0,
  "knobs": {
    "SPT": 1,
    "CHUNK": 0,
    "CHUNK_SMULT": 1255.29423877632,
    "HOLD": 0,
    "WAVES": 67.7573142499191,
    ...
  }
}

Usage Examples:
  1. From CLI passing a file:
       python Ultility/cpp_to_json.py Calibration/SampleCode.cpp
       python Ultility/cpp_to_json.py Calibration/SampleCode.cpp -o Calibration/champion.json

  2. Interactive mode (prompt for file path or paste code snippet):
       python Ultility/cpp_to_json.py

  3. Piping via stdin:
       Get-Content Calibration/SampleCode.cpp | python Ultility/cpp_to_json.py -o Calibration/my_run.json

  4. Pass raw snippet via CLI:
       python Ultility/cpp_to_json.py --text "KN_SPT = envi(\"V4_SPT\", 1); KN_WAVES = envd(\"V4_WAVES\", 67.75);"
"""

import argparse
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Optional


# Standard known integer knobs in the scheduler codebase
KNOWN_INT_KNOBS = {
    "SPT",
    "CHUNK",
    "HOLD",
    "UPPRE_MAX",
    "UPPRE_MAX_TP",
    "WAVES_PROC",
    "LATHOLD",
    "CONS",
    "AGE_NORM",
    "CHUNK_PRED",
    "HOLD_ACT",
    "WAVE_CAPS_BATCH",
}

# Standard known float knobs
KNOWN_FLOAT_KNOBS = {
    "CHUNK_SMULT",
    "WAVES",
    "HOLD_WFRAC",
    "HOLD_SMULT",
    "UPGATE_FRAC",
    "RATE_EFF",
    "DECW",
    "LATFRAC",
    "CONS_PEN",
    "CHUNK_MINS",
    "CHUNK_TPP",
    "BASE_W",
    "B_DPOST",
    "B_PPOST",
    "B_DPRE",
    "B_PPRE",
    "B_DPROC",
    "B_PPROC",
    "AGE_FLOOR",
    "AGE_AW",
    "AGE_PRESS",
    "AGE_SLO_W",
    "DECQ",
    "CHUNK_RATIO",
    "LAT_MULT",
    "GATE_TDR",
    "HOLD_AW",
    "PPRE_AGECAP",
}


def clean_knob_name(name: str) -> str:
    """Normalize knob names by stripping V4_, KN_, etc."""
    name = name.strip()
    if name.startswith("V4_"):
        name = name[3:]
    if name.startswith("KN_"):
        name = name[3:]
    if name.startswith("V4_"):
        name = name[3:]
    return name


def cast_knob_value(name: str, val_str: str) -> int | float:
    """Cast a string value to int or float based on value and knob schema."""
    val_str = val_str.strip().rstrip(";,").strip("\"'")
    
    normalized_name = clean_knob_name(name)
    if normalized_name in KNOWN_INT_KNOBS:
        try:
            return int(round(float(val_str)))
        except ValueError:
            pass

    try:
        f_val = float(val_str)
        if normalized_name in KNOWN_INT_KNOBS:
            return int(round(f_val))
        if f_val.is_integer() and "." not in val_str and "e" not in val_str.lower() and normalized_name not in KNOWN_FLOAT_KNOBS:
            return int(f_val)
        return f_val
    except ValueError:
        raise ValueError(f"Cannot parse value '{val_str}' for knob '{name}'")


def strip_cpp_comments(text: str) -> str:
    """Remove C and C++ style comments from text."""
    # Remove block comments
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    # Remove single line comments
    text = re.sub(r'//.*$', '', text, flags=re.MULTILINE)
    return text


def extract_knobs_from_text(content: str) -> dict[str, Any]:
    """
    Extract knobs dictionary from C++ source code, headers, .env exports, or raw snippets.
    Supports:
      - envi("V4_NAME", <default>) / envd("V4_NAME", <default>)
      - KN_NAME = envi(...) / KN_NAME = envd(...)
      - KN_NAME = <val>;
      - int/double/float NAME = <val>;
      - export V4_NAME=<val> / V4_NAME=<val> / NAME=<val>
    """
    clean_text = strip_cpp_comments(content)
    knobs: dict[str, Any] = {}

    # Pattern 1: envi / envd calls e.g. envi("V4_SPT", 1) or envd("V4_CHUNK_SMULT", 1255.29)
    env_pattern = re.compile(
        r'env[id]\s*\(\s*["\']?(?:V4_)?([A-Za-z0-9_]+)["\']?\s*,\s*([^)\n;]+)\)',
        re.IGNORECASE
    )
    for m in env_pattern.finditer(clean_text):
        raw_name, raw_val = m.group(1), m.group(2)
        knob_name = clean_knob_name(raw_name)
        try:
            knobs[knob_name] = cast_knob_value(knob_name, raw_val)
        except ValueError:
            continue

    # Pattern 2: Struct member declarations e.g. int SPT = 1; double CHUNK_SMULT = 182.5;
    struct_pattern = re.compile(
        r'(?:int|double|float|int32_t|int64_t)\s+([A-Za-z0-9_]+)\s*=\s*([^;\n]+);'
    )
    for m in struct_pattern.finditer(clean_text):
        raw_name, raw_val = m.group(1), m.group(2)
        knob_name = clean_knob_name(raw_name)
        if knob_name in KNOWN_INT_KNOBS or knob_name in KNOWN_FLOAT_KNOBS:
            try:
                if knob_name not in knobs:
                    knobs[knob_name] = cast_knob_value(knob_name, raw_val)
            except ValueError:
                continue

    # Pattern 3: Direct assignment e.g. KN_SPT = 1; or KN_CHUNK_SMULT = 1255.29;
    assign_pattern = re.compile(
        r'(?:KN_|V4_)?([A-Za-z0-9_]+)\s*=\s*([^;\n]+);'
    )
    for m in assign_pattern.finditer(clean_text):
        raw_name, raw_val = m.group(1), m.group(2)
        knob_name = clean_knob_name(raw_name)
        if "env" not in raw_val and (knob_name in KNOWN_INT_KNOBS or knob_name in KNOWN_FLOAT_KNOBS):
            try:
                if knob_name not in knobs:
                    knobs[knob_name] = cast_knob_value(knob_name, raw_val)
            except ValueError:
                continue

    # Pattern 4: .env style lines e.g. export V4_SPT=1 or V4_CHUNK_SMULT=1255.29
    env_line_pattern = re.compile(
        r'^(?:export\s+)?(?:V4_)?([A-Za-z0-9_]+)\s*=\s*([^\s#\n;]+)',
        re.MULTILINE
    )
    for m in env_line_pattern.finditer(clean_text):
        raw_name, raw_val = m.group(1), m.group(2)
        knob_name = clean_knob_name(raw_name)
        if knob_name in KNOWN_INT_KNOBS or knob_name in KNOWN_FLOAT_KNOBS:
            try:
                if knob_name not in knobs:
                    knobs[knob_name] = cast_knob_value(knob_name, raw_val)
            except ValueError:
                continue

    return knobs


def build_json_payload(knobs: dict[str, Any], score: float = 0.0) -> dict[str, Any]:
    """Build the final JSON structure with score (always 0 by default) and knobs."""
    return {
        "score": int(score) if float(score).is_integer() else float(score),
        "knobs": knobs
    }


def parse_source_code_file_or_text(source_input: str) -> tuple[dict[str, Any], Optional[str]]:
    """
    Given a file path or raw string, extract knobs.
    Returns (knobs_dict, detected_filename_stem).
    """
    path = Path(source_input.strip('"\''))
    if path.is_file():
        content = path.read_text(encoding="utf-8", errors="replace")
        stem = path.stem
    else:
        content = source_input
        stem = None

    knobs = extract_knobs_from_text(content)
    return knobs, stem


def save_json(payload: dict[str, Any], output_path: Path) -> None:
    """Save the JSON payload to disk with 2-space indentation."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
    print(f"[OK] Successfully saved {len(payload['knobs'])} knob(s) to: {output_path}")


def interactive_mode():
    """Interactive CLI workflow when no arguments are provided."""
    print("=" * 70)
    print(" C++ Code -> Knobs JSON Converter (Score = 0)")
    print("=" * 70)
    print("Enter a C++ file path (e.g. Calibration/SampleCode.cpp)")
    print("OR paste your C++ code snippet below.")
    print("Type 'DONE' or press Ctrl+Z (Windows) / Ctrl+D (Linux) on a new line when finished:")
    print("-" * 70)

    lines = []
    try:
        while True:
            line = input()
            if line.strip() == "DONE":
                break
            lines.append(line)
    except EOFError:
        pass

    user_text = "\n".join(lines).strip()
    if not user_text:
        print("[!] No input provided. Exiting.")
        sys.exit(1)

    # Check if first line or whole text is a file path
    first_line = user_text.splitlines()[0].strip().strip('"\'')
    if Path(first_line).is_file():
        knobs, stem = parse_source_code_file_or_text(first_line)
        default_out = f"Calibration/{stem}.json"
    else:
        knobs, stem = parse_source_code_file_or_text(user_text)
        default_out = "Calibration/champion_extracted.json"

    if not knobs:
        print("[!] Warning: No knobs could be parsed from the provided input.")
        sys.exit(1)

    print(f"\n[+] Extracted {len(knobs)} knob(s):")
    for k, v in list(knobs.items())[:10]:
        print(f"    {k}: {v}")
    if len(knobs) > 10:
        print(f"    ... and {len(knobs) - 10} more.")

    out_input = input(f"\nEnter output JSON path [default: {default_out}]: ").strip()
    out_file = Path(out_input) if out_input else Path(default_out)

    payload = build_json_payload(knobs, score=0.0)
    save_json(payload, out_file)


def main():
    parser = argparse.ArgumentParser(
        description="Extract knobs from C++ code/files and output formatted JSON with score=0."
    )
    parser.add_argument(
        "input_files",
        nargs="*",
        help="Path to one or more C++ source files (e.g. Calibration/SampleCode.cpp)",
    )
    parser.add_argument(
        "-o", "--output",
        help="Path for output JSON file (default: Calibration/<input_stem>.json)",
    )
    parser.add_argument(
        "-t", "--text",
        help="Direct C++ code snippet string passed via command line",
    )
    parser.add_argument(
        "-s", "--score",
        type=float,
        default=0.0,
        help="Score value to include in JSON (default: 0)",
    )
    parser.add_argument(
        "--stdout",
        action="store_true",
        help="Print JSON to stdout instead of saving to a file",
    )

    args = parser.parse_args()

    # Case 1: Direct text flag
    if args.text:
        knobs = extract_knobs_from_text(args.text)
        payload = build_json_payload(knobs, score=args.score)
        if args.stdout:
            print(json.dumps(payload, indent=2))
        else:
            out_path = Path(args.output) if args.output else Path("Calibration/extracted.json")
            save_json(payload, out_path)
        return

    # Case 2: Pipe / stdin
    if not sys.stdin.isatty() and not args.input_files:
        stdin_content = sys.stdin.read()
        knobs = extract_knobs_from_text(stdin_content)
        payload = build_json_payload(knobs, score=args.score)
        if args.stdout or not args.output:
            if args.stdout:
                print(json.dumps(payload, indent=2))
            else:
                out_path = Path("Calibration/extracted.json")
                save_json(payload, out_path)
        else:
            save_json(payload, Path(args.output))
        return

    # Case 3: File paths provided
    if args.input_files:
        for file_arg in args.input_files:
            file_path = Path(file_arg)
            if not file_path.exists():
                print(f"[!] File not found: {file_path}", file=sys.stderr)
                continue

            knobs, stem = parse_source_code_file_or_text(str(file_path))
            payload = build_json_payload(knobs, score=args.score)

            if args.stdout:
                print(json.dumps(payload, indent=2))
            else:
                if args.output:
                    out_path = Path(args.output)
                else:
                    out_path = Path(f"Calibration/{stem}.json")
                save_json(payload, out_path)
        return

    # Case 4: No arguments -> Interactive mode
    interactive_mode()


if __name__ == "__main__":
    main()
