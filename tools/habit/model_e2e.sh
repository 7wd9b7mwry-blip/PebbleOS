#!/bin/bash
# Emulator: resting and picking with the classifier on, then picking with it off (the emulator has
# no gyroscope, so the classifier's rotation features are 0 there and its decisions are not
# representative: the classifier itself is checked on real recorded windows in the unit tests).
P="$(cd "$(dirname "$0")" && pwd)"
cd /root/pebbleos
. /root/pebbleos-sdk-0.1.11/env.sh
. .venv/bin/activate
for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
sleep 2
(timeout 300 pbl run -- -display none < /dev/null > /root/model-run.log 2>&1 &)
sleep 40
for scen in resting picking; do
  python3 $P/inject_accel.py $scen > /dev/null
  echo "model on, after $scen:"; python3 $P/console.py "habit status" | grep -E "episodes|model "
done
python3 $P/console.py "habit model off" > /dev/null
python3 $P/inject_accel.py picking > /dev/null
echo "model off, after picking:"; python3 $P/console.py "habit status" | grep -E "episodes|model "
for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
