"""Episode-level evaluation of a training run (train.py's oof.csv: out-of-fold scores, i.e. for
people the model did not see).

As the watch uses it: the hand is already at a calibrated head region (stage 1), so the
negatives that matter are the look-alikes performed at the head; an episode is caught when any
of its gesture windows passes the threshold (the alert follows within a second of that window).
Prints, for a range of thresholds: the share of BFRB sequences caught, per gesture, and the share
of head look-alike sequences that would raise a false alert.

  python tools/habit/evaluate.py <run dir>
"""
import json
import os
import sys

import numpy as np
import pandas as pd

HEAD_LOOKALIKES = ["Drink from bottle/cup", "Glasses on/off", "Pull air toward your face"]

run = sys.argv[1]
oof = pd.read_csv(os.path.join(run, "oof.csv"), dtype={"seq": str})
seq = oof.groupby(["seq", "gesture", "y"], as_index=False)["oof"].max()
pos = seq[seq.y == 1]
neg_seq = oof[oof.y == 0].groupby(["seq", "gesture"], as_index=False)["oof"].max()
head = neg_seq[neg_seq.gesture.isin(HEAD_LOOKALIKES)]
other = neg_seq[~neg_seq.gesture.isin(HEAD_LOOKALIKES)]
rows = []
for thr in [0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]:
    rows.append(dict(threshold=thr, episodes_caught=round(float((pos.oof > thr).mean()), 3),
                     head_lookalike_false_alerts=round(float((head.oof > thr).mean()), 3),
                     other_false_alerts=round(float((other.oof > thr).mean()), 3)))
table = pd.DataFrame(rows)
print(table.to_string(index=False))
best = table[table.head_lookalike_false_alerts <= 0.05].sort_values("episodes_caught").iloc[-1]
thr = float(best.threshold)
per = pos.assign(c=pos.oof > thr).groupby("gesture")["c"].mean().round(3).sort_values()
print(f"\nat {thr}: per gesture caught\n{per.to_string()}")
print(f"\nhead look-alikes falsely alerting at {thr}:")
print(head.assign(f=head.oof > thr).groupby("gesture")["f"].mean().round(3).to_string())
json.dump(dict(table=rows, chosen_threshold=thr, per_gesture=per.to_dict()),
          open(os.path.join(run, "episodes.json"), "w"), indent=2)
