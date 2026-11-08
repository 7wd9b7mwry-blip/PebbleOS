"""Check the generated C model against LightGBM: the features tool, built with the generated
habit_model_data.h, must score every training window as Python did (logit x 1024, within the
rounding of the leaf values: <= 1 per tree, plus threshold flooring which is exact for integer
features).

  python tools/habit/check_export.py --source synthetic --features-cli <tool built with the model>
      --out <train.py output dir>
"""
import argparse
import json
import os
import sys

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--source", required=True)
ap.add_argument("--stressense")
ap.add_argument("--stressense-hz", type=float, default=50.0)
ap.add_argument("--data")
ap.add_argument("--src-hz", type=float, default=10.0)
ap.add_argument("--features-cli", required=True)
ap.add_argument("--out", required=True)
args = ap.parse_args()

seqs, _ = train.load_sources(args)
c = train.run_features(seqs, args.features_cli)
py = pd.read_csv(os.path.join(args.out, "py_scores.csv"), dtype={"seq": str})
m = py.merge(c[["seq", "end", "score"]], on=["seq", "end"], how="left")
trees = json.load(open(os.path.join(args.out, "report.json")))["trees"]
missing = int(m["score"].isna().sum())
diff = (m["score"] - m["py_logit"] * 1024).abs()
worst = float(diff.max())
print(f"{len(m)} windows, {missing} missing; max |C - Python| = {worst:.1f} (allowed {trees})")
same_side = np.mean(np.sign(m["score"]) == np.sign(m["py_logit"]))
print(f"same decision sign: {same_side:.4f}")
sys.exit(0 if missing == 0 and worst <= trees else 1)
