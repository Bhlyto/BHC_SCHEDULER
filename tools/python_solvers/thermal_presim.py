#!/usr/bin/env python3
"""
Thermal presim solver in Python. Can be used as a worker app without recompiling orchestrator.

Usage (as an orchestrator job):
  python3 tools/python_solvers/thermal_presim.py --parent <PARENT_ID> --out presim.json

This script produces the same presim.json schema used by the C presim runner.
"""
import argparse
import json
import os
import time
import hashlib


def deterministic_zone_error(job_id, zone_idx, base=0.05):
    h = hashlib.sha256((job_id + str(zone_idx)).encode('utf-8')).digest()
    v = int.from_bytes(h[:4], 'little') % 10000
    m = 0.6 + (v / 10000.0) * 0.8
    return base * m


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--parent', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--input-dir', default=None)
    # Increased default zone count for higher presim resolution; can be overridden with --zones
    p.add_argument('--zones', type=int, default=64)
    p.add_argument('--threshold', type=float, default=0.05)
    p.add_argument('--model', default=None, help='optional ML model to use')
    args = p.parse_args()

    # Placeholder for ML model usage: if a model is provided, load and use it here.
    if args.model:
        # e.g., load a PyTorch / TensorFlow model and run inference on inputs
        print(f"[info] model provided ({args.model}), but ML inference not implemented in this template")

    zones_out = []
    zone_errors = []
    zone_sizes = []
    zone_uncertainty = []
    for i in range(args.zones):
        # finer-grained deterministic errors across more zones
        err = deterministic_zone_error(args.parent, i, base=args.threshold)
        # vary sizes more smoothly for many zones
        size = 0.5 + ((i % 8) / 8.0) * 2.0
        zones_out.append({"zone": i, "error": round(err, 6), "size": round(size, 6)})
        zone_errors.append(round(err, 6))
        zone_sizes.append(round(size, 6))
        # Synthetic uncertainty: smaller zones or higher error get higher uncertainty
        unc = min(1.0, abs(err) / (args.threshold * 2.0) * (0.5 + ((i % 4) / 4.0)))
        zone_uncertainty.append(round(unc, 4))

    out = {
        "parent_job_id": args.parent,
        "case": "thermal",
        "method": "thermal_presim_py_v1",
        "timestamp": time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        "error_threshold": args.threshold,
        "zones": zones_out,
        "zone_errors": zone_errors,
        "zone_sizes": zone_sizes,
        "zone_uncertainty": zone_uncertainty,
        "notes": "python thermal presim"
    }

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w') as f:
        json.dump(out, f, indent=2)

    print(f"Wrote presim JSON to {args.out}")


if __name__ == '__main__':
    main()
