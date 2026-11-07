#!/bin/bash
# Settings persistence: set them, reboot the emulator keeping its flash, check they are back.
P="$(cd "$(dirname "$0")" && pwd)"
cd /root/pebbleos
. /root/pebbleos-sdk-0.1.11/env.sh
. .venv/bin/activate
boot() {
  for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
  sleep 2
  (timeout 300 pbl run "$@" -- -display none < /dev/null > /root/persist-run.log 2>&1 &)
  sleep 40
}
boot
echo "== fresh boot"
python3 $P/console.py "habit status" | grep -E "vibrate|hand at"
python3 $P/inject_accel.py resting > /dev/null &
sleep 8
python3 $P/console.py "habit cal hair" "habit vibrate off" "habit sleepalerts on" "sleep:1" "habit status" | grep -E "vibrate"
wait
echo "== after reboot (flash kept)"
boot --keep-flash-image
python3 $P/console.py "habit status" | grep -E "vibrate|hand at"
echo "== reset"
python3 $P/console.py "habit uncal" "habit vibrate on" "habit sleepalerts off" "sleep:1" "habit status" | grep -E "vibrate"
for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
