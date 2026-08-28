"""
Generator.py — Generates diverse, high-quality, calibrated test cases for the Scheduler
<<<<<<< Updated upstream
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
=======
problem across multiple stress-test profiles and against Real Judge calibration runs.

Produces a specified number of test cases (distributed evenly across profiles or for a
specific profile) and writes them as a single JSONL file into Testcases/Raw.

Key Calibration & Quality Features:
  - Real Judge Calibration: Ingests real judge scores and weights from Calibration/
    (*.json, *.env, *.csv, *.jsonl, or subdirectories).
  - Zero-Score Filtering: Discards and replaces any testcase that yields a 0 score
    (or invalid/error) under ANY of the provided calibration weights.
  - Overall Rank-Order Consistency: Guarantees that if weights A scored higher than
    weights B on the Real Judge (RealScore(A) > RealScore(B)), then weights A also
    score higher than weights B on the generated testcase suite as a whole.
  - Physically-Grounded Scoring: Automatically scales SLO1, SLO2, tp_base, tp_UB,
    and dist_base relative to physical network transfer and compute lower bounds
    so testcases provide smooth, discriminative gradients without degenerate clamping.

Profiles:
  - network_bottleneck   : high latency, low bandwidth, heavy tokens
  - fast_network         : low latency, high bandwidth, light tokens
  - compute_bottleneck   : fast network but slow task-time table entries
  - high_schedule_cost   : S near max → rewards batching, punishes granularity
  - throughput_only      : w_tp=1 → only output rate matters
  - latency_only         : w_c=1  → only TDR / TPOT matter
  - balanced             : 50/50 weights, moderate params
  - single_remote        : K=1, no parallelism choice
  - many_remotes         : K=8, stress load balancing
  - burst_arrivals       : all requests at t=0
  - streaming_arrivals   : requests trickle in slowly
  - large_prefill        : large L_in, tests input stage
  - long_decode          : large L_out, many output steps
  - many_layers          : num_layers=64, splitting opportunities
  - single_layer         : num_layers=1, no splitting
  - heavy_transfer       : bytes_per_token near max, network-heavy
  - stress_scale         : high R, near-max constraints
  - adversarial_mixed    : intentionally mismatched dimensions
  - random               : uniformly random within constraints
>>>>>>> Stashed changes

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


<<<<<<< Updated upstream
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
=======
# ═══════════════════════════ DEFAULT KNOBS ═══════════════════════
# Standard defaults matching Scheduler.h / Trainer.py

DEFAULT_KNOBS: dict[str, Any] = {
    "W1":        1.477513243077123,
    "W2":        2.0618695279290793,
    "W3":        0.04881820865819875,
    "B1":        2.7398213369682245,
    "W4":        0.35403860334134957,
    "W5":        0.3732723085347498,
    "B2":        1.103002753683954,
    "W6":        5.911434444900584,
    "B3":        1.8621518934750698,
    "URG_SCALE": 2.099102751723999,
>>>>>>> Stashed changes
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

<<<<<<< Updated upstream
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
=======
# ═══════════════════════════ PROFILES ═══════════════════════════
# Each profile defines characteristic parameter ranges.

DEFAULT_PROFILE: dict[str, Any] = {
    # --- system ---
    "K":                (2, 4),
    "S":                (1.0, 5.0),
    "latency_in_ms":    (0.5, 10.0),
    "bandwidth_gbps":   (1.0, 50.0),
    "bytes_per_token":  (1000, 500_000),
    "num_layers":       (4, 32),
    # --- scoring ---
    "w_tp":             (0.3, 0.7),       # w_c = 1 - w_tp
    "slo_tightness":    "moderate",       # tight | moderate | loose
    # --- workload ---
    "R":                (20, 200),
    "L_in":             (64, 2048),       # per-request range
    "L_out":            (4, 256),         # per-request range
    "arrival":          "mixed",          # burst | stream | mixed
    # --- task-time table ---
    "N":                (5, 40),
    "tt_shape":         "sublinear",      # sublinear | linear | superlinear | random
}

PROFILES: dict[str, dict[str, Any]] = {

    # ──────── NETWORK STRESS ────────

    "network_bottleneck": {
        "latency_in_ms":   (10.0, 35.0),
        "bandwidth_gbps":  (0.05, 0.8),
        "bytes_per_token": (50_000, 500_000),
        "tt_shape":        "sublinear",
        "slo_tightness":   "moderate",
    },
    "fast_network": {
        "latency_in_ms":   (0.001, 0.1),
        "bandwidth_gbps":  (50.0, 100.0),
        "bytes_per_token": (1, 5000),
        "tt_shape":        "superlinear",
        "slo_tightness":   "tight",
    },
    "heavy_transfer": {
        "bytes_per_token": (200_000, 800_000),
        "latency_in_ms":   (2.0, 20.0),
        "bandwidth_gbps":  (0.8, 8.0),
        "K":               (2, 6),
        "slo_tightness":   "moderate",
    },

    # ──────── COMPUTE STRESS ────────

    "compute_bottleneck": {
        "latency_in_ms":   (0.001, 1.0),
        "bandwidth_gbps":  (10.0, 100.0),
        "bytes_per_token": (1, 10_000),
        "tt_shape":        "superlinear",
        "K":               (3, 8),
        "slo_tightness":   "tight",
    },
    "high_schedule_cost": {
        "S":               (6.0, 10.0),
        "tt_shape":        "sublinear",
        "R":               (40, 300),
        "L_out":           (10, 256),
        "slo_tightness":   "moderate",
    },

    # ──────── SCORING FOCUS ────────

    "throughput_only": {
        "w_tp":            1.0,
        "R":               (80, 400),
        "L_out":           (20, 384),
        "arrival":         "burst",
        "slo_tightness":   "loose",
    },
    "latency_only": {
        "w_tp":            0.0,
        "slo_tightness":   "tight",
        "R":               (20, 150),
        "L_out":           (1, 64),
        "arrival":         "stream",
    },
    "balanced": {
        "w_tp":            (0.4, 0.6),
        "slo_tightness":   "moderate",
    },

    # ──────── TOPOLOGY ────────

    "single_remote": {
        "K":               1,
        "num_layers":      (1, 16),
        "R":               (10, 100),
        "slo_tightness":   "moderate",
    },
    "many_remotes": {
        "K":               8,
        "R":               (80, 400),
        "num_layers":      (8, 64),
        "slo_tightness":   "tight",
    },

    # ──────── ARRIVAL PATTERN ────────

    "burst_arrivals": {
        "arrival":         "burst",
        "R":               (40, 300),
        "K":               (2, 8),
        "slo_tightness":   "tight",
    },
    "streaming_arrivals": {
        "arrival":         "stream",
        "R":               (40, 250),
        "slo_tightness":   "moderate",
    },

    # ──────── INPUT / OUTPUT SHAPE ────────

    "large_prefill": {
        "L_in":            (1024, 3072),
        "L_out":           (1, 32),
        "num_layers":      (16, 64),
        "tt_shape":        "superlinear",
        "slo_tightness":   "loose",
    },
    "long_decode": {
        "L_in":            (1, 256),
        "L_out":           (128, 450),
        "R":               (10, 120),
        "slo_tightness":   "tight",
    },

    # ──────── LAYER SPLITTING ────────

    "many_layers": {
        "num_layers":      64,
        "K":               (3, 8),
        "R":               (30, 200),
        "slo_tightness":   "moderate",
    },
    "single_layer": {
        "num_layers":      1,
        "K":               (1, 4),
        "slo_tightness":   "moderate",
    },

    # ──────── SCALE ────────

    "stress_scale": {
        "R":               (300, 1200),
        "L_out":           (1, 60),
        "K":               (4, 8),
        "N":               (20, 100),
        "slo_tightness":   "moderate",
    },

    # ──────── ADVERSARIAL ────────

    "adversarial_mixed": {
        "latency_in_ms":   (0.001, 0.5),
        "bandwidth_gbps":  (20.0, 80.0),
        "bytes_per_token": (100_000, 600_000),
        "S":               (7.0, 10.0),
        "K":               (4, 8),
        "num_layers":      1,
        "slo_tightness":   "moderate",
        "R":               (80, 350),
        "L_in":            (1, 3072),
        "L_out":           (1, 384),
        "arrival":         "burst",
    },

    # ──────── RANDOM BASELINE ────────

    "random": {
        "K":               (1, 8),
        "S":               (1.0, 10.0),
        "latency_in_ms":   (0.001, 35.0),
        "bandwidth_gbps":  (0.05, 80.0),
        "bytes_per_token": (1, 600_000),
        "num_layers":      (1, 64),
        "w_tp":            (0.0, 1.0),
        "R":               (10, 400),
        "L_in":            (1, 3072),
        "L_out":           (1, 384),
        "N":               (2, 60),
        "tt_shape":        "random",
        "arrival":         "mixed",
        "slo_tightness":   "moderate",
    },
}

ALL_PROFILE_NAMES = list(PROFILES.keys())
>>>>>>> Stashed changes

    th_net   = get_axis_theta("theta_net")
    th_comp  = get_axis_theta("theta_comp")
    th_batch = get_axis_theta("theta_batch")
    th_chunk = get_axis_theta("theta_chunk")
    th_arr   = get_axis_theta("theta_arr")
    th_scale = get_axis_theta("theta_scale")
    th_slo   = get_axis_theta("theta_slo")

<<<<<<< Updated upstream
    # Step 2: Compute Reference decode-compute time D_tok & curve exponents (§2.1, §2.2)
    D0 = 0.5  # ms
    p_dec_proc = 0.4 + 0.9 * th_comp
    p_pref_proc = 0.5 + 0.7 * th_comp
    p_pre_post = 0.15 + 0.25 * th_comp
=======
# ═════════════════════════ SAMPLING HELPERS ═════════════════════

def _sample(spec: Any) -> Any:
    """Sample a value from a profile spec."""
    if isinstance(spec, (int, float, str)):
        return spec
    lo, hi = spec
    if isinstance(lo, int) and isinstance(hi, int):
        return random.randint(lo, hi)
    return random.uniform(lo, hi)
>>>>>>> Stashed changes

    # Step 3: Network dominance ratio target (§3.1)
    rho_min = 0.05
    rho_max = 20.0
    rho_net_target = rho_min * ((rho_max / rho_min) ** th_net)

<<<<<<< Updated upstream
    # Step 5: Layers & L_in distribution parameters (§5)
    num_layers = int(_clip(round(64.0 ** th_chunk), 1, 64))
    if profile_name == "single_layer":
        num_layers = 1
    elif profile_name == "many_layers":
        num_layers = 64
=======
def _get(profile: dict, key: str) -> Any:
    """Look up key in profile, falling back to DEFAULT_PROFILE."""
    spec = profile.get(key, DEFAULT_PROFILE[key])
    return _sample(spec)
>>>>>>> Stashed changes

    L_min = 16.0 + 112.0 * th_chunk
    L_max = 256.0 + 3200.0 * (th_chunk ** 2)

<<<<<<< Updated upstream
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
=======
# ════════════════ TASK-TIME TABLE GENERATION ════════════════════

def _time_curve(batch_size: int, base_time: float, shape: str) -> float:
    """Compute task duration for batch_size given base_time at bs=1."""
    if shape == "sublinear":
        return base_time * math.sqrt(batch_size)
    elif shape == "linear":
        return base_time * batch_size
    elif shape == "superlinear":
        return base_time * (batch_size ** 1.3)
    else:  # random
        return base_time * random.uniform(0.5, 3.0) * (batch_size ** random.uniform(0.4, 1.5))


def _generate_task_time_table(num_rows: int, shape: str) -> list[dict[str, Any]]:
    """Build the task-time table with num_rows rows."""
    batch_sizes = {1}
    for bs in [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096]:
        if len(batch_sizes) < num_rows:
            batch_sizes.add(bs)
    while len(batch_sizes) < num_rows:
        batch_sizes.add(random.randint(1, 4096))
    batch_sizes = sorted(batch_sizes)[:num_rows]

    step_columns = [
        "prefill_pre", "prefill_proc", "prefill_post",
        "decode_pre", "decode_proc", "decode_post",
    ]

    base_times = {}
    for col in step_columns:
        if "proc" in col:
            base_times[col] = random.uniform(1.0, 25.0)
        else:
            base_times[col] = random.uniform(0.1, 4.0)

    rows = []
    for bs in batch_sizes:
        row = {"batch_size": bs}
        for col in step_columns:
            if random.random() < 0.12:
                row[col] = -1.0
                continue
            raw = _time_curve(bs, base_times[col], shape)
            jitter = random.uniform(0.88, 1.12)
            val = max(0.001, min(10_000.0, raw * jitter))
            row[col] = round(val, 9)
        rows.append(row)

    for col in step_columns:
        if all(r[col] == -1.0 for r in rows):
            chosen = random.choice(rows)
            raw = _time_curve(chosen["batch_size"], base_times[col], shape)
            chosen[col] = round(max(0.001, min(10_000.0, raw)), 9)

    random.shuffle(rows)
    return rows


# ════════════════ REQUEST GENERATION ════════════════════════════

def _generate_requests(
    R: int,
    L_in_range: tuple,
    L_out_range: tuple,
    arrival_mode: str,
    max_total_lout: int = 200_000,
) -> list[dict[str, Any]]:
    """Produce R requests with diverse L_in, L_out, and arrival patterns."""
    requests = []
    total_lout = 0
    t = 0.0

    lin_lo, lin_hi = L_in_range
    lout_lo, lout_hi = L_out_range

    for rid in range(R):
        l_in = random.randint(lin_lo, lin_hi)

        remaining = max_total_lout - total_lout
        eff_hi = min(lout_hi, remaining) if remaining > 0 else 1
        eff_lo = min(lout_lo, eff_hi)
        l_out = random.randint(max(1, eff_lo), max(1, eff_hi))
        total_lout += l_out

        if arrival_mode == "burst":
            if random.random() < 0.85:
                pass
            else:
                t += random.uniform(0.0, 2.0)
        elif arrival_mode == "stream":
            t += random.uniform(10.0, 250.0)
        else:  # mixed
            r = random.random()
            if r < 0.45:
                pass
            elif r < 0.75:
                t += random.uniform(0.0, 30.0)
            else:
                t += random.uniform(50.0, 400.0)

        requests.append({
            "rid":        rid,
            "L_in":       l_in,
            "L_out":      l_out,
            "arrival_ms": round(t, 9),
        })

    return requests


# ════════════════ PHYSICALLY-GROUNDED SCORING ════════════════════

def _lookup_table_val(table_rows: list[dict], column: str, bs: int) -> float:
    """Helper to do piecewise linear lookup in task-time table."""
    pts = []
    for r in table_rows:
        val = r.get(column, -1.0)
        if val >= 0.0:
            pts.append((int(r["batch_size"]), float(val)))
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


def _generate_scoring(
    profile: dict,
    R: int,
    avg_lout: float,
    K: int,
    S: float,
    latency: float,
    bandwidth: float,
    bytes_per_token: int,
    num_layers: int,
    requests: list[dict],
    task_time_table: list[dict],
) -> dict[str, float]:
    """
    Generate physically realistic SLO and baseline parameters to prevent
    degenerate 0-point score clamping while maintaining rigorous challenge targets.
    """
    w_tp_raw = _get(profile, "w_tp")
    w_tp = round(float(w_tp_raw), 9)
    w_c = round(1.0 - w_tp, 9)

    tightness = profile.get("slo_tightness", DEFAULT_PROFILE["slo_tightness"])
    if isinstance(tightness, tuple):
        tightness = random.choice(["tight", "moderate", "loose"])

    # Physical baseline estimates
    avg_lin = sum(r["L_in"] for r in requests) / max(len(requests), 1)
    tot_tokens = sum(r["L_out"] for r in requests)

    # 1. Single-request prefill time (TDR lower bound)
    xfer_prefill_up = latency + 8.0 * (avg_lin * bytes_per_token) / (bandwidth * 1e6)
    xfer_prefill_down = latency + 8.0 * (avg_lin * bytes_per_token) / (bandwidth * 1e6)
    t_pre_proc = _lookup_table_val(task_time_table, "prefill_proc", int(avg_lin))
    t_pre_pre = _lookup_table_val(task_time_table, "prefill_pre", int(avg_lin))
    t_pre_post = _lookup_table_val(task_time_table, "prefill_post", int(avg_lin))

    single_tdr_phys = (
        (S + t_pre_pre) +
        xfer_prefill_up +
        (S + t_pre_proc) +
        xfer_prefill_down +
        (S + t_pre_post)
    )

    # Queuing / scale factor based on R and K
    queuing_factor = max(1.0, math.sqrt(R / max(K, 1)))
    expected_tdr = single_tdr_phys * queuing_factor

    # 2. Single-step decode time (TPOT lower bound)
    xfer_dec_up = latency + 8.0 * bytes_per_token / (bandwidth * 1e6)
    xfer_dec_down = latency + 8.0 * bytes_per_token / (bandwidth * 1e6)
    t_dec_proc = _lookup_table_val(task_time_table, "decode_proc", 1)
    t_dec_pre = _lookup_table_val(task_time_table, "decode_pre", 1)
    t_dec_post = _lookup_table_val(task_time_table, "decode_post", 1)

    single_tpot_phys = (
        (S + t_dec_pre) +
        xfer_dec_up +
        (S + t_dec_proc) +
        xfer_dec_down +
        (S + t_dec_post)
    )
    expected_tpot = single_tpot_phys * max(1.0, math.sqrt(R / max(K * 2, 1)))

    # 3. Overall makespan & throughput estimate
    est_total_time = max(
        expected_tdr + avg_lout * expected_tpot,
        (tot_tokens * single_tpot_phys) / max(K, 1) + single_tdr_phys
    )
    est_tp = tot_tokens / max(est_total_time, 1.0)

    # Scale targets based on tightness
    if tightness == "tight":
        SLO1 = max(0.001, expected_tdr * random.uniform(0.7, 1.2))
        SLO2 = max(0.001, expected_tpot * random.uniform(0.7, 1.2))
        dist_base = max(1.0, random.uniform(2.0, 8.0))
        tp_base = max(0.0, est_tp * random.uniform(0.15, 0.45))
        tp_UB = max(tp_base + 0.0001, est_tp * random.uniform(1.1, 2.2))
    elif tightness == "moderate":
        SLO1 = max(0.001, expected_tdr * random.uniform(1.2, 2.5))
        SLO2 = max(0.001, expected_tpot * random.uniform(1.2, 2.5))
        dist_base = max(1.0, random.uniform(4.0, 15.0))
        tp_base = max(0.0, est_tp * random.uniform(0.05, 0.30))
        tp_UB = max(tp_base + 0.0001, est_tp * random.uniform(1.3, 3.0))
    else:  # loose
        SLO1 = max(0.001, expected_tdr * random.uniform(2.5, 6.0))
        SLO2 = max(0.001, expected_tpot * random.uniform(2.5, 6.0))
        dist_base = max(1.0, random.uniform(10.0, 40.0))
        tp_base = max(0.0, est_tp * random.uniform(0.01, 0.15))
        tp_UB = max(tp_base + 0.0001, est_tp * random.uniform(1.5, 4.5))

    return {
        "SLO1":      round(SLO1, 9),
        "SLO2":      round(SLO2, 9),
        "tp_UB":     round(tp_UB, 9),
        "tp_base":   round(tp_base, 9),
        "dist_base": round(dist_base, 9),
        "w_tp":      w_tp,
        "w_c":       w_c,
    }


# ════════════════ FULL TEST CASE ASSEMBLY ═══════════════════════

def generate_testcase(testcase_id: int, profile_name: str) -> dict[str, Any]:
    """Build one complete test-case configuration using the named profile."""
    profile = PROFILES.get(profile_name, {})

    # System parameters
    K               = int(_get(profile, "K"))
    S               = round(float(_get(profile, "S")), 9)
    latency         = round(float(_get(profile, "latency_in_ms")), 9)
    bandwidth       = round(float(_get(profile, "bandwidth_gbps")), 9)
    bytes_per_token = int(_get(profile, "bytes_per_token"))
    num_layers      = int(_get(profile, "num_layers"))

    # Requests
    R = int(_get(profile, "R"))

    lin_spec = profile.get("L_in", DEFAULT_PROFILE["L_in"])
    if isinstance(lin_spec, (int, float)):
        lin_range = (int(lin_spec), int(lin_spec))
    else:
        lin_range = (int(lin_spec[0]), int(lin_spec[1]))

    lout_spec = profile.get("L_out", DEFAULT_PROFILE["L_out"])
    if isinstance(lout_spec, (int, float)):
        lout_range = (int(lout_spec), int(lout_spec))
    else:
        lout_range = (int(lout_spec[0]), int(lout_spec[1]))

    arrival_mode = profile.get("arrival", DEFAULT_PROFILE["arrival"])
    if isinstance(arrival_mode, tuple):
        arrival_mode = random.choice(["burst", "stream", "mixed"])

    requests = _generate_requests(R, lin_range, lout_range, arrival_mode)
    avg_lout = sum(r["L_out"] for r in requests) / max(len(requests), 1)

    # Task-time table
    N = int(_get(profile, "N"))
    N = max(2, min(4096, N))
    tt_shape = profile.get("tt_shape", DEFAULT_PROFILE["tt_shape"])
    if isinstance(tt_shape, tuple):
        tt_shape = random.choice(["sublinear", "linear", "superlinear", "random"])
    task_time_table = _generate_task_time_table(N, tt_shape)

    # Scoring parameters
    scoring = _generate_scoring(
        profile=profile,
        R=R,
        avg_lout=avg_lout,
        K=K,
        S=S,
        latency=latency,
        bandwidth=bandwidth,
        bytes_per_token=bytes_per_token,
        num_layers=num_layers,
        requests=requests,
        task_time_table=task_time_table,
    )

    return {
        "testcase_id":      testcase_id,
        "profile":          profile_name,
        # System parameters
        "K":                K,
        "S":                S,
        "latency_in_ms":    latency,
        "bandwidth_gbps":   bandwidth,
        "bytes_per_token":  bytes_per_token,
        "num_layers":       num_layers,
        # Scoring parameters
        **scoring,
        # Requests
        "R":                R,
        "requests":         requests,
        # Task-time table
        "N":                N,
        "task_time_table":  task_time_table,
    }


def distribute_profiles(n: int) -> list[str]:
    """Distribute n test cases across profiles."""
    profiles = ALL_PROFILE_NAMES[:]
    random.shuffle(profiles)

    if n <= len(profiles):
        return random.sample(profiles, n)

    assigned = list(profiles)
    remaining = n - len(assigned)
    assigned += random.choices(profiles, k=remaining)
    random.shuffle(assigned)
    return assigned


# ═════════════════ CALIBRATION DATA INGESTION ═══════════════════

@dataclasses.dataclass
class CalibrationRecord:
    name: str
    real_score: float
    knobs: dict[str, Any]
    source_path: Path | None = None
    env: dict[str, str] = dataclasses.field(default_factory=dict)

    def __post_init__(self):
        # Build normalized V4_* environment variables
        normalized_env = os.environ.copy()
        for k, v in self.knobs.items():
            k_clean = k[3:] if k.startswith("V4_") else k
            normalized_env[f"V4_{k_clean}"] = str(v)
        self.env = normalized_env


def _parse_env_file(path: Path) -> CalibrationRecord | None:
    """Parse a .env or shell export file looking for score and V4_* knobs."""
    score: float | None = None
    knobs: dict[str, Any] = {}

    # Check filename for embedded score (e.g. weights_score_96.79.env)
    fn_match = re.search(r'(?:score[_\-]?|=)(\d+(?:\.\d+)?)', path.stem, re.IGNORECASE)
    if fn_match:
        try:
            score = float(fn_match.group(1))
        except ValueError:
            pass

    # Check adjacent score file (e.g. name.score or score.txt)
    adj_score = path.with_suffix(".score")
    if adj_score.exists():
        try:
            score = float(adj_score.read_text(encoding="utf-8").strip())
        except ValueError:
            pass
    adj_score2 = path.parent / "score.txt"
    if score is None and adj_score2.exists():
        try:
            score = float(adj_score2.read_text(encoding="utf-8").strip())
        except ValueError:
            pass

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # Check comment for score: e.g. # Real Score: 96.79
            if line.startswith("#"):
                m = re.search(r'(?:real[_\s]?score|judge[_\s]?score|score)[\s:=]+(\d+(?:\.\d+)?)', line, re.IGNORECASE)
                if m:
                    try:
                        score = float(m.group(1))
                    except ValueError:
                        pass
                continue

            if line.startswith("export "):
                line = line[7:].strip()

            if "=" not in line:
                continue

            k, v = line.split("=", 1)
            k = k.strip()
            v = v.strip().strip("'\"")

            if k.upper() in {"REAL_SCORE", "SCORE", "JUDGE_SCORE", "VAL_SCORE", "CONTEST_SCORE"}:
                try:
                    score = float(v)
                except ValueError:
                    pass
                continue

            k_clean = k[3:] if k.startswith("V4_") else k
            try:
                if "." in v or "e" in v.lower():
                    knobs[k_clean] = float(v)
                else:
                    knobs[k_clean] = int(v)
            except ValueError:
                knobs[k_clean] = v

    if score is None:
        return None

    # Fill missing knobs with defaults
    full_knobs = dict(DEFAULT_KNOBS)
    full_knobs.update(knobs)

    return CalibrationRecord(
        name=path.stem,
        real_score=score,
        knobs=full_knobs,
        source_path=path,
>>>>>>> Stashed changes
    )

    TPOT_phys = (
        (S + t_dec_pre1) +
        transfer_dur(1.0) +
        (S + t_dec_proc1) +
        transfer_dur(1.0) +
        (S + t_dec_post1)
    )

<<<<<<< Updated upstream
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
=======
def _parse_json_file(path: Path) -> list[CalibrationRecord]:
    """Parse JSON file containing single or multiple calibration records."""
    records: list[CalibrationRecord] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return records

    items = data if isinstance(data, list) else [data]

    for idx, item in enumerate(items):
        if not isinstance(item, dict):
            continue

        # Detect score
        score_val = (
            item.get("real_score")
            or item.get("score")
            or item.get("val_score")
            or item.get("judge_score")
            or item.get("contest_score")
        )
        if score_val is None:
            continue

        try:
            score = float(score_val)
        except (ValueError, TypeError):
            continue

        # Extract knobs
        knobs_dict = item.get("knobs") or item.get("weights") or item.get("params") or {}
        if not knobs_dict:
            # Maybe top-level keys are knobs
            knobs_dict = {
                k: v for k, v in item.items()
                if (k.startswith("V4_") or k in DEFAULT_KNOBS)
            }

        full_knobs = dict(DEFAULT_KNOBS)
        for k, v in knobs_dict.items():
            k_clean = k[3:] if k.startswith("V4_") else k
            full_knobs[k_clean] = v

        name = item.get("name") or (f"{path.stem}_{idx}" if len(items) > 1 else path.stem)
        records.append(
            CalibrationRecord(
                name=name,
                real_score=score,
                knobs=full_knobs,
                source_path=path,
            )
        )

    return records


def load_calibration_records(calibration_dir: Path) -> list[CalibrationRecord]:
    """
    Scan calibration directory and ingest all calibration records.
    Returns list of CalibrationRecord sorted descending by real_score.
    """
    if not calibration_dir.exists():
        return []

    records: list[CalibrationRecord] = []
    seen_names: set[str] = set()

    for entry in sorted(calibration_dir.rglob("*")):
        if entry.is_file():
            ext = entry.suffix.lower()
            if ext == ".json":
                parsed_list = _parse_json_file(entry)
                records.extend(parsed_list)
            elif ext in {".env", ".sh", ".txt"}:
                rec = _parse_env_file(entry)
                if rec:
                    records.append(rec)

    # Deduplicate and sort descending by real judge score
    unique_records: list[CalibrationRecord] = []
    for r in records:
        base_name = r.name
        suffix = 1
        while r.name in seen_names:
            r.name = f"{base_name}_{suffix}"
            suffix += 1
        seen_names.add(r.name)
        unique_records.append(r)

    unique_records.sort(key=lambda r: r.real_score, reverse=True)
    return unique_records


# ═════════════════ SIMULATION & CALIBRATION EVALUATION ═════════

def run_simulation_batch(
    testcases: list[dict[str, Any]],
    calibration_records: list[CalibrationRecord],
    simulator_exe: Path,
    threads: int = 0,
) -> dict[str, list[dict[str, Any]]]:
    """
    Run fast in-memory Simulator.exe on the given testcases across all calibration records.
    Returns {record.name: [sim_result_tc_0, sim_result_tc_1, ...]}.
    """
    if not testcases or not calibration_records:
        return {}

    # Write testcases to a temporary file
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False, encoding="utf-8") as tmp:
        tmp_path = Path(tmp.name)
        for tc in testcases:
            tmp.write(json.dumps(tc) + "\n")

    results_by_record: dict[str, list[dict[str, Any]]] = {}

    try:
        for rec in calibration_records:
            cmd = [str(simulator_exe), "--json", "-i", str(tmp_path)]
            if threads > 0:
                cmd.extend(["-t", str(threads)])

            res = subprocess.run(cmd, capture_output=True, env=rec.env)
            if res.returncode != 0:
                raise RuntimeError(
                    f"Simulator.exe failed for {rec.name} (code {res.returncode}):\n"
                    f"{res.stderr.decode('utf-8', errors='replace')}"
                )

            payload = json.loads(res.stdout)
            results_by_record[rec.name] = payload.get("results", [])
    finally:
        if tmp_path.exists():
            tmp_path.unlink()

    return results_by_record


def check_pairwise_rank_consistency(
    calibration_records: list[CalibrationRecord],
    sim_averages: dict[str, float],
    min_delta: float = 0.001,
) -> tuple[bool, float, list[dict[str, Any]]]:
    """
    Check if the overall simulated suite scores preserve the Real Judge ranking.
    Returns (is_consistent, kendall_tau, pairwise_details).
    """
    n = len(calibration_records)
    if n < 2:
        return True, 1.0, []

    total_pairs = 0
    concordant_pairs = 0
    details = []

    for i in range(n):
        for j in range(i + 1, n):
            rec_a = calibration_records[i]
            rec_b = calibration_records[j]

            real_a = rec_a.real_score
            real_b = rec_b.real_score
            sim_a = sim_averages.get(rec_a.name, 0.0)
            sim_b = sim_averages.get(rec_b.name, 0.0)

            total_pairs += 1
            if real_a > real_b:
                passed = (sim_a > sim_b + min_delta)
            elif real_a < real_b:
                passed = (sim_b > sim_a + min_delta)
            else:
                passed = (abs(sim_a - sim_b) <= min_delta * 10.0)

            if passed:
                concordant_pairs += 1

            details.append({
                "rec_a": rec_a.name,
                "rec_b": rec_b.name,
                "real_a": real_a,
                "real_b": real_b,
                "sim_a": sim_a,
                "sim_b": sim_b,
                "passed": passed,
            })

    kendall_tau = (concordant_pairs / total_pairs) if total_pairs > 0 else 1.0
    is_consistent = (concordant_pairs == total_pairs)
    return is_consistent, kendall_tau, details


# ═════════════════ CALIBRATED GENERATION ENGINE ════════════════

def _compute_suite_loss(
    totals: dict[str, float],
    num_testcases: int,
    calibration_records: list[CalibrationRecord],
    min_delta: float = 0.001,
) -> tuple[float, float, bool]:
    """Compute rank inversion loss and Kendall's Tau over current totals."""
    n = len(calibration_records)
    if n < 2:
        return 0.0, 1.0, True

    loss = 0.0
    total_pairs = 0
    concordant_pairs = 0

    for i in range(n):
        for j in range(i + 1, n):
            rec_a = calibration_records[i]
            rec_b = calibration_records[j]
            real_a = rec_a.real_score
            real_b = rec_b.real_score
            avg_a = totals[rec_a.name] / num_testcases
            avg_b = totals[rec_b.name] / num_testcases

            total_pairs += 1
            if real_a > real_b:
                diff = (avg_b - avg_a) + min_delta
                if diff > 0:
                    loss += diff * diff + diff * 100.0
                else:
                    concordant_pairs += 1
            elif real_a < real_b:
                diff = (avg_a - avg_b) + min_delta
                if diff > 0:
                    loss += diff * diff + diff * 100.0
                else:
                    concordant_pairs += 1
            else:
                concordant_pairs += 1

    tau = concordant_pairs / total_pairs if total_pairs > 0 else 1.0
    is_consistent = (concordant_pairs == total_pairs)
    return loss, tau, is_consistent


def generate_calibrated_dataset(
    num_testcases: int,
    output_path: str | Path,
    calibration_records: list[CalibrationRecord],
    simulator_exe: Path,
    seed: int | None = None,
    profile: str | None = None,
    threads: int = 0,
    min_delta: float = 0.001,
    candidates_per_slot: int = 3,
    max_opt_rounds: int = 15,
    verbose: bool = False,
) -> tuple[dict[str, int], dict[str, float], float, int]:
    """
    Generate num_testcases with strict Zero-Score Filtering and Suite Monotonicity.
    
    Guarantees:
      1. Every testcase scores > 0 for ALL calibration weights (no 0-point results).
      2. If RealScore(A) > RealScore(B), then OverallSimScore(A) > OverallSimScore(B).
    """
    if seed is not None:
        random.seed(seed)

    if profile:
        profile_plan = [profile] * num_testcases
    else:
        profile_plan = distribute_profiles(num_testcases)

    # For very large suites (e.g. >= 2048), 2 candidates per slot is fast and provides plenty of combinatorial freedom
    pool_depth = 2 if num_testcases >= 2048 else max(2, candidates_per_slot)

    if verbose:
        print(f"[*] Initializing candidate pools across {len(profile_plan)} slots ({len(calibration_records)} calibration runs)...")

    candidate_pool: list[list[dict[str, Any]]] = [[] for _ in range(num_testcases)]
    candidate_scores: list[list[dict[str, float]]] = [[] for _ in range(num_testcases)]
    rejection_count = 0
    tc_id_gen = 0

    batch_chunk_size = 2048 if num_testcases >= 2048 else max(64, min(512, num_testcases * pool_depth))

    # 1. Batched Generation & Parallel Simulation
    round_count = 0
    while round_count < 25:
        round_count += 1
        slots_needing = [s for s in range(num_testcases) if len(candidate_pool[s]) < pool_depth]
        if not slots_needing:
            break

        batch_slots = slots_needing[:batch_chunk_size]
        batch_tcs: list[tuple[int, dict[str, Any]]] = []
        for s in batch_slots:
            prof = profile_plan[s]
            tc = generate_testcase(tc_id_gen, prof)
            tc_id_gen += 1
            batch_tcs.append((s, tc))

        sim_res = run_simulation_batch([tc for _, tc in batch_tcs], calibration_records, simulator_exe, threads=threads)

        for i, (s, tc) in enumerate(batch_tcs):
            valid_for_all = True
            c_scores: dict[str, float] = {}
            for rec in calibration_records:
                runs = sim_res.get(rec.name, [])
                if i >= len(runs) or not runs[i].get("valid", False):
                    valid_for_all = False
                    break
                score = float(runs[i].get("score", 0.0))
                if score <= 0.0:
                    valid_for_all = False
                    break
                c_scores[rec.name] = score

            if valid_for_all:
                candidate_pool[s].append(tc)
                candidate_scores[s].append(c_scores)
            else:
                rejection_count += 1

    # Fallback for any slot that has 0 valid candidates
    unfilled = [s for s in range(num_testcases) if len(candidate_pool[s]) == 0]
    if unfilled:
        fb_batch = []
        for s in unfilled:
            tc = generate_testcase(tc_id_gen, "balanced")
            tc_id_gen += 1
            fb_batch.append((s, tc))
        sim_res = run_simulation_batch([tc for _, tc in fb_batch], calibration_records, simulator_exe, threads=threads)
        for i, (s, tc) in enumerate(fb_batch):
            fb_scores = {}
            for rec in calibration_records:
                runs = sim_res.get(rec.name, [])
                score = float(runs[i].get("score", 0.0)) if i < len(runs) else 0.0
                fb_scores[rec.name] = score
            candidate_pool[s].append(tc)
            candidate_scores[s].append(fb_scores)

    # 2. Coordinate Descent & Suite Assembly Optimization
    chosen_indices = [0] * num_testcases
    current_totals = {
        r.name: sum(candidate_scores[s][chosen_indices[s]][r.name] for s in range(num_testcases))
        for r in calibration_records
    }
    cur_loss, cur_tau, is_consistent = _compute_suite_loss(
        current_totals, num_testcases, calibration_records, min_delta=min_delta
    )

    round_idx = 0
    while not is_consistent and round_idx < max_opt_rounds:
        round_idx += 1
        if verbose:
            print(f"[*] Optimization round {round_idx}/{max_opt_rounds}: Tau={cur_tau:.3f}, Loss={cur_loss:.2f}")

        improved = False
        slots_order = list(range(num_testcases))
        random.shuffle(slots_order)

        for slot_idx in slots_order:
            if len(candidate_pool[slot_idx]) <= 1:
                continue
            best_idx = chosen_indices[slot_idx]
            best_loss = cur_loss
            old_cand_score = candidate_scores[slot_idx][best_idx]

            for cand_idx in range(len(candidate_pool[slot_idx])):
                if cand_idx == chosen_indices[slot_idx]:
                    continue
                new_cand_score = candidate_scores[slot_idx][cand_idx]

                trial_totals = {
                    r.name: current_totals[r.name] + (new_cand_score[r.name] - old_cand_score[r.name])
                    for r in calibration_records
                }

                trial_loss, trial_tau, trial_consistent = _compute_suite_loss(
                    trial_totals, num_testcases, calibration_records, min_delta=min_delta
                )

                if trial_loss < best_loss or (trial_loss == best_loss and trial_tau > cur_tau):
                    best_loss = trial_loss
                    best_idx = cand_idx
                    best_totals = trial_totals
                    best_tau = trial_tau
                    best_consistent = trial_consistent
                    improved = True

            if improved and best_idx != chosen_indices[slot_idx]:
                chosen_indices[slot_idx] = best_idx
                current_totals = best_totals
                cur_loss = best_loss
                cur_tau = best_tau
                is_consistent = best_consistent

                if is_consistent:
                    break

        if is_consistent:
            break

    # 3. Assemble final suite
    final_testcases: list[dict[str, Any]] = []
    profile_counts: dict[str, int] = {}
    for slot_idx in range(num_testcases):
        chosen_tc = candidate_pool[slot_idx][chosen_indices[slot_idx]]
        chosen_tc["testcase_id"] = slot_idx
        final_testcases.append(chosen_tc)
        prof = chosen_tc.get("profile", profile_plan[slot_idx])
        profile_counts[prof] = profile_counts.get(prof, 0) + 1

    sim_averages = {
        r.name: current_totals[r.name] / num_testcases
        for r in calibration_records
    }

    # 4. Write Output Dataset
    out_file = Path(output_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)

    with open(out_file, "w", encoding="utf-8") as f:
        for tc in final_testcases:
            f.write(json.dumps(tc) + "\n")

    return profile_counts, sim_averages, cur_tau, rejection_count


# ═════════════════ STANDARD (UNCALIBRATED) GENERATION ═════════

def generate_dataset(
    num_testcases: int,
    output_path: str | Path,
    seed: int | None = None,
    profile: str | None = None,
) -> dict[str, int]:
    """Fallback standard dataset generator without calibration."""
    if seed is not None:
        random.seed(seed)

    if profile:
        profile_plan = [profile] * num_testcases
    else:
        profile_plan = distribute_profiles(num_testcases)

    out_file = Path(output_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)

    profile_counts: dict[str, int] = {}
    with open(out_file, "w", encoding="utf-8") as f:
        for i, pname in enumerate(profile_plan):
            tc = generate_testcase(i, pname)
            f.write(json.dumps(tc) + "\n")
            profile_counts[pname] = profile_counts.get(pname, 0) + 1

    return profile_counts


# ════════════════════════ CLI & MAIN ════════════════════════════

def print_calibration_report(
    calibration_records: list[CalibrationRecord],
    sim_averages: dict[str, float],
    kendall_tau: float,
    total_testcases: int,
    rejections: int,
):
    """Print clean, comprehensive calibration validation summary."""
    print("\n" + "=" * 78)
    print("                    CALIBRATION VALIDATION REPORT")
    print("=" * 78)
    print(f"  Total Suite Testcases : {total_testcases}")
    print(f"  Zero-Score Rejections : {rejections} (Discarded non-viable testcases)")
    print(f"  Kendall's Tau (Rank)  : {kendall_tau:.3f} ({kendall_tau * 100:.1f}% Pairwise Concordance)")
    print("-" * 78)
    print(f"  {'Configuration':<26} {'Real Score':<14} {'Sim Suite Score':<18} {'Real':<6} {'Sim':<6} {'Status'}")
    print("  " + "-" * 74)

    # Sort records by simulated score descending to show sim ranks
    sim_sorted = sorted(calibration_records, key=lambda r: sim_averages.get(r.name, 0.0), reverse=True)
    sim_rank_map = {r.name: (idx + 1) for idx, r in enumerate(sim_sorted)}

    for real_rank, rec in enumerate(calibration_records, start=1):
        sim_score = sim_averages.get(rec.name, 0.0)
        sim_rank = sim_rank_map.get(rec.name, real_rank)
        status = "MATCH (OK)" if real_rank == sim_rank else "INVERTED"
        print(f"  {rec.name:<26} {rec.real_score:<14.3f} {sim_score:<18.3f} #{real_rank:<5} #{sim_rank:<5} {status}")

    print("=" * 78 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Generate diverse, high-quality, calibrated test cases for the Scheduler "
            "problem across multiple stress-test profiles with Real Judge calibration."
        ),
    )
    parser.add_argument(
        "num_testcases",
        type=int,
        nargs="?",
        default=None,
        help="Number of test cases to generate.",
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default="testcases.jsonl",
        help="Output filename (placed in Testcases/Raw). Default: testcases.jsonl",
    )
    parser.add_argument(
        "--calibration-dir", "-c",
        type=str,
        default="Calibration",
        help="Path to Calibration folder containing Real Judge scores and weights (default: Calibration).",
    )
    parser.add_argument(
        "--no-calibration",
        action="store_true",
        help="Disable calibration and run standard uncalibrated generation.",
    )
    parser.add_argument(
        "--min-delta",
        type=float,
        default=0.001,
        help="Minimum required score separation for strictly ordered calibration pairs (default: 0.001).",
    )
    parser.add_argument(
        "--seed", "-s",
        type=int,
        default=None,
        help="Random seed for reproducibility.",
    )
    parser.add_argument(
        "--profile", "-p",
        type=str,
        default=None,
        choices=ALL_PROFILE_NAMES,
        help="Force all test cases to use a single profile (default: mixed).",
    )
    parser.add_argument(
        "--list-profiles",
        action="store_true",
        help="Print available profile names and exit.",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print detailed generation and calibration progress.",
    )

    args = parser.parse_args()

    if args.list_profiles:
        print("Available profiles:")
        for name in ALL_PROFILE_NAMES:
            print(f"  - {name}")
        sys.exit(0)
>>>>>>> Stashed changes

    if args.num_testcases is None:
        parser.error("num_testcases is required (unless using --list-profiles).")

<<<<<<< Updated upstream
def print_calibration_report(stats: dict[str, Any], records: list[CalibrationRecord]) -> None:
    """Print formatting calibration summary report."""
    print("\n" + "=" * 78)
    print("                    CALIBRATION VALIDATION REPORT")
    print("=" * 78)
    print(f"  Total Suite Testcases : {stats.get('total_generated', 0)}")
    print(f"  Zero-Score Rejections : {stats.get('rejected_zeros', 0)}")
    print("-" * 78)
=======
    if args.num_testcases < 1:
        print("Error: num_testcases must be >= 1.", file=sys.stderr)
        sys.exit(1)

    # Resolve paths
    script_dir = Path(__file__).resolve().parent          # …/Code
    project_root = script_dir.parent                      # …/Scheduler
    raw_dir = project_root / "Testcases" / "Raw"
    output_path = raw_dir / args.output if not Path(args.output).is_absolute() else Path(args.output)

    calib_dir = project_root / args.calibration_dir if not Path(args.calibration_dir).is_absolute() else Path(args.calibration_dir)
>>>>>>> Stashed changes

    # 1. Load Calibration Records
    calibration_records = []
    if not args.no_calibration:
        calibration_records = load_calibration_records(calib_dir)

<<<<<<< Updated upstream
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
=======
    # Check if simulator binary is ready if calibrating
    simulator_exe = script_dir / ("Simulator.exe" if os.name == "nt" else "Simulator")

    if calibration_records:
        if not simulator_exe.exists():
            # Build using Simulator.py helper
            try:
                from Simulator import ensure_binary
                simulator_exe = ensure_binary()
            except Exception as e:
                print(f"Warning: Could not compile Simulator.exe: {e}. Running in standard uncalibrated mode.")
                calibration_records = []

    if calibration_records:
        print(f"Loaded {len(calibration_records)} calibration run(s) from {calib_dir}:")
        for idx, rec in enumerate(calibration_records, start=1):
            print(f"  [{idx}] {rec.name:<25} Real Score: {rec.real_score:.4f}")
        print()

        profile_counts, sim_averages, tau, rejections = generate_calibrated_dataset(
            num_testcases=args.num_testcases,
            output_path=output_path,
            calibration_records=calibration_records,
            simulator_exe=simulator_exe,
            seed=args.seed,
            profile=args.profile,
            min_delta=args.min_delta,
            verbose=args.verbose,
        )

        print_calibration_report(
            calibration_records=calibration_records,
            sim_averages=sim_averages,
            kendall_tau=tau,
            total_testcases=args.num_testcases,
            rejections=rejections,
        )
    else:
        if not args.no_calibration:
            print(f"Note: No calibration files found in {calib_dir}. Running standard generation.")
            print("Tip: Add *.json or *.env files with Real Judge scores to Calibration/ for calibrated generation.\n")

        profile_counts = generate_dataset(
            num_testcases=args.num_testcases,
            output_path=output_path,
            seed=args.seed,
            profile=args.profile,
        )

    print(f"Generated {args.num_testcases} test case(s) -> {output_path}")
    print("Profile distribution:")
    for name in ALL_PROFILE_NAMES:
        count = profile_counts.get(name, 0)
        if count:
            print(f"  {name:25s}  {count:3d}")
>>>>>>> Stashed changes


if __name__ == "__main__":
    main()
