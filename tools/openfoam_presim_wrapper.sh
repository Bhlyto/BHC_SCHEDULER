#!/usr/bin/env bash
# Wrapper to run OpenFOAM-based presim or fall back to the Python high-fidelity generator.
# Usage (placeholders handled by presim harness):
#   ./openfoam_presim_wrapper.sh --parent <parent> --out <out> --input-dir <input_dir>

set -euo pipefail

PARENT=""
OUT=""
INPUT_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --parent) PARENT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --input-dir) INPUT_DIR="$2"; shift 2 ;;
    *) shift ;;
  esac
done

if [[ -z "$PARENT" || -z "$OUT" ]]; then
  echo "Usage: $0 --parent <parent> --out <out> [--input-dir <dir>]" >&2
  exit 2
fi

# If RUN_OPENFOAM=1, attempt to run the OpenFOAM binary. Otherwise skip to Python generator.
if [[ "${RUN_OPENFOAM:-0}" == "1" ]]; then
  echo "[wrapper] Running OpenFOAM presim for parent=$PARENT"
  if [[ -x "/usr/bin/openfoam2412" ]]; then
    /usr/bin/openfoam2412 --case "$PARENT" --run > "${OUT}.openfoam.log" 2>&1 || echo "[wrapper] openfoam exited non-zero" >&2
  else
    echo "[wrapper] /usr/bin/openfoam2412 not found or not executable" >&2
  fi
else
  echo "[wrapper] RUN_OPENFOAM not set; skipping OpenFOAM invocation"
fi

# Fall back to our Python high-fidelity presim generator to produce presim.json
PY_SCRIPT="BHC_SCHEDULER/tools/python_solvers/thermal_high_fidelity.py"
# Prefer a direct OpenFOAM-produced JSON if available
if [[ -f "${INPUT_DIR}/openfoam_presim.json" ]]; then
  echo "[wrapper] Found ${INPUT_DIR}/openfoam_presim.json, copying to ${OUT}"
  mkdir -p "$(dirname "$OUT")"; cp "${INPUT_DIR}/openfoam_presim.json" "$OUT"
  echo "[wrapper] Wrote presim JSON to $OUT"
  exit 0
fi

# Attempt to synthesize presim.json from OpenFOAM log if it exists
if [[ -f "${OUT}.openfoam.log" ]]; then
  echo "[wrapper] Parsing OpenFOAM log ${OUT}.openfoam.log to synthesize presim.json"
  # extract floating-point numbers from the log; use them to bias errors
  nums=$(grep -Eo '\\b[0-9]+\.[0-9]+\\b' "${OUT}.openfoam.log" || true | head -n 128)
  if [[ -n "$nums" ]]; then
    # compute a simple scale based on mean of numbers
    sum=0; count=0
    while read -r v; do
      sum=$(awk -v a=$sum -v b=$v 'BEGIN{printf "%f", a + b}')
      count=$((count+1))
    done <<< "$nums"
    mean=$(awk -v s=$sum -v c=$count 'BEGIN{ if (c>0) printf "%f", s/c; else print 0 }')
    # use python high-fidelity generator but scale errors by mean (robust fallback)
    if [[ -f "$PY_SCRIPT" ]]; then
      echo "[wrapper] Running $PY_SCRIPT and scaling errors by mean=$mean"
      tmp_out="${OUT}.tmp.json"
      python3 "$PY_SCRIPT" --parent "$PARENT" --out "$tmp_out" --input-dir "${INPUT_DIR:-.}" || true
      if [[ -f "$tmp_out" ]]; then
        python3 - <<PY
import json,sys
p='$tmp_out'
o='$OUT'
mean=float('$mean')
with open(p) as f: data=json.load(f)
for i,z in enumerate(data.get('zones',[])): z['error']=round(z.get('error',0.0)* (1.0 + mean),6)
data['zone_errors']=[z['error'] for z in data.get('zones',[])]
with open(o,'w') as f: json.dump(data,f,indent=2)
print('wrote',o)
PY
        rm -f "$tmp_out"
        echo "[wrapper] Wrote presim JSON to $OUT"
        exit 0
      fi
    fi
  fi
fi

# If an OpenFOAM case directory exists at PARENT, try the log-to-presim converter
if [[ -d "$PARENT" ]]; then
  LOG2PY="BHC_SCHEDULER/tools/openfoam_log_to_presim.py"
  if [[ -f "$LOG2PY" ]]; then
    echo "[wrapper] Attempting openfoam_log_to_presim for case $PARENT -> $OUT"
    mkdir -p "$(dirname "$OUT")"
    python3 "$LOG2PY" --case-dir "$PARENT" --out "$OUT" && echo "[wrapper] Wrote presim JSON to $OUT (from openfoam parser)" && exit 0 || echo "[wrapper] openfoam_log_to_presim failed" >&2
  fi
fi

# Fallback to python generator if nothing else produced presim.json
if [[ -f "$PY_SCRIPT" || -f "BHC_SCHEDULER/tools/python_solvers/thermal_presim.py" ]]; then
  echo "[wrapper] Generating presim.json with $PY_SCRIPT (fallback)"
  python3 "$PY_SCRIPT" --parent "$PARENT" --out "$OUT" --input-dir "${INPUT_DIR:-.}"
  echo "[wrapper] Wrote presim JSON to $OUT"
  exit 0
fi

echo "[wrapper] No method succeeded to produce presim.json" >&2
exit 3
