"""Single source of truth for the V4_* scheduler knobs and their search bounds.

Defaults mirror ``loadKnobs()`` in scheduler.cpp exactly -- ``verify_against_cpp()``
re-reads the C++ source and asserts that they still do, so a knob added on the
C++ side can never be silently missed by the tuner.

Bounds are *search seeds* for the tuner only; no tuned value is asserted here.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import Literal, Mapping

Kind = Literal["float", "logfloat", "int", "cat"]


@dataclass(frozen=True)
class Knob:
    name: str
    kind: Kind
    default: float | int
    low: float | int | None = None
    high: float | int | None = None
    choices: tuple[int, ...] | None = None
    doc: str = ""

    def __post_init__(self) -> None:
        if self.kind == "cat":
            if not self.choices:
                raise ValueError(f"{self.name}: categorical knob needs choices")
            if self.default not in self.choices:
                raise ValueError(f"{self.name}: default {self.default} not in {self.choices}")
        else:
            if self.low is None or self.high is None:
                raise ValueError(f"{self.name}: numeric knob needs low/high")
            if not (self.low <= self.high):
                raise ValueError(f"{self.name}: low > high")
            if not (self.low <= self.default <= self.high):
                raise ValueError(
                    f"{self.name}: default {self.default} outside [{self.low}, {self.high}]")
            if self.kind == "logfloat" and self.low <= 0:
                raise ValueError(f"{self.name}: logfloat needs low > 0")

    def cast(self, v: float | int) -> float | int:
        """Coerce an arbitrary value into this knob's own domain."""
        if self.kind in ("int", "cat"):
            iv = int(round(float(v)))
            if self.kind == "cat":
                assert self.choices is not None
                return min(self.choices, key=lambda c: (abs(c - iv), c))
            assert self.low is not None and self.high is not None
            return max(int(self.low), min(int(self.high), iv))
        assert self.low is not None and self.high is not None
        return max(float(self.low), min(float(self.high), float(v)))

    def to_env(self, v: float | int) -> str:
        c = self.cast(v)
        return str(int(c)) if self.kind in ("int", "cat") else f"{float(c):.10g}"


# ---------------------------------------------------------------- registry ---
# Order matches loadKnobs() in scheduler.cpp.
KNOBS: tuple[Knob, ...] = (
    Knob("V4_SPT", "cat", 1, choices=(0, 1),
         doc="shortest-prefill-first admission + SRPT at the remotes"),
    Knob("V4_CHUNK", "cat", 1, choices=(0, 1),
         doc="split P PROC into layer pieces when decode work is blocked behind it"),
    Knob("V4_CHUNK_SMULT", "logfloat", 20.0, 1.0, 400.0,
         doc="target piece duration, in multiples of S"),
    Knob("V4_HOLD", "cat", -1, choices=(-1, 0, 1),
         doc="hold a forming D PRE batch: -1 auto, 0 never, 1 always"),
    Knob("V4_WAVES", "float", 3.0, 1.0, 12.0,
         doc="target number of in-flight decode waves"),
    Knob("V4_HOLD_WFRAC", "float", 0.25, 0.0, 1.0,
         doc="hold cap as a fraction of SLO2"),
    Knob("V4_HOLD_SMULT", "logfloat", 10.0, 0.5, 200.0,
         doc="hold cap in multiples of S"),
    Knob("V4_UPGATE_FRAC", "float", 0.5, 0.0, 1.0,
         doc="uplink-backlog gate for admitting P PRE (latency mode)"),
    Knob("V4_UPPRE_MAX", "int", 3, 1, 16,
         doc="max in-flight prefill uploads (latency mode)"),
    Knob("V4_UPPRE_MAX_TP", "int", 6, 1, 32,
         doc="max in-flight prefill uploads (throughput mode)"),
    Knob("V4_RATE_EFF", "float", 0.7, 0.1, 1.0,
         doc="min fraction of the best rate a wave-capped batch may drop to"),
    Knob("V4_DECW", "float", 1.0, 0.0, 4.0,
         doc="decode-load weight in remote assignment"),
    Knob("V4_WAVES_PROC", "cat", 0, choices=(0, 1),
         doc="also wave-cap D PROC"),
    Knob("V4_LATHOLD", "cat", 1, choices=(0, 1),
         doc="lockstep hold + big waves when link latency dominates"),
    Knob("V4_LATFRAC", "float", 0.5, 0.05, 1.0,
         doc="fraction of active decodes to gather per latency wave"),
    Knob("V4_CONS", "cat", 1, choices=(0, 1),
         doc="consolidate assignments onto few remotes when latency dominates"),
    Knob("V4_CONS_PEN", "logfloat", 20.0, 1.0, 500.0,
         doc="spill penalty (multiples of 2*latency) for out-of-set remotes"),
    Knob("V4_CHUNK_MINS", "float", 30.0, 1.0, 200.0,
         doc="only chunk prefills longer than this many S"),
    Knob("V4_CHUNK_TPP", "float", 0.8, 0.0, 1.0,
         doc="minimum tpot pressure before chunking engages"),
)

BY_NAME: dict[str, Knob] = {k.name: k for k in KNOBS}
DEFAULTS: dict[str, float | int] = {k.name: k.default for k in KNOBS}

if len(BY_NAME) != len(KNOBS):
    raise RuntimeError("duplicate knob name in KNOBS")


def as_env(values: Mapping[str, float | int] | None = None) -> dict[str, str]:
    """Coerce a (possibly partial) knob dict into subprocess-ready env strings.

    Every knob is always emitted, so a child process can never inherit a stale
    V4_* value from the parent environment.
    """
    values = values or {}
    return {k.name: k.to_env(values.get(k.name, k.default)) for k in KNOBS}


DEFAULT_ENV: dict[str, str] = as_env()


def clamp(values: Mapping[str, float | int]) -> dict[str, float | int]:
    """Project an arbitrary dict onto the legal knob domain, dropping strays."""
    return {k.name: k.cast(values[k.name]) for k in KNOBS if k.name in values}


def strip_from_environ(env: Mapping[str, str] | None = None) -> dict[str, str]:
    """A copy of `env` with every V4_* key removed (used to build clean envs)."""
    src = os.environ if env is None else env
    return {k: v for k, v in src.items() if k not in BY_NAME}


# ------------------------------------------------------------------ env I/O ---
def write_env_file(values: Mapping[str, float | int], path: str) -> dict[str, str]:
    env = as_env(values)
    with open(path, "w") as f:
        f.write("# generated by Trainer.py -- source with: set -a; source this; set +a\n")
        for k in KNOBS:
            f.write(f"{k.name}={env[k.name]}\n")
    return env


def read_env_file(path: str) -> dict[str, float | int]:
    out: dict[str, float | int] = {}
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith("#") or "=" not in ln:
                continue
            name, _, raw = ln.partition("=")
            name, raw = name.strip(), raw.strip()
            if name in BY_NAME:
                out[name] = BY_NAME[name].cast(float(raw))
    return out


# -------------------------------------------------------------- cpp cross-check
_CPP_PAT = re.compile(r'env([di])\s*\(\s*"(V4_[A-Z0-9_]+)"\s*,\s*(-?[0-9.]+)\s*\)')


def parse_cpp_knobs(path: str = "scheduler.cpp") -> dict[str, tuple[str, float]]:
    """Extract {name: (envd|envi, default)} from the scheduler source."""
    with open(path) as f:
        src = f.read()
    return {m.group(2): (m.group(1), float(m.group(3))) for m in _CPP_PAT.finditer(src)}


def verify_against_cpp(path: str = "scheduler.cpp", strict: bool = True) -> list[str]:
    """Return a list of mismatches between this registry and scheduler.cpp."""
    problems: list[str] = []
    try:
        cpp = parse_cpp_knobs(path)
    except OSError as exc:
        return [f"cannot read {path}: {exc}"]

    for name in cpp.keys() - BY_NAME.keys():
        problems.append(f"{name}: read by scheduler.cpp but missing from KNOBS")
    for name in BY_NAME.keys() - cpp.keys():
        problems.append(f"{name}: in KNOBS but never read by scheduler.cpp")
    for name in cpp.keys() & BY_NAME.keys():
        fn, dflt = cpp[name]
        knob = BY_NAME[name]
        want_int = knob.kind in ("int", "cat")
        if want_int != (fn == "i"):
            problems.append(
                f"{name}: kind={knob.kind} but scheduler.cpp uses env{fn}")
        if abs(float(knob.default) - dflt) > 1e-12:
            problems.append(
                f"{name}: default {knob.default} != scheduler.cpp default {dflt}")
    if problems and strict:
        raise AssertionError("knob registry out of sync with scheduler.cpp:\n  "
                             + "\n  ".join(problems))
    return problems


def describe(values: Mapping[str, float | int] | None = None) -> str:
    env = as_env(values)
    w = max(len(k.name) for k in KNOBS)
    return "\n".join(
        f"{k.name:<{w}} = {env[k.name]:>10}   ({k.doc})" for k in KNOBS)


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cpp", default="scheduler.cpp")
    ap.add_argument("--env-file", help="print the knob table for a saved .env")
    a = ap.parse_args()

    vals = read_env_file(a.env_file) if a.env_file else None
    print(describe(vals))
    print()
    if os.path.exists(a.cpp):
        bad = verify_against_cpp(a.cpp, strict=False)
        print("cpp cross-check: OK" if not bad else "cpp cross-check FAILED:")
        for b in bad:
            print("  -", b)
    else:
        print(f"cpp cross-check skipped ({a.cpp} not found)")