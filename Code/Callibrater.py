import os
import json
import random
import math
from statistics import mean
import Generator
import Simulator

# 1. DEFINE YOUR REAL JUDGE SCORES HERE
REAL_JUDGE_SCORES = {
    "Schedulers/schedulerA": 12040.376,
    "Schedulers/schedulerB": 833.636,
    #"Schedulers/schedulerC": 12019.115,
    #"Schedulers/schedulerD": 13416.759,
    #"Schedulers/schedulerF": 13416.759,
    "Schedulers/schedulerG": 15574.578,
    #"Schedulers/schedulerH": 12873.409,
    "Schedulers/schedulerI": 13932.366,
    #"Schedulers/schedulerJ": 14611.362,
    "Schedulers/schedulerK": 15570.559,
    #"Schedulers/schedulerL": 12959.146,
    "Schedulers/schedulerM": 15844.316,
    #"Schedulers/schedulerN": 14735.223,
    "Schedulers/schedulerO": 15659.345,
    "Schedulers/schedulerP": 14333.189,
    "Schedulers/schedulerQ": 15968.34,
    #"Schedulers/schedulerR": 12149.604,
    "Schedulers/schedulerS": 15671.682,
    "Schedulers/schedulerT": 15960.639,
    "Schedulers/schedulerU": 15872.057,
    "Schedulers/schedulerV": 15968.34,
    "Schedulers/schedulerW": 16022.176,
    "Schedulers/schedulerX": 16026.039,
    "Schedulers/schedulerY": 16039.325,
    "Schedulers/schedulerZ": 16040.685,
}

POOL_SIZE = 10000
TARGET_SET_SIZE = 400
TIMEOUT_S = 60.0
WORKERS = os.cpu_count() or 4
CACHE_FILE = "score_matrix_cache.json"


def main():
    global REAL_JUDGE_SCORES

    if os.name == 'nt':
        REAL_JUDGE_SCORES = {
            (k if k.endswith('.exe') else f"{k}.exe"): v
            for k, v in REAL_JUDGE_SCORES.items()
        }

    sorted_versions = sorted(REAL_JUDGE_SCORES.keys(), key=lambda v: REAL_JUDGE_SCORES[v], reverse=True)

    print(f"Generating massive pool of {POOL_SIZE} cases...")
    pool_cases = Generator.generate(
        n_cases=POOL_SIZE,
        seed=42,
        profile="mixed",
        token_budget=Generator.TOKEN_BUDGET,
        max_R=Generator.MAX_R,
        probe=False
    )

    score_matrix = {}
    if os.path.exists(CACHE_FILE):
        print(f"Found existing cache at '{CACHE_FILE}'. Loading previous results...")
        with open(CACHE_FILE, "r") as f:
            score_matrix = json.load(f)

    print("Evaluating all versions against the massive pool...")
    for binary in sorted_versions:
        if binary in score_matrix and len(score_matrix[binary]) == POOL_SIZE:
            print(f"Skipping {binary}, full results found in cache.")
            continue

        print(f"Running {binary}...")
        results = Simulator.run_many(pool_cases, env_overrides={}, timeout_s=TIMEOUT_S, workers=WORKERS, binary=binary)
        score_matrix[binary] = [r.score for r in results]

        with open(CACHE_FILE, "w") as f:
            json.dump(score_matrix, f)
        print(f"--> Saved progress for {binary} to cache.")

    print("\nSearching for a rank-preserving subset using Simulated Annealing...")

    # Pre-calculate normalized real judge scores for accurate MSE spacing
    real_scores = list(REAL_JUDGE_SCORES.values())
    min_real, max_real = min(real_scores), max(real_scores)
    norm_real = {k: (REAL_JUDGE_SCORES[k] - min_real) / (max_real - min_real + 1e-9) for k in sorted_versions}

    def get_metrics(sums):
        means = {b: sums[b] / TARGET_SET_SIZE for b in sorted_versions}
        violations = 0
        for i in range(len(sorted_versions) - 1):
            for j in range(i + 1, len(sorted_versions)):
                # Only check for violations if it is a strict inequality on the real judge
                if REAL_JUDGE_SCORES[sorted_versions[i]] > REAL_JUDGE_SCORES[sorted_versions[j]]:
                    if means[sorted_versions[i]] <= means[sorted_versions[j]]:
                        violations += 1

        min_sub, max_sub = min(means.values()), max(means.values())
        mse = 0
        for b in sorted_versions:
            norm_sub = (means[b] - min_sub) / (max_sub - min_sub + 1e-9)
            mse += (norm_real[b] - norm_sub) ** 2

        return violations, mse, means

    best_subset = random.sample(range(POOL_SIZE), TARGET_SET_SIZE)
    current_subset = set(best_subset)
    remaining_pool = set(range(POOL_SIZE)) - current_subset

    # Track sums dynamically for O(1) performance per iteration
    current_sums = {b: sum(score_matrix[b][i] for i in current_subset) for b in sorted_versions}
    current_violations, current_mse, _ = get_metrics(current_sums)

    best_violations = current_violations
    best_mse = current_mse
    best_subset_final = list(current_subset)

    T = 1.0
    cooling_rate = 0.99995

    # Run up to 200,000 iterations (takes <5 seconds due to O(1) updates)
    for iteration in range(200000):
        out_idx = random.choice(tuple(current_subset))
        in_idx = random.choice(tuple(remaining_pool))

        new_sums = {b: current_sums[b] + score_matrix[b][in_idx] - score_matrix[b][out_idx] for b in sorted_versions}
        new_violations, new_mse, _ = get_metrics(new_sums)

        # Heavily penalize ordinal violations, use MSE to fine-tune spacing
        cost_diff = (new_violations - current_violations) + 5.0 * (new_mse - current_mse)

        if cost_diff < 0 or math.exp(min(0, -cost_diff / T)) > random.random():
            current_subset.remove(out_idx)
            current_subset.add(in_idx)
            remaining_pool.remove(in_idx)
            remaining_pool.add(out_idx)

            current_sums = new_sums
            current_violations = new_violations
            current_mse = new_mse

            if (new_violations < best_violations) or (new_violations == best_violations and new_mse < best_mse):
                best_violations = new_violations
                best_mse = new_mse
                best_subset_final = list(current_subset)

        T *= cooling_rate
        if iteration % 20000 == 0:
            print(
                f"Iter {iteration:6d} | Temp: {T:.4f} | Violations: {current_violations} (Best: {best_violations}) | MSE: {current_mse:.5f}")

        if best_violations == 0 and best_mse < 0.005:
            print(f"Perfect subset found early at iteration {iteration}!")
            break

    print(f"\nFinal Subset Violations: {best_violations}")

    _, _, final_means = get_metrics({b: sum(score_matrix[b][i] for i in best_subset_final) for b in sorted_versions})
    print("Mean scores on this subset:")
    for binary in sorted_versions:
        print(f"{binary:<25} {final_means[binary]:>8.2f} (Real Judge: {REAL_JUDGE_SCORES[binary]})")

    final_cases = [pool_cases[i] for i in best_subset_final]
    with open("calibrated_cases.jsonl", "w") as f:
        for c in final_cases:
            f.write(json.dumps(c) + "\n")

    print("\nSaved rank-preserving cases to 'calibrated_cases.jsonl'.")


if __name__ == "__main__":
    main()