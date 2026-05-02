#!/usr/bin/env python3
"""
Higher-fidelity thermal presim solver (Python).
Produces more accurate per-zone error estimates and lower uncertainty.
Usage:
  python3 tools/python_solvers/thermal_high_fidelity.py --parent <PARENT_ID> --out presim.json
"""
import argparse
import json
import os
import time
import math
import hashlib


def true_zone_error(job_id, zone_idx, base=0.02):
    # deterministic "true" error pattern with spatial correlation
    s = hashlib.sha256((job_id + ":true:" + str(zone_idx)).encode('utf-8')).digest()
    v = int.from_bytes(s[:4], 'little') % 10000
    # Create a banded pattern: some zones have higher true error
    band = (zone_idx % 16)
    bias = 1.0 + (band / 16.0) * 1.5
    return base * bias * (0.9 + (v / 10000.0) * 0.2)


def presim_estimate(true_err, zone_idx, noise_level=0.05):
    # presim approximates true error with small multiplicative noise
    # for higher fidelity solver, use lower noise and slight smoothing
    s = hashlib.sha256((str(zone_idx) + ":presim").encode('utf-8')).digest()
    v = int.from_bytes(s[:2], 'little') / 65535.0
    factor = 1.0 + (v - 0.5) * 2.0 * noise_level
    # small smoothing using neighbouring index
    smooth = 0.8 * true_err + 0.2 * true_err * (1.0 + ((zone_idx % 7) - 3) * 0.01)
    return smooth * factor


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--parent', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--input-dir', default=None)
    p.add_argument('--zones', type=int, default=64)
    p.add_argument('--base', type=float, default=0.02)
    args = p.parse_args()

    zones = args.zones
    zone_list = []
    zone_errors = []
    zone_sizes = []
    zone_uncertainty = []

    for i in range(zones):
        true_err = true_zone_error(args.parent, i, base=args.base)
        est = presim_estimate(true_err, i, noise_level=0.03)
        size = 0.5 + ((i % 8) / 8.0) * 2.0
        # uncertainty is low for high-fidelity solver
        unc = max(0.01, min(0.2, abs(est - true_err) / max(1e-6, true_err)))
        zone_list.append({"zone": i, "error": round(est, 6), "size": round(size, 6)})
        zone_errors.append(round(est, 6))
        zone_sizes.append(round(size, 6))
        zone_uncertainty.append(round(unc, 4))

    out = {
        "parent_job_id": args.parent,
        "case": "thermal",
        "method": "thermal_high_fidelity_py_v1",
        "timestamp": time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        "error_threshold": 0.03,
        "zones": zone_list,
        "zone_errors": zone_errors,
        "zone_sizes": zone_sizes,
        "zone_uncertainty": zone_uncertainty,
        "notes": "high-fidelity presim (synthetic, lower noise)"
    }

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w') as f:
        json.dump(out, f, indent=2)

    print(f"Wrote high-fidelity presim JSON to {args.out}")


if __name__ == '__main__':
    main()
