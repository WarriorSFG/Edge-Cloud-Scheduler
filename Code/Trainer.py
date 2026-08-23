"""Part 3 -- knob tuner for the V4_* env knobs of scheduler.cpp.

    python3 Trainer.py --n_trials 200            # tune
    set -a; source best_hparams.env; set +a      # then use the result
    ./scheduler                                  # real judge run

CONCURRENCY MODEL
  Optuna trials run SEQUENTIALLY; --n_jobs applies to the case batch inside each
  trial. That avoids nested oversubscription between Optuna's own pool and the
  simulator's. Cases are handed to the worker processes once, at pool startup,
  and referenced by index afterwards -- a case with 2e5 tokens is megabytes and
  re-pickling it per trial dominated the runtime otherwise.

OBJECTIVE
  Mean Score in [0, 1000] over a FIXED batch, matching the contest metric (the
  arithmetic mean over the 20 frozen final tests). The batch is fixed on
  purpose: the scheduler and the interactor are both deterministic, so a fixed
  batch makes the objective deterministic and trial A directly comparable to
  trial B. Resampling the batch every trial -- as an earlier version of this
  script did -- turns knob effects into noise that the sampler cannot separate
  from case-to-case variation, and TPE then models nothing.

TRAIN / VALIDATION
  * train set: --train_cases stratified cases, scored every trial. The
    degenerate canaries run first as a LEGALITY GATE and are deliberately not
    averaged into the objective -- the contest metric is an unweighted mean over
    20 ordinary tests, and letting six edge cases carry a fifth of the weight
    (which is what --train_cases 24 did) optimises for the wrong thing.
    Rotation stays off by default: at --train_cases 200 the fixed batch is
    already large enough that case-overfitting is weak, and the stationarity TPE
    assumes is worth more -- more so now that the space is 40-dimensional.
  * val set:   --val_cases cases generated once with a different seed and never
    trained on, scored for the running best every --eval_every trials and again
    at the end. Watch train-minus-val: that gap is overfitting to Generator
    quirks, not a real improvement.

ILLEGAL OUTPUT
  A protocol violation scores 0 for that case and the rest of the batch is
  filled with zeros, so the trial returns a real (very low) value instead of
  being pruned. Pruning throws the information away; the sampler needs to learn
  that the region is bad. Every violation is printed with its reason -- if one
  shows up for the default knobs, that is a scheduler bug, not a tuning result.
"""
from __future__ import annotations

import argparse
import atexit
import json
import math
import os
import random
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from statistics import mean
from typing import Any, Sequence

import Generator
import Knobs
import Simulator

PRUNER_WARMUP_TRIALS = 8
PRUNER_EXTRA_WARMUP_CASES = 8

# ------------------------------------------------------- worker-side globals --
_CASES: list[dict] = []


def _init_worker(cases: list[dict]) -> None:
    global _CASES
    _CASES = cases


def _run_idx(idx: int, env: dict[str, Any], timeout_s: float,
             binary: str) -> Simulator.SimResult:
    return Simulator.run_one(_CASES[idx], env, timeout_s, binary)


# --------------------------------------------------------------- evaluation --
class Evaluator:
    """Owns the worker pool and evaluates (knobs, case-index list) -> scores."""

    def __init__(self, cases: list[dict], n_jobs: int, timeout_s: float,
                 binary: str) -> None:
        self.cases = cases
        self.n_jobs = max(1, n_jobs)
        self.timeout_s = timeout_s
        self.binary = binary
        self.ex: ProcessPoolExecutor | None = None
        if self.n_jobs > 1:
            self.ex = ProcessPoolExecutor(max_workers=self.n_jobs,
                                          initializer=_init_worker,
                                          initargs=(cases,))
            atexit.register(self.close)
        else:
            _init_worker(cases)

    def close(self) -> None:
        if self.ex is not None:
            self.ex.shutdown(wait=False, cancel_futures=True)
            self.ex = None

    def chunks(self, idxs: Sequence[int]) -> list[list[int]]:
        n = self.n_jobs
        return [list(idxs[i:i + n]) for i in range(0, len(idxs), n)]

    def run_chunk(self, idxs: Sequence[int],
                  env: dict[str, Any]) -> list[Simulator.SimResult]:
        if self.ex is None:
            return [_run_idx(i, env, self.timeout_s, self.binary) for i in idxs]
        futs = [self.ex.submit(_run_idx, i, env, self.timeout_s, self.binary)
                for i in idxs]
        return [f.result() for f in futs]

    def run_all(self, idxs: Sequence[int],
                env: dict[str, Any]) -> list[Simulator.SimResult]:
        out: list[Simulator.SimResult] = []
        for ch in self.chunks(idxs):
            out.extend(self.run_chunk(ch, env))
        return out


def report(cases: Sequence[dict], results: Sequence[Simulator.SimResult],
           label: str) -> float:
    """Print a per-profile breakdown and return the mean score."""
    by: dict[str, list[float]] = {}
    bad: list[tuple[str, str]] = []
    for c, r in zip(cases, results):
        by.setdefault(c["profile"], []).append(r.score)
        if not r.ok:
            bad.append((c["profile"], r.violation or "?"))
    total = mean([r.score for r in results]) if results else 0.0
    print(f"\n[{label}] mean = {total:.2f} over {len(results)} cases")
    for prof in sorted(by):
        v = by[prof]
        print(f"    {prof:<20} n={len(v):3d}  mean={mean(v):7.2f}  "
              f"min={min(v):7.2f}  max={max(v):7.2f}")
    for prof, why in bad[:10]:
        print(f"    !! VIOLATION {prof}: {why}")
    return total


# ------------------------------------------------------------ search space ----
def suggest(trial: Any) -> dict[str, float | int]:
    """Search space straight from Knobs.KNOBS -- bounds live there, not here."""
    v: dict[str, float | int] = {}
    for k in Knobs.KNOBS:
        if k.kind == "cat":
            v[k.name] = trial.suggest_categorical(k.name, list(k.choices or ()))
        elif k.kind == "int":
            v[k.name] = trial.suggest_int(k.name, int(k.low), int(k.high))
        elif k.kind == "logfloat":
            v[k.name] = trial.suggest_float(k.name, float(k.low), float(k.high), log=True)
        else:
            v[k.name] = trial.suggest_float(k.name, float(k.low), float(k.high))
    return v


class Objective:
    """Objective = unweighted mean Score over the *main* batch, minus a penalty
    for any canary that came out illegal.

    Two deliberate departures from the first version:

    * The canaries no longer contribute score mass. They used to sit at the
      front of the batch and be averaged in, so with the old --train_cases 24
      the six degenerate cases carried 20% of the objective -- nothing like the
      contest metric, which is an unweighted mean over 20 ordinary tests.
    * A canary violation no longer zero-fills the whole trial. It costs
      --canary_penalty * (fraction of canaries illegal), which keeps illegal
      configurations firmly out of the running while still giving the sampler a
      real, ordered value for the rest of the batch. One fragile edge case can
      no longer make an otherwise-strong region look worthless.

    A violation on a *main* case still scores that case 0, because that is
    exactly what the contest would award for it.
    """

    def __init__(self, ev: Evaluator, canary_idx: list[int], main_idx: list[int],
                 reservoir_idx: list[int], args: argparse.Namespace) -> None:
        self.ev = ev
        self.canary_idx = list(canary_idx)
        self.main_idx = list(main_idx)
        self.reservoir_idx = list(reservoir_idx)
        self.a = args
        self.n_violations = 0
        self.canary_fragility: dict[str, int] = {}

    def _batch(self, trial_number: int) -> list[int]:
        if self.a.rotate_every <= 0 or not self.reservoir_idx:
            return self.main_idx
        rng = random.Random(trial_number // self.a.rotate_every)
        k = min(len(self.main_idx), len(self.reservoir_idx))
        return rng.sample(self.reservoir_idx, k)

    def _run_canaries(self, trial: Any, env: dict[str, Any]) -> float:
        """Return the objective penalty from illegal canaries (0 if all legal)."""
        if not self.canary_idx:
            return 0.0
        bad: list[str] = []
        for i, res in zip(self.canary_idx, self.ev.run_all(self.canary_idx, env)):
            if not res.ok:
                prof = self.ev.cases[i]["profile"]
                bad.append(f"{prof}({res.violation})")
                self.n_violations += 1
                self.canary_fragility[prof] = self.canary_fragility.get(prof, 0) + 1
        if not bad:
            return 0.0
        trial.set_user_attr("canary_violations", "; ".join(bad))
        print(f"[canary] trial {trial.number}: {len(bad)}/{len(self.canary_idx)} "
              f"illegal -> {'; '.join(bad)}")
        return self.a.canary_penalty * len(bad) / len(self.canary_idx)

    def __call__(self, trial: Any) -> float:
        import optuna

        env = suggest(trial)
        penalty = self._run_canaries(trial, env)

        idxs = self._batch(trial.number)
        scores: list[float] = []
        step = 0
        for ch in self.ev.chunks(idxs):
            for i, res in zip(ch, self.ev.run_chunk(ch, env)):
                if not res.ok:
                    self.n_violations += 1
                    prof = self.ev.cases[i]["profile"]
                    print(f"[violation] trial {trial.number} profile={prof} "
                          f"reason={res.violation}")
                    trial.set_user_attr("violation", f"{prof}: {res.violation}")
                    scores.append(0.0)
                    if self.a.violation_policy == "zero_fill":
                        scores.extend([0.0] * (len(idxs) - len(scores)))
                        return mean(scores) - penalty
                else:
                    scores.append(res.score)
                trial.report(mean(scores) - penalty, step)
                step += 1
            if trial.should_prune():
                raise optuna.TrialPruned()
        return mean(scores) - penalty


# ----------------------------------------------------- optuna-free fallback ---
def fallback_search(ev: Evaluator, train_idx: list[int], n_trials: int,
                    seed: int) -> tuple[dict[str, float | int], float]:
    """Random search with local refinement, for environments without optuna."""
    rng = random.Random(seed)

    def sample(base: dict[str, float | int] | None, scale: float) -> dict[str, float | int]:
        out: dict[str, float | int] = {}
        for k in Knobs.KNOBS:
            if base is None or rng.random() < scale:
                if k.kind == "cat":
                    out[k.name] = rng.choice(list(k.choices or ()))
                elif k.kind == "int":
                    out[k.name] = rng.randint(int(k.low), int(k.high))
                elif k.kind == "logfloat":
                    lo, hi = math.log(float(k.low)), math.log(float(k.high))
                    out[k.name] = math.exp(rng.uniform(lo, hi))
                else:
                    out[k.name] = rng.uniform(float(k.low), float(k.high))
            else:
                out[k.name] = base[k.name]
        return Knobs.clamp(out)

    best = dict(Knobs.DEFAULTS)
    best_score = mean([r.score for r in ev.run_all(train_idx, best)])
    print(f"[fallback] defaults -> {best_score:.2f}")
    for i in range(n_trials):
        cand = sample(None if i < n_trials // 3 else best, 0.35)
        sc = mean([r.score for r in ev.run_all(train_idx, cand)])
        tag = ""
        if sc > best_score:
            best, best_score, tag = cand, sc, "  <-- new best"
        print(f"[fallback] trial {i:4d} {sc:8.2f} (best {best_score:8.2f}){tag}")
    return best, best_score


def show_importance(study: Any) -> None:
    """Rank the knobs by influence, degrading gracefully.

    The default evaluator needs scikit-learn; PedANOVA does not. If neither is
    usable (too few completed trials, missing numpy) fall back to a plain
    rank-correlation over the completed trials, which needs nothing at all.
    """
    import warnings

    import optuna

    warnings.simplefilter("ignore", category=optuna.exceptions.ExperimentalWarning)
    for label, kwargs in (("fANOVA/MDI", {}),
                          ("PedANOVA", {"evaluator":
                                        optuna.importance.PedAnovaImportanceEvaluator()})):
        try:
            imp = optuna.importance.get_param_importances(study, **kwargs)
            print(f"\n[importance] most influential knobs ({label}):")
            for k, v in list(imp.items())[:8]:
                print(f"    {k:<18} {v:.3f}")
            return
        except Exception:
            continue

    done = [t for t in study.trials if t.value is not None and t.params]
    if len(done) < 8:
        print("[importance] unavailable (too few completed trials)")
        return

    def ranks(xs: list[float]) -> list[float]:
        order = sorted(range(len(xs)), key=lambda i: xs[i])
        r = [0.0] * len(xs)
        for pos, i in enumerate(order):
            r[i] = float(pos)
        return r

    ys = ranks([t.value for t in done])          # type: ignore[misc]
    my = mean(ys)
    dy = [y - my for y in ys]
    var_y = sum(v * v for v in dy) ** 0.5
    scored: list[tuple[float, str]] = []
    for k in Knobs.KNOBS:
        col = [float(t.params.get(k.name, k.default)) for t in done]
        if len(set(col)) < 2:
            continue
        xs = ranks(col)
        mx = mean(xs)
        dx = [x - mx for x in xs]
        var_x = sum(v * v for v in dx) ** 0.5
        if var_x <= 0 or var_y <= 0:
            continue
        rho = sum(a * b for a, b in zip(dx, dy)) / (var_x * var_y)
        scored.append((abs(rho), k.name))
    scored.sort(reverse=True)
    print("\n[importance] |rank correlation| with the score (fallback):")
    for v, k in scored[:8]:
        print(f"    {k:<18} {v:.3f}")


# ------------------------------------------------------------------- output ---
def dump_best(params: dict[str, float | int], train_score: float, val_score: float,
              args: argparse.Namespace, extra: dict[str, Any] | None = None) -> None:
    envmap = Knobs.write_env_file(params, args.env_out)
    with open(args.json_out, "w") as f:
        json.dump({"mean_train_score": train_score, "mean_val_score": val_score,
                   "seed": args.seed, "train_cases": args.train_cases,
                   "val_cases": args.val_cases, "token_budget": args.token_budget,
                   "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
                   "params": envmap, **(extra or {})}, f, indent=2)
    print(f"\n[best] -> {args.env_out} and {args.json_out}")
    print(Knobs.describe(params))


# --------------------------------------------------------------------- main ---
def build_pools(a: argparse.Namespace) -> tuple[list[dict], list[int], list[int],
list[int], list[int]]:
    """Load cases from a calibrated file if available, otherwise generate them."""

    calibrated_file = "calibrated_cases.jsonl"

    # --- NEW LOGIC: Use calibrated file if it exists ---
    if os.path.exists(calibrated_file):
        print(f"[pool] Found '{calibrated_file}'. Loading calibrated cases...")
        with open(calibrated_file, "r") as f:
            cases = [json.loads(ln) for ln in f if ln.strip()]

        n_can = 0
        canary_idx = []
        main_idx = list(range(len(cases)))
        reservoir_idx = list(range(len(cases)))
        val_idx = list(range(len(cases)))

        a.n_canaries = n_can
        print(f"[pool] Loaded {len(cases)} calibrated cases for tuning.")
        return cases, canary_idx, main_idx, reservoir_idx, val_idx

    # --- ORIGINAL LOGIC: Fallback to dynamic generation ---
    print(f"[pool] generating cases (token_budget={a.token_budget}, "
          f"probe={not a.no_probe}) ...")
    t0 = time.monotonic()
    probe = not a.no_probe
    canaries = Generator.edge_cases(a.seed + 7, a.token_budget, probe, a.binary,
                                    a.timeout_s)
    n_reservoir = max(a.pool_size, a.train_cases) if a.rotate_every > 0 else a.train_cases
    train = Generator.generate(n_reservoir, a.seed, "mixed", a.token_budget,
                               a.max_R, probe, a.binary, a.timeout_s, a.n_jobs,
                               mix=a.profile_mix)
    val = Generator.generate(a.val_cases, a.seed + 90_001, "mixed", a.token_budget,
                             a.max_R, probe, a.binary, a.timeout_s, a.n_jobs,
                             mix=a.profile_mix)

    cases = canaries + train + val
    n_can = len(canaries)
    canary_idx = list(range(n_can))
    main_idx = list(range(n_can, n_can + min(a.train_cases, len(train))))
    reservoir_idx = list(range(n_can, n_can + len(train)))
    val_idx = list(range(n_can + len(train), len(cases)))
    a.n_canaries = n_can
    print(f"[pool] {len(cases)} cases ({n_can} canaries, legality gate only "
          f"+ {len(train)} train + {len(val)} val, profile_mix={a.profile_mix}) "
          f"in {time.monotonic() - t0:.1f}s")

    return cases, canary_idx, main_idx, reservoir_idx, val_idx


STAGES: dict[int, dict[str, Any]] = {
    # Stage 1 -- explore: many trials, moderate pool, cheap cases.
    1: dict(n_trials=1500, train_cases=200, val_cases=200, token_budget=6000,
            eval_every=100, timeout_s=600),
    # Stage 2 -- refine: large pool for precision, warm-started from stage 1.
    2: dict(n_trials=400, train_cases=2000, val_cases=400, token_budget=60000,
            eval_every=50, timeout_s=900),
    # Stage 3 -- select: no search, score saved configs at the real token limit.
    3: dict(n_trials=0, train_cases=200, val_cases=400, token_budget=200000,
            eval_every=0, timeout_s=1800),
}


def main() -> None:
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--stage", type=int, choices=(1, 2, 3))
    stage_args, _ = pre.parse_known_args()

    ap = argparse.ArgumentParser(description=__doc__, parents=[pre],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n_trials", type=int, default=1500)
    ap.add_argument("--train_cases", type=int, default=200)
    ap.add_argument("--val_cases", type=int, default=200)
    ap.add_argument("--pool_size", type=int, default=200,
                    help="rotation reservoir; only used when --rotate_every > 0")
    ap.add_argument("--rotate_every", type=int, default=0,
                    help="rotate the train batch every N trials (0 = fixed batch)")
    ap.add_argument("--eval_every", type=int, default=100,
                    help="validate the running best every N trials")
    ap.add_argument("--token_budget", type=int, default=6000,  # noqa: E501
                    help="cap on sum(L_out) per case; the real limit is 2e5, but "
                         "smaller cases make a tuning run tractable")
    ap.add_argument("--max_R", type=int, default=Generator.MAX_R)
    ap.add_argument("--timeout_s", type=float, default=600.0)
    ap.add_argument("--n_jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--binary", default="./scheduler")
    ap.add_argument("--storage", default="sqlite:///study.db")
    ap.add_argument("--study_name", default="v4_knobs")
    ap.add_argument("--env_out", default="best_hparams.env")
    ap.add_argument("--json_out", default="best_hparams.json")
    ap.add_argument("--violation_policy", choices=("score_zero", "zero_fill"),
                    default="score_zero",
                    help="score_zero: an illegal case scores 0 and the batch "
                         "continues (what the contest would award). zero_fill: "
                         "abandon the trial at the first illegal case (legacy, "
                         "fast but it hides how broad the illegality is)")
    ap.add_argument("--canary_penalty", type=float, default=500.0,
                    help="objective points subtracted when ALL canaries are "
                         "illegal, pro-rated by the fraction that are")
    ap.add_argument("--profile_mix", choices=tuple(Generator.PROFILE_MIX),
                    default="balanced",
                    help="case-profile weighting; 'uniform' restores the old "
                         "equal round-robin over all 7 profiles")
    ap.add_argument("--no_probe", action="store_true",
                    help="skip probe calibration (faster, less gradient)")
    ap.add_argument("--baseline_only", action="store_true",
                    help="just score the default knobs and exit")
    ap.add_argument("--eval_env", help="score a saved .env file and exit")
    ap.add_argument("--no_prune", action="store_true")
    ap.add_argument("--prune_warmup", type=int, default=0,
                    help="cases to score before pruning may kick in "
                         "(0 = canaries + 8, which is what you want)")
    ap.add_argument("--seed_env", help="enqueue this .env as the first trial of a "
                                       "fresh study (warm-start a refinement stage)")
    if stage_args.stage:
        ap.set_defaults(**STAGES[stage_args.stage])   # explicit flags still win
    a = ap.parse_args()
    if a.stage:
        print(f"[stage] preset {a.stage}: "
              + " ".join(f"--{k} {v}" for k, v in STAGES[a.stage].items()))
        if a.stage == 3 and not a.eval_env:
            raise SystemExit("--stage 3 is selection-only; pass --eval_env FILE.env")

    if not os.path.exists(a.binary):
        print(f"error: {a.binary} not found -- compile it first:\n"
              f"    g++ -O2 -std=c++17 -o scheduler scheduler.cpp", file=sys.stderr)
        raise SystemExit(2)
    stale = Knobs.verify_against_cpp("scheduler.cpp", strict=False) \
        if os.path.exists("scheduler.cpp") else []
    for s in stale:
        print(f"[warn] knob registry: {s}")

    cases, canary_idx, train_idx, reservoir_idx, val_idx = build_pools(a)
    ev = Evaluator(cases, a.n_jobs, a.timeout_s, a.binary)
    try:
        report(_sel(cases, canary_idx), ev.run_all(canary_idx, Knobs.DEFAULTS),
               "baseline canaries (legality gate, not in objective)")
        base_train = report(_sel(cases, train_idx),
                            ev.run_all(train_idx, Knobs.DEFAULTS), "baseline train")
        base_val = report(_sel(cases, val_idx),
                          ev.run_all(val_idx, Knobs.DEFAULTS), "baseline val")
        if a.baseline_only:
            return
        if a.eval_env:
            params = Knobs.read_env_file(a.eval_env)
            tr = report(_sel(cases, train_idx), ev.run_all(train_idx, params),
                        f"{a.eval_env} train")
            va = report(_sel(cases, val_idx), ev.run_all(val_idx, params),
                        f"{a.eval_env} val")
            print(f"\n[eval] train {tr:.2f} (baseline {base_train:.2f}, "
                  f"{tr - base_train:+.2f})   val {va:.2f} "
                  f"(baseline {base_val:.2f}, {va - base_val:+.2f})")
            return

        try:
            import optuna
        except ImportError:
            print("[warn] optuna not installed -- using the built-in fallback search")
            best, best_train = fallback_search(ev, train_idx, a.n_trials, a.seed)
            best_val = report(_sel(cases, val_idx), ev.run_all(val_idx, best),
                              "best val")
            dump_best(best, best_train, best_val, a,
                      {"baseline_train": base_train, "baseline_val": base_val,
                       "sampler": "fallback"})
            return

        optuna.logging.set_verbosity(optuna.logging.WARNING)
        # Reported steps are main-batch cases only now (the canaries run first
        # and are scored separately), and Generator interleaves profiles so any
        # prefix is representative -- so a plain case count is the right warmup.
        warmup_steps = a.prune_warmup if a.prune_warmup > 0 else \
            PRUNER_EXTRA_WARMUP_CASES
        print(f"[prune] median pruner starts after {warmup_steps} main-batch cases")
        study = optuna.create_study(
            study_name=a.study_name, storage=a.storage, load_if_exists=True,
            direction="maximize",
            sampler=optuna.samplers.TPESampler(seed=a.seed, multivariate=True),
            pruner=(optuna.pruners.NopPruner() if a.no_prune else
                    optuna.pruners.MedianPruner(n_startup_trials=PRUNER_WARMUP_TRIALS,
                                                n_warmup_steps=warmup_steps)))
        if not study.trials:                      # seed known-good points, once
            study.enqueue_trial(dict(Knobs.DEFAULTS))
            if a.seed_env:
                warm = Knobs.clamp(Knobs.read_env_file(a.seed_env))
                if len(warm) == len(Knobs.KNOBS):
                    study.enqueue_trial(warm)
                    print(f"[pool] warm-starting from {a.seed_env}")
                else:
                    print(f"[warn] {a.seed_env} covers {len(warm)}/{len(Knobs.KNOBS)} "
                          "knobs -- not warm-starting")

        obj = Objective(ev, canary_idx, train_idx, reservoir_idx, a)
        best_seen = -1.0

        def cb(st: "optuna.Study", tr: "optuna.trial.FrozenTrial") -> None:
            nonlocal best_seen
            val = tr.value if tr.value is not None else float("nan")
            done = len(st.trials)
            mark = ""
            try:
                if st.best_value > best_seen:
                    best_seen = st.best_value
                    mark = "  <-- new best"
            except ValueError:
                pass
            print(f"[trial {tr.number:4d}] {tr.state.name:9s} value={val:8.2f} "
                  f"best={best_seen:8.2f}{mark}")
            if a.eval_every > 0 and done % a.eval_every == 0:
                try:
                    params = st.best_params
                except ValueError:
                    return
                v = mean([r.score for r in ev.run_all(val_idx, params)])
                print(f"[trial {tr.number:4d}] val(best) = {v:.2f}   "
                      f"train-minus-val = {st.best_value - v:+.2f}")

        study.optimize(obj, n_trials=a.n_trials, n_jobs=1, callbacks=[cb],
                       gc_after_trial=True)

        if not [t for t in study.trials if t.value is not None]:
            print("no trial completed -- nothing to dump")
            return
        best = study.best_trial
        params = Knobs.clamp(best.params) or dict(Knobs.DEFAULTS)
        print(f"\n[best] trial {best.number}: train {best.value:.2f}")
        val_train = report(_sel(cases, train_idx), ev.run_all(train_idx, params),
                           "best train (re-check)")
        best_val = report(_sel(cases, val_idx), ev.run_all(val_idx, params), "best val")
        print(f"\n[result] train {val_train:.2f} (baseline {base_train:.2f}, "
              f"{val_train - base_train:+.2f})")
        print(f"[result] val   {best_val:.2f} (baseline {base_val:.2f}, "
              f"{best_val - base_val:+.2f})")
        print(f"[result] train-minus-val gap {val_train - best_val:+.2f} "
              f"(large gap = overfitting to the generator)")
        print(f"[result] {obj.n_violations} illegal-output case runs during tuning")
        if obj.canary_fragility:
            print("[result] canary fragility (illegal at some knob values) -- "
                  "these are edge cases the policy can break, not harness bugs:")
            for prof, cnt in sorted(obj.canary_fragility.items(),
                                    key=lambda kv: -kv[1]):
                print(f"    {prof:<20} illegal in {cnt} trials")
        else:
            print("[result] no canary ever went illegal")

        show_importance(study)

        dump_best(params, val_train, best_val, a,
                  {"baseline_train": base_train, "baseline_val": base_val,
                   "best_trial": best.number, "n_trials": len(study.trials),
                   "sampler": "TPESampler"})
    finally:
        ev.close()


def _sel(cases: Sequence[dict], idxs: Sequence[int]) -> list[dict]:
    return [cases[i] for i in idxs]


if __name__ == "__main__":
    main()