# Scheduler — Code Scripts

## Generator.py

### Script name
`Generator.py`

### Script function
Generates diverse, high-quality test cases for the Scheduler problem using **19 stress-test profiles**. Each profile targets a different scheduler dimension (e.g. network bottleneck, fast network, heavy transfer, compute bottleneck, high schedule cost, throughput-only, latency-only, balanced, topology, burst/streaming arrivals, layer splitting, and adversarial setups) to prevent overfitting while ensuring comprehensive coverage. Test cases are distributed evenly across profiles by default.

Output is written as a **JSONL** file (one JSON object per line) into `Testcases/Raw/`.

### Arguments

| Argument | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `num_testcases` | `int` | **Yes** | — | Number of test cases to generate. |
| `--output`, `-o` | `str` | No | `testcases.jsonl` | Output filename (placed inside `Testcases/Raw/`). |
| `--seed`, `-s` | `int` | No | `None` | Random seed for reproducible generation. |
| `--profile`, `-p` | `str` | No | `None` (mixed) | Force all test cases to use a single named profile. |
| `--list-profiles` | flag | No | — | Print available profile names and exit. |

### Script usage

```bash
# Generate 30 mixed-profile test cases (at least 1 of each profile)
python Code/Generator.py 30

# Generate 50 test cases with a fixed seed for reproducibility
python Code/Generator.py 50 --seed 42

# Generate 10 network-bottleneck-only test cases
python Code/Generator.py 10 --profile network_bottleneck

# List all available profiles
python Code/Generator.py --list-profiles

# Custom output filename
python Code/Generator.py 100 -o stress_batch.jsonl -s 123
```

---

## Simulator.py / Simulator (C++)

### Script name
`Simulator.py` (CLI wrapper) / `Simulator.cpp` (OpenMP-accelerated C++ core)

### Script function
High-performance, parallel discrete-event simulation engine that evaluates schedulers on test cases with complete fidelity to the protocol and scoring model specified in `ProblemStatement.md`.

- **Faithful Simulation**: Replicates all event frames (`ARR`, `TDN`, `XDN`, `FIN`), local/remote server tracking, schedule cost $S$, piece splitting duration arithmetic, independent FIFO UP/DOWN link latency/bandwidth queues, and exact score calculation (Throughput component, TDR/TPOT waiting-time component, SLO excess calculation, and $0..1000$ scoring).
- **Blazingly Fast Parallelism**: Multi-threaded execution via OpenMP with zero heap allocations per event in inner simulation loops, achieving hundreds to thousands of full simulations per second across CPU cores.
- **Automatic Build**: Automatically detects and invokes C++ compilers (`g++`, `clang++`, or `cl`) with `-O3 -fopenmp` optimization flags.

### Arguments

| Argument | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `testcases` | `str` | No | `Testcases/Raw/testcases.jsonl` | Path to JSONL testcases file. |
| `--threads`, `-t`, `-j` | `int` | No | `0` (hardware max) | Number of parallel OpenMP worker threads. |
| `--repeat`, `-r` | `int` | No | `1` | Number of times to repeat test suite evaluation for benchmarking. |
| `--profile`, `-p` | `str` | No | `""` (all) | Filter testcases to evaluate only a specific profile. |
| `--verbose`, `-v` | flag | No | `False` | Print detailed per-testcase score breakdown table. |
| `--json` | flag | No | `False` | Output summary metrics in machine-readable JSON format. |
| `--rebuild` | flag | No | `False` | Force recompilation of the C++ Simulator binary. |

### Script usage

```bash
# Run simulation across all test cases with full summary report
python Code/Simulator.py

# Run with per-testcase detailed breakdown (ID, Profile, Score, TP, TDR, TPOT)
python Code/Simulator.py -v

# Run 100 benchmark iterations across all CPU cores (e.g. 3,000 simulations)
python Code/Simulator.py -r 100

# Filter by a specific test profile
python Code/Simulator.py --profile burst_arrivals -v

# Specify custom testcase file and 8 worker threads
python Code/Simulator.py Testcases/Raw/testcases.jsonl -j 8

# Output machine-readable JSON metrics
python Code/Simulator.py --json

# Direct C++ binary execution (if preferred)
./Code/Simulator.exe -i Testcases/Raw/testcases.jsonl -v
```

---

## Trainer.py

### Script name
`Trainer.py`

### Script function
Black-box parameter optimizer using **Optuna** with the **Tree-structured Parzen Estimator (TPE)** sampler to search all ~40 `KnobSet` parameters in `Scheduler.h` / `Scheduler.cpp`.

- **Ground-Truth Objective**: Evaluates candidates directly via `Simulator.exe` subprocess calls with OpenMP parallelism and `V4_*` environment variables.
- **Floor-Raising Monotonic Fitness**: Uses a numerically stable soft-min over per-profile average scores combined with overall mean score to lift weak profiles without degrading strong ones.
- **Anti-Overfitting & Generalization**:
  - Independent **Train / Val / Holdout** dataset splits across all 19 stress profiles.
  - **Multi-Candidate Validation**: Top-3 candidates by train fitness are evaluated on `val.jsonl` at each validation checkpoint, not just the single best.
  - **Automatic Dataset Sizing**: Checks existing datasets and auto-generates via `Generator.py` if fewer testcases than requested.
  - **Unbiased Holdout Verification**: Evaluates generalization gap against `holdout.jsonl`.
- **Stagnation Recovery**: Automatically switches from TPE to **CMA-ES** (Covariance Matrix Adaptation) local refinement after a configurable stagnation window with no champion improvement.
- **Robust Resume**: On resume from SQLite, restores champion state by re-evaluating the best trial and `champion.json` on the current validation set. Enqueues champion knobs as warm-start.
- **Persistence & Artifacts**: Checkpoints progress into `study.db` (SQLite) and exports the champion configuration to ready-to-source `.env` (`champion.env`) and JSON metadata (`champion.json`).

### Arguments

| Argument | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `--trials`, `-n` | `int` | No | `50` | Number of optimization trials to execute. |
| `--train-size` | `int` | No | `300` | Number of test cases in `train.jsonl`. |
| `--val-size` | `int` | No | `150` | Number of test cases in `val.jsonl`. |
| `--holdout-size` | `int` | No | `150` | Number of test cases in `holdout.jsonl`. |
| `--threads`, `-t`, `-j` | `int` | No | `0` (hardware max) | OpenMP worker threads for Simulator. |
| `--val-every` | `int` | No | `10` | Frequency (in trials) to test candidate on validation set. |
| `--reroll-every` | `int` | No | `0` (disabled) | Frequency (in trials) to re-roll train set with fresh seed. |
| `--study-name` | `str` | No | `scheduler_knobs` | Optuna study name in SQLite database. |
| `--storage` | `str` | No | `sqlite:///study.db` | SQLite database URI for persistence / resuming. |
| `--seed` | `int` | No | `42` | RNG seed for sampler and dataset generation. |
| `--alpha` | `float` | No | `0.6` | Weight on overall mean score in fitness ($1-\alpha$ on soft-min). |
| `--beta` | `float` | No | `0.05` | Soft-min sharpness parameter. |
| `--stagnation-window` | `int` | No | `80` | Trials without improvement before switching to CMA-ES. |

### Script usage

```bash
# Run standard 50-trial training study
python Code/Trainer.py --trials 50

# Large-scale training with 26 threads
python Code/Trainer.py --trials 1500 -t 26 --train-size 16384 --val-size 8192 --holdout-size 8192

# Resume an existing study (champion state auto-restored)
python Code/Trainer.py --trials 500 -t 26 --study-name study --storage sqlite:///study.db

# Apply exported champion knobs in simulation
source Artifacts/champion.env
python Code/Simulator.py Testcases/Train/holdout.jsonl
```

---

## Patcher.py

### Script name
`Patcher.py` (located in `Patcher/`)

### Script function
Automated code patcher that extracts tuned knob values from any `.env` file (e.g. `Artifacts/champion.env`) and injects them directly into C++ submission or scheduler source files (e.g. `Patcher/Submission.cpp` or `Schedulers/Scheduler.h`).

- **Submission-Ready C++**: Prepares self-contained C++ submission files for competitive programming / online judges where environment variables are not set, baking the tuned champion parameters into the fallback defaults.
- **Flexible Pattern Matching**: Supports `envi(...)` / `envd(...)` fallback arguments, C++ struct member initializers, and static constant assignments.
- **Safety Features**: Includes `--dry-run` to preview unified diffs before modifying, and `--backup` to create a `.bak` backup file before in-place overwrites.

### Arguments

| Argument | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `--env`, `-e` | `str` | No | `Artifacts/champion.env` | Path to `.env` file containing knob definitions. |
| `--cpp`, `-c` | `str` | No | `Patcher/Submission.cpp` | Path to C++ source file to patch. |
| `--output`, `-o` | `str` | No | `None` (in-place) | Optional path to write patched file instead of overwriting. |
| `--backup`, `-b` | flag | No | `False` | Create a `.bak` backup copy before modifying in-place. |
| `--dry-run` | flag | No | `False` | Show unified diff preview without writing changes. |
| `--verbose`, `-v` | flag | No | `False` | Print detailed breakdown for every knob. |

### Script usage

```bash
# Preview diff without modifying files
python Patcher/Patcher.py --env Artifacts/champion.env --cpp Patcher/Submission.cpp --dry-run

# Patch Submission.cpp in-place with automatic backup
python Patcher/Patcher.py --env Artifacts/champion.env --cpp Patcher/Submission.cpp --backup

# Patch a custom C++ file and save to a new output path
python Patcher/Patcher.py -e Artifacts/champion.env -c Patcher/Submission.cpp -o Submissions/Final.cpp
```


