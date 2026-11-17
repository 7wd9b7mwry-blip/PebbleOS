#!/bin/bash
# Sound level: silence for a minute, then a 25% (-12 dBFS peak, -15 dBFS RMS) 440 Hz tone.
P="$(cd "$(dirname "$0")" && pwd)"
cd /root/pebbleos
. /root/pebbleos-sdk-0.1.11/env.sh
. .venv/bin/activate
for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
sleep 2
(timeout 300 pbl run -- -display none < /dev/null > /root/sound-run.log 2>&1 &)
sleep 40
echo "== after a silent minute"
sleep 65
python3 $P/console.py "habit status" | grep -E "sound"
echo "== with a tone"
timeout 75 pbl emulator mic --tone 440 --volume 25 --duration 70 > /root/mic.log 2>&1
python3 $P/console.py "habit status" | grep -E "sound"
tail -2 /root/mic.log
for pid in $(pgrep -x qemu-pebble); do kill "$pid"; done
