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
for name, args in P.items():
    w = wave(*args)
    assert len(w) < 255, (name, len(w))
    body = ", ".join(f"{{{a}, {b}, {c}}}" for a, b, c in w)
    print(f"static const VibeRamp s_{name}[] = {{  // {len(w)} ramps, {sum(r[0] for r in w)} ms a wave\n    {body},\n}};")
