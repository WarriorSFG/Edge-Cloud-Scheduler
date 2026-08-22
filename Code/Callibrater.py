import os
import json
import random
from statistics import mean
import Generator
import Simulator

# 1. DEFINE YOUR REAL JUDGE SCORES HERE
REAL_JUDGE_SCORES = {
    "Schedulers/schedulerA": 12040.376,
    "Schedulers/schedulerB": 833.636,
    "Schedulers/schedulerC": 12019.115,
    "Schedulers/schedulerD": 13416.759,
    "Schedulers/schedulerF": 13416.759,
    "Schedulers/schedulerG": 15574.578,
    "Schedulers/schedulerH": 12873.409,
    "Schedulers/schedulerI": 13932.366,
    "Schedulers/schedulerJ": 14611.362,
    "Schedulers/schedulerK": 15570.559,
    "Schedulers/schedulerL": 12959.146,
    "Schedulers/schedulerM": 15844.316,
    "Schedulers/schedulerN": 14735.223,
    "Schedulers/schedulerO": 15659.345,
    "Schedulers/schedulerP": 14333.189,
    "Schedulers/schedulerQ": 15968.34,
    "Schedulers/schedulerR": 12149.604,
    "Schedulers/schedulerS": 15671.682,
    "Schedulers/schedulerT": 15960.639,
    "Schedulers/schedulerU": 15872.057,
    "Schedulers/schedulerV": 15968.34,
    "Schedulers/schedulerW": 16022.176,
    "Schedulers/schedulerX": 16026.039,
    "Schedulers/schedulerY": 16039.325,
    "Schedulers/schedulerZ": 16040.685,
}

POOL_SIZE = 10000  # Total cases to generate for the search pool
TARGET_SET_SIZE = 500  # How many cases you want in your final calibrated set
TIMEOUT_S = 60.0  # Timeout per run
WORKERS = os.cpu_count() or 4
CACHE_FILE = "score_matrix_cache.json"


def main():
    global REAL_JUDGE_SCORES

    # Windows OS safeguard: automatically append .exe so the subprocess doesn't throw a FileNotFoundError
    if os.name == 'nt':
        REAL_JUDGE_SCORES = {
            (k if k.endswith('.exe') else f"{k}.exe"): v
            for k, v in REAL_JUDGE_SCORES.items()
        }

    # Sort versions by real judge score (descending)
    sorted_versions = sorted(REAL_JUDGE_SCORES.keys(), key=lambda v: REAL_JUDGE_SCORES[v], reverse=True)

    print(f"Generating massive pool of {POOL_SIZE} cases...")
    # Generate cases using your Generator.py logic
    pool_cases = Generator.generate(
        n_cases=POOL_SIZE,
        seed=42,
        profile="mixed",
        token_budget=Generator.TOKEN_BUDGET,
        max_R=Generator.MAX_R,
        probe=False
    )

    # Load existing cache if it exists
    score_matrix = {}
    if os.path.exists(CACHE_FILE):
        print(f"Found existing cache at '{CACHE_FILE}'. Loading previous results...")
        with open(CACHE_FILE, "r") as f:
            score_matrix = json.load(f)

    # Evaluate all versions against all cases
    print("Evaluating all versions against the massive pool...")
    for binary in sorted_versions:
        # Skip if we already have a complete run for this binary
        if binary in score_matrix and len(score_matrix[binary]) == POOL_SIZE:
            print(f"Skipping {binary}, full results found in cache.")
            continue

        print(f"Running {binary}...")
        results = Simulator.run_many(pool_cases, env_overrides={}, timeout_s=TIMEOUT_S, workers=WORKERS, binary=binary)
        score_matrix[binary] = [r.score for r in results]

        # Save to disk immediately after the binary finishes evaluating
        with open(CACHE_FILE, "w") as f:
            json.dump(score_matrix, f)
        print(f"--> Saved progress for {binary} to cache.")

    print("\nSearching for a rank-preserving subset...")

    # A simple greedy search to find a subset of cases that preserves the rank order
    best_subset = []
    best_violations = float('inf')

    # Try 5000 random subsets to find one that perfectly matches the real judge order
    for iteration in range(5000):
        candidate_idx = random.sample(range(POOL_SIZE), TARGET_SET_SIZE)

        # Calculate mean scores for this subset
        subset_means = {}
        for binary in sorted_versions:
            subset_scores = [score_matrix[binary][i] for i in candidate_idx]
            subset_means[binary] = mean(subset_scores)

        # Check how many ranking violations occur
        violations = 0
        for i in range(len(sorted_versions) - 1):
            v_better = sorted_versions[i]
            v_worse = sorted_versions[i + 1]
            if subset_means[v_better] <= subset_means[v_worse]:
                violations += 1

        if violations < best_violations:
            best_violations = violations
            best_subset = candidate_idx

        if best_violations == 0:
            print(f"Perfect subset found at iteration {iteration}!")
            break

    # Output the results
    print(f"\nFinal Subset Violations: {best_violations} (0 means perfect rank ordering)")
    print("Mean scores on this subset:")
    for binary in sorted_versions:
        subset_scores = [score_matrix[binary][i] for i in best_subset]
        print(f"{binary:<25} {mean(subset_scores):>8.2f} (Real Judge: {REAL_JUDGE_SCORES[binary]})")

    # Export the calibrated cases
    final_cases = [pool_cases[i] for i in best_subset]
    with open("calibrated_cases.jsonl", "w") as f:
        for c in final_cases:
            f.write(json.dumps(c) + "\n")

    print("\nSaved rank-preserving cases to 'calibrated_cases.jsonl'.")
    print("Point Trainer.py to this file for accurate tuning!")


if __name__ == "__main__":
    main()