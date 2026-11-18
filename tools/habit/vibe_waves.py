"""Apollo-style wave programs for the Pebble's LRA as tap trains: within each wave the taps come closer together and
stronger as it swells, then spread out and soften, then a pause (the LRA's felt strength barely changes with gain, its
rhythm does). Prints C VibeRamp tables."""
def wave(rise_ms, fall_ms, pause_ms, gap_max, gap_min, s_lo, s_hi, tap_ms):
    out, t = [], 0
    def phase(dur, g0, g1, a0, a1):
        nonlocal t
        start = t
        while t - start < dur:
            f = (t - start) / dur
            gap = int(g0 + (g1 - g0) * f)
            s = int(a0 + (a1 - a0) * f)
            out.append((tap_ms, s, s)); out.append((max(gap - tap_ms, 20), 0, 0)); t += gap
    phase(rise_ms, gap_max, gap_min, s_lo, s_hi)
    phase(fall_ms, gap_min, gap_max, s_hi, s_lo)
    out.append((pause_ms, 0, 0))
    return out

P = {
  # name: (rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap) ; one wave = one breath
  "calm":     (4000, 5000, 1500, 700, 220, 25, 70, 45),   # ~5.7 waves/min: breathe in with the swell
  "relax":    (5000, 6000, 2000, 800, 260, 20, 60, 45),   # ~4.6/min
  "sleep":    (6000, 7000, 3000, 900, 350, 12, 40, 40),   # ~3.75/min, very soft, sparse
  "meditate": (4500, 5500, 1000, 650, 200, 20, 65, 45),   # ~5.5/min (resonance breathing)
  "focus":    (800, 1200, 400, 400, 180, 30, 55, 35),     # quick gentle ripples, 25/min
  # brisk: a swell every 1.8 s of taps 3-8 a second (quicker than a resting heart), firmer than calm but under the alerts; for waking
  # up (the wake-up trial), not an alarm. Apollo's energy modes work on the same idea (fast rhythm = alerting)
  "energy":   (500, 700, 200, 300, 120, 35, 60, 35),
}

# The lab (Vibes app, Lab): variants of calm and sleep at the motor's full range, to compare on the wrist.
LOW = 1      # the weakest non-zero strength (the player maps it to the floor, about 12 % of the drive)
EDGE = 25    # ms of silence around the low level ("edges": silence makes the next change felt)


def wave_floor(rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap):
    """the gaps between taps at the lowest strength instead of silence"""
    return [(ms, LOW, LOW) if a == 0 and ms < 1000 else (ms, a, b)
            for ms, a, b in wave(rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap)]


def wave_edges(rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap):
    """tap, a moment of silence, the lowest strength, a moment of silence, the next tap"""
    out = []
    for ms, a, b in wave(rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap):
        if a == 0 and ms < 1000 and ms > 2 * EDGE + 20:
            out += [(EDGE, 0, 0), (ms - 2 * EDGE, LOW, LOW), (EDGE, 0, 0)]
        else:
            out.append((ms, a, b))
    return out


def wave_chop(rise, fall, pause, s_lo, s_hi, on=80, off=15):
    """the smooth swell, cut into short pieces: every piece starts with an edge"""
    out = []
    for dur, a0, a1 in ((rise, s_lo, s_hi), (fall, s_hi, s_lo)):
        t = 0
        while t < dur:
            out += [(on, int(a0 + (a1 - a0) * t / dur), int(a0 + (a1 - a0) * t / dur)), (off, 0, 0)]
            t += on + off
    return out + [(pause, 0, 0)]


def wave_kick(rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap, kick=8):
    """each tap starts with a short full-strength burst (overdrive: the LRA gets going at once, so taps are crisp)"""
    out = []
    for ms, a, b in wave(rise, fall, pause, gap_max, gap_min, s_lo, s_hi, tap):
        out += [(kick, 100, 100), (ms - kick, a, b)] if a > 0 else [(ms, a, b)]
    return out


def breath(inhale=4000, exhale=6000, pause=1000):
    """breathe in with taps closing in and rising, out with a soft continuous hum fading away: two textures"""
    return wave(inhale, 1, 0, 600, 200, 30, 70, 45)[:-1] + [(exhale, 30, 8), (pause, 0, 0)]


def smooth(rise, fall, rest, lo, hi, rest_level, pieces=12):
    """one continuous vibration swelling and ebbing like a breath (eased: a raised cosine, in `pieces` ramps a side),
    then `rest` ms at `rest_level` (0: a moment of silence; above 0 it never stops)"""
    import math
    out = []
    for dur, a, b in ((rise, lo, hi), (fall, hi, lo)):
        pts = [a + (b - a) * (1 - math.cos(math.pi * i / pieces)) / 2 for i in range(pieces + 1)]
        out += [(dur // pieces, int(round(pts[i])), int(round(pts[i + 1]))) for i in range(pieces)]
    return out + [(rest, rest_level, rest_level)]


LAB = {
    "lab_calm_full": wave(*P["calm"]),
    "lab_calm_floor": wave_floor(*P["calm"]),
    "lab_calm_edges": wave_edges(*P["calm"]),
    "lab_calm_chop": wave_chop(4000, 5000, 1500, 25, 70),
    "lab_calm_kick": wave_kick(*P["calm"]),
    "lab_sleep_floor": wave_floor(*P["sleep"]),
    "lab_sleep_edges": wave_edges(*P["sleep"]),
    "lab_breath": breath(),
    # the smooth waves over the full range (the 2026-11-16 swell was inside a 25-49 % range: a steady hum)
    "lab_calm_smooth": smooth(4000, 5000, 1500, 6, 85, 6),        # never stops: the floor between waves
    "lab_calm_smooth_gap": smooth(4000, 5000, 1500, 6, 85, 0),    # a moment of silence between waves
    "lab_sleep_smooth": smooth(6000, 7000, 3000, 4, 45, 4),
}

for name, w in list(P.items()) + [(k, None) for k in LAB]:
    w = LAB[name] if w is None else wave(*w)
    assert len(w) < 255, (name, len(w))
    body = ", ".join(f"{{{a}, {b}, {c}}}" for a, b, c in w)
    print(f"static const VibeRamp s_{name}[] = {{  // {len(w)} ramps, {sum(r[0] for r in w)} ms a wave\n    {body},\n}};")
