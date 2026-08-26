# Utility Tools

This directory provides utility scripts for working with the Scheduler optimizer, calibration files, and C++ source code.

---

## 1. `cpp_to_json.py` / `extract_knobs.py`

Extracts knobs and hyperparameters from C++ scheduler files, header definitions, `.env` exports, or pasted code snippets and saves them into a standardized JSON format with `"score": 0` for use in `Calibration/` or tuning pipelines.

### Standard Output Format:
```json
{
  "score": 0,
  "knobs": {
    "SPT": 1,
    "CHUNK": 0,
    "CHUNK_SMULT": 1255.29423877632,
    "HOLD": 0,
    "WAVES": 67.7573142499191,
    "HOLD_WFRAC": 0.545894650037297,
    "UPGATE_FRAC": 0.635440451903414,
    "UPPRE_MAX": 32,
    "UPPRE_MAX_TP": 19,
    "RATE_EFF": 0.469871001495353,
    "DECW": 6.20420582854029,
    ...
  }
}
```

---

### Usage Modes

#### A. File Input via Command Line (Quickest)
```bash
# Saves automatically to Calibration/SampleCode.json
python Ultility/cpp_to_json.py Calibration/SampleCode.cpp

# Or specify a custom output path
python Ultility/cpp_to_json.py Calibration/SampleCode.cpp -o Calibration/my_champion.json
```

#### B. Interactive Prompt (Paste Code or Path)
Simply run without arguments:
```bash
python Ultility/cpp_to_json.py
```
You can paste any C++ code snippet (e.g., `loadKnobs()` function) and type `DONE` or press `Ctrl+Z` (Windows) / `Ctrl+D` (Linux) to generate the JSON.

#### C. Command Line Text Snippet
```bash
python Ultility/cpp_to_json.py --text "KN_SPT = envi(\"V4_SPT\", 1); KN_WAVES = envd(\"V4_WAVES\", 67.75);" -o Calibration/quick.json
```

#### D. Pipeline / Stdin
```powershell
# PowerShell
Get-Content Calibration/SampleCode.cpp | python Ultility/cpp_to_json.py -o Calibration/extracted.json
```
```bash
# Bash
cat Calibration/SampleCode.cpp | python Ultility/cpp_to_json.py -o Calibration/extracted.json
```

#### E. Preview JSON in Console (Stdout)
```bash
python Ultility/cpp_to_json.py Calibration/SampleCode.cpp --stdout
```
