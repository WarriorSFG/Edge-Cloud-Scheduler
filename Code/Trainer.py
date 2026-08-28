"""
Trainer.py — High-Performance Optuna-Based Black-Box Optimizer for Scheduler Knobs.

Searches the ~40 KnobSet parameters in Scheduler.h / Scheduler.cpp using the
high-performance C++ Simulator engine as the ground-truth objective.

Key Architecture & Anti-Stagnation Features:
- Cyclic Multi-Phase Search Engine:
    1. Multivariate TPE (joint modeling of coupled parameter groups, relative startup budget)
    2. IPOP CMA-ES (increasing population local refinement with automatic restarts)
    3. Multi-Scale Elite Mutation & Basin Jumps (fine, medium, coarse perturbations)
    4. TPE Recycle & Re-anchoring across newly discovered basins
- Relative & Scalable Hyperparameters: percentages of total trials rather than rigid constants
- Gradient-Preserving Concave Fitness: power-mean formulation that raises profile floors
  without killing gradients when bounded profiles (e.g. latency_only) score near zero
- Non-Lossy Global Elite Candidate Pool: tracks top all-time candidates across the entire study
- Dual Validation Trigger: immediate validation on all-time training records + periodic elite batch validation
- Effective Validation Scoring: val_score * valid_ratio for robust promotion gating
- Resumability: full state restoration from SQLite (study.db), champion.json, and champion.env
- Automatic dataset sizing check: generates required testcases if fewer than requested
- Rich Live Diagnostics: live progress reporting, phase indicators, and per-profile promotion breakdowns

Usage:
    python Code/Trainer.py [options]
"""

import argparse
import collections
import datetime
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import time
from typing import Any

import optuna
from optuna.samplers import TPESampler, CmaEsSampler
import orjson

import warnings
warnings.filterwarnings("ignore")

# Silence verbose Optuna per-trial parameter dump logs
optuna.logging.set_verbosity(optuna.logging.WARNING)

# Local imports
from Generator import (
    ALL_PROFILE_NAMES,
    generate_dataset,
    load_calibration_records,
    generate_calibrated_dataset,
    print_calibration_report,
)
from Simulator import ensure_binary


# ============================ KNOB SPECIFICATIONS ============================
# Mathematical parameters from Mathematics.md & Schedulers/Scheduler.h.

KNOB_SPECS: dict[str, dict[str, Any]] = {
    # -- Dynamic Decode Batching (beta) per Mathematics.md §11-16, §18-19 --
    "W1":           {"type": "float", "low": 0.0, "high": 2.0, "default": 0.40},   # weight of S
    "W2":           {"type": "float", "low": 0.0, "high": 5.0, "default": 1.50},   # weight of w_tp ratio
    "W3":           {"type": "float", "low": 0.0, "high": 0.1, "default": 0.02},   # weight of SLO2
    "B1":           {"type": "float", "low": 0.5, "high": 4.0, "default": 1.00},   # bias

    # -- Decode Time-to-Live (tau) per Mathematics.md §Algorithmic Workflow --
    "W4":           {"type": "float", "low": 0.05, "high": 0.8, "default": 0.25},  # weight of SLO2
    "W5":           {"type": "float", "low": 0.05, "high": 0.8, "default": 0.20},  # weight of latency
    "B2":           {"type": "float", "low": 0.0, "high": 2.0, "default": 0.20},   # bias

    # -- Input Chunking (gamma) per Mathematics.md §9 --
    "W6":           {"type": "float", "low": 0.5, "high": 6.0, "default": 2.50},   # weight of L_in/1000
    "B3":           {"type": "float", "low": 0.5, "high": 3.0, "default": 1.00},   # bias

    # -- Priority Urgency Scaling per Mathematics.md §21-22 --
    "URG_SCALE":    {"type": "float", "low": 0.5, "high": 2.5, "default": 1.20},
}

DEFAULT_KNOBS: dict[str, Any] = {name: spec["default"] for name, spec in KNOB_SPECS.items()}



def suggest_knobs(trial: optuna.Trial) -> dict[str, Any]:
    """Suggest all parameters in unified search space."""
    params = {}
    for name, spec in KNOB_SPECS.items():
        if spec["type"] == "int":
            params[name] = trial.suggest_int(name, spec["low"], spec["high"])
        elif spec.get("log", False):
            params[name] = trial.suggest_float(name, spec["low"], spec["high"], log=True)
        else:
            params[name] = trial.suggest_float(name, spec["low"], spec["high"])
    return params


def mutate_knobs(
    base_knobs: dict[str, Any],
    scale: float = 0.10,
    discrete_flip_prob: float = 0.15,
    rng: random.Random | None = None,
) -> dict[str, Any]:
    """Generate a mutated neighbor of base_knobs with multi-scale noise."""
    r = rng or random.Random()
    mutated = {}

    for name, spec in KNOB_SPECS.items():
        cur_val = base_knobs.get(name, spec["default"])
        low, high = spec["low"], spec["high"]

        if spec["type"] == "int":
            if r.random() < discrete_flip_prob:
                if high - low <= 2:
                    # Discrete flip
                    mutated[name] = low if cur_val == high else high
                else:
                    # Integer shift or random choice
                    shift = r.choice([-2, -1, 1, 2])
                    mutated[name] = max(low, min(high, int(cur_val + shift)))
            else:
                mutated[name] = int(cur_val)
        elif spec.get("log", False):
            # Log-scale Gaussian perturbation
            cur_log = math.log10(max(cur_val, low))
            log_low, log_high = math.log10(low), math.log10(high)
            new_log = cur_log + r.gauss(0.0, scale * (log_high - log_low))
            mutated[name] = float(10.0 ** max(log_low, min(log_high, new_log)))
        else:
            # Multiplicative log-normal or additive Gaussian perturbation
            if cur_val > 0 and low > 0 and high / low > 10.0:
                # Wide range: multiplicative noise
                mult = math.exp(r.gauss(0.0, scale))
                new_val = cur_val * mult
            else:
                # Narrow / fraction range: bounded additive noise
                span = high - low
                new_val = cur_val + r.gauss(0.0, scale * span)
            mutated[name] = float(max(low, min(high, new_val)))

    return mutated


def knobs_to_env(knobs: dict[str, Any]) -> dict[str, str]:
    """Convert knob dict to V4_* environment variables."""
    env = os.environ.copy()
    for name, val in knobs.items():
        env[f"V4_{name}"] = str(val)
    return env


def export_champion_env(knobs: dict[str, Any], filepath: Path) -> None:
    """Export champion knobs to ready-to-source bash .env file."""
    filepath.parent.mkdir(parents=True, exist_ok=True)
    with open(filepath, "w", encoding="utf-8") as f:
        f.write("# Scheduler Champion Knobs -- Auto-generated by Trainer.py\n")
        f.write(f"# Timestamp: {datetime.datetime.now().isoformat()}\n\n")
        for name, val in sorted(knobs.items()):
            f.write(f"export V4_{name}={val}\n")


def export_champion_json(metadata: dict[str, Any], filepath: Path) -> None:
    """Export champion metrics and metadata to JSON."""
    filepath.parent.mkdir(parents=True, exist_ok=True)
    with open(filepath, "wb") as f:
        f.write(orjson.dumps(metadata, option=orjson.OPT_INDENT_2))


def count_jsonl_lines(path: Path) -> int:
    """Count non-empty lines in a JSONL file."""
    if not path.exists():
        return 0
    count = 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.strip():
                count += 1
    return count


# ============================ EVALUATION & FITNESS ============================

def run_simulation_subprocess(
    exe_path: Path,
    dataset_path: Path,
    knobs: dict[str, Any],
    threads: int = 0,
) -> dict[str, Any]:
    """Invoke Simulator.exe in a subprocess with candidate environment."""
    cmd = [str(exe_path), "--json", "-i", str(dataset_path)]
    if threads > 0:
        cmd.extend(["-t", str(threads)])

    env = knobs_to_env(knobs)
    res = subprocess.run(cmd, capture_output=True, env=env)
    if res.returncode != 0:
        raise RuntimeError(
            f"Simulator failed with return code {res.returncode}:\n{res.stderr.decode('utf-8', errors='replace')}"
        )

    return orjson.loads(res.stdout)


def compute_fitness(
    payload: dict[str, Any],
    alpha: float = 0.7,
    fitness_mode: str = "avg_only",
    beta: float = 0.05,
    power: float = 0.5,
    lambda_invalid: float = 1.0,
) -> tuple[float, float, float, dict[str, float]]:
    """
    Compute robust monotonic fitness from simulation results.

    Modes:
    - 'power_mean': Concave generalized power-mean raising profile floors while preserving
      non-zero gradients across ALL profiles (immune to 0-score bounded profiles).
    - 'avg_only': Direct arithmetic mean score with invalid penalty (exact contest target).
    - 'soft_min': Legacy LogSumExp soft-min formulation.

    Returns:
        (fitness, avg_score, floor_metric, per_profile_averages)
    """
    results = payload.get("results", [])
    total = payload.get("total_testcases", len(results))
    valid_count = payload.get("valid_count", 0)
    invalid_count = total - valid_count

    if not results:
        return -10000.0, 0.0, 0.0, {}

    profile_scores: dict[str, list[float]] = collections.defaultdict(list)
    all_scores: list[float] = []

    for r in results:
        score = float(r.get("score", 0.0)) if r.get("valid", False) else 0.0
        pname = r.get("profile", "unknown")
        profile_scores[pname].append(score)
        all_scores.append(score)

    avg_score = sum(all_scores) / max(len(all_scores), 1)

    profile_means: dict[str, float] = {
        pname: (sum(scores) / max(len(scores), 1))
        for pname, scores in profile_scores.items()
    }

    floor_metric = 0.0
    if profile_means:
        p_vals = list(profile_means.values())
        if fitness_mode == "power_mean":
            # Generalized concave power mean: M_p(x) = (1/K * sum((p_i + 1)^p))^(1/p) - 1
            # Provides strictly positive, smooth gradients for all profiles across [0, 1000]
            mean_powered = sum((max(0.0, p) + 1.0) ** power for p in p_vals) / len(p_vals)
            floor_metric = (mean_powered ** (1.0 / power)) - 1.0
        elif fitness_mode == "soft_min":
            # Legacy soft-min
            m = min(p_vals)
            floor_metric = m - math.log(sum(math.exp(-beta * (p - m)) for p in p_vals)) / beta
        else:  # avg_only
            floor_metric = avg_score
    else:
        floor_metric = avg_score

    invalid_ratio = invalid_count / max(total, 1)
    invalid_penalty = lambda_invalid * invalid_ratio * 1000.0

    if fitness_mode == "avg_only":
        fitness = avg_score - invalid_penalty
    else:
        fitness = alpha * avg_score + (1.0 - alpha) * floor_metric - invalid_penalty

    return fitness, avg_score, floor_metric, profile_means


# ============================ ELITE CANDIDATE POOL ============================

class EliteCandidatePool:
    """Non-lossy priority tracking of top-performing candidate knob sets."""

    def __init__(self, capacity: int = 15):
        self.capacity = max(5, capacity)
        # List of dicts: {"fitness", "avg_score", "knobs", "trial_number", "val_score", "val_valid", "val_total", "validated"}
        self.pool: list[dict[str, Any]] = []

    def add(self, knobs: dict[str, Any], fitness: float, avg_score: float, trial_number: int) -> bool:
        """Add candidate to elite pool if competitive."""
        # Check for near-identical duplicate in pool
        for item in self.pool:
            if abs(item["fitness"] - fitness) < 1e-4:
                return False

        entry = {
            "fitness": fitness,
            "avg_score": avg_score,
            "knobs": knobs.copy(),
            "trial_number": trial_number,
            "val_score": None,
            "val_valid": None,
            "val_total": None,
            "validated": False,
        }

        self.pool.append(entry)
        self.pool.sort(key=lambda x: x["fitness"], reverse=True)

        if len(self.pool) > self.capacity:
            self.pool = self.pool[:self.capacity]
            # Returns True if candidate survived in pool
            return any(x["trial_number"] == trial_number for x in self.pool)
        return True

    def get_unvalidated(self) -> list[dict[str, Any]]:
        """Retrieve candidates that have not yet been evaluated on validation set."""
        return [c for c in self.pool if not c["validated"]]

    def mark_validated(self, trial_number: int, val_score: float, val_valid: int, val_total: int) -> None:
        """Update candidate validation outcomes."""
        for c in self.pool:
            if c["trial_number"] == trial_number:
                c["val_score"] = val_score
                c["val_valid"] = val_valid
                c["val_total"] = val_total
                c["validated"] = True
                break

    def get_top(self, k: int = 5) -> list[dict[str, Any]]:
        """Get top k elite candidates."""
        return self.pool[:k]

    def best_train_fitness(self) -> float:
        """Return highest train fitness in pool."""
        return self.pool[0]["fitness"] if self.pool else -1e9


# ============================ TRAINER ORCHESTRATOR ============================

class KnobTrainer:
    def __init__(
        self,
        study_name: str = "scheduler_knobs",
        storage: str = "sqlite:///study.db",
        train_size: int = 16384,
        val_size: int = 8192,
        holdout_size: int = 8192,
        threads: int = 0,
        val_every: int = 10,
        startup_ratio: float = 0.15,
        stagnation_ratio: float = 0.06,
        alpha: float = 0.70,
        fitness_mode: str = "avg_only",
        power: float = 0.50,
        beta: float = 0.05,
        lambda_invalid: float = 1.0,
        seed: int = 42,
        calibration_dir: Path | str | None = None,
        enable_calibration: bool = True,
        output_dir: Path | None = None,
    ):
        self.project_root = Path(__file__).resolve().parent.parent
        self.output_dir = output_dir or (self.project_root / "Artifacts")
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.train_dir = self.project_root / "Testcases" / "Train"
        self.train_dir.mkdir(parents=True, exist_ok=True)

        self.calibration_dir = Path(calibration_dir) if calibration_dir else (self.project_root / "Calibration")
        self.enable_calibration = enable_calibration

        self.train_path = self.train_dir / "train.jsonl"
        self.val_path = self.train_dir / "val.jsonl"
        self.holdout_path = self.train_dir / "holdout.jsonl"

        self.study_name = study_name
        self.storage = storage
        self.train_size = train_size
        self.val_size = val_size
        self.holdout_size = holdout_size
        self.threads = threads
        self.val_every = val_every
        self.startup_ratio = startup_ratio
        self.stagnation_ratio = stagnation_ratio
        self.alpha = alpha
        self.fitness_mode = fitness_mode
        self.power = power
        self.beta = beta
        self.lambda_invalid = lambda_invalid
        self.seed = seed
        self.rng = random.Random(seed)

        # Champion state tracking
        self.champion_val_score: float = -1.0
        self.champion_effective_val: float = -1.0
        self.champion_train_score: float = -1.0
        self.champion_val_valid: int = 0
        self.champion_val_total: int = 0
        self.champion_knobs: dict[str, Any] = DEFAULT_KNOBS.copy()
        self.champion_trial: int = -1
        self.promotions_count: int = 0
        self.last_promotion_trial: int = 0

        # Optimization phase state machine
        # Phases: 'TPE_GLOBAL' -> 'CMAES_REFINE' -> 'ELITE_MUTATE' -> 'TPE_RECYCLE'
        self.current_phase: str = "TPE_GLOBAL"
        self.phase_start_trial: int = 0
        self.stagnation_window: int = 80
        self.cmaes_restarts_count: int = 0

        # Elite candidate pool
        self.elite_pool = EliteCandidatePool(capacity=15)
        self.all_time_best_train_fitness: float = -1e9

        # Compile simulator binary if needed
        self.exe_path = ensure_binary()

    def ensure_datasets(self) -> None:
        """Verify datasets exist and contain at least the requested number of testcases. Auto-calibrates against Calibration/ if records exist."""
        train_count = count_jsonl_lines(self.train_path)
        val_count = count_jsonl_lines(self.val_path)
        holdout_count = count_jsonl_lines(self.holdout_path)

        needs_gen = (train_count < self.train_size) or (val_count < self.val_size) or (holdout_count < self.holdout_size)

        if not needs_gen:
            print(f"[Dataset] Verified existing datasets: Train={train_count}, Val={val_count}, Holdout={holdout_count}")
            return

        # Ingest calibration records if enabled
        cal_records = []
        if self.enable_calibration and self.calibration_dir.exists():
            cal_records = load_calibration_records(self.calibration_dir)

        if cal_records:
            print(f"\n[Dataset] Loaded {len(cal_records)} Real Judge calibration run(s) from {self.calibration_dir}:")
            for idx, rec in enumerate(cal_records, start=1):
                print(f"  [{idx}] {rec.name:<25} Real Score: {rec.real_score:.4f}")
            print(f"[Dataset] Auto-calibration ENABLED: Enforcing zero-score filtering and suite rank monotonicity.\n")
        else:
            if self.enable_calibration:
                print(f"[Dataset] No calibration files found in {self.calibration_dir}. Running standard generator.")

        def _generate(count_needed: int, path: Path, seed: int, name: str) -> None:
            if cal_records:
                print(f"[Dataset] Generating {count_needed} CALIBRATED testcases for {name} -> {path}...")
                prof_counts, sim_avgs, tau, rejections = generate_calibrated_dataset(
                    num_testcases=count_needed,
                    output_path=path,
                    calibration_records=cal_records,
                    simulator_exe=self.exe_path,
                    seed=seed,
                    threads=self.threads,
                    verbose=False,
                )
                print_calibration_report(cal_records, sim_avgs, tau, count_needed, rejections)
            else:
                print(f"[Dataset] Generating {count_needed} testcases for {name} -> {path}...")
                generate_dataset(count_needed, path, seed=seed)

        if train_count != self.train_size:
            print(f"[Dataset] Train set has {train_count} testcases (need {self.train_size}). Regenerating...")
            _generate(self.train_size, self.train_path, self.seed, "Train")

        if val_count != self.val_size:
            print(f"[Dataset] Val set has {val_count} testcases (need {self.val_size}). Regenerating...")
            _generate(self.val_size, self.val_path, self.seed + 1000, "Validation")

        if holdout_count != self.holdout_size:
            print(f"[Dataset] Holdout set has {holdout_count} testcases (need {self.holdout_size}). Regenerating...")
            _generate(self.holdout_size, self.holdout_path, self.seed + 2000, "Holdout")


    def evaluate_dataset(self, dataset_path: Path, knobs: dict[str, Any]) -> tuple[float, dict[str, float], int, int]:
        """Evaluate a dataset and return (avg_score, profile_means, valid_count, total)."""
        payload = run_simulation_subprocess(self.exe_path, dataset_path, knobs, threads=self.threads)
        results = payload.get("results", [])
        total = payload.get("total_testcases", len(results))
        valid_cnt = payload.get("valid_count", 0)

        profile_scores = collections.defaultdict(list)
        all_scores = []
        for r in results:
            score = float(r.get("score", 0.0)) if r.get("valid", False) else 0.0
            profile_scores[r.get("profile", "unknown")].append(score)
            all_scores.append(score)

        avg_score = sum(all_scores) / max(len(all_scores), 1)
        profile_means = {p: (sum(s) / max(len(s), 1)) for p, s in profile_scores.items()}
        return avg_score, profile_means, valid_cnt, total

    def evaluate_and_promote_champion(self, candidate_knobs: dict[str, Any], trial_number: int, train_score: float) -> bool:
        """Evaluate candidate on val.jsonl and promote if it improves effective validation score."""
        val_score, val_profiles, val_valid, val_total = self.evaluate_dataset(self.val_path, candidate_knobs)

        # Mark in elite pool
        self.elite_pool.mark_validated(trial_number, val_score, val_valid, val_total)

        effective_val = val_score * (val_valid / max(val_total, 1))
        min_valid_req = min(self.champion_val_valid, int(val_total * 0.999)) if self.champion_val_valid > 0 else val_total

        # Robust promotion gate: effective val score must exceed champion
        is_promoted = (
            (effective_val > self.champion_effective_val + 1e-4) and
            (val_valid >= min_valid_req)
        )

        if is_promoted:
            old_val = self.champion_val_score
            old_eff = self.champion_effective_val
            self.champion_val_score = val_score
            self.champion_effective_val = effective_val
            self.champion_train_score = train_score
            self.champion_val_valid = val_valid
            self.champion_val_total = val_total
            self.champion_knobs = candidate_knobs.copy()
            self.champion_trial = trial_number
            self.promotions_count += 1
            self.last_promotion_trial = trial_number

            # Export champion artifacts
            env_file = self.output_dir / "champion.env"
            json_file = self.output_dir / "champion.json"
            export_champion_env(candidate_knobs, env_file)

            meta = {
                "trial_id": trial_number,
                "promoted_at": datetime.datetime.now().isoformat(),
                "val_score": round(val_score, 4),
                "effective_val_score": round(effective_val, 4),
                "train_score": round(train_score, 4),
                "val_valid_count": val_valid,
                "val_total_testcases": val_total,
                "val_profile_scores": {k: round(v, 3) for k, v in sorted(val_profiles.items())},
                "knobs": candidate_knobs,
            }
            export_champion_json(meta, json_file)

            delta_str = f"+{val_score - max(0.0, old_val):.3f}" if old_val >= 0 else "Baseline"
            print(
                f"\n  ********************************************************************************\n"
                f"  *** NEW CHAMPION (Trial {trial_number:4d}): Val {old_val:.3f} -> {val_score:.3f} ({delta_str}) | Train: {train_score:.3f} | Valid: {val_valid}/{val_total} ***\n"
                f"  ********************************************************************************"
            )

            # Print top 3 weakest & strongest profile scores for live insight
            sorted_p = sorted(val_profiles.items(), key=lambda x: x[1])
            weakest = ", ".join(f"{k}: {v:.1f}" for k, v in sorted_p[:3])
            strongest = ", ".join(f"{k}: {v:.1f}" for k, v in sorted_p[-3:])
            print(f"      Weakest Profiles : {weakest}")
            print(f"      Strongest Profiles: {strongest}\n")

            return True
        else:
            print(f"  [Val Check] Trial {trial_number:4d}: Val = {val_score:.3f} (Champ Val: {self.champion_val_score:.3f}, Valid: {val_valid}/{val_total})")
            return False

    def check_holdout(self, knobs: dict[str, Any], trial_number: int) -> float:
        """Run unbiased evaluation against holdout.jsonl (report only)."""
        holdout_score, holdout_profiles, hold_valid, hold_total = self.evaluate_dataset(self.holdout_path, knobs)
        gap = self.champion_val_score - holdout_score
        print(f"  [Holdout] Champion from Trial {trial_number:4d}: Holdout = {holdout_score:.3f} (Val: {self.champion_val_score:.3f}, Gap: {gap:+.3f}, Valid: {hold_valid}/{hold_total})")
        return holdout_score

    def restore_state_from_study(self, study: optuna.Study) -> None:
        """Restore champion and elite state from existing SQLite study."""
        completed_trials = [t for t in study.trials if t.state == optuna.trial.TrialState.COMPLETE and t.value is not None]
        if not completed_trials:
            return

        print(f"\n[Resume] Found {len(completed_trials)} completed trials in study '{self.study_name}'. Restoring state...")

        # Reconstruct Elite Pool
        for t in sorted(completed_trials, key=lambda x: x.value, reverse=True)[:25]:
            self.elite_pool.add(t.params, t.value, t.value, t.number)

        best_trial = max(completed_trials, key=lambda t: t.value)
        self.all_time_best_train_fitness = best_trial.value
        best_knobs = best_trial.params.copy()

        # Re-evaluate best trial on current val set
        val_score, val_profiles, val_valid, val_total = self.evaluate_dataset(self.val_path, best_knobs)
        self.champion_val_score = val_score
        self.champion_effective_val = val_score * (val_valid / max(val_total, 1))
        self.champion_train_score = best_trial.value
        self.champion_val_valid = val_valid
        self.champion_val_total = val_total
        self.champion_knobs = best_knobs.copy()
        self.champion_trial = best_trial.number
        self.last_promotion_trial = max(t.number for t in study.trials)

        print(f"[Resume] Best trial in SQLite: Trial {best_trial.number} | Train Fitness: {best_trial.value:.4f} | Val: {val_score:.3f}")

        # Check if saved champion.json has better candidate
        json_file = self.output_dir / "champion.json"
        if json_file.exists():
            try:
                with open(json_file, "rb") as f:
                    saved = orjson.loads(f.read())
                saved_knobs = saved.get("knobs", {})
                if saved_knobs:
                    sv_score, _, sv_valid, sv_total = self.evaluate_dataset(self.val_path, saved_knobs)
                    sv_eff = sv_score * (sv_valid / max(sv_total, 1))
                    if sv_eff > self.champion_effective_val:
                        self.champion_val_score = sv_score
                        self.champion_effective_val = sv_eff
                        self.champion_knobs = saved_knobs.copy()
                        self.champion_trial = saved.get("trial_id", -1)
                        self.champion_train_score = saved.get("train_score", 0.0)
                        self.champion_val_valid = sv_valid
                        self.champion_val_total = sv_total
                        print(f"[Resume] champion.json was superior: Trial {self.champion_trial} | Val: {sv_score:.3f} (Valid: {sv_valid}/{sv_total})")
            except Exception as e:
                print(f"[Resume] Warning reading champion.json: {e}")

    def run_study(self, n_trials: int = 1500) -> optuna.Study:
        """Run adaptive Multi-Phase Optimization Study with relative budgets."""

        # Derive relative hyperparameters based on total trial budget
        startup_trials = max(10, min(250, int(n_trials * self.startup_ratio)))
        self.stagnation_window = max(20, min(120, int(n_trials * self.stagnation_ratio)))
        val_freq = max(5, self.val_every)

        print(f"\n================ Optimization Configuration ================")
        print(f"  Target Budget       : {n_trials} trials")
        print(f"  Startup Trials      : {startup_trials} ({self.startup_ratio * 100:.1f}%)")
        print(f"  Stagnation Window   : {self.stagnation_window} trials ({self.stagnation_ratio * 100:.1f}%)")
        print(f"  Validation Cadence  : every {val_freq} trials + immediate on record breaks")
        print(f"  Fitness Mode        : {self.fitness_mode} (alpha={self.alpha:.2f}, power={self.power:.2f})")
        print(f"  Parallel Threads    : {self.threads or 'all available'}")
        print(f"============================================================")

        # Initialize primary Multivariate TPE Sampler
        tpe_sampler = TPESampler(
            seed=self.seed,
            n_startup_trials=startup_trials,
            multivariate=True,
            group=True,
            constant_liar=True,
        )

        study = optuna.create_study(
            study_name=self.study_name,
            storage=self.storage,
            sampler=tpe_sampler,
            direction="maximize",
            load_if_exists=True,
        )

        cal_records = []
        if self.enable_calibration and self.calibration_dir.exists():
            cal_records = load_calibration_records(self.calibration_dir)

        existing_trials = len(study.trials)
        is_resume = existing_trials > 0

        if is_resume:
            self.restore_state_from_study(study)
            # Enqueue champion for warm start
            study.enqueue_trial(self.champion_knobs)
            print(f"[Resume] Enqueued champion knobs as trial {existing_trials} for warm-start anchor.")
            for rec in cal_records:
                self.elite_pool.add(rec.knobs, 0.0, 0.0, -1)
        else:
            # Seed Optuna with all calibration runs in descending score order
            if cal_records:
                print(f"\n[Warm-Start] Enqueuing {len(cal_records)} Real Judge calibration run(s) as anchor trials:")
                for rec in cal_records:
                    print(f"  -> Enqueued {rec.name:<22} (Real Score: {rec.real_score:.4f})")
                    study.enqueue_trial(rec.knobs)
                    self.elite_pool.add(rec.knobs, 0.0, 0.0, -1)
                best_cal = cal_records[0].knobs
            else:
                best_cal = DEFAULT_KNOBS

            # Evaluate top baseline on val set
            val_base, _, v_val, v_tot = self.evaluate_dataset(self.val_path, best_cal)
            self.champion_val_score = val_base
            self.champion_effective_val = val_base * (v_val / max(v_tot, 1))
            self.champion_train_score = val_base
            self.champion_val_valid = v_val
            self.champion_val_total = v_tot
            self.champion_knobs = best_cal.copy()
            print(f"[Baseline] Anchor Champion Val Score: {val_base:.3f} (Valid: {v_val}/{v_tot})\n")
            if not cal_records:
                study.enqueue_trial(DEFAULT_KNOBS)

        # Recent candidate buffer for window validation
        window_candidates: list[tuple[float, float, dict[str, Any], int]] = []
        TOP_K_WINDOW = 3

        def objective(trial: optuna.Trial) -> float:
            nonlocal window_candidates

            # -------------------- Phase Transition State Machine --------------------
            trials_since_promotion = trial.number - self.last_promotion_trial

            if trials_since_promotion >= self.stagnation_window:
                if self.current_phase == "TPE_GLOBAL":
                    # Transition: TPE -> IPOP CMA-ES
                    print(f"\n>>> [Phase Shift] Stagnation ({trials_since_promotion} trials). Switching to IPOP CMA-ES local refinement...")
                    self.current_phase = "CMAES_REFINE"
                    self.phase_start_trial = trial.number
                    self.last_promotion_trial = trial.number  # Reset counter for CMA-ES phase

                    cma_sampler = CmaEsSampler(
                        seed=self.seed + trial.number,
                        n_startup_trials=0,
                        with_margin=True,
                        restart_strategy="ipop",
                        inc_popsize=2,
                    )
                    study.sampler = cma_sampler
                    study.enqueue_trial(self.champion_knobs)

                elif self.current_phase == "CMAES_REFINE":
                    # Transition: CMA-ES -> Elite Mutation & Basin Jumps
                    print(f"\n>>> [Phase Shift] CMA-ES converged without promotion. Injecting Multi-Scale Elite Mutations & Basin Jumps...")
                    self.current_phase = "ELITE_MUTATE"
                    self.phase_start_trial = trial.number
                    self.last_promotion_trial = trial.number

                    # Enqueue multi-scale mutations around top elites
                    elites = self.elite_pool.get_top(5)
                    seeds_to_mutate = [e["knobs"] for e in elites] if elites else [self.champion_knobs]

                    mutant_count = 0
                    # Fine perturbations (sigma = 0.05)
                    for base in seeds_to_mutate[:3]:
                        study.enqueue_trial(mutate_knobs(base, scale=0.05, discrete_flip_prob=0.10, rng=self.rng))
                        mutant_count += 1
                    # Medium perturbations (sigma = 0.15)
                    for base in seeds_to_mutate:
                        study.enqueue_trial(mutate_knobs(base, scale=0.15, discrete_flip_prob=0.25, rng=self.rng))
                        mutant_count += 1
                    # Coarse exploratory basin jumps (sigma = 0.35)
                    for base in seeds_to_mutate[:2]:
                        study.enqueue_trial(mutate_knobs(base, scale=0.35, discrete_flip_prob=0.40, rng=self.rng))
                        mutant_count += 1

                    print(f"    Injected {mutant_count} multi-scale mutant candidates into optimization queue.")

                elif self.current_phase == "ELITE_MUTATE":
                    # Transition: Elite Mutation -> TPE Recycle
                    print(f"\n>>> [Phase Shift] Re-seeding Multivariate TPE to assimilate explored basins...")
                    self.current_phase = "TPE_GLOBAL"
                    self.phase_start_trial = trial.number
                    self.last_promotion_trial = trial.number

                    recycle_tpe = TPESampler(
                        seed=self.seed + trial.number * 31,
                        n_startup_trials=max(5, int(startup_trials * 0.25)),
                        multivariate=True,
                        group=True,
                        constant_liar=True,
                    )
                    study.sampler = recycle_tpe
                    study.enqueue_trial(self.champion_knobs)

            # -------------------- Candidate Evaluation --------------------
            knobs = suggest_knobs(trial)
            t0 = time.perf_counter()
            payload = run_simulation_subprocess(self.exe_path, self.train_path, knobs, threads=self.threads)
            elapsed = time.perf_counter() - t0

            fitness, avg_score, floor_metric, _ = compute_fitness(
                payload,
                alpha=self.alpha,
                fitness_mode=self.fitness_mode,
                power=self.power,
                beta=self.beta,
                lambda_invalid=self.lambda_invalid,
            )

            valid_cnt = payload.get("valid_count", 0)
            total = payload.get("total_testcases", 0)
            sims_per_sec = payload.get("sims_per_sec", 0.0)

            # Phase Tag
            phase_tags = {
                "TPE_GLOBAL": "TPE-Multi",
                "CMAES_REFINE": "CMA-IPOP",
                "ELITE_MUTATE": "Elite-Mut",
            }
            tag = phase_tags.get(self.current_phase, "TPE")
            status_tag = f"Valid: {valid_cnt}/{total}" if valid_cnt == total else f"INVALID: {total - valid_cnt}/{total}"

            print(
                f"[Trial {trial.number:4d}] Train: {avg_score:6.2f} | Fitness: {fitness:6.2f} | "
                f"ChampVal: {self.champion_val_score:6.2f} | {status_tag} | {tag:9s} | {sims_per_sec:4.0f} sim/s | {elapsed:.1f}s"
            )

            # -------------------- Elite Candidate Tracking --------------------
            if valid_cnt == total:
                self.elite_pool.add(knobs, fitness, avg_score, trial.number)
                window_candidates.append((fitness, avg_score, knobs.copy(), trial.number))
                window_candidates.sort(key=lambda x: x[0], reverse=True)
                if len(window_candidates) > TOP_K_WINDOW:
                    window_candidates = window_candidates[:TOP_K_WINDOW]

            # -------------------- Dual Validation Triggers --------------------
            # Trigger 1: Immediate Validation on Record Break
            if valid_cnt == total and fitness > self.all_time_best_train_fitness:
                old_best = self.all_time_best_train_fitness
                self.all_time_best_train_fitness = fitness
                if old_best > -1e8:
                    print(f"  [Record] New All-Time Best Train Fitness ({old_best:.3f} -> {fitness:.3f}). Running immediate validation...")
                    self.evaluate_and_promote_champion(knobs, trial.number, avg_score)

            # Trigger 2: Periodic Batch Validation Gate
            if (trial.number + 1) % val_freq == 0:
                # 1. Validate top window candidates
                for cand_fit, cand_avg, cand_knobs, cand_tnum in window_candidates:
                    if self.evaluate_and_promote_champion(cand_knobs, cand_tnum, cand_avg):
                        break

                # 2. Also validate any unvalidated candidates in global elite pool
                unvalidated = self.elite_pool.get_unvalidated()
                if unvalidated:
                    for elite_item in unvalidated:
                        self.evaluate_and_promote_champion(
                            elite_item["knobs"],
                            elite_item["trial_number"],
                            elite_item["avg_score"],
                        )

                window_candidates.clear()

            return fitness

        print(f"\n================ Starting Optimization ({n_trials} trials) ================")
        study.optimize(objective, n_trials=n_trials)

        # Final champion evaluation against holdout
        print("\n================ Finalizing Study & Champion ================")
        if self.champion_val_score > 0:
            final_holdout = self.check_holdout(self.champion_knobs, self.champion_trial)
            print(f"\nFinal Champion (Trial {self.champion_trial}):")
            print(f"  Train Score         : {self.champion_train_score:.3f}")
            print(f"  Val Score           : {self.champion_val_score:.3f}")
            print(f"  Effective Val Score : {self.champion_effective_val:.3f}")
            print(f"  Holdout Score       : {final_holdout:.3f}")
            print(f"  Exported Artifacts  :")
            print(f"    - {self.output_dir / 'champion.env'}")
            print(f"    - {self.output_dir / 'champion.json'}\n")

        return study


# ============================ CLI & MAIN ============================

def main():
    parser = argparse.ArgumentParser(
        description="High-Performance Multi-Phase Optuna Optimizer for Scheduler Knobs.",
    )
    parser.add_argument("--trials", "-n", type=int, default=1500, help="Number of trials to run (default: 1500).")
    parser.add_argument("--train-size", type=int, default=16384, help="Number of testcases in train set (default: 16384).")
    parser.add_argument("--val-size", type=int, default=8192, help="Number of testcases in validation set (default: 8192).")
    parser.add_argument("--holdout-size", type=int, default=8192, help="Number of testcases in holdout set (default: 8192).")
    parser.add_argument("--threads", "-t", "-j", type=int, default=0, help="OpenMP worker threads for Simulator (default: hardware max).")
    parser.add_argument("--val-every", type=int, default=10, help="Evaluate champion on val set every M trials (default: 10).")
    parser.add_argument("--startup-ratio", type=float, default=0.15, help="Fraction of trials allocated to random startup exploration (default: 0.15 = 15%%).")
    parser.add_argument("--stagnation-ratio", type=float, default=0.06, help="Fraction of trials without improvement before phase shift (default: 0.06 = 6%%).")
    parser.add_argument("--fitness-mode", type=str, default="avg_only", choices=["avg_only", "power_mean", "soft_min"], help="Fitness calculation formulation (default: avg_only).")
    parser.add_argument("--alpha", type=float, default=0.70, help="Weight on overall mean score in fitness (default: 0.70).")
    parser.add_argument("--power", type=float, default=0.50, help="Exponent p for generalized power-mean floor metric (default: 0.50).")
    parser.add_argument("--study-name", type=str, default="study", help="Optuna study name.")
    parser.add_argument("--storage", type=str, default="sqlite:///study.db", help="Optuna SQLite storage URI (default: sqlite:///study.db).")
    parser.add_argument("--seed", type=int, default=42, help="Random seed (default: 42).")
    parser.add_argument("--calibration-dir", type=str, default="Calibration", help="Directory with Real Judge calibration runs (default: Calibration).")
    parser.add_argument("--no-calibration", action="store_true", help="Disable calibration when auto-generating testcases.")

    args = parser.parse_args()

    trainer = KnobTrainer(
        study_name=args.study_name,
        storage=args.storage,
        train_size=args.train_size,
        val_size=args.val_size,
        holdout_size=args.holdout_size,
        threads=args.threads,
        val_every=args.val_every,
        startup_ratio=args.startup_ratio,
        stagnation_ratio=args.stagnation_ratio,
        fitness_mode=args.fitness_mode,
        alpha=args.alpha,
        power=args.power,
        seed=args.seed,
        calibration_dir=args.calibration_dir,
        enable_calibration=not args.no_calibration,
    )

    # Automatically check testcase counts and generate if fewer than requested
    trainer.ensure_datasets()

    trainer.run_study(n_trials=args.trials)


if __name__ == "__main__":
    main()
