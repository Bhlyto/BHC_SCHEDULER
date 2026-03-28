#!/usr/bin/env python3
"""
Generate openfoam_presim.json from an OpenFOAM case log and sampled fields.

Usage: tools/openfoam_log_to_presim.py --case-dir <case> --out <out.json>

This script:
- parses `log.*` files (solver log) to extract final residuals for variables
- runs the Python high-fidelity generator as a base and scales zone errors
- writes `openfoam_presim.json` with fields: zones, zone_errors, zone_sizes, zone_uncertainty
"""
import argparse
import os
import re
import json
import subprocess
import math


def parse_final_residuals(logpath):
    """Return a dict of variable -> last final residual found in the log."""
    final_re = re.compile(r'Final residual =\s*([0-9.eE+-]+)')
    var_re = re.compile(r'Solving for (\w+)')
    residuals = {}
    last_vars = []
    try:
        with open(logpath, 'r') as f:
            for line in f:
                # track variable name if present on same line as solver start
                mvar = var_re.search(line)
                if mvar:
                    last_vars.append(mvar.group(1))
                m = final_re.search(line)
                if m:
                    val = float(m.group(1))
                    # assign to last seen var if available, else to 'unknown'
                    if last_vars:
                        v = last_vars[-1]
                    else:
                        v = 'unknown'
                    residuals[v] = val
    except FileNotFoundError:
        return {}
    return residuals


def run_base_hf(case_dir, tmp_out):
    py = os.path.join('BHC_SCHEDULER', 'tools', 'python_solvers', 'thermal_high_fidelity.py')
    if not os.path.isfile(py):
        raise SystemExit('HF generator not found: ' + py)
    subprocess.run(['python3', py, '--parent', case_dir, '--out', tmp_out, '--input-dir', case_dir], check=True)


def compute_scale_from_residuals(residuals):
    # Prefer temperature residual 'T' or 'T_' variants
    candidates = [k for k in residuals.keys() if k.lower().startswith('t')]
    if candidates:
        vals = [residuals[k] for k in candidates]
    else:
        vals = list(residuals.values())
    if not vals:
        mean = 0.0
    else:
        mean = sum(vals) / len(vals)
    # map mean residual to a multiplicative error scale
    # residuals are often small (1e-6..1e-2); use a conservative gain to avoid over-scaling
    # scaled by 500 and capped at +0.5 (i.e., scale in [1.0,1.5])
    scale = 1.0 + max(0.0, min(0.5, mean * 500.0))
    return scale, mean


def synthesize_presim(case_dir, out_path):
    # find a solver log in the case dir
    log_candidates = [p for p in os.listdir(case_dir) if p.startswith('log.')]
    logpath = None
    if log_candidates:
        # prefer the solver log (contains solver name)
        logpath = os.path.join(case_dir, log_candidates[0])
    else:
        # try common names
        for n in ('log.buoyantSimpleFoam', 'log.simpleFoam', 'log.buoyantPimpleFoam'):
            p = os.path.join(case_dir, n)
            if os.path.isfile(p):
                logpath = p
                break

    residuals = parse_final_residuals(logpath) if logpath else {}
    scale, mean_res = compute_scale_from_residuals(residuals)

    # generate base HF presim
    tmp = out_path + '.tmp.json'
    run_base_hf(case_dir, tmp)
    with open(tmp) as f:
        data = json.load(f)

    # scale errors and set uncertainty based on residuals
    for z in data.get('zones', []):
        z_err = z.get('error', 0.0)
        z['error'] = round(z_err * scale, 6)
    data['zone_errors'] = [z['error'] for z in data.get('zones', [])]
    # set zone_uncertainty proportional to mean residual
    uncertainty = round(min(0.5, max(0.01, mean_res * 10.0)), 4)
    data['zone_uncertainty'] = [uncertainty for _ in data.get('zones', [])]
    data['notes'] = f'openfoam-derived presim (scale={scale:.3f}, mean_res={mean_res:.3e})'

    with open(out_path, 'w') as f:
        json.dump(data, f, indent=2)

    os.remove(tmp)
    return out_path


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--case-dir', required=True)
    p.add_argument('--out', required=True)
    args = p.parse_args()
    out = synthesize_presim(args.case_dir, args.out)
    print('wrote', out)


if __name__ == '__main__':
    main()
