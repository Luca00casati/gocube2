"""Makes the game's mood sounds (content/sounds, content/music): the wind
loop, the rare cave sounds and the slow piano pieces. All synthesized here,
from fixed seeds: running it again makes the same files.

    python tools/make_mood_sounds.py

Plain Python (no packages); takes a minute or two. Mono, 16-bit, 22050 Hz.
"""
import math
import os
import random
import struct
import wave

RATE = 22050
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "content")


def write(path, samples, peak=0.7):
    """Normalized to `peak`, written as 16-bit mono."""
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
    print("%-28s %5.1f s" % (path, len(samples) / RATE))


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


# --- Wind -------------------------------------------------------------------------------

def wind():
    rng = random.Random(11)
    seconds = 24.0
    fade = 2.0
    n = int((seconds + fade) * RATE)
    # Gusts: slow waves that fit the loop exactly (k / seconds Hz).
    def gust(t):
        return 0.55 + 0.25 * math.sin(2 * math.pi * 2 * t / seconds) + 0.15 * math.sin(2 * math.pi * 5 * t / seconds + 1.3) \
            + 0.08 * math.sin(2 * math.pi * 11 * t / seconds + 0.4)
    white = noise(n, rng)
    body = lowpass(white, 380.0, 2)
    whistle = resonator(white, 520.0, 18.0)
    out = []
    for i in range(n):
        g = gust(i / RATE)
        out.append(body[i] * g * 2.2 + whistle[i] * max(0.0, g - 0.55) * 0.9)
    # Seamless: the extra end fades into the start.
    m = int(seconds * RATE)
    f = int(fade * RATE)
    loop = out[:m]
    for i in range(f):
        t = i / f
        loop[i] = loop[i] * t + out[m + i] * (1.0 - t)
    write("sounds/wind.wav", loop, 0.6)


# --- Cave sounds -------------------------------------------------------------------------

def rumble():
    rng = random.Random(21)
    n = int(4.5 * RATE)
    x = lowpass(noise(n, rng), 70.0, 3)
    env = [math.sin(math.pi * i / n) ** 1.5 for i in range(n)]
    out = [v * e * 40.0 + 0.15 * e * math.sin(2 * math.pi * 38.0 * i / RATE) for i, (v, e) in enumerate(zip(x, env))]
    out = highpass(out, 18.0)  # (no drift off zero)
    write("sounds/cave_rumble.wav", reverb(out, 0.35, 1.3, 1.5), 0.8)


def scrape():
    rng = random.Random(22)
    n = int(2.4 * RATE)
    x = resonator(highpass(noise(n, rng), 600.0), 1300.0, 2.5)
    # Jerky: the stone catches and slips.
    amp = []
    level = 0.0
    target = 0.0
    for i in range(n):
        if i % 900 == 0:
            target = rng.choice((0.0, 0.2, 0.6, 1.0, 0.9))
        level += (target - level) * 0.004
        amp.append(level)
    env = envelope(n, 0.3, 0.6)
    out = [v * a * e for v, a, e in zip(x, amp, env)]
    write("sounds/cave_scrape.wav", reverb(out, 0.5, 1.4, 1.8), 0.6)


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


# --- Music: slow, sparse piano ------------------------------------------------------------

def note_freq(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def piano(out, start, midi, velocity, length=4.0):
    """A soft piano-like note added into `out` at `start` seconds."""
    f0 = note_freq(midi)
    s = int(start * RATE)
    n = min(int(length * RATE), len(out) - s)
    if n <= 0:
        return
    for k in range(1, 6):
        f = f0 * k * math.sqrt(1.0 + 0.0004 * k * k)
        if f > RATE * 0.45:
            break
        amp = velocity / k ** 1.4
        decay = math.exp(-(0.55 + 0.45 * k) / RATE)
        # A rotating phasor: cheap sine.
        w = 2.0 * math.pi * f / RATE
        c, sn = math.cos(w), math.sin(w)
        x, y = 0.0, 1.0
        a = amp
        attack = int(0.006 * RATE)
        for i in range(n):
            x, y = x * c + y * sn, y * c - x * sn
            v = x * a
            if i < attack:
                v *= i / attack
            out[s + i] += v
            a *= decay


def piece(path, seed, scale, root, bars, beat):
    """Bars of four beats: a low note now and then, a few melody notes
    wandering the scale, long rests between phrases."""
    rng = random.Random(seed)
    seconds = bars * 4 * beat + 6.0
    out = [0.0] * int(seconds * RATE)
    degree = rng.randrange(len(scale))
    t = 1.0
    for bar in range(bars):
        if bar % 2 == 0 or rng.random() < 0.3:
            bass = root - 12 + scale[rng.choice((0, 0, 2, 3))]
            piano(out, t, bass, 0.5, 5.0)
            if rng.random() < 0.5:
                piano(out, t + 0.02, bass + 7, 0.25, 4.5)
        resting = rng.random() < 0.3 and bar not in (0, bars - 1)
        if not resting:
            beats = sorted(rng.sample(range(8), rng.choice((2, 3, 3, 4))))
            for b in beats:
                degree = max(0, min(len(scale) * 2 - 1, degree + rng.choice((-2, -1, -1, 1, 1, 2, 0))))
                midi = root + 12 * (degree // len(scale)) + scale[degree % len(scale)]
                piano(out, t + b * beat * 0.5 + rng.uniform(0.0, 0.04), midi, rng.uniform(0.28, 0.42))
        t += 4 * beat
    # The last chord.
    for i, d in enumerate((0, 2, 4)):
        piano(out, t, root + scale[d % len(scale)], 0.3 - i * 0.05, 6.0)
    out = reverb(out, 0.35, 1.2, 3.0)
    fade = int(3.0 * RATE)
    for i in range(fade):
        out[len(out) - fade + i] *= 1.0 - i / fade
    write(path, out, 0.5)


if __name__ == "__main__":
    wind()
    rumble()
    scrape()
    drip()
    steps()
    moan()
    pebbles()
    piece("music/drift_1.wav", 101, (0, 3, 5, 7, 10), 57, 16, 1.05)   # A minor pentatonic
    piece("music/drift_2.wav", 202, (0, 2, 4, 6, 7, 9, 11), 62, 14, 1.15)  # D lydian
    piece("music/drift_3.wav", 303, (0, 2, 4, 7, 9), 53, 16, 1.0)     # F major pentatonic
