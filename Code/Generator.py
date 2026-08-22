"""Part 1 -- stratified synthetic test-case generator.

Emits JSON-lines; each object is self-contained enough for Simulator.py to play
interactor without touching disk again. Bounds follow the problem statement's
"Constraints"; the scoring parameters follow "Scoring".

WHY THE SCORING PARAMETERS ARE *COMPUTED*, NOT SAMPLED
------------------------------------------------------
The statement pins tp_base and dist_base to a concrete artefact: "tp_base comes
from a fixed one-request-at-a-time reference schedule" and dist_base is "the
reference scheduler's amount above the waiting-time targets". Drawing them from
thin air is what makes a synthetic benchmark useless for tuning: pick them too
low and every knob setting scores 1000, too high and everything scores 0 -- in
both cases the gradient the tuner needs is gone.

So each case is calibrated:
  * tp_base   = throughput of the reference schedule (Simulator.reference_schedule)
  * dist_base = that same reference's SLO excess, so dist_base == 0 happens
                exactly when the reference already meets both targets
  * tp_UB     = tp_base + u * (work-conservation upper bound - tp_base)
  * SLO1/SLO2 = multiples of the reference's own tdr/tpot
On the judge's public test #1 this reproduces the test's own tp_base to all
nine printed digits (see `python3 Simulator.py --validate-sample`).

Every real is stored already rounded to the 9 decimals the protocol prints, so
the value the scheduler reads is bit-identical to the value used for scoring.
"""
from __future__ import annotations

import argparse
import json
import math
import random
from typing import Any, Sequence

import Simulator

# ADDED: prefill_only, linear_scaling, and flash_crowd profiles
PROFILES = ("latency_stress", "throughput_stress", "mixed_load", "large_R",
            "small_K", "deep", "shallow", "prefill_only", "linear_scaling", "flash_crowd")
ALL_PROFILES = PROFILES + ("mixed",)

# Round-robining the profiles gives each a share of the objective.
# PROFILE_MIX["balanced"] is an explicit guess at a finals-like shape.
PROFILE_MIX: dict[str, dict[str, int]] = {
    "uniform": {p: 1 for p in PROFILES},
    "balanced": {"mixed_load": 5, "large_R": 3, "deep": 3, "latency_stress": 3,
                 "throughput_stress": 3, "small_K": 2, "shallow": 1,
                 "prefill_only": 2, "linear_scaling": 2, "flash_crowd": 2},
}


def profile_sequence(n_cases: int, mix: str = "balanced") -> list[str]:
    """A deterministic, interleaved profile order with the requested weights.

    Interleaved rather than blocked so that any *prefix* of the pool is
    representative -- the median pruner judges trials on a prefix, and a
    blocked order would judge them on one profile alone.
    """
    weights = PROFILE_MIX[mix]
    order = [p for p in PROFILES if weights.get(p, 0) > 0]
    out: list[str] = []
    credit = {p: 0.0 for p in order}
    total = float(sum(weights[p] for p in order))
    for _ in range(n_cases):
        for p in order:
            credit[p] += weights[p] / total
        pick = max(order, key=lambda p: (credit[p], -order.index(p)))
        credit[pick] -= 1.0
        out.append(pick)
    return out

# --- statement "Constraints" -------------------------------------------------
MAX_R = 2000
MAX_LIN = 4096
MAX_LOUT = 512
TOKEN_BUDGET = 200_000          # sum_i L_out[i] <= 2e5 per test
MAX_ARRIVAL = 1e9
CELL_LO, CELL_HI = 0.001, 1e4
SLO_LO, SLO_HI = 0.001, 1e9


def q9(x: float) -> float:
    """Round to the 9 decimals the protocol prints, so printed == internal."""
    return float(f"{x:.9f}")


def _loguniform(rng: random.Random, lo: float, hi: float) -> float:
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


def _clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


# ------------------------------------------------------------ system params --
def _sys_params(rng: random.Random, profile: str) -> dict[str, Any]:
    """System line 1: K S latency bandwidth bytes_per_token num_layers."""
    if profile == "small_K":
        K = 1 if rng.random() < 0.6 else 2
    elif profile == "throughput_stress":
        K = rng.randint(4, 8)
    else:
        K = rng.randint(1, 8)

    if profile == "latency_stress":               # link dominates compute
        lat = rng.uniform(10.0, 50.0)
        bw = rng.uniform(0.001, 0.05)
        bpt = rng.randint(10 ** 4, 10 ** 6)
    elif profile == "throughput_stress":          # compute dominates link
        lat = rng.uniform(0.001, 0.5)
        bw = rng.uniform(20.0, 100.0)
        bpt = rng.randint(1, 4096)
    else:
        lat = 10 ** rng.uniform(-3, 1.7)
        bw = 10 ** rng.uniform(-3, 2)
        bpt = int(10 ** rng.uniform(0, 6))

    if profile == "deep":
        layers = rng.randint(32, 64)
    elif profile == "shallow":
        layers = 1
    else:
        layers = rng.choice([1, 2, 4, 8, 16, 24, 32, 48, 64])

    return dict(K=K,
                S=_clamp(rng.uniform(1.0, 10.0), 1.0, 10.0),
                latency_in_ms=_clamp(lat, 0.001, 50.0),
                bandwidth_gbps=_clamp(bw, 0.001, 100.0),
                bytes_per_token=max(1, min(10 ** 6, bpt)),
                num_layers=max(1, min(64, layers)))


# ------------------------------------------------------------- time table ----
def _table(rng: random.Random, S: float, layers: int, profile: str) -> dict[str, Any]:
    """Task-Time Table: N>=2 distinct batch sizes, >=1 non-missing cell per
    column. Handles sub-linear (genuinely profitable batching) and linear_scaling
    (penalized batching)."""
    n = rng.choice([2, 3, 4, 6, 8, 12, 16, 24, 32, 64])
    caps = sorted(rng.sample(range(1, MAX_LIN + 1), n))
    if rng.random() < 0.5:                      # often list group size 1
        caps[0] = 1
    a_pre, a_proc, a_post = (10 ** rng.uniform(-2, 0.5) for _ in range(3))

    # PATCH: If linear scaling profile, make batching scale linearly or super-linearly
    if profile == "linear_scaling":
        exp = rng.uniform(1.0, 1.1)
        dexp = rng.uniform(1.0, 1.1)
    else:
        exp = rng.uniform(0.45, 0.95)               # sub-linear in batch size
        dexp = exp * rng.uniform(0.55, 0.85)        # decode scales even better

    rows: list[list[float]] = []
    for b in caps:
        scale = b ** exp
        dscale = b ** dexp

        def jit() -> float:
            return rng.uniform(0.85, 1.18)

        rows.append([float(b),
                     a_pre * scale * S * jit(),
                     a_proc * scale * S * layers * 0.05 * jit(),
                     a_post * scale * S * jit(),
                     a_pre * dscale * S * jit(),
                     a_proc * dscale * S * layers * 0.02 * jit(),
                     a_post * dscale * S * jit()])

    for r in rows:                              # clamp + quantize every cell
        for c in range(1, 7):
            r[c] = max(CELL_LO, q9(_clamp(r[c], CELL_LO, CELL_HI)))

    for col in range(1, 7):                     # -1 holes, keeping >=1 real cell
        keep = rng.randrange(n)
        for i in range(n):
            if i != keep and rng.random() < 0.12:
                rows[i][col] = -1.0

    rng.shuffle(rows)                           # "Rows are given in no guaranteed order"
    return dict(N=n, rows=rows)


# --------------------------------------------------------------- requests ----
def _requests(rng: random.Random, profile: str, token_budget: int, max_R: int,
              tbl: Simulator.Table, S: float) -> list[dict[str, Any]]:
    """Arrival stream: nondecreasing timestamps, hidden L_out, sum(L_out) capped."""
    if profile == "large_R" or profile == "flash_crowd":
        R = rng.randint(min(400, max_R), max_R)   # max_R may be below 400
    else:
        choices = [c for c in (1, 2, 5, 20, 60, 150, 400, 900) if c <= max_R]
        R = rng.choice(choices or [max_R])
    R = max(1, min(R, max_R))

    lout_cap = max(1, min(MAX_LOUT, token_budget // R))
    lens: list[tuple[int, int]] = []
    budget = token_budget
    for _ in range(R):
        if budget <= 0:
            break
        L_in = max(1, min(MAX_LIN, int(10 ** rng.uniform(0, 3.6))))

        # PATCH: If prefill_only, force L_out to 1 for all requests
        if profile == "prefill_only":
            L_out = 1
        else:
            L_out = max(1, min(lout_cap,
                               int(10 ** rng.uniform(0, math.log10(lout_cap) + 0.3))))

        L_out = min(L_out, budget)
        budget -= L_out
        lens.append((L_in, L_out))

    if not lens:
        lens = [(1, 1)]

    # Local-computer service rate
    mean_lin = sum(a for a, _ in lens) / len(lens)
    mean_lout = sum(b for _, b in lens) / len(lens)
    g = 8.0
    per_req = (2.0 * S + tbl.at(Simulator.C_PRE_PRE, mean_lin)
               + tbl.at(Simulator.C_PRE_POST, mean_lin)
               + mean_lout * (2.0 * S + tbl.at(Simulator.C_DEC_PRE, g)
                              + tbl.at(Simulator.C_DEC_POST, g)) / g)
    mu = 1.0 / max(per_req, 1e-9)

    if profile == "throughput_stress":
        load = _loguniform(rng, 1.5, 12.0)
    elif profile == "latency_stress":
        load = _loguniform(rng, 0.4, 3.0)
    else:
        load = _loguniform(rng, 0.5, 8.0)

    rate = max(mu * load, 1e-12)

    # PATCH: Force flash crowd pattern
    if profile == "flash_crowd":
        pattern = "flash"
    else:
        pattern = rng.choice(("poisson", "bursty", "uniform"))

    t = 0.0
    out: list[dict[str, Any]] = []
    for L_in, L_out in lens:
        if pattern == "flash":
            t = 0.0
        elif pattern == "poisson":
            t += rng.expovariate(rate)
        elif pattern == "uniform":
            t += 1.0 / rate
        else:                                   # bursty: bunches with idle gaps
            t += 0.0 if rng.random() < 0.7 else rng.expovariate(rate / 6.0)
        out.append(dict(rid=len(out), arrival=q9(min(t, MAX_ARRIVAL)),
                        L_in=L_in, L_out=L_out))
    return out


# -------------------------------------------------------------- calibration --
def _draw_calibration(rng: random.Random, profile: str) -> dict[str, Any]:
    """All random choices of the scoring line, drawn once and stored."""
    r = rng.random()
    if profile == "throughput_stress" or r < 0.25:
        w_tp = rng.uniform(0.85, 1.0)
    elif profile == "latency_stress" or r < 0.50:
        w_tp = rng.uniform(0.0, 0.15)
    else:
        w_tp = rng.uniform(0.2, 0.8)
    if rng.random() < 0.06:
        w_tp = 0.0 if rng.random() < 0.5 else 1.0

    if profile == "latency_stress":
        f1lo, f1hi = 0.0005, 0.3
    elif profile == "throughput_stress":
        f1lo, f1hi = 0.01, 2.0
    else:
        f1lo, f1hi = 0.001, 1.5

    return dict(
        w_tp=q9(_clamp(w_tp, 0.0, 1.0)),
        f1=_loguniform(rng, f1lo, f1hi),
        f2=_loguniform(rng, 0.8, 8.0),
        q=rng.uniform(0.15, 0.6) if profile == "throughput_stress"
        else rng.uniform(0.25, 0.95),
        m=_loguniform(rng, 0.8, 4.0),
        generous=rng.random() < 0.15,
        g1=_loguniform(rng, 1.05, 8.0),
        g2=_loguniform(rng, 1.5, 20.0),
        free1=rng.random() < 0.05,
        free2=rng.random() < 0.05,
    )


def _fallback_scales(case: dict, ref: dict, ideal: float) -> dict[str, float]:
    P = max(2.0, min(float(len(case["requests"])), 4.0 * case["K"]))
    return dict(tpot=max(ref["step"] * P, 1e-9), tp=max(ideal * 0.5, ref["tp"]))


def probe_scales(case: dict, binary: str = "./scheduler",
                 timeout_s: float = 120.0) -> dict[str, float] | None:
    res = Simulator.run_one(case, {}, timeout_s=timeout_s, binary=binary)
    if not res.ok or res.tp <= 0.0:
        return None
    tpot = res.mean_tpot if res.mean_tpot > 0 else None
    if tpot is None:
        return None
    return dict(tpot=tpot, tp=res.tp, tdr=res.mean_tdr)


def apply_calibration(case: dict, scales: dict[str, float]) -> dict:
    cal = case["_cal"]
    ref = Simulator.reference_schedule(case)
    ideal = Simulator.ideal_throughput(case)

    w_tp = cal["w_tp"]
    w_c = q9(1.0 - w_tp)

    generous = cal["generous"] or len(case["requests"]) <= 2
    if generous:
        SLO1 = ref["tdr"] * cal["g1"]
        SLO2 = scales["tpot"] * cal["g2"]
    else:
        SLO1 = ref["tdr"] * cal["f1"]
        SLO2 = scales["tpot"] * cal["f2"]
    if cal["free1"]:
        SLO1 = SLO_HI
    if cal["free2"]:
        SLO2 = SLO_HI
    SLO1 = _clamp(SLO1, SLO_LO, SLO_HI)
    SLO2 = _clamp(SLO2, SLO_LO, SLO_HI)

    ex_tdr = max(0.0, (ref["tdr"] - SLO1) / SLO1)
    ex_tpot = max(0.0, (ref["tpot"] - SLO2) / SLO2)
    dist_base = _clamp(math.sqrt(ex_tdr ** 2 + ex_tpot ** 2), 0.0, 1e9)
    if dist_base < 1e-6:
        dist_base = 0.0

    if dist_base == 0.0:
        SLO2 = max(SLO2, max(ref["step"], scales["tpot"]) * cal["g2"])
        SLO2 = _clamp(SLO2, SLO_LO, SLO_HI)
        SLO1 = max(SLO1, ref["tdr"] * 1.05)
        SLO1 = _clamp(SLO1, SLO_LO, SLO_HI)

    tp_base = max(0.0, ref["tp"])
    if scales.get("probed") and scales["tp"] > tp_base:
        tp_UB = tp_base + (scales["tp"] - tp_base) / cal["q"]
    else:
        tp_UB = max(scales["tp"] * cal["m"], tp_base * 1.05)
    tp_UB = _clamp(tp_UB, 1e-9, min(1e9, max(ideal * 3.0, tp_base * 1.05)))

    case.update(SLO1=q9(SLO1), SLO2=q9(SLO2), tp_UB=q9(tp_UB), tp_base=q9(tp_base),
                dist_base=q9(dist_base), w_tp=w_tp, w_c=w_c)
    if case["tp_UB"] <= case["tp_base"]:
        case["tp_UB"] = q9(case["tp_base"] + max(1e-9, 0.05 * case["tp_base"]))
    case["SLO1"] = _clamp(case["SLO1"], SLO_LO, SLO_HI)
    case["SLO2"] = _clamp(case["SLO2"], SLO_LO, SLO_HI)

    case["meta"] = dict(ref_tp=ref["tp"], ref_tdr=ref["tdr"], ref_tpot=ref["tpot"],
                        ref_step=ref["step"], ideal_tp=ideal, total_out=ref["total_out"],
                        scale_tpot=scales["tpot"], scale_tp=scales["tp"],
                        probed=bool(scales.get("probed", False)))
    return case


def calibrate(case: dict, probe: bool = False, binary: str = "./scheduler",
              timeout_s: float = 120.0, rounds: int = 2) -> dict:
    ref = Simulator.reference_schedule(case)
    ideal = Simulator.ideal_throughput(case)
    scales = _fallback_scales(case, ref, ideal)
    apply_calibration(case, scales)
    if probe:
        for _ in range(max(1, rounds)):
            measured = probe_scales(case, binary, timeout_s)
            if measured is None:
                break
            scales = dict(tpot=max(measured["tpot"], ref["step"]),
                          tp=max(measured["tp"], ref["tp"]), probed=True)
            apply_calibration(case, scales)
    check_case(case)
    return case


# ------------------------------------------------------------------ assembly --
def make_case(rng: random.Random, profile: str, token_budget: int = TOKEN_BUDGET,
              max_R: int = MAX_R, force: dict[str, Any] | None = None) -> dict:
    sysp = _sys_params(rng, profile)
    if force:
        sysp.update({k: v for k, v in force.items() if k in sysp})
    case: dict[str, Any] = dict(profile=profile, **sysp)
    case["S"] = q9(case["S"])
    case["latency_in_ms"] = max(0.001, q9(case["latency_in_ms"]))
    case["bandwidth_gbps"] = max(0.001, q9(case["bandwidth_gbps"]))

    # PATCH: Pass profile to _table for correct scaling behavior
    case["table"] = _table(rng, case["S"], case["num_layers"], profile)

    tbl = Simulator.Table(case["table"]["rows"])
    case["requests"] = force["requests"] if (force and "requests" in force) else \
        _requests(rng, profile, min(token_budget, TOKEN_BUDGET), min(max_R, MAX_R),
                  tbl, case["S"])
    case["_cal"] = _draw_calibration(rng, profile)
    calibrate(case, probe=False)
    if force:
        for k, v in force.items():
            if k != "requests" and k not in sysp:
                case[k] = v
        check_case(case)
    return case


def check_case(case: dict) -> None:
    c = case
    assert 1 <= c["K"] <= 8, c["K"]
    assert 1.0 <= c["S"] <= 10.0, c["S"]
    assert 0.001 <= c["latency_in_ms"] <= 50.0, c["latency_in_ms"]
    assert 0.001 <= c["bandwidth_gbps"] <= 100.0, c["bandwidth_gbps"]
    assert 1 <= c["bytes_per_token"] <= 10 ** 6, c["bytes_per_token"]
    assert 1 <= c["num_layers"] <= 64, c["num_layers"]
    assert 0.001 <= c["SLO1"] <= 1e9 and 0.001 <= c["SLO2"] <= 1e9
    assert 0.0 <= c["dist_base"] <= 1e9, c["dist_base"]
    assert 1e-9 <= c["tp_UB"] <= 1e9, c["tp_UB"]
    assert c["tp_base"] >= 0.0 and c["tp_UB"] > c["tp_base"], (c["tp_base"], c["tp_UB"])
    assert c["w_tp"] >= 0 and c["w_c"] >= 0
    assert abs(c["w_tp"] + c["w_c"] - 1.0) < 1e-9, (c["w_tp"], c["w_c"])

    t = c["table"]
    assert 2 <= t["N"] <= 4096 and len(t["rows"]) == t["N"]
    sizes = [int(r[0]) for r in t["rows"]]
    assert len(set(sizes)) == len(sizes), "batch sizes must be distinct"
    assert all(1 <= s <= 4096 for s in sizes)
    for col in range(1, 7):
        vals = [float(r[col]) for r in t["rows"] if float(r[col]) >= 0]
        assert vals, f"column {col} is entirely missing"
        assert all(CELL_LO <= v <= CELL_HI for v in vals), f"column {col} out of range"

    rs = c["requests"]
    assert 1 <= len(rs) <= MAX_R, len(rs)
    assert [r["rid"] for r in rs] == list(range(len(rs)))
    prev = -1.0
    for r in rs:
        assert 0.0 <= r["arrival"] <= MAX_ARRIVAL
        assert r["arrival"] >= prev, "arrivals must be nondecreasing"
        prev = r["arrival"]
        assert 1 <= r["L_in"] <= MAX_LIN
        assert 1 <= r["L_out"] <= MAX_LOUT
    assert sum(r["L_out"] for r in rs) <= TOKEN_BUDGET


def generate(n_cases: int, seed: int, profile: str = "mixed",
             token_budget: int = TOKEN_BUDGET, max_R: int = MAX_R,
             probe: bool = False, binary: str = "./scheduler",
             timeout_s: float = 120.0, workers: int = 1,
             probe_rounds: int = 2, mix: str = "balanced") -> list[dict]:
    rng = random.Random(seed)
    seq = (profile_sequence(n_cases, mix) if profile == "mixed"
           else [profile] * n_cases)
    cases = [make_case(rng, seq[i], token_budget, max_R) for i in range(n_cases)]
    return (_probe_pool(cases, binary, timeout_s, workers, probe_rounds)
            if probe else cases)


def _probe_pool(cases: list[dict], binary: str, timeout_s: float,
                workers: int, rounds: int = 2) -> list[dict]:
    if workers <= 1 or len(cases) <= 1:
        return [calibrate(c, True, binary, timeout_s, rounds) for c in cases]
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(max_workers=workers) as ex:
        futs = [ex.submit(calibrate, c, True, binary, timeout_s, rounds) for c in cases]
        return [f.result() for f in futs]


def edge_cases(seed: int = 1_234_567, token_budget: int = TOKEN_BUDGET,
               probe: bool = False, binary: str = "./scheduler",
               timeout_s: float = 120.0) -> list[dict]:
    rng = random.Random(seed)
    out: list[dict] = []

    # PATCH: Added the new profiles to the degenerate canary set
    specs: list[tuple[str, str, dict[str, Any], int]] = [
        ("edge_K1", "small_K", dict(K=1), MAX_R),
        ("edge_layers1", "shallow", dict(num_layers=1), MAX_R),
        ("edge_singleR", "mixed_load", {}, 1),
        ("edge_tp_only", "mixed_load", dict(w_tp=1.0, w_c=0.0), MAX_R),
        ("edge_wait_only", "mixed_load", dict(w_tp=0.0, w_c=1.0), MAX_R),
        ("edge_deep", "deep", {}, MAX_R),
        ("edge_prefill", "prefill_only", {}, MAX_R),
        ("edge_linear", "linear_scaling", {}, MAX_R),
        ("edge_flash", "flash_crowd", {}, MAX_R),
    ]
    for name, profile, force, max_R in specs:
        weights = {k: v for k, v in force.items() if k in ("w_tp", "w_c")}
        sysf = {k: v for k, v in force.items() if k not in ("w_tp", "w_c")}
        c = make_case(rng, profile, token_budget, max_R, sysf or None)
        if weights:
            c["_cal"]["w_tp"] = weights["w_tp"]
            calibrate(c, probe=False)
        if probe:
            calibrate(c, True, binary, timeout_s)
        c["profile"] = name
        check_case(c)
        out.append(c)
    return out


# ------------------------------------------------------------------ summary ---
def summarize(cases: Sequence[dict]) -> str:
    lines = [f"{'profile':<18} {'R':>5} {'tok':>7} {'K':>2} {'lay':>4} "
             f"{'tp_base':>11} {'tp_UB':>11} {'ratio':>6} {'SLO1':>10} {'SLO2':>10} "
             f"{'dist_base':>10} {'w_tp':>5}"]
    for c in cases:
        tok = sum(r["L_out"] for r in c["requests"])
        ratio = c["tp_UB"] / c["tp_base"] if c["tp_base"] > 0 else float("inf")
        lines.append(f'{c["profile"]:<18} {len(c["requests"]):5d} {tok:7d} {c["K"]:2d} '
                     f'{c["num_layers"]:4d} {c["tp_base"]:11.6g} {c["tp_UB"]:11.6g} '
                     f'{ratio:6.2f} {c["SLO1"]:10.4g} {c["SLO2"]:10.4g} '
                     f'{c["dist_base"]:10.4g} {c["w_tp"]:5.2f}')
    n0 = sum(1 for c in cases if c["dist_base"] == 0.0)
    lines.append(f"\n{len(cases)} cases, dist_base==0 in {n0} "
                 f"({100.0 * n0 / max(1, len(cases)):.0f}%), "
                 f"median tokens {sorted(sum(r['L_out'] for r in c['requests']) for c in cases)[len(cases) // 2]}")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n_cases", type=int, default=100)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--profile", choices=ALL_PROFILES, default="mixed")
    ap.add_argument("--out", default="cases.jsonl")
    ap.add_argument("--token_budget", type=int, default=TOKEN_BUDGET,
                    help="cap on sum(L_out) per case; lower it for fast tuning")
    ap.add_argument("--max_R", type=int, default=MAX_R)
    ap.add_argument("--edge_cases", action="store_true",
                    help="emit the degenerate canary set instead")
    ap.add_argument("--probe", action="store_true",
                    help="anchor the scoring line on a measured default-knob run")
    ap.add_argument("--binary", default="./scheduler")
    ap.add_argument("--timeout_s", type=float, default=120.0)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--profile_mix", choices=tuple(PROFILE_MIX), default="balanced")
    ap.add_argument("--summary", action="store_true")
    a = ap.parse_args()

    cases = (edge_cases(a.seed, a.token_budget, a.probe, a.binary, a.timeout_s)
             if a.edge_cases
             else generate(a.n_cases, a.seed, a.profile, a.token_budget, a.max_R,
                           a.probe, a.binary, a.timeout_s, a.workers,
                           mix=a.profile_mix))
    with open(a.out, "w") as f:
        for c in cases:
            f.write(json.dumps(c) + "\n")
    print(f"wrote {len(cases)} cases -> {a.out}")
    if a.summary:
        print(summarize(cases))


if __name__ == "__main__":
    main()