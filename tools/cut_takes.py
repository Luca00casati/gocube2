"""Cuts a recording session (many hits of one thing, a second or so apart)
into separate sounds for the game: finds each hit, trims it from just
before it starts to where it has died away, fades the ends, removes rumble
below 40 Hz, and keeps the best ones (not clipped, not much quieter than
the rest), each made as loud as the others (peak -3 dB).

    python tools/cut_takes.py TAKE.wav OUT_PREFIX [--keep=N] [--max=SECONDS] [--gap=SECONDS] [--length=SECONDS]

--length: for a take that goes on (crumbling a handful of dirt): each
sound is that long from where a gesture starts, faded out, whatever follows.

Writes OUT_PREFIX_1.wav, OUT_PREFIX_2.wav ... (mono, 16-bit, the take's
rate) and prints what it kept. Plain Python (no packages). Reads 16/24-bit
PCM and 32-bit float WAV.
"""
import math
import struct
import sys

FRAME = 0.005  # seconds per loudness step


def read(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        sys.exit("%s: not a WAV file" % path)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        size = struct.unpack("<I", d[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", d[pos + 8:pos + 24])
            if fmt[0] == 0xFFFE:
                fmt = (struct.unpack("<H", d[pos + 32:pos + 34])[0],) + fmt[1:]
        elif cid == b"data":
            data = d[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    if tag == 1 and bits == 16:
        x = [v / 32768.0 for v in struct.unpack("<%dh" % (len(data) // 2), data)]
    elif tag == 1 and bits == 24:
        x = [int.from_bytes(data[i:i + 3], "little", signed=True) / 8388608.0 for i in range(0, len(data) - 2, 3)]
    elif tag == 3 and bits == 32:
        x = list(struct.unpack("<%df" % (len(data) // 4), data))
    else:
        sys.exit("%s: unsupported format %s" % (path, fmt))
    if channels > 1:
        x = [sum(x[i:i + channels]) / channels for i in range(0, len(x) - channels + 1, channels)]
    return x, rate


def write(path, x, rate):
    data = struct.pack("<%dh" % len(x), *(int(max(-1.0, min(1.0, v)) * 32767) for v in x))
    head = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    with open(path, "wb") as f:
        f.write(head + b"data" + struct.pack("<I", len(data)) + data)


def highpass(x, hz, rate):
    a = 1.0 - math.exp(-2.0 * math.pi * hz / rate)
    lo, out = 0.0, []
    for v in x:
        lo += a * (v - lo)
        out.append(v - lo)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    if len(args) != 2:
        sys.exit(__doc__)
    keep = int(opts.get("keep", 8))
    longest = float(opts.get("max", 0.8))
    gap = float(opts.get("gap", 0.12))  # onsets closer than this are one hit
    length = float(opts.get("length", 0))
    x, rate = read(args[0])
    x = highpass(x, 40.0, rate)
    step = max(1, int(rate * FRAME))
    rms = [math.sqrt(sum(v * v for v in x[i:i + step]) / step) for i in range(0, len(x) - step, step)]
    floor = sorted(rms)[len(rms) // 5] + 1e-7
    top = max(rms)
    on_level = max(floor * 6.0, top * 0.06)
    hits = []
    i = 0
    while i < len(rms):
        if rms[i] < on_level:
            i += 1
            continue
        start = i
        peak = rms[i]
        j = i + 1
        quiet = 0
        need = int(0.04 / FRAME)
        limit = start + int(longest / FRAME)
        while j < len(rms) and j < limit:
            peak = max(peak, rms[j])
            if rms[j] < max(floor * 2.0, peak * 0.03):
                quiet += 1
                if quiet >= need:
                    break
            else:
                quiet = 0
            j += 1
        end = j - quiet + int(0.02 / FRAME)
        if hits and (start - hits[-1][1]) * FRAME < gap:  # still the same hit
            s0, _, p0 = hits[-1]
            hits[-1] = (s0, end, max(p0, peak))
        else:
            hits.append((start, end, peak))
        i = max(j, start + 1)
    if not hits:
        sys.exit("no hits found")
    # The usable ones: not clipped, not much quieter than the typical hit,
    # not cut off by the length limit.
    peaks = sorted(h[2] for h in hits)
    median = peaks[len(peaks) // 2]
    good = []
    for s, e, p in hits:
        a = max(0, s * step - int(0.008 * rate))
        b = min(len(x), a + int(length * rate)) if length else min(len(x), e * step)
        seg = x[a:b]
        clipped = max(abs(v) for v in seg) >= 0.99
        if clipped or p < median * 0.4 or (not length and (e - s) * FRAME >= longest - 0.01):
            continue
        good.append((p, a, b))
    # The ones nearest the typical loudness (the loudest few are often a slip).
    good.sort(key=lambda g: abs(math.log(g[0] / median)))
    chosen = sorted(good[:keep], key=lambda g: g[1])
    for n, (p, a, b) in enumerate(chosen, 1):
        seg = x[a:b]
        fade_in = min(len(seg), int(0.003 * rate))
        fade_out = min(len(seg), int((0.12 if length else 0.02) * rate))
        for k in range(fade_in):
            seg[k] *= k / fade_in
        for k in range(fade_out):
            seg[len(seg) - 1 - k] *= k / fade_out
        g = 0.708 / (max(abs(v) for v in seg) or 1.0)  # -3 dB
        write("%s_%d.wav" % (args[1], n), [v * g for v in seg], rate)
        print("%s_%d.wav  %.2f s  (at %.2f s in the take)" % (args[1], n, len(seg) / rate, a / rate))
    print("%d hits found, %d usable, %d kept" % (len(hits), len(good), len(chosen)))


if __name__ == "__main__":
    main()
