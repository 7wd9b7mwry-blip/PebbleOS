"""The wave programs for the Pebble's LRA (AW86225, 240 Hz): one continuous vibration swelling and ebbing like a
breath (a raised cosine in short ramps; the driver changes the gain mid-playback), at the motor's full range. On the
wearer's watch (2026-11-18) these smooth waves read as waves where tap trains felt choppy; the 2026-11-16 smooth
swell had felt like a steady hum only because the range was squeezed (25-49 % of the drive). Prints C VibeRamp tables.

Standard programs (calm, relax, meditate, sleep, focus, energy) and the lab's experiments (the Vibes app's Lab):
  slowing   breathing led down from 10 to 6 waves a minute, then calm (paced breathing works best started near the
            wearer's own pace and slowed: e.g. Paredes et al., "Calm Commute", CHI 2018)
  exhale    4 s in, 8 s out (a longer exhale than inhale goes with more vagal, calming heart activity)
  coherent  5.5 s in, 5.5 s out: 5.5 a minute, the resonance breathing pace of HRV biofeedback
  rocking   a soft 4 s rise and fall (about 0.25 Hz): rocking at that pace sped sleep onset (Bayer et al. 2011,
            a rocking bed; here by touch, an experiment)
  calm_gap  calm with a moment of silence between waves (to compare with calm, which never stops)
"""
import math


def smooth(rise, fall, rest, lo, hi, rest_level, pieces=12):
    """one swell and ebb, eased (`pieces` ramps a side), then `rest` ms at `rest_level` (0: silence)"""
    out = []
    for dur, a, b in ((rise, lo, hi), (fall, hi, lo)):
        pts = [a + (b - a) * (1 - math.cos(math.pi * i / pieces)) / 2 for i in range(pieces + 1)]
        out += [(dur // pieces, int(round(pts[i])), int(round(pts[i + 1]))) for i in range(pieces)]
    return out + ([(rest, rest_level, rest_level)] if rest else [])


def slowing(start_per_min=10.0, end_per_min=6.0, waves=8, lo=6, hi=85):
    """waves slowing from start_per_min to end_per_min (the inhale 45 % of each), played once before calm"""
    out = []
    for i in range(waves):
        per_min = start_per_min + (end_per_min - start_per_min) * i / (waves - 1)
        t = 60000 / per_min
        out += smooth(int(t * 0.45), int(t * 0.55), 0, lo, hi, lo, pieces=10)
    return out


P = {
    # one wave = one breath; never stops between waves (the floor), except calm_gap
    "calm":     smooth(4000, 5000, 1500, 6, 85, 6),        # ~5.7 waves a minute
    "relax":    smooth(5000, 6000, 2000, 5, 70, 5),        # ~4.6 a minute, softer
    "meditate": smooth(5450, 5450, 0, 6, 80, 6),           # 5.5 a minute (resonance breathing)
    "sleep":    smooth(6000, 7000, 3000, 4, 45, 4),        # ~3.75 a minute, very soft
    "focus":    smooth(1000, 1400, 0, 15, 55, 15, pieces=6),   # gentle ripples, 25 a minute
    "energy":   smooth(600, 800, 0, 25, 90, 25, pieces=5),     # brisk swells, ~43 a minute (alerting)
    "lab_slowing": slowing(),
    "lab_exhale": smooth(4000, 8000, 0, 6, 85, 6),
    "lab_coherent": smooth(5450, 5450, 0, 6, 85, 6),
    "lab_rocking": smooth(2000, 2000, 0, 4, 35, 4, pieces=8),
    "lab_calm_gap": smooth(4000, 5000, 1500, 6, 85, 0),
}

for name, w in P.items():
    assert len(w) < 255, (name, len(w))
    body = ", ".join(f"{{{a}, {b}, {c}}}" for a, b, c in w)
    print(f"static const VibeRamp s_{name}[] = {{  // {len(w)} ramps, {sum(r[0] for r in w)} ms a pass\n    {body},\n}};")
