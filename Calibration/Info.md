# Calibration Directory Guide

This directory holds calibration records (weights / knobs and their corresponding Real Judge scores) used by `Code/Generator.py` to ensure that:
1. **No testcase scores 0** on any of your calibration weights.
2. **Overall score ranking is preserved**: If weights $A$ scored higher than weights $B$ on the Real Judge ($\text{RealScore}(A) > \text{RealScore}(B)$), then weights $A$ will also achieve a higher overall score than weights $B$ on the generated testcase suite ($\text{OverallSimScore}(A) > \text{OverallSimScore}(B)$).

---

## Supported File Formats

You can place your calibration runs in `Calibration/` using any of the following convenient formats:

### Format 1: JSON File (`*.json`)
Name the file anything ending with `.json` (e.g. `champion_142.json`, `run1.json`, `baseline.json`).

```json
{
  "real_score": 142.50,
  "knobs": {
    "SPT": 1,
    "CHUNK": 0,
    "CHUNK_SMULT": 1255.29,
    "HOLD": 0,
    "WAVES": 67.75,
    "HOLD_WFRAC": 0.545,
    "RATE_EFF": 0.469,
    "DECW": 6.204,
    "UPPRE_MAX": 32,
    "UPPRE_MAX_TP": 19
  }
}
```
*Note: Any field name `real_score`, `score`, `val_score`, `judge_score`, or `contest_score` is accepted.*

---

### Format 2: Bash Environment File (`*.env`)
Place your `.env` export file (e.g. `champion.env`, `run2.env`) with the real judge score indicated in a comment or environment variable:

```bash
# Real Score: 96.79
export V4_SPT=1
export V4_CHUNK=0
export V4_CHUNK_SMULT=1255.2942387763183
export V4_HOLD=0
export V4_WAVES=67.75731424991909
export V4_HOLD_WFRAC=0.5458946500372974
export V4_UPGATE_FRAC=0.6354404519034136
export V4_RATE_EFF=0.4698710014953528
export V4_UPPRE_MAX=32
export V4_UPPRE_MAX_TP=19
```
*Alternative score lines inside `.env`: `REAL_SCORE=96.79` or `SCORE=96.79` or filename `weights_score_96.79.env`.*

---

### Format 3: Subdirectory per Run
Create a folder like `Calibration/submission_1/` containing:
- `weights.env` or `champion.env`
- `score.txt` (containing the single float score e.g. `120.45`)

---

### Format 4: Unified Manifest (`calibration.json` or `calibration.csv`)
You can define multiple runs in a single file:

```json
[
  {
    "name": "Champion_v2",
    "real_score": 142.50,
    "knobs": { "SPT": 1, "CHUNK": 0, "CHUNK_SMULT": 1255.29 }
  },
  {
    "name": "Baseline_Default",
    "real_score": 95.20,
    "knobs": { "SPT": 1, "CHUNK": 1, "CHUNK_SMULT": 182.53 }
  }
]
```

---

## How to Run the Calibrated Generator

```bash
# Generate 50 testcases calibrated against all runs in Calibration/
python Code/Generator.py 50 --output Testcases/Raw/calibrated_50.jsonl

# Generate testcases for a specific profile (e.g. balanced or stress_scale)
python Code/Generator.py 30 --profile balanced --output Testcases/Raw/balanced_30.jsonl

# Run uncalibrated generation (skipping Calibration/ files)
python Code/Generator.py 50 --no-calibration
```
