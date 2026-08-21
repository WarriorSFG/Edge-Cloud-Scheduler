# Edge/Cloud scheduler knob-tuning kit

Four cooperating modules that tune the 19 `V4_*` env knobs read by
`scheduler.cpp`:

| file | role |
| --- | --- |
| `Knobs.py` | the knob registry: kinds, defaults, search bounds, env I/O. Cross-checks itself against `scheduler.cpp`. |
| `Simulator.py` | the interactor/judge **and** the analytic system model (task-time table, link, reference schedule). |
| `Generator.py` | stratified synthetic cases with judge-faithful, *calibrated* scoring lines. |
| `Trainer.py` | Optuna search over the knobs, with train/val separation and a baseline comparison. |

## Quick start

```bash
g++ -O2 -std=c++17 -o scheduler scheduler.cpp

# Install optuna for the SAME interpreter that runs Trainer.py. On this box
# `pip3` belongs to python3.8 while `python3` is 3.10, so `pip3 install optuna`
# installs it somewhere Trainer.py cannot see it, and the tuner silently falls
# back to its built-in random+local search.
python3 -m pip install --user optuna

python3 Simulator.py --validate-sample     # conformance vs the real judge's test #1
python3 Trainer.py --baseline_only         # what do the current defaults score?
python3 Trainer.py --n_trials 300          # tune

set -a; source best_hparams.env; set +a    # use the result
./scheduler
```

Useful extras:

```bash
python3 Knobs.py                                   # knob table + scheduler.cpp cross-check
python3 Generator.py --n_cases 40 --probe --summary --out cases.jsonl
python3 Simulator.py --cases cases.jsonl --workers 8      # score a case file
python3 Simulator.py --cases cases.jsonl --env-file best_hparams.env
python3 Trainer.py --eval_env best_hparams.env     # re-score a saved config
```

## Is the judge faithful?

`python3 Simulator.py --validate-sample` does two independent checks against the
real judge's own transcript for public test #1:

1. replays the fixed trace and diffs the scheduler's 17 output lines against
   `Expected output 1`;
2. rebuilds the case, drives it through this interactor, and compares the score
   with the judge's reported **500.0000027586** points.

Both pass exactly. The second one is the strong check: matching to ten
significant digits means the timing model, the link FIFO, the schedule cost `S`,
the piecewise-linear table lookup and the whole scoring formula all agree with
the contest judge, including the fact that the judge reads `tp_base` as the
9-decimal value it prints (that rounding is the entire `+2.7586e-6` above 500).

## Why the scoring parameters are computed, not sampled

`tp_base`, `dist_base` and `tp_UB` are not free constants: the statement ties
them to a one-request-at-a-time reference schedule. Sampling them at random is
what makes a synthetic benchmark useless — too generous and every knob setting
scores 1000, too harsh and everything scores 0, and either way the gradient the
tuner needs is gone.

So `Generator.py` calibrates every case:

* `tp_base` = throughput of the reference schedule (`Simulator.reference_schedule`).
  On public test #1 this reproduces the test's own `tp_base` to all nine printed
  digits.
* `dist_base` = that same reference's SLO excess, so `dist_base == 0` happens
  exactly when the reference already meets both targets.
* `SLO1` is drawn *tight* relative to the reference's TDR and `SLO2` *loose*
  relative to an achievable concurrent TPOT. Solving the judge's own report for
  its hidden constants shows that is what its tests do — e.g. test #22 implies
  `SLO1 ≈ 7.5 ms` against `mean_tdr 2782`, while `mean_tpot 8.0` contributes no
  excess at all. In that regime
  `norm_c ≈ 1 - (your TDR excess)/(reference TDR excess)`, which is scale-free
  and measures exactly "how much better than the reference are you".
* `tp_UB` is placed so the *measured* achievable throughput lands at a drawn
  point `q` on the `[0,1]` output-rate axis, which pins the operating point
  inside the interval instead of hoping it lands there.

`--probe` (on by default in `Trainer.py`) measures the achievable TPOT/throughput
scale by running the scheduler at its default knobs, twice, once per pool. That
is the only part that needs the binary; `--no_probe` falls back to an analytic
pipelining estimate.

Sanity check on the result: baseline mean score lands near 550-700, matching the
real judge's spread over its 22 preliminary tests, with `norm_tp` and `norm_c`
both spread across `(0,1)` rather than pinned at the ends.

## What was broken before

The rewrite fixes these; each one alone was enough to make tuning meaningless.

**Simulator (verdict-changing)**

* `FIN` was pushed as a separate event and therefore arrived in the *next*
  frame. The statement guarantees it appears beside the final `D POST` TDN, and
  a correct scheduler relies on that: it would re-issue work for a finished
  request and take a spurious zero.
* Transfers were enqueued at *assignment* time instead of at task completion.
  A task assigned later but finishing earlier then queued behind one that was
  still running, silently reordering both FIFO links.
* Missing legality checks: `D PROC` members were never checked against the
  remote they name, `Ck` was never checked against the `<remote>` field, the
  `n ≤ K+1` response limit and empty pieces (`ls == le`) were unchecked. Illegal
  schedules scored instead of failing.
* `_read_response` used blocking `readline()`, so a scheduler that stopped
  printing hung the tuner forever rather than reporting a timeout. Reads are now
  deadline-bounded with `select(2)`.
* Responses are parsed as a token stream, since the statement permits arbitrary
  whitespace between tokens, and TDN specs are echoed in canonical form.
* `Table.at` mis-scanned columns and `_validate`'s busy check compared against a
  stale timestamp.

**Generator**

* Scoring parameters were sampled blind (see above) — the core problem.
* The `sum(L_out) ≤ 2e5` budget could be overshot by one token, and the tail of
  a long stream got `L_out` truncated to 1.
* Arrival rates were drawn independent of the machine's service rate, so most
  cases were wildly underloaded: throughput was pinned by the arrival spread and
  no knob could move the score. The rate is now a load factor times an estimate
  of the local computer's service rate.
* `PROFILES` contained `"mixed"` and then filtered it out again; `argparse`
  offered it twice.
* No constraint checking, so a generator bug would have looked like a scheduler
  bug. `check_case()` now asserts every "Constraints" bullet.

**Trainer**

* A violation raised `TrialPruned`, discarding the observation. The sampler
  learned nothing about illegal regions and kept revisiting them. Violations now
  score 0.
* Each trial sampled a *different* case batch (`random.Random(trial.number)`),
  so trial-to-trial differences were dominated by which cases were drawn, not by
  the knobs. TPE cannot separate those. The batch is now fixed, which makes the
  objective deterministic.
* `--refresh_every_n_trials` and `--eval_every_n_trials` were accepted and never
  used; the docstring described pool refreshing that did not happen. Validation
  is now real and periodic.
* Cases were re-pickled to the workers on every trial; they are now sent once at
  pool startup and referenced by index.
* `Knobs.Knobs` / `knobs.as_env` typos; `DEFAULT_ENV` built with `repr()` instead
  of the same coercion used for real runs; the knob count in the docstring did
  not match the 19 knobs `loadKnobs()` reads.
* No baseline. There was no way to answer "did tuning actually help?" — the run
  now reports baseline vs best on both train and val, plus the train-minus-val
  gap.

## Recommended tuning protocol for a long run

With unlimited compute the binding constraint is **not** trial count -- it is
overfitting to this generator's distribution, which no amount of held-out
validation from the same generator can detect. So spend the budget on *cases*
and on *independent seeds*, not only on trials.

Everything below is packaged as `./run_protocol.sh` (all three stages, with
the two correctness gates run first) or `--stage {1,2,3}` presets on
`Trainer.py`. Explicit flags still override a preset, so
`--stage 2 --n_trials 800` does what it looks like. Run
`SEED=2 ./run_protocol.sh` a few times for the independent-seed comparison.

**Stage 1 -- explore: many trials, moderate pool** (~2h on 8 cores). TPE needs
*trials* to search 19 dimensions, and early candidates differ by enough that
200 cases can rank them.

```bash
python3 Trainer.py --study_name s1 --storage sqlite:///s1.db \
    --n_trials 1500 --train_cases 200 --val_cases 200 \
    --token_budget 6000 --timeout_s 600 --n_jobs 7 --eval_every 100 --seed 1
```

**Stage 2 -- refine: large pool, warm-started from stage 1** (~6h). Now the
candidates are close together, so precision matters more than breadth.

```bash
cp best_hparams.env s1.env
python3 Trainer.py --study_name s2 --storage sqlite:///s2.db --seed_env s1.env \
    --n_trials 400 --train_cases 2000 --val_cases 400 \
    --token_budget 60000 --timeout_s 900 --n_jobs 7 --eval_every 50 --seed 2
```

A 2000-case pool costs ~50 MB per worker and ~100 s per fully-evaluated trial on
7 cores, so it is entirely practical. Pruning keeps it affordable — a bad config
dies after a dozen cases. One caveat: this Optuna version's `TPESampler` has no
`consider_pruned_trials`, so pruned trials do not feed the surrogate model. If
pruning is killing most trials, the sampler is flying half blind; check that a
few hundred trials still reach COMPLETE, and pass `--no_prune` for the final
refinement stage if not.

**Stage 3 -- repeat stages 1-2 under 3-5 different `--seed` values**, then pick
the winner on a *common* fresh pool at the true constraint limit:

```bash
python3 Generator.py --n_cases 120 --seed 999999 --token_budget 200000 \
    --probe --workers 8 --out final.jsonl
for e in s2_seed*.env; do
  echo "== $e"; python3 Simulator.py --cases final.jsonl --env-file "$e" --workers 8 | tail -1
done
```

Why these numbers:

* `--train_cases` is by far the most valuable place to spend extra time, and
  the numbers below say to be much more generous than a knob-count heuristic
  suggests. Measured on 400 fresh probe-calibrated cases, the per-case *paired*
  difference between two knob configs has sd ~71 points, so the pool size you
  need depends entirely on the effect you are trying to resolve:

  | difference you want to resolve | cases needed (95% conf.) |
  | --- | --- |
  | 20 pts | 49 |
  | 10 pts | 193 |
  | 5 pts  | 772 |
  | 3 pts  | 2143 |

  Early in a search, candidates differ by tens of points and 50 cases suffice.
  Late in a search, when you are separating near-optimal configs that differ by
  2-5 points, you genuinely need 1000-3000. A 24-case batch ranked a known-better
  config above the defaults in only 86 of 100 random subsamples; 200 cases got
  it right 100 times out of 100.

  The cost of overfitting to a small batch is directly measurable: a config
  tuned on a 22-case batch showed +34.6 there but only **+13.5** on 400 unseen
  cases, so roughly two thirds of the apparent gain was fitting that batch.
* `--n_trials` — TPE in 19 dimensions is still improving at a few hundred
  trials; 2000 is well past saturation, so stage 1 is where to be generous.
* `--token_budget` — cheap cases for exploration, near-real cases for stage 2
  (queue-length and uplink-backlog effects only appear at scale), the true
  `2·10^5` for *selection only*. Worst case measured at full budget is ~2s per
  case even for a deliberately bad config, so this is affordable.
* `--timeout_s` generously high. This is the one setting that can silently
  corrupt the objective: a timeout is recorded as a violation and scored 0, not
  as the low-but-real score the config deserves, and it can also kill a *good*
  config on one unusually large case.
* Several `--seed` values guard against both TPE getting stuck in one basin and
  fitting one pool's quirks; selecting among the finalists on a common unseen
  pool is an unbiased comparison, which selecting on their own train pools is
  not.
* Leave `--rotate_every 0`. With 120 fixed cases, case-overfitting is already
  weak and stationarity is worth more to the sampler.
* Leave pruning on: the fixed case order makes the median comparison
  like-for-like, and it buys roughly 3-4x more trials per hour. Pruning is also
  what makes a *large* train set affordable — a bad config dies after a dozen
  cases while a promising one is scored on all of them, which is exactly the
  cost profile you want. The pruner deliberately does not start until the
  canary prefix plus 8 real cases have been scored (`--prune_warmup`), because
  judging a trial on degenerate cases alone would throw away good configs.
* Do **not** pass `--no_probe` for a real run — it is the calibration that puts
  the scores in a gradient-rich range.

A caution on small pools: on a 6-case pool a deliberately pathological config
scored 716 against the defaults' 706. Six cases cannot rank configurations.
Trust nothing measured on fewer than ~50.

## Shipping the tuned values

**The judge runs your submitted program with no environment of its own**, so
`best_hparams.env` never reaches it. Environment variables are a tuning-harness
mechanism only; the tuned values have to be baked into the source.

```bash
python3 Knobs.py --env-file best_hparams.env --patch-cpp scheduler_submit.cpp
g++ -O2 -std=c++17 -o scheduler_submit scheduler_submit.cpp

# verify: the baked binary with NO env vars must match the env-var run exactly
python3 Simulator.py --cases final.jsonl --env-file best_hparams.env --workers 8
python3 Simulator.py --cases final.jsonl --binary ./scheduler_submit \
        --use-binary-defaults --workers 8
```

`--patch-cpp` rewrites only the default literal inside each
`envd/envi("V4_*", ...)` call and leaves comments and layout untouched, so the
submitted file stays reviewable and still honours env overrides for future
tuning. `python3 Knobs.py --env-file ... --emit-cpp` prints a ready
`loadKnobs()` instead, if you would rather paste it in yourself.

For local runs against the real judge, exporting is enough:

```bash
set -a; source best_hparams.env; set +a
./scheduler
```

Note that `Knobs.KNOBS` keeps the *original* defaults as its search priors even
after you bake tuned values into the C++, so `Trainer.py --baseline_only` still
reports the original baseline. That is deliberate and harmless — `as_env()`
always exports every knob explicitly, so a simulator measurement never depends
on the binary's compiled defaults — but use `--eval_env` if you want the tuned
configuration as your reference point.

## Cost control

The real limit is `sum(L_out) ≤ 2·10^5`, which at group size 1 is ~10^6 frames —
fine for one judged run, far too slow for hundreds of tuning trials. So
`Trainer.py` defaults to `--token_budget 6000`; raise it (up to 200000) for a
final validation pass:

```bash
python3 Generator.py --n_cases 40 --probe --token_budget 200000 --out final.jsonl --workers 8
python3 Simulator.py --cases final.jsonl --env-file best_hparams.env --workers 8
```