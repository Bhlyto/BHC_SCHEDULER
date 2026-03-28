#!/usr/bin/env python3
"""
Presim calibration sweep.

Writes `config/orchestrator.conf` with presim parameters for each combo,
runs `build/bin/presim_e2e_test` several times, collects full vs presim errors
and runtimes, and selects the best parameter set.

Usage:
  python3 tools/presim_calibrate.py --runs 3
"""
import argparse
import subprocess
import os
import csv
import itertools
import statistics

ROOT = os.getcwd()
BUILD_BIN = os.path.join(ROOT, 'build', 'bin', 'presim_e2e_test')
# Use a dedicated presim config file so we don't overwrite the main orchestrator.conf
CONFIG_PATH = os.path.join(ROOT, 'config', 'presim.conf')


def write_config(threshold_max, refine_mult, high_mult, uncertainty_weight, fidelity_map="0,1,3,6"):
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, 'w') as f:
        f.write(f"presim_threshold_max={threshold_max}\n")
        f.write(f"presim_refine_multiplier={refine_mult}\n")
        f.write(f"presim_high_multiplier={high_mult}\n")
        f.write(f"presim_uncertainty_weight={uncertainty_weight}\n")
        f.write(f"presim_fidelity_map={fidelity_map}\n")


def parse_output(out_text):
    # Extract metrics printed by presim_e2e_test
    rt_full = None; total_presim = None; err_full = None; err_presim = None
    for line in out_text.splitlines():
        line = line.strip()
        if line.startswith('Estimated runtimes'):
            # format: Estimated runtimes (ms): full=3520, presim_run=1184 (presim=320 + refine=864)
            try:
                part = line.split(':',1)[1]
                for kv in part.split(','):
                    if '=' in kv:
                        k,v = kv.split('=',1); k=k.strip(); v=v.strip().split()[0]
                        if k=='full': rt_full = float(v)
                    if 'presim_run' in kv:
                        # find first number after '='
                        v2 = kv.split('=')[1].strip().split()[0]
                        total_presim = float(v2)
            except Exception:
                pass
        if line.startswith('Estimated final avg error'):
            # format: Estimated final avg error: full=0.008999, presim=0.035996
            try:
                part = line.split(':',1)[1]
                for kv in part.split(','):
                    if '=' in kv:
                        k,v = kv.split('=',1); k=k.strip(); v=v.strip()
                        if k=='full': err_full = float(v)
                        if k=='presim': err_presim = float(v)
            except Exception:
                pass
    return rt_full, total_presim, err_full, err_presim


def run_trial(env_cmd):
    env = os.environ.copy()
    env['PRESIM_SOLVER_CMD'] = env_cmd
    p = subprocess.run([BUILD_BIN], env=env, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--runs', type=int, default=3)
    p.add_argument('--zones', type=int, default=64)
    p.add_argument('--out-csv', default='presim_calibration_results.csv')
    p.add_argument('--use-openfoam', action='store_true', help='Enable running /usr/bin/openfoam2412 via wrapper')
    args = p.parse_args()

    # solver command template -> use wrapper that can invoke OpenFOAM when env RUN_OPENFOAM=1
    solver_cmd = f"BHC_SCHEDULER/tools/openfoam_presim_wrapper.sh --parent {{parent}} --out {{out}} --input-dir {{input_dir}}"

    # small grid to start
    thresholds = [0.02, 0.03]
    refine_mults = [0.6, 0.8]
    high_mults = [1.5, 2.0]
    uncertainty_weights = [0.5, 1.0]

    combos = list(itertools.product(thresholds, refine_mults, high_mults, uncertainty_weights))

    rows = []
    for (thr, rmult, hmult, uw) in combos:
        print(f"Testing combo thr={thr} rmult={rmult} hmult={hmult} uw={uw}")
        write_config(thr, rmult, hmult, uw)
        trial_metrics = []
        # ensure RUN_OPENFOAM is visible to trials
        if args.use_openfoam:
            os.environ['RUN_OPENFOAM'] = '1'
        else:
            os.environ.pop('RUN_OPENFOAM', None)

        for i in range(args.runs):
            # run the full e2e test with PRESIM_SOLVER_CMD pointing at the wrapper
            rc, out = run_trial(solver_cmd)
            rt_full, total_presim, err_full, err_presim = parse_output(out)
            success = (rc == 0)
            trial_metrics.append((success, rt_full, total_presim, err_full, err_presim, out))
            print(f" run {i+1}/{args.runs}: rc={rc} full_err={err_full} presim_err={err_presim} full_rt={rt_full} presim_rt={total_presim}")
        # aggregate over all trials (use parsed metrics even if CI failed)
        vals_full_err = [t[3] for t in trial_metrics if t[3] is not None]
        vals_presim_err = [t[4] for t in trial_metrics if t[4] is not None]
        vals_full_rt = [t[1] for t in trial_metrics if t[1] is not None]
        vals_presim_rt = [t[2] for t in trial_metrics if t[2] is not None]
        avg_full_err = statistics.mean(vals_full_err) if vals_full_err else None
        avg_presim_err = statistics.mean(vals_presim_err) if vals_presim_err else None
        avg_full_rt = statistics.mean(vals_full_rt) if vals_full_rt else None
        avg_presim_rt = statistics.mean(vals_presim_rt) if vals_presim_rt else None

        rows.append({
            'threshold_max': thr,
            'refine_mult': rmult,
            'high_mult': hmult,
            'uncertainty_weight': uw,
            'avg_full_err': avg_full_err,
            'avg_presim_err': avg_presim_err,
            'avg_full_rt': avg_full_rt,
            'avg_presim_rt': avg_presim_rt,
            'raw_trials': trial_metrics,
        })

    # choose best: prefer avg_presim_err <= 1.2 * avg_full_err, minimize avg_presim_rt
    candidates = [r for r in rows if r['avg_full_err'] and r['avg_presim_err'] and r['avg_presim_err'] <= 1.2 * r['avg_full_err']]
    if candidates:
        best = min(candidates, key=lambda r: r['avg_presim_rt'] if r['avg_presim_rt'] is not None else float('inf'))
    else:
        # fallback: minimize error ratio
        best = min([r for r in rows if r['avg_full_err'] and r['avg_presim_err']], key=lambda r: (r['avg_presim_err']/r['avg_full_err']))

    # write CSV
    with open(args.out_csv, 'w', newline='') as csvf:
        writer = csv.writer(csvf)
        writer.writerow(['threshold_max','refine_mult','high_mult','uncertainty_weight','avg_full_err','avg_presim_err','avg_full_rt','avg_presim_rt'])
        for r in rows:
            writer.writerow([r['threshold_max'], r['refine_mult'], r['high_mult'], r['uncertainty_weight'], r['avg_full_err'], r['avg_presim_err'], r['avg_full_rt'], r['avg_presim_rt']])

    print('\nBest parameters:')
    print(best)
    print(f"Results written to {args.out_csv}")


if __name__ == '__main__':
    main()
