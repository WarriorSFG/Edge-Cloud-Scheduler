#!/usr/bin/env python3
"""
extract_knobs.py — Alias / wrapper for cpp_to_json.py
"""
import sys
from pathlib import Path

# Add current directory to path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from cpp_to_json import main

if __name__ == "__main__":
    main()
