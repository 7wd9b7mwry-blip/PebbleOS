#!/bin/bash
# Rebuild the emulator firmware, boot it headless, and run the habit end-to-end checks.
set -u
P="$(cd "$(dirname "$0")" && pwd)"
cd /root/pebbleos
. /root/pebbleos-sdk-0.1.11/env.sh
. .venv/bin/activate
pbl build > /root/e2e-build.log 2>&1 || { grep -E " error: " /root/e2e-build.log | head; exit 1; }
# stop a running emulator (match the binary name only, not this script)
for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
sleep 2
(timeout 400 pbl run -- -display none < /dev/null > /root/e2e-run.log 2>&1 &)
sleep 40
echo "== stream check: calm for 6 s"
python3 $P/console.py "habit play calm 6" "sleep:2" "habit status" "sleep:6" "habit status" | grep -E "^\\$|playing"
for scen in resting picking; do
  echo "== scenario $scen"
  python3 $P/console.py "habit status" | grep episodes
  python3 $P/inject_accel.py $scen &
  sleep 12
  python3 $P/console.py "habit status" | grep -E "hand at|episodes|playing"
  wait
  python3 $P/console.py "habit status" | grep -E "hand at|episodes"
done
