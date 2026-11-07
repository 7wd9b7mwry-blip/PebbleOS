"""Feed a synthetic wrist-motion scenario into the running emulator's accelerometer, in real time.

Phases: (seconds, (gx, gy, gz) unit gravity direction, motion amplitude mG, motion Hz).
Each 200 ms a 5-sample message (25 Hz) replaces the emulator's buffer; it replays at the rate
the firmware runs the accelerometer, so live feeding keeps the timing right.
"""
import math
import sys
import time

from libpebble2.communication import PebbleConnection
from libpebble2.communication.transports.qemu import MessageTargetQemu, QemuTransport
from libpebble2.communication.transports.qemu.protocol import QemuAccel, QemuAccelSample

DESK = (0.0, 0.0, -1.0)
FACE = (-0.94, 0.0, -0.34)
SCENARIOS = {
    # hand to the face with small 4 Hz movements for 12 s
    "picking": [(4, DESK, 3, 2), (12, FACE, 60, 4), (5, DESK, 3, 2)],
    # chin on the hand, still
    "resting": [(4, DESK, 3, 2), (12, FACE, 2, 4), (5, DESK, 3, 2)],
}

phases = SCENARIOS[sys.argv[1] if len(sys.argv) > 1 else "picking"]
pebble = PebbleConnection(QemuTransport("127.0.0.1", 12344))
pebble.connect()
pebble.run_async()

t0 = time.monotonic()
t = 0.0
prev = DESK
for dur, g, amp, hz in phases:
    n = int(dur * 25)
    for i in range(0, n, 5):
        samples = []
        for k in range(5):
            # 0.5 s blend from the previous orientation
            f = min(1.0, (i + k) / 12.5)
            gx, gy, gz = (prev[j] + (g[j] - prev[j]) * f for j in range(3))
            nrm = math.sqrt(gx * gx + gy * gy + gz * gz)
            m = amp * math.sin(2 * math.pi * hz * t)
            samples.append(QemuAccelSample(x=int(gx / nrm * 1000 + m), y=int(gy / nrm * 1000 + 0.7 * m),
                                           z=int(gz / nrm * 1000 - 0.5 * m)))
            t += 0.04
        pebble.transport.send_packet(QemuAccel(samples=samples), target=MessageTargetQemu())
        ahead = t0 + t - time.monotonic()
        if ahead > 0:
            time.sleep(ahead)
    prev = g
print(f"fed {t:.1f} s")
