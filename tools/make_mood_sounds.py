"""Makes some of the game's rare cave sounds (content/sounds/cave_*.flac:
a rumble, a drip, steps, a moan, falling pebbles). All synthesized here,
from fixed seeds: running it again makes the same files. (The wind, the
scrape, the splash and the blocks' sounds are recordings: tools/cut_takes.py
and tools/make_loop.py make them from refs/, tools/to_flac.py compresses them.)

    python tools/make_mood_sounds.py

Plain Python (no packages); takes a minute or two. Mono, 16-bit, 22050 Hz.
"""
import math
import os
import random
import struct
import wave

import to_flac

RATE = 22050
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content")


def write(path, samples, peak=0.7):
    """Normalized to `peak`, written as 16-bit mono .flac (`path`: its .wav name)."""
    top = max(1e-9, max(abs(s) for s in samples))
    k = peak / top
    data = struct.pack("<%dh" % len(samples), *(int(max(-1.0, min(1.0, s * k)) * 32767) for s in samples))
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with wave.open(full, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    to_flac.to_flac(full, full[:-4] + ".flac")
    os.remove(full)
    print("%-28s %5.1f s" % (path[:-4] + ".flac", len(samples) / RATE))


def coef(hz):
    """A one-pole low-pass's coefficient for a cutoff of `hz`."""
    return 1.0 - math.exp(-2.0 * math.pi * hz / RATE)


def lowpass(x, hz, passes=1):
    for _ in range(passes):
        a = coef(hz)
        y = 0.0
        out = []
        for v in x:
            y += a * (v - y)
            out.append(y)
        x = out
    return x


def highpass(x, hz):
    lo = lowpass(x, hz)
    return [a - b for a, b in zip(x, lo)]


def resonator(x, hz, q):
    """A two-pole band-pass at `hz` (q: sharpness)."""
    w = 2.0 * math.pi * hz / RATE
    r = math.exp(-w / (2.0 * q))
    b1 = 2.0 * r * math.cos(w)
    b2 = -r * r
    y1 = y2 = 0.0
    out = []
    for v in x:
        y = (1.0 - r) * v + b1 * y1 + b2 * y2
        y2, y1 = y1, y
        out.append(y)
    return out


def noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def reverb(x, wet=0.3, size=1.0, tail=2.0):
    """A small Schroeder reverb: four combs, two all-passes; `tail` seconds added."""
    x = x + [0.0] * int(tail * RATE)
    combs = [int(d * size) for d in (1116, 1188, 1277, 1356)]
    feedback = 0.8
    acc = [0.0] * len(x)
    for d in combs:
        buf = [0.0] * d
        i = 0
        for n, v in enumerate(x):
            y = buf[i]
            buf[i] = v + y * feedback
            acc[n] += y
            i += 1
            if i == d:
                i = 0
    for d in (556, 441):
        buf = [0.0] * d
        i = 0
        g = 0.5
        out = []
        for v in acc:
            b = buf[i]
            y = -v * g + b
            buf[i] = v + b * g
            out.append(y)
            i += 1
            if i == d:
                i = 0
        acc = out
    return [v * (1.0 - wet) + r * wet * 0.25 for v, r in zip(x, acc)]


def envelope(n, attack, release):
    a = max(1, int(attack * RATE))
    r = max(1, int(release * RATE))
    return [min(1.0, i / a, (n - i) / r) for i in range(n)]


# --- Cave sounds -------------------------------------------------------------------------

def rumble():
    rng = random.Random(21)
    n = int(4.5 * RATE)
    x = lowpass(noise(n, rng), 70.0, 3)
    env = [math.sin(math.pi * i / n) ** 1.5 for i in range(n)]
    out = [v * e * 40.0 + 0.15 * e * math.sin(2 * math.pi * 38.0 * i / RATE) for i, (v, e) in enumerate(zip(x, env))]
    out = highpass(out, 18.0)  # (no drift off zero)
    write("sounds/cave_rumble.wav", reverb(out, 0.35, 1.3, 1.5), 0.8)


def drip():
    n = int(0.12 * RATE)
    one = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        f = 900.0 + 900.0 * math.exp(-t * 60.0)
        phase += 2 * math.pi * f / RATE
        one.append(math.sin(phase) * math.exp(-t * 45.0))
    out = [0.0] * int(1.6 * RATE)
    for delay, gain in ((0.0, 1.0), (0.19, 0.4), (0.41, 0.22), (0.66, 0.12), (0.95, 0.06)):
        s = int(delay * RATE)
        for i, v in enumerate(one):
            out[s + i] += v * gain
    write("sounds/cave_drip.wav", reverb(out, 0.4, 1.5, 1.0), 0.6)


def steps():
    rng = random.Random(24)
    out = [0.0] * int(3.2 * RATE)
    t = 0.2
    for k in range(4):
        n = int(0.09 * RATE)
        burst = lowpass(noise(n, rng), 300.0, 2)
        s = int(t * RATE)
        for i in range(n):
            e = math.exp(-i / RATE * 35.0)
            out[s + i] += (burst[i] * 9.0 + 0.5 * math.sin(2 * math.pi * 72.0 * i / RATE)) * e * (0.7 + 0.3 * (k % 2))
        t += 0.52 + rng.uniform(-0.04, 0.06)
    write("sounds/cave_steps.wav", reverb(lowpass(out, 900.0), 0.45, 1.6, 1.6), 0.5)


def moan():
    rng = random.Random(25)
    n = int(5.5 * RATE)
    out = []
    p1 = p2 = 0.0
    for i in range(n):
        t = i / n
        f = 112.0 * (1.0 - 0.1 * t)
        p1 += 2 * math.pi * f / RATE
        p2 += 2 * math.pi * f * 1.031 / RATE
        e = math.sin(math.pi * t) ** 2
        tri = lambda p: 2.0 / math.pi * math.asin(math.sin(p))
        out.append((tri(p1) + tri(p2)) * e)
    out = lowpass(out, 420.0, 2)
    air = lowpass(noise(n, rng), 200.0)
    out = [v + a * 0.6 * math.sin(math.pi * i / n) for i, (v, a) in enumerate(zip(out, air))]
    write("sounds/cave_moan.wav", reverb(out, 0.5, 1.5, 2.0), 0.55)


def pebbles():
    rng = random.Random(26)
    out = [0.0] * int(2.6 * RATE)
    t = 0.05
    gap = 0.32
    for k in range(9):
        n = int(0.03 * RATE)
        click = resonator(noise(n, rng), rng.uniform(1800.0, 3200.0), 6.0)
        s = int(t * RATE)
        g = 1.0 - k * 0.08
        for i in range(n):
            out[s + i] += click[i] * math.exp(-i / RATE * 120.0) * g
        t += gap
        gap *= 0.78
    write("sounds/cave_pebbles.wav", reverb(out, 0.5, 1.4, 1.6), 0.5)


if __name__ == "__main__":
    rumble()
    drip()
    steps()
    moan()
    pebbles()
