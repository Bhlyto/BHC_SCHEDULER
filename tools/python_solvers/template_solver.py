#!/usr/bin/env python3
"""
Generic solver template for presimulation tasks.
One solver = one domain. This script reads inputs from parent job input dir
and writes a standardized presim JSON to --out.

Usage:
  template_solver.py --parent <PARENT_ID> --case <domain> --out <out.json> [--input-dir <dir>]
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


def run_presim(parent_job_id, case_name, input_dir, out_path, zones=8, threshold=0.05):
    zones_out = []
    zone_errors = []
    zone_sizes = []
    for i in range(zones):
        err = deterministic_zone_error(parent_job_id, i, base=threshold)
        size = 1.0 + (i % 3) * 0.5
        zones_out.append({"zone": i, "error": round(err, 6), "size": round(size, 6)})
        zone_errors.append(round(err, 6))
        zone_sizes.append(round(size, 6))

    out = {
        "parent_job_id": parent_job_id,
        "case": case_name,
        "method": "template_presim_v1",
        "timestamp": time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        "error_threshold": threshold,
        "zones": zones_out,
        "zone_errors": zone_errors,
        "zone_sizes": zone_sizes,
        "notes": "template presim output"
    }

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w') as f:
        json.dump(out, f, indent=2)

    print(f"Wrote presim JSON to {out_path}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--parent', required=True)
    p.add_argument('--case', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--input-dir', default=None)
    p.add_argument('--zones', type=int, default=8)
    p.add_argument('--threshold', type=float, default=0.05)
    args = p.parse_args()

    run_presim(args.parent, args.case, args.input_dir, args.out, zones=args.zones, threshold=args.threshold)


if __name__ == '__main__':
    main()
