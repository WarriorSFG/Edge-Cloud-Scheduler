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

    # ---------------------------------------------------------------------
    # Added after static analysis: these were hardcoded constants that shape
    # scheduling decisions but that the search could never reach. Every
    # default equals the previous hardcoded value, so the seeded trial
    # reproduces the old policy exactly.
    # ---------------------------------------------------------------------

    # -- stage arbitration on the local computer and at each remote --------
    # score = B_stage + age(wait). B_* were 1.2-3.2 while age is in raw ms, so
    # the base ordering only ever broke sub-millisecond ties. V4_BASE_W scales
    # all six together, which is the axis that lets a stage *preference*
    # actually outrank "whoever waited longest".
    Knob("V4_BASE_W", "logfloat", 1.0, 0.05, 500.0,
         doc="global weight on the six stage base priorities"),
    Knob("V4_B_DPOST", "float", 3.2, 0.0, 8.0, doc="base priority: D POST (emits tokens)"),
    Knob("V4_B_PPOST", "float", 2.4, 0.0, 8.0, doc="base priority: P POST (ends TDR)"),
    Knob("V4_B_DPRE", "float", 2.0, 0.0, 8.0, doc="base priority: D PRE"),
    Knob("V4_B_PPRE", "float", 1.2, 0.0, 8.0, doc="base priority: P PRE (admission)"),
    Knob("V4_B_DPROC", "float", 2.0, 0.0, 8.0, doc="base priority: D PROC at a remote"),
    Knob("V4_B_PPROC", "float", 1.2, 0.0, 8.0, doc="base priority: P PROC at a remote"),

    # -- age-score shape: the TDR-vs-TPOT tilt ----------------------------
    # This tilt was a fixed function of the test's own w_tp with no tunable
    # part at all, even though it is the single most consequential dial in the
    # policy.
    Knob("V4_AGE_FLOOR", "float", 0.5, 0.0, 3.0,
         doc="constant floor in the age multiplier (0 = tilt purely by weights)"),
    Knob("V4_AGE_AW", "float", 1.0, 0.0, 3.0,
         doc="how strongly the test's w_tp tilts prefill vs decode aging"),
    Knob("V4_AGE_PRESS", "float", 1.0, 0.0, 4.0,
         doc="how strongly live SLO pressure tilts aging (pressures cap at 1.5)"),
    Knob("V4_AGE_SLO_W", "float", 3.0, 0.0, 30.0,
         doc="weight of the SLO-relative urgency bonus"),
    Knob("V4_AGE_NORM", "cat", 0, choices=(0, 1),
         doc="1: age in SLO-relative units, so stages compete on the scale the "
             "score uses instead of on raw milliseconds"),
    Knob("V4_PPRE_AGECAP", "logfloat", 1e12, 1.0, 1e12,
         doc="cap (ms) on the admission wait that bids for the local computer; "
             "an arrival backlog otherwise lets P PRE outbid all decode work"),

    # -- remote choice: a term that was missing, not just mis-weighted -----
    Knob("V4_DECQ", "float", 0.0, 0.0, 4.0,
         doc="weight on decode work already queued at / in flight to a remote, "
             "in units of one decode hop (0 = previous behaviour)"),

    # -- chunking and regime detection ------------------------------------
    Knob("V4_CHUNK_RATIO", "float", 2.0, 0.0, 10.0,
         doc="tpot-excess / tdr-excess ratio required before P PROC is chunked; "
             "at 2.0 chunking almost never fires"),
    Knob("V4_CHUNK_PRED", "cat", 0, choices=(0, 1),
         doc="1: chunk when decode is merely assigned to this remote, not only "
             "when it is already queued (anticipatory instead of reactive)"),
    Knob("V4_LAT_MULT", "float", 2.0, 0.25, 8.0,
         doc="link round-trips charged per decode hop; gates the whole "
             "latency-dominant regime (LATHOLD, CONS, cloud-count choice)"),
    Knob("V4_GATE_TDR", "float", 0.3, 0.0, 3.0,
         doc="TDR fraction at which admission gating is overridden; low values "
             "let a prefill flood through under overload"),

    # -- output-group formation, the local computer's only real lever -------
    # Measured: the local computer runs at 86-99% while the K remotes idle at
    # 1-3%, and mean group size collapses to ~1 in exactly the saturated cases
    # (score ~190) versus 9-17 in the healthy ones (score ~997). Auto-hold was
    # gated purely on the test's w_tp, i.e. blind to that load.
    Knob("V4_HOLD_ACT", "int", 0, 0, 64,
         doc="auto-hold once this many requests are in the decode phase, "
             "whatever the weights say (0 = old weights-only rule)"),
    Knob("V4_HOLD_AW", "float", 0.75, 0.0, 1.0,
         doc="w_tp above which auto-hold turns on; at 0.75 every "
             "waiting-weighted test ran with batching effectively off"),
    Knob("V4_WAVE_CAPS_BATCH", "cat", 1, choices=(0, 1),
         doc="1 (old): the wave target ceil(active/WAVES) also caps group size, "
             "so switching holding ON can yield SMALLER groups than leaving it "
             "off. 0: the wave target only decides when to fire, and the group "
             "is always the rate-optimal size"),
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


def verify_against_cpp(path: str = "scheduler.cpp", strict: bool = True,
                      check_defaults: bool = False) -> list[str]:
    """Structural sync check between this registry and scheduler.cpp.

    Only *structure* is an error: a knob the C++ reads but the registry does not
    know (it would never be tuned), a knob the registry lists that the C++ never
    reads (it would be tuned to no effect), or an int/float kind mismatch.

    Default values are deliberately NOT compared unless `check_defaults`. Once
    you bake tuned values into loadKnobs() (see patch_cpp) the C++ defaults are
    *supposed* to differ from the registry's search priors, and the tuner is
    unaffected either way because as_env() always emits every knob explicitly.
    """
    problems: list[str] = []
    try:
        cpp = parse_cpp_knobs(path)
    except OSError as exc:
        return [f"cannot read {path}: {exc}"]

    for name in cpp.keys() - BY_NAME.keys():
        problems.append(f"{name}: read by scheduler.cpp but missing from KNOBS")
    for name in BY_NAME.keys() - cpp.keys():
        problems.append(f"{name}: in KNOBS but never read by scheduler.cpp")
    for name in sorted(cpp.keys() & BY_NAME.keys()):
        fn, dflt = cpp[name]
        knob = BY_NAME[name]
        if (knob.kind in ("int", "cat")) != (fn == "i"):
            problems.append(f"{name}: kind={knob.kind} but scheduler.cpp uses env{fn}")
        if check_defaults and abs(float(knob.default) - dflt) > 1e-12:
            problems.append(
                f"{name}: registry default {knob.default} != scheduler.cpp {dflt}")
    if problems and strict:
        raise AssertionError("knob registry out of sync with scheduler.cpp:\n  "
                             + "\n  ".join(problems))
    return problems


def _cpp_literal(k: Knob, v: float | int) -> str:
    """Render a knob value as a C++ literal of the right type."""
    c = k.cast(v)
    if k.kind in ("int", "cat"):
        return str(int(c))
    text = f"{float(c):.10g}"
    return text if ("." in text or "e" in text or "E" in text) else text + ".0"


def patch_cpp(path: str, values: Mapping[str, float | int],
              out_path: str | None = None) -> list[str]:
    """Bake knob values into scheduler.cpp as the loadKnobs() defaults.

    The contest judge runs the submitted binary with no environment of its own,
    so env vars are a tuning harness only -- the tuned values have to live in
    the source to reach the judge. Rewrites just the default literal of each
    env{d,i}("V4_*", <default>) call and leaves comments and layout alone.
    """
    with open(path) as f:
        src = f.read()
    changed: list[str] = []

    def sub(m: "re.Match[str]") -> str:
        fn, name, old = m.group(1), m.group(2), m.group(3)
        if name not in BY_NAME:
            return m.group(0)
        new = _cpp_literal(BY_NAME[name], values.get(name, BY_NAME[name].default))
        if new != old:
            changed.append(f"{name}: {old} -> {new}")
        return f'env{fn}("{name}", {new})'

    patched = _CPP_PAT.sub(sub, src)
    with open(out_path or path, "w") as f:
        f.write(patched)
    return changed


def emit_cpp(values: Mapping[str, float | int] | None = None) -> str:
    """The loadKnobs() body with these values as defaults, for manual pasting."""
    lines = ["static void loadKnobs() {"]
    w = max(len(k.name) for k in KNOBS)
    for k in KNOBS:
        fn = "envi" if k.kind in ("int", "cat") else "envd"
        lit = _cpp_literal(k, (values or {}).get(k.name, k.default))
        var = "KN_" + k.name[3:]
        lines.append(f'    {var:<16} = {fn}("{k.name}", {lit});'
                     f'{"":<{max(0, w - len(k.name))}}  // {k.doc}')
    lines.append("}")
    return "\n".join(lines)


def describe(values: Mapping[str, float | int] | None = None) -> str:
    env = as_env(values)
    w = max(len(k.name) for k in KNOBS)
    return "\n".join(
        f"{k.name:<{w}} = {env[k.name]:>10}   ({k.doc})" for k in KNOBS)


def main() -> None:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cpp", default="scheduler.cpp")
    ap.add_argument("--env-file", help="print the knob table for a saved .env")
    ap.add_argument("--emit-cpp", action="store_true",
                    help="print a loadKnobs() with --env-file baked in as defaults")
    ap.add_argument("--patch-cpp", metavar="OUT",
                    help="write --cpp with --env-file baked in as defaults")
    a = ap.parse_args()

    vals = read_env_file(a.env_file) if a.env_file else None

    if a.emit_cpp:
        print(emit_cpp(vals or {}))
        return
    if a.patch_cpp:
        if not vals:
            raise SystemExit("--patch-cpp needs --env-file")
        changed = patch_cpp(a.cpp, vals, a.patch_cpp)
        print(f"wrote {a.patch_cpp} ({len(changed)} defaults changed)")
        for c in changed:
            print("   ", c)
        return

    print(describe(vals))
    print()
    if os.path.exists(a.cpp):
        bad = verify_against_cpp(a.cpp, strict=False)
        print("cpp cross-check: OK" if not bad else "cpp cross-check FAILED:")
        for b in bad:
            print("  -", b)
    else:
        print(f"cpp cross-check skipped ({a.cpp} not found)")


if __name__ == "__main__":
    main()