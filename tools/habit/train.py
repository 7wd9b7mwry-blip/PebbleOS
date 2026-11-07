"""Train the habit classifier and generate fw/services/habit/habit_model_data.h.

The features are the firmware's own (habit_features.c, run through the features_cli host tool), so
the watch scores exactly what was trained. Sources:

  --source cmi --data DIR      Kaggle "CMI - Detect Behavior with Sensor Data" (train.csv,
                               train_demographics.csv): Helios wrist IMU, 8 BFRB-like gestures
                               (hair / eyebrow / eyelash pulling, forehead / neck scratching,
                               cheek / neck pinching) against 10 look-alikes (drinking, glasses,
                               phone, waving, ...). Converted to 25 Hz, mG and 0.1 deg/s; the
                               rotation rate is derived from the orientation quaternions.
  --source synthetic           Generated sequences, to check the pipeline end to end.

Windows are labelled from the gesture phase: a window mostly inside a BFRB gesture is positive,
one inside a non-BFRB gesture (or anything in a non-BFRB sequence) negative; target-sequence
windows still moving the hand into place are left out. Evaluation: GroupKFold by subject, so the
numbers are for people the model has not seen.

  python tools/habit/train.py --source cmi --data ~/cmi --features-cli <path> --out <dir>
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

import numpy as np
import pandas as pd

HZ = 25
G = 9.80665
FEATURES = ["hp_mag_mean", "hp_mag_std", "hp_mag_max", "slow_motion", "jerk", "gyro_mag_mean",
            "gyro_mag_std", "gyro_mag_max", "gyro_hp_mean", "grav_turn", "grav_path", "hp_zcr",
            "gyro_zcr", "repetition", "gyro_to_acc", "gravity_error"]
WINDOW, HOP = 50, 25


# ---- sources ----------------------------------------------------------------------------------

def quat_rate(q, hz):
    """Body-frame angular rate (deg/s) from consecutive unit quaternions (w, x, y, z)."""
    q = q / np.linalg.norm(q, axis=1, keepdims=True)
    w = np.zeros((len(q), 3))
    if len(q) < 2:
        return w
    a, b = q[:-1], q[1:]
    # dq = conj(a) * b
    aw, ax, ay, az = a.T
    bw, bx, by, bz = b.T
    dw = aw * bw + ax * bx + ay * by + az * bz
    dx = aw * bx - ax * bw - ay * bz + az * by
    dy = aw * by + ax * bz - ay * bw - az * bx
    dz = aw * bz - ax * by + ay * bx - az * bw
    sign = np.where(dw < 0, -1.0, 1.0)  # shortest rotation
    dx, dy, dz, dw = dx * sign, dy * sign, dz * sign, np.clip(dw * sign, -1, 1)
    angle = 2 * np.arccos(dw)
    s = np.sqrt(np.maximum(1 - dw * dw, 1e-12))
    rate = (angle * hz)[:, None] * np.stack([dx, dy, dz], 1) / s[:, None]
    w[1:] = np.degrees(rate)
    w[0] = w[1]
    return w


def resample(x, src_hz, dst_hz=HZ):
    if len(x) < 2 or src_hz == dst_hz:
        return x
    t_src = np.arange(len(x)) / src_hz
    t_dst = np.arange(0, t_src[-1] + 1e-9, 1 / dst_hz)
    return np.stack([np.interp(t_dst, t_src, x[:, i]) for i in range(x.shape[1])], 1)


def load_cmi(data_dir, src_hz):
    df = pd.read_csv(os.path.join(data_dir, "train.csv"),
                     usecols=lambda c: not c.startswith(("thm_", "tof_")))
    seqs, labels = [], []
    for seq_id, g in df.groupby("sequence_id", sort=False):
        g = g.sort_values("sequence_counter")
        acc = g[["acc_x", "acc_y", "acc_z"]].to_numpy(float) * 1000 / G
        q = g[["rot_w", "rot_x", "rot_y", "rot_z"]].to_numpy(float)
        has_rot = not np.isnan(q).any()
        gyro = quat_rate(q, src_hz) * 10 if has_rot else np.zeros_like(acc)
        x = resample(np.hstack([acc, gyro]), src_hz)
        # phase per resampled sample
        phase_src = g["behavior"].astype(str).to_numpy()
        idx = np.minimum((np.arange(len(x)) * src_hz / HZ).round().astype(int), len(phase_src) - 1)
        phase = phase_src[idx]
        target = (g["sequence_type"].iloc[0] == "Target")
        seqs.append((str(seq_id), x, has_rot))
        labels.append(dict(seq=str(seq_id), subject=str(g["subject"].iloc[0]),
                           gesture=str(g["gesture"].iloc[0]), target=target,
                           orientation=str(g.get("orientation", pd.Series(["?"])).iloc[0]),
                           phase=phase))
    return seqs, labels


def load_synthetic(n_subjects=12, per=40, seed=1):
    rng = np.random.default_rng(seed)
    seqs, labels = [], []
    kinds = ["picking", "pulling", "scratch", "still", "drink", "wave", "phone", "glasses"]
    for s in range(n_subjects):
        gain = rng.uniform(0.7, 1.4)
        for k in range(per):
            kind = kinds[k % len(kinds)]
            n = int(HZ * rng.uniform(5, 8))
            t = np.arange(n) / HZ
            base = np.array([-940, 0, -340]) + rng.normal(0, 60, 3)
            acc = np.tile(base, (n, 1)).astype(float)
            gyro = rng.normal(0, 20, (n, 3))
            f = rng.uniform(3, 6)
            if kind in ("picking", "pulling", "scratch"):
                amp = {"picking": 40, "pulling": 70, "scratch": 110}[kind] * gain
                acc += amp * np.stack([np.sin(2 * np.pi * f * t + p) for p in (0, 1, 2)], 1)
                gyro += 10 * amp * np.stack([np.sin(2 * np.pi * f * t + p) for p in (2, 0, 1)], 1)
            elif kind == "wave":
                acc += 350 * gain * np.stack([np.sin(2 * np.pi * 2 * t)] * 3, 1)
                gyro += 2500 * np.stack([np.sin(2 * np.pi * 2 * t), 0 * t, 0 * t], 1)
            elif kind == "drink":
                ang = np.clip(t / 2, 0, 1) * 0.9
                acc[:, 0] = -940 * np.cos(ang)
                acc[:, 2] = -340 - 600 * np.sin(ang)
                gyro[:, 1] += np.where(t < 2, 260, 0)
            elif kind == "phone":
                acc += 8 * rng.normal(0, 1, (n, 3))
            elif kind == "glasses":
                acc += 150 * np.stack([np.sin(2 * np.pi * 0.8 * t)] * 3, 1)
                gyro += 600 * np.stack([np.sin(2 * np.pi * 0.8 * t)] * 3, 1)
            acc += rng.normal(0, 4, (n, 3))
            seq = f"s{s}_{k}"
            phase = np.array(["Performs gesture"] * n)
            phase[: HZ] = "Moves hand to target location"
            seqs.append((seq, np.hstack([acc, gyro]), True))
            labels.append(dict(seq=seq, subject=f"subj{s}", gesture=kind,
                               target=kind in ("picking", "pulling", "scratch"),
                               orientation="synthetic", phase=phase))
    return seqs, labels


# ---- features (the firmware's code) -----------------------------------------------------------

def run_features(seqs, cli):
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(cli))) as d:
        inp, out = os.path.join(d, "samples.csv"), os.path.join(d, "features.csv")
        with open(inp, "w", newline="\n") as fh:
            for seq, x, has_gyro in seqs:
                xi = np.rint(np.clip(x, -32767, 32767)).astype(int)
                for row in xi:
                    vals = row if has_gyro else row[:3]
                    fh.write(seq + "," + ",".join(map(str, vals)) + "\n")
        cmd = cli_command(cli)
        with open(inp, "rb") as fi, open(out, "wb") as fo:
            subprocess.run(cmd, stdin=fi, stdout=fo, check=True)
        return pd.read_csv(out, dtype={"seq": str})


def cli_command(cli):
    if os.name == "nt":  # the host tool is a Linux binary built in WSL
        p = os.path.abspath(cli).replace("\\", "/")
        wsl_path = "/mnt/" + p[0].lower() + p[2:]
        return ["wsl.exe", "-d", "Ubuntu-24.04", "--", wsl_path]
    return [cli]


def label_windows(feat, labels):
    by_seq = {l["seq"]: l for l in labels}
    y, keep, meta = [], [], []
    for seq, end in zip(feat["seq"], feat["end"]):
        l = by_seq[seq]
        ph = l["phase"][end - WINDOW + 1: end + 1]
        in_gesture = np.mean(np.char.startswith(ph.astype(str), "Performs")) >= 0.5
        if l["target"]:
            if in_gesture:
                y.append(1); keep.append(True)
            else:
                y.append(0); keep.append(False)  # hand still moving into place: ambiguous
        else:
            y.append(0); keep.append(True)
        meta.append((l["subject"], l["gesture"]))
    feat = feat.assign(y=y, keep=keep, subject=[m[0] for m in meta], gesture=[m[1] for m in meta])
    return feat[feat.keep].drop(columns="keep")


# ---- model ------------------------------------------------------------------------------------

def train_eval(df, args):
    import lightgbm as lgb
    from sklearn.metrics import roc_auc_score
    from sklearn.model_selection import GroupKFold

    X, y, groups = df[FEATURES].to_numpy(), df["y"].to_numpy(), df["subject"].to_numpy()
    params = dict(objective="binary", n_estimators=args.trees, num_leaves=args.leaves,
                  max_depth=args.depth, learning_rate=0.1, min_child_samples=40,
                  subsample=0.8, subsample_freq=1, colsample_bytree=0.8, verbose=-1)
    oof = np.zeros(len(y))
    for tr, te in GroupKFold(n_splits=min(5, len(set(groups)))).split(X, y, groups):
        m = lgb.LGBMClassifier(**params).fit(X[tr], y[tr])
        oof[te] = m.predict_proba(X[te])[:, 1]
    auc = roc_auc_score(y, oof)
    # operating point: at most args.fpr of the look-alike windows flagged
    neg = np.sort(oof[y == 0])
    thr = float(neg[int((1 - args.fpr) * (len(neg) - 1))])
    recall = float(np.mean(oof[y == 1] > thr))
    per_gesture = df.assign(p=oof > thr).groupby("gesture")["p"].mean().sort_values()
    final = lgb.LGBMClassifier(**params).fit(X, y)
    return final, dict(auc=auc, threshold_prob=thr, recall_at_fpr=recall, fpr=args.fpr,
                       windows=int(len(y)), positives=int(y.sum()),
                       subjects=int(len(set(groups))),
                       per_gesture_flag_rate={k: round(float(v), 3)
                                              for k, v in per_gesture.items()})


def export_c(model, threshold_prob, version, path):
    dump = model.booster_.dump_model()
    nodes, roots = [], []

    def walk(t, out):
        i = len(out)
        out.append(None)
        if "leaf_value" in t:
            out[i] = (0xFF, 0, 0, 0, int(round(t["leaf_value"] * 1024)))
        else:
            assert t["decision_type"] == "<=", t["decision_type"]
            left = walk(t["left_child"], out)
            right = walk(t["right_child"], out)
            # integer features: x <= t  <=>  x <= floor(t)
            out[i] = (t["split_feature"], int(math.floor(t["threshold"])), left, right, 0)
        return i

    for tree in dump["tree_info"]:
        local = []
        walk(tree["tree_structure"], local)
        roots.append(len(nodes))
        nodes.extend(local)
    thr_logit = math.log(threshold_prob / (1 - threshold_prob)) if 0 < threshold_prob < 1 else 0
    lines = [
        "/* SPDX-FileCopyrightText: 2026 7wd9b7mwry-blip */",
        "/* SPDX-License-Identifier: Apache-2.0 */",
        "",
        f"// Generated by tools/habit/train.py ({version}). Do not edit.",
        "",
        "static const HabitModelNode s_nodes[] = {",
    ]
    for f, thr, l, r, v in nodes:
        lines.append(f"    {{{f}, {thr}, {l}, {r}, {v}}},")
    lines += ["};", "", "static const uint16_t s_roots[] = {"]
    lines += ["    " + ", ".join(map(str, roots[i:i + 16])) + "," for i in range(0, len(roots), 16)]
    lines += ["};", "",
              "const HabitModel g_habit_model = {",
              "    .nodes = s_nodes,",
              f"    .num_nodes = {len(nodes)},",
              "    .tree_roots = s_roots,",
              f"    .num_trees = {len(roots)},",
              "    .bias = 0,",
              f"    .threshold = {int(round(thr_logit * 1024))},",
              f'    .version = "{version}",',
              "};", ""]
    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(lines))
    return len(nodes), len(roots)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", choices=["cmi", "synthetic"], required=True)
    ap.add_argument("--data")
    ap.add_argument("--src-hz", type=float, default=10.0, help="CMI IMU sample rate")
    ap.add_argument("--features-cli", required=True, help="habit_features host binary")
    ap.add_argument("--out", required=True, help="directory for habit_model_data.h and report")
    ap.add_argument("--trees", type=int, default=60)
    ap.add_argument("--leaves", type=int, default=15)
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--fpr", type=float, default=0.05)
    ap.add_argument("--version", default="dev")
    args = ap.parse_args()

    seqs, labels = (load_cmi(args.data, args.src_hz) if args.source == "cmi"
                    else load_synthetic())
    print(f"{len(seqs)} sequences", file=sys.stderr)
    feat = run_features(seqs, args.features_cli)
    df = label_windows(feat, labels)
    model, report = train_eval(df, args)
    os.makedirs(args.out, exist_ok=True)
    n_nodes, n_trees = export_c(model, report["threshold_prob"], args.version,
                                os.path.join(args.out, "habit_model_data.h"))
    report.update(nodes=n_nodes, trees=n_trees, version=args.version, source=args.source)
    # raw scores for the C-vs-Python check
    df.assign(py_logit=model.predict(df[FEATURES].to_numpy(), raw_score=True)) \
      [["seq", "end", "py_logit"]].to_csv(os.path.join(args.out, "py_scores.csv"), index=False)
    with open(os.path.join(args.out, "report.json"), "w") as fh:
        json.dump(report, fh, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
