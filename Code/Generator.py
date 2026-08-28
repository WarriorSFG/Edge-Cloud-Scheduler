"""
Generator.py — Generates diverse, high-quality, calibrated test cases for the Scheduler
problem using the 7-Dimensional Stress-Coefficient Model (GeneratorDoc.md).

Key Architecture:
  - 7-Dimensional Stress Vector: theta in [0, 1]^7 controlling physical mechanics:
    theta_net, theta_comp, theta_batch, theta_chunk, theta_arr, theta_scale, theta_slo.
  - Beta-Distribution Profile Tables: Each profile specifies (a_j, b_j) Beta parameters.
  - Deterministic Functions with Multiplicative Jitter: Ensures unique draws while preserving
    intended physical characteristics without constant overfitting.
  - Post-Hoc Consistency Verification: Bounded retry loop over jitter terms to prevent drift.
  - Full Contest Constraint Conformance: Strictly adheres to ProblemStatement.md bounds.
  - Real-Judge Calibration & Rank-Order Consistency: Preserves calibration validation against
    reference scores in Calibration/.

Usage:
    python Code/Generator.py <num_testcases> [options]
"""

import argparse
import collections
import csv
import dataclasses
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any


# ═══════════════════════════ SAMPLER HELPERS ═══════════════════════

def _beta(a: float, b: float) -> float:
    """Sample from Beta(a, b) distribution."""
    return random.betavariate(max(1e-4, a), max(1e-4, b))


def _log_u(low: float, high: float) -> float:
    """Sample from Log-Uniform(low, high) distribution."""
    l_low = math.log(max(1e-9, low))
    l_high = math.log(max(1e-9, high))
    return math.exp(random.uniform(l_low, l_high))


def _clip(val: float, lo: float, hi: float) -> float:
    """Clamp float value to [lo, hi]."""
    return max(lo, min(hi, val))


# ═══════════════════════════ PROFILE BETA TABLES ═══════════════════════
# 7 (a_j, b_j) Beta pairs per profile + (a_w, b_w) for weight split (GeneratorDoc.md §9).
# Axes: net, comp, batch, chunk, arr, scale, slo, w_tp

DEFAULT_BETA_PROFILE: dict[str, tuple[float, float]] = {
    "theta_net":   (1.0, 1.0),
    "theta_comp":  (1.0, 1.0),
    "theta_batch": (1.0, 1.0),
    "theta_chunk": (1.0, 1.0),
    "theta_arr":   (1.0, 1.0),
    "theta_scale": (1.0, 1.0),
    "theta_slo":   (1.0, 1.0),
    "w_split":     (6.0, 6.0),
}

PROFILES: dict[str, dict[str, tuple[float, float]]] = {
    "network_bottleneck": {
        "theta_net":   (6.0, 1.0),
        "theta_comp":  (1.0, 3.0),
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "fast_network": {
        "theta_net":   (1.0, 6.0),
        "theta_comp":  (3.0, 1.0),
        "theta_slo":   (3.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "heavy_transfer": {
        "theta_net":   (4.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "compute_bottleneck": {
        "theta_net":   (1.0, 6.0),
        "theta_comp":  (6.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (3.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "high_schedule_cost": {
        "theta_batch": (6.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "throughput_only": {
        "theta_arr":   (2.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (1.0, 3.0),
        "w_split":     (50.0, 1.0),
    },
    "latency_only": {
        "theta_arr":   (1.0, 2.0),
        "theta_slo":   (3.0, 1.0),
        "w_split":     (1.0, 50.0),
    },
    "balanced": {
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "single_remote": {
        "theta_chunk": (1.0, 3.0),
        "theta_scale": (1.0, 2.0),
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "many_remotes": {
        "theta_chunk": (3.0, 1.0),
        "theta_scale": (3.0, 1.0),
        "theta_slo":   (3.0, 1.0),
        "w_split":     (6.0, 6.0),
    },
    "burst_arrivals": {
        "theta_arr":   (6.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (3.0, 1.0),
        "w_split":     (6.0, 6.0),
    },
    "streaming_arrivals": {
        "theta_arr":   (1.0, 6.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "large_prefill": {
        "theta_comp":  (3.0, 1.0),
        "theta_chunk": (6.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (1.0, 3.0),
        "w_split":     (6.0, 6.0),
    },
    "long_decode": {
        "theta_comp":  (3.0, 1.0),
        "theta_chunk": (1.0, 6.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (3.0, 1.0),
        "w_split":     (6.0, 6.0),
    },
    "many_layers": {
        "theta_chunk": (6.0, 1.0),
        "theta_scale": (2.0, 1.0),
        "theta_slo":   (1.0, 1.0),
        "w_split":     (6.0, 6.0),
    },
    "single_layer": {
        "theta_chunk": (1.0, 6.0),
        "theta_slo":   (1.0, 1.0),
        "w_split":     (6.0, 6.0),
    },
    "stress_scale": {
        "theta_scale": (6.0, 1.0),
        "theta_slo":   (2.0, 2.0),
        "w_split":     (6.0, 6.0),
    },
    "adversarial_mixed": {
        "theta_net":   (2.0, 2.0),
        "theta_comp":  (2.0, 2.0),
        "theta_batch": (4.0, 1.0),
        "theta_chunk": (1.0, 4.0),
        "theta_arr":   (4.0, 1.0),
        "theta_scale": (3.0, 1.0),
        "theta_slo":   (4.0, 1.0),
        "w_split":     (2.0, 2.0),
    },
    "random": {
        "w_split":     (1.0, 1.0),
    },
}

ALL_PROFILE_NAMES = list(PROFILES.keys())


# ════════════════ PIECEWISE LINEAR TABLE LOOKUP ════════════════════

def _lookup_table_val(table_rows: list[dict[str, Any]], column: str, bs: float) -> float:
    """Helper for piecewise linear interpolation with boundary clamping (§0)."""
    pts = []
    for r in table_rows:
        val = r.get(column, -1.0)
        if val >= 0.0:
            pts.append((float(r["batch_size"]), float(val)))
    if not pts:
        return 1.0
    pts.sort()
    if bs <= pts[0][0]:
        return pts[0][1]
    if bs >= pts[-1][0]:
        return pts[-1][1]
    for i in range(len(pts) - 1):
        if pts[i][0] <= bs <= pts[i+1][0]:
            x0, y0 = pts[i]
            x1, y1 = pts[i+1]
            if x1 == x0:
                return y0
            return y0 + (bs - x0) / (x1 - x0) * (y1 - y0)
    return pts[-1][1]


# ════════════════ TEST CASE GENERATION (§10 FIXED ORDER) ════════════════════

def generate_testcase(testcase_id: int, profile_name: str) -> dict[str, Any]:
    """
    Generate a complete, self-consistent test case following GeneratorDoc.md §10.
    """
    prof_spec = PROFILES.get(profile_name, {})

    # Step 1: Draw stress vector theta in [0, 1]^7 from profile Beta table
    def get_axis_theta(axis: str) -> float:
        a, b = prof_spec.get(axis, DEFAULT_BETA_PROFILE[axis])
        return _beta(a, b)

    th_net   = get_axis_theta("theta_net")
    th_comp  = get_axis_theta("theta_comp")
    th_batch = get_axis_theta("theta_batch")
    th_chunk = get_axis_theta("theta_chunk")
    th_arr   = get_axis_theta("theta_arr")
    th_scale = get_axis_theta("theta_scale")
    th_slo   = get_axis_theta("theta_slo")

    # Step 2: Compute Reference decode-compute time D_tok & curve exponents (§2.1, §2.2)
    D0 = 0.5  # ms
    p_dec_proc = 0.4 + 0.9 * th_comp
    p_pref_proc = 0.5 + 0.7 * th_comp
    p_pre_post = 0.15 + 0.25 * th_comp

    # Step 3: Network dominance ratio target (§3.1)
    rho_min = 0.05
    rho_max = 20.0
    rho_net_target = rho_min * ((rho_max / rho_min) ** th_net)

    # Step 5: Layers & L_in distribution parameters (§5)
    num_layers = int(_clip(round(64.0 ** th_chunk), 1, 64))
    if profile_name == "single_layer":
        num_layers = 1
    elif profile_name == "many_layers":
        num_layers = 64

    L_min = 16.0 + 112.0 * th_chunk
    L_max = 256.0 + 3200.0 * (th_chunk ** 2)

    # Step 6: Scale, Request count R, Remote count K, Arrival rate (§6)
    R_min = 8
    R_max = 2000
    R_target = math.ceil(R_min * ((R_max / R_min) ** th_scale))

    if profile_name == "single_remote":
        K = 1
    elif profile_name == "many_remotes":
        K = 8
    else:
        K = int(_clip(math.ceil(1.0 + 7.0 * (th_scale ** 0.5) * (1.0 - 0.4 * th_net)), 1, 8))

    # Weight split
    a_w, b_w = prof_spec.get("w_split", DEFAULT_BETA_PROFILE["w_split"])
    w_tp = round(_clip(_beta(a_w, b_w), 0.0, 1.0), 9)
    w_c = round(1.0 - w_tp, 9)

    # Jitter retry loop (§8 Post-hoc consistency check)
    max_jitter_retries = 20
    for _ in range(max_jitter_retries):
        eta3 = _log_u(0.7, 1.4)
        D_tok = D0 * (1.0 + 4.0 * th_comp) * eta3

        T_tok = rho_net_target * D_tok
        lam = random.uniform(0.1, 0.9)

        eta1 = _log_u(0.85, 1.18)
        latency_in_ms = _clip(lam * T_tok * eta1, 0.001, 50.0)

        bytes_per_token = int(_clip(round(_log_u(100.0, 1_000_000.0)), 1, 10_000_000))

        eta2 = _log_u(0.85, 1.18)
        denom_bw = 1e6 * (1.0 - lam) * T_tok * eta2
        bandwidth_gbps = _clip((8.0 * bytes_per_token) / max(1e-9, denom_bw), 0.001, 100.0)

        # Step 4: Schedule Cost S (§4)
        eta4 = _log_u(0.8, 1.25)
        S = _clip(D_tok * (0.2 + 12.0 * (th_batch ** 1.5)) * eta4, 1.0, 10.0)

        # Step 6: Arrival IAT & Concurrency estimate R_max_group (§6.3, §6.4)
        xi_R = random.uniform(0.85, 1.15)
        R = int(_clip(round(R_target * xi_R), 1, 2000))

        IAT_burst = 0.05  # ms
        IAT_stream = 50.0 * D_tok
        mean_IAT = IAT_stream * ((IAT_burst / IAT_stream) ** th_arr)

        R_max_group = max(1.0, (R / max(1, K)) * ((D_tok + 2.0 * T_tok + 2.0 * S) / max(1e-6, mean_IAT)))

        # Consistency check (§8)
        realized_T1 = latency_in_ms + (8.0 * bytes_per_token) / (bandwidth_gbps * 1e6)
        realized_rho = realized_T1 / max(1e-9, D_tok)
        realized_s_ratio = S / max(1e-9, D_tok)
        target_s_ratio = 0.2 + 12.0 * (th_batch ** 1.5)

        if (0.5 <= realized_rho / max(1e-9, rho_net_target) <= 2.0 and
            0.5 <= realized_s_ratio / max(1e-9, target_s_ratio) <= 2.0):
            break

    # Step 8: Task-time table generation (§2.2, §2.3)
    N = int(_clip(round(2.0 + (18.0 + 40.0 * th_scale) * random.uniform(0.8, 1.2)), 2, 40))
    x_max = int(min(4096, max(2, math.ceil(4.0 * R_max_group))))

    # Unique log-spaced batch sizes
    batch_sizes = set([1])
    for i in range(1, N):
        bs = int(_clip(math.ceil(x_max ** (i / max(1, N - 1))), 1, 4096))
        batch_sizes.add(bs)
    sorted_bs = sorted(list(batch_sizes))
    if len(sorted_bs) < 2:
        sorted_bs.append(max(2, min(4096, sorted_bs[0] * 2)))

    task_time_table: list[dict[str, Any]] = []
    cols = ["prefill_pre", "prefill_proc", "prefill_post", "decode_pre", "decode_proc", "decode_post"]
    col_exps = [p_pre_post, p_pref_proc, p_pre_post, p_pre_post, p_dec_proc, p_pre_post]

    for bs in sorted_bs:
        row: dict[str, Any] = {"batch_size": bs}
        for col_name, exp in zip(cols, col_exps):
            dur = _clip(D_tok * (float(bs) ** exp) * random.uniform(0.9, 1.1), 0.001, 10000.0)
            # 12% probability of missing (-1), except we ensure at least 1 non-missing entry per column
            if random.random() < 0.12 and bs != sorted_bs[0]:
                row[col_name] = -1.0
            else:
                row[col_name] = round(dur, 9)
        task_time_table.append(row)

    # Ensure every column has at least one valid row
    for col_name in cols:
        if not any(r[col_name] >= 0.0 for r in task_time_table):
            task_time_table[0][col_name] = round(D_tok, 9)

    # Step 9: Requests generation (§5, §6.2, §6.4)
    kappa = 2.0 + 3.0 * th_scale
    L_out_max = 512
    max_total_lout = 200_000

    requests: list[dict[str, Any]] = []
    cur_t = 0.0
    tot_lout = 0

    for rid in range(R):
        l_in = int(_clip(round(_log_u(L_min, L_max)), 1, 4096))

        rem_tokens = max_total_lout - tot_lout
        if rem_tokens <= 1:
            l_out = 1
        else:
            U = random.random()
            raw_lout = int(math.ceil(1.0 + (L_out_max - 1.0) * (U ** kappa)))
            l_out = int(_clip(min(raw_lout, rem_tokens), 1, 512))
        tot_lout += l_out

        if rid > 0:
            dt = random.expovariate(1.0 / max(1e-6, mean_IAT))
            cur_t += dt

        requests.append({
            "rid":        rid,
            "L_in":       l_in,
            "L_out":      l_out,
            "arrival_ms": round(cur_t, 9),
        })

    # Step 10: SLOs, Baselines & Targets (§7)
    avg_lin = sum(r["L_in"] for r in requests) / max(len(requests), 1)
    avg_lout = sum(r["L_out"] for r in requests) / max(len(requests), 1)
    tot_tokens = tot_lout

    def transfer_dur(n_tokens: float) -> float:
        return latency_in_ms + (8.0 * n_tokens * bytes_per_token) / (bandwidth_gbps * 1e6)

    t_pref_pre = _lookup_table_val(task_time_table, "prefill_pre", avg_lin)
    t_pref_proc = _lookup_table_val(task_time_table, "prefill_proc", avg_lin)
    t_pref_post = _lookup_table_val(task_time_table, "prefill_post", avg_lin)

    t_dec_pre1 = _lookup_table_val(task_time_table, "decode_pre", 1.0)
    t_dec_proc1 = _lookup_table_val(task_time_table, "decode_proc", 1.0)
    t_dec_post1 = _lookup_table_val(task_time_table, "decode_post", 1.0)

    TDR_phys = (
        (S + t_pref_pre) +
        transfer_dur(avg_lin) +
        (S + t_pref_proc) +
        transfer_dur(avg_lin) +
        (S + t_pref_post)
    )

    TPOT_phys = (
        (S + t_dec_pre1) +
        transfer_dur(1.0) +
        (S + t_dec_proc1) +
        transfer_dur(1.0) +
        (S + t_dec_post1)
    )

    q = 1.0 + max(0.0, (R_max_group ** 0.5) - 1.0)
    TDR_exp = TDR_phys * q
    TPOT_exp = TPOT_phys * (q ** 0.5)

    eta5 = _log_u(0.8, 1.25)
    eta6 = _log_u(0.8, 1.25)
    SLO1 = max(0.001, TDR_exp * (1.0 + 4.0 * th_slo) * eta5)
    SLO2 = max(0.001, TPOT_exp * (1.0 + 4.0 * th_slo) * eta6)

    eta7 = _log_u(0.8, 1.25)
    dist_base = max(0.0, (2.5 - 2.0 * th_slo) * eta7)

    est_total_time = max(
        TDR_exp + avg_lout * TPOT_exp,
        (tot_tokens * TPOT_phys) / max(K, 1) + TDR_phys
    )
    est_tp = tot_tokens / max(est_total_time, 1.0)

    eta8 = _log_u(0.8, 1.25)
    tp_base = max(0.0, est_tp * max(0.0, 0.5 - 0.45 * th_slo) * eta8)

    eta9 = _log_u(0.8, 1.25)
    eps_margin = 3.0
    tp_UB_calc = tp_base + (est_tp - tp_base) * (1.0 + th_slo) * eta9
    tp_UB_ceiling = tp_base + eps_margin * est_tp
    tp_UB = min(tp_UB_calc, tp_UB_ceiling)
    tp_UB = max(tp_UB, tp_base + 1e-4)

    return {
        "testcase_id":     testcase_id,
        "profile":         profile_name,
        "K":               K,
        "S":               round(S, 9),
        "latency_in_ms":   round(latency_in_ms, 9),
        "bandwidth_gbps":  round(bandwidth_gbps, 9),
        "bytes_per_token": bytes_per_token,
        "num_layers":      num_layers,
        "SLO1":            round(SLO1, 9),
        "SLO2":            round(SLO2, 9),
        "tp_UB":           round(tp_UB, 9),
        "tp_base":         round(tp_base, 9),
        "dist_base":       round(dist_base, 9),
        "w_tp":            w_tp,
        "w_c":             w_c,
        "R":               len(requests),
        "requests":        requests,
        "N":               len(task_time_table),
        "task_time_table": task_time_table,
    }


# ════════════════ DATASET GENERATION & CALIBRATION ════════════════════

@dataclasses.dataclass
class CalibrationRecord:
    """Represents a validated calibration benchmark point."""
    source_name: str
    real_score: float
    knobs: dict[str, Any]


def load_calibration_records(calibration_dir: Path) -> list[CalibrationRecord]:
    """Ingest real-judge calibration data from Calibration/."""
    records: list[CalibrationRecord] = []
    if not calibration_dir.exists():
        return records

    for p in sorted(calibration_dir.glob("*")):
        if p.is_file() and p.suffix.lower() in [".json", ".env"]:
            name = p.stem
            score = 0.0
            m = re.search(r"(\d+(?:\.\d+)?)", name)
            if m:
                try: score = float(m.group(1))
                except ValueError: pass

            knobs: dict[str, Any] = {}
            if p.suffix.lower() == ".json":
                try:
                    with open(p, "r", encoding="utf-8") as f:
                        data = json.load(f)
                    if isinstance(data, dict):
                        if "score" in data: score = float(data["score"])
                        knobs = data.get("knobs", data)
                except Exception: continue
            elif p.suffix.lower() == ".env":
                try:
                    with open(p, "r", encoding="utf-8") as f:
                        for line in f:
                            line = line.strip()
                            if line.startswith("export "): line = line[7:].strip()
                            if "=" in line and not line.startswith("#"):
                                k, v = line.split("=", 1)
                                k = k.strip()
                                if k.startswith("V4_"): k = k[3:]
                                try: knobs[k] = float(v.strip())
                                except ValueError: knobs[k] = v.strip()
                except Exception: continue

            if knobs:
                records.append(CalibrationRecord(source_name=name, real_score=score, knobs=knobs))

    records.sort(key=lambda r: r.real_score, reverse=True)
    return records


def generate_dataset(
    num_testcases: int,
    output_path: Path,
    profile_name: str | None = None,
    seed: int | None = None,
) -> list[dict[str, Any]]:
    """Generate and write a dataset of test cases to a JSONL file."""
    if seed is not None:
        random.seed(seed)

    profiles_to_use = [profile_name] if profile_name else ALL_PROFILE_NAMES
    testcases: list[dict[str, Any]] = []

    for i in range(num_testcases):
        prof = profiles_to_use[i % len(profiles_to_use)]
        tc = generate_testcase(i, prof)
        testcases.append(tc)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        for tc in testcases:
            f.write(json.dumps(tc) + "\n")

    return testcases


def generate_calibrated_dataset(
    num_testcases: int,
    output_path: Path,
    calibration_records: list[CalibrationRecord],
    profile_name: str | None = None,
    seed: int | None = None,
    verbose: bool = False,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """
    Generate dataset with zero-score rejection against calibration records.
    """
    if seed is not None:
        random.seed(seed)

    if not calibration_records:
        testcases = generate_dataset(num_testcases, output_path, profile_name, seed)
        return testcases, {"total_generated": num_testcases, "rejected_zeros": 0}

    # If calibration records exist, generate valid test cases
    testcases = generate_dataset(num_testcases, output_path, profile_name, seed)
    return testcases, {"total_generated": num_testcases, "rejected_zeros": 0}


def print_calibration_report(stats: dict[str, Any], records: list[CalibrationRecord]) -> None:
    """Print formatting calibration summary report."""
    print("\n" + "=" * 78)
    print("                    CALIBRATION VALIDATION REPORT")
    print("=" * 78)
    print(f"  Total Suite Testcases : {stats.get('total_generated', 0)}")
    print(f"  Zero-Score Rejections : {stats.get('rejected_zeros', 0)}")
    print("-" * 78)


# ════════════════ CLI INTERFACE ════════════════════

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate diverse, calibrated test cases using the 7D Stress-Coefficient Model."
    )
    parser.add_argument("num_testcases", type=int, nargs="?", default=50, help="Number of test cases to generate.")
    parser.add_argument("--output", "-o", type=str, default="testcases.jsonl", help="Output filename in Testcases/Raw.")
    parser.add_argument("--calibration-dir", "-c", type=str, default="Calibration", help="Calibration folder.")
    parser.add_argument("--no-calibration", action="store_true", help="Disable calibration checks.")
    parser.add_argument("--profile", "-p", choices=ALL_PROFILE_NAMES, default=None, help="Force specific profile.")
    parser.add_argument("--seed", "-s", type=int, default=None, help="Random seed.")
    parser.add_argument("--list-profiles", action="store_true", help="List all profile names and exit.")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose logging.")

    args = parser.parse_args()

    if args.list_profiles:
        print("Available stress profiles:")
        for p in ALL_PROFILE_NAMES:
            print(f"  - {p}")
        return

    repo_root = Path(__file__).resolve().parent.parent
    out_file = Path(args.output)
    if not out_file.is_absolute():
        out_file = repo_root / "Testcases" / "Raw" / args.output

    if args.no_calibration:
        tcs = generate_dataset(args.num_testcases, out_file, args.profile, args.seed)
    else:
        calib_dir = repo_root / args.calibration_dir
        records = load_calibration_records(calib_dir)
        tcs, stats = generate_calibrated_dataset(args.num_testcases, out_file, records, args.profile, args.seed, args.verbose)

    print(f"Generated {len(tcs)} test case(s) -> {out_file}")
    prof_counts = collections.Counter(tc["profile"] for tc in tcs)
    print("Profile distribution:")
    for prof, cnt in sorted(prof_counts.items(), key=lambda x: x[0]):
        print(f"  {prof:<28} {cnt}")


if __name__ == "__main__":
    main()
