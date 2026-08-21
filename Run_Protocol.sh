# !/usr/bin/env bash
# The full tuning protocol as one command. Each stage is resumable: re-running
# reuses its sqlite study, so you can interrupt and continue.
#
#   ./run_protocol.sh            # all three stages
#   ./run_protocol.sh 1          # just stage 1
#   SEED=7 ./run_protocol.sh     # a second independent search
set -euo pipefail
cd "$(dirname "$0")"

SEED="${SEED:-1}"
JOBS="${JOBS:-$(( $(nproc) - 1 ))}"
ONLY="${1:-all}"
S1_ENV="s1_seed${SEED}.env"
S2_ENV="s2_seed${SEED}.env"

command -v g++ >/dev/null || { echo "g++ not found"; exit 2; }
g++ -O2 -std=c++17 -o scheduler scheduler.cpp
python3 -c "import Knobs,sys; b=Knobs.verify_against_cpp('scheduler.cpp',strict=False); sys.exit(bool(b)) if not b else (print(b),sys.exit(1))"
python3 Simulator.py --validate-sample || { echo "FATAL: judge-faithfulness gate failed"; exit 1; }

if [[ "$ONLY" == "all" || "$ONLY" == "1" ]]; then
  echo "=================== STAGE 1: explore ==================="
  python3 Trainer.py --stage 1 --seed "$SEED" --n_jobs "$JOBS" \
      --study_name "s1_seed${SEED}" --storage "sqlite:///s1_seed${SEED}.db" \
      --env_out "$S1_ENV" --json_out "s1_seed${SEED}.json"
fi

if [[ "$ONLY" == "all" || "$ONLY" == "2" ]]; then
  echo "=================== STAGE 2: refine ==================="
  python3 Trainer.py --stage 2 --seed "$((SEED + 100))" --n_jobs "$JOBS" \
      --seed_env "$S1_ENV" \
      --study_name "s2_seed${SEED}" --storage "sqlite:///s2_seed${SEED}.db" \
      --env_out "$S2_ENV" --json_out "s2_seed${SEED}.json"
fi

if [[ "$ONLY" == "all" || "$ONLY" == "3" ]]; then
  echo "============ STAGE 3: select at the real token limit ============"
  python3 Trainer.py --stage 3 --seed "$((SEED + 200))" --n_jobs "$JOBS" \
      --eval_env "$S2_ENV"
  echo "--- bake the winner into a submittable source file ---"
  python3 Knobs.py --env-file "$S2_ENV" --patch-cpp scheduler_submit.cpp
  g++ -O2 -std=c++17 -o scheduler_submit scheduler_submit.cpp
  echo "wrote scheduler_submit.cpp / scheduler_submit (no env vars needed)"
fi