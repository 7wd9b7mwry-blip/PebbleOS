# Habit detection: tools

Two-stage detection on the watch (`fw/services/habit`): the wearer's calibrated head regions say
where the hand is (`habit_detect`), and a classifier on orientation-invariant motion features says
whether it is picking / pulling / scratching or a look-alike (`habit_features`, `habit_model`).

## Classifier

- **Features** (`habit_features.c`): 16 integers from a 3 s window at 10 Hz (the watch averages
  its 25 Hz samples into 100 ms bins). 10 Hz is the rate of the training data; the watch's faster
  detail would otherwise look unlike anything the model saw.
- **Model**: 60 gradient-boosted trees (LightGBM), generated into
  `fw/services/habit/habit_model_data.h` as integer node tables (about 27 KB).
- **Data**: Kaggle "CMI - Detect Behavior with Sensor Data" (Child Mind Institute, Helios wrist
  device, 81 people, 8151 sequences; requires accepting the competition rules). Rotation rates are
  derived from the orientation quaternions. The leg gestures (pinch / scratch / write on leg) are
  left out: their motion is a BFRB's, at a place the region stage already rules out.

### Results (cmi-v2, out of fold: people the model did not see)

| decision threshold | BFRB episodes caught | head look-alikes falsely alerting |
|---|---|---|
| 0.5 | 85 % | 7.5 % |
| **0.6 (shipped)** | **78 %** | **4 %** (drink 5 %, glasses 3.7 %, pull air to face 3.8 %) |
| 0.7 | 69 % | 1.6 % |

Per gesture at 0.6: eyelash 70 %, hairline 72 %, above ear 74 %, forehead scratch 75 %, cheek
pinch 79 %, eyebrow 83 %, neck scratch 83 %, neck pinch 85 %. Window AUC 0.906. Nail biting and
nose picking are not in the public data: they come from the wearer's own labelled windows (data
logging tag 121) once the watch is worn.

### Retraining

```shell
# host tool: the firmware's own feature and model code
cc -O2 -I include -o habit_features tools/habit/features_cli.c \
   fw/services/habit/habit_features.c fw/services/habit/habit_model.c
python tools/habit/train.py --source cmi --data <dir with train.csv> --src-hz 10 \
   --features-cli habit_features --out run --version cmi-vN \
   --exclude-gestures "Pinch knee/leg skin;Scratch knee/leg skin;Write name on leg"
python tools/habit/evaluate.py run            # episode-level table, pick the threshold
# set .threshold in run/habit_model_data.h (logit x 1024 of the chosen probability),
# rebuild habit_features with it, then:
python tools/habit/check_export.py --source cmi --data <dir> --features-cli habit_features --out run
cp run/habit_model_data.h fw/services/habit/
```

`check_export.py` requires the C scores to match LightGBM's (cmi-v2: at most 7.6 / 1024 apart on
22228 windows). `test_habit_features` pins two real windows' scores.

## Emulator

`e2e.sh` (detection scenarios through `inject_accel.py`), `persist_test.sh` (settings survive a
reboot), `sound_check.sh` (sound level). `console.py` sends console commands.
