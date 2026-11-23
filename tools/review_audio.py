"""Shows a recording (gocube --record=DIR: DIR/audio.wav) for review, without
listening: a picture (a spectrogram over a loudness curve) and a summary in
text (levels second by second, the sounds that start, clipping).

    python tools/review_audio.py DIR/audio.wav [out.png]

Plain Python (no packages). The picture: time left to right; above, the
frequencies (30 Hz at the bottom to 6 kHz at the top, log scale; brighter is
louder); below, the loudness (-60..0 dB). Mono 16-bit files work too.
"""
import cmath
import math
import struct
import sys
import wave
import zlib

DECIMATE = 4       # 48 kHz -> 12 kHz: up to 6 kHz shown
WINDOW = 512
HOP = 256
ROWS = 180         # spectrogram height
LOUD_ROWS = 70     # loudness strip height
MAX_WIDTH = 1600


def read(path):
    """Mono samples (-1..1) and the rate."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit("not a WAV file")
    pos = 12
    fmt = None
    samples = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, bits = fmt
    if tag == 3 and bits == 32:
        values = struct.unpack("<%df" % (len(samples) // 4), samples)
    elif tag == 1 and bits == 16:
        values = [v / 32768.0 for v in struct.unpack("<%dh" % (len(samples) // 2), samples)]
    else:
        sys.exit("unsupported format %s" % (fmt,))
    if channels > 1:
        values = [sum(values[i:i + channels]) / channels for i in range(0, len(values) - channels + 1, channels)]
    return list(values), rate, channels


def fft(x):
    n = len(x)
    if n == 1:
        return x
    even = fft(x[0::2])
    odd = fft(x[1::2])
    out = [0j] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * odd[k]
        out[k] = even[k] + t
        out[k + n // 2] = even[k] - t
    return out


def db(v):
    return 20.0 * math.log10(max(v, 1e-6))


def colour(t):
    """0..1 to a dark-to-bright ramp (black, purple, orange, pale yellow)."""
    stops = ((0.0, (0, 0, 0)), (0.35, (70, 20, 110)), (0.65, (220, 80, 40)), (0.85, (250, 180, 60)), (1.0, (255, 250, 200)))
    t = max(0.0, min(1.0, t))
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if t <= b:
            u = (t - a) / (b - a)
            return tuple(int(ca[i] + (cb[i] - ca[i]) * u) for i in range(3))
    return stops[-1][1]


def write_png(path, width, height, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixels[y][x])
    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    out_png = sys.argv[2] if len(sys.argv) > 2 else path.rsplit(".", 1)[0] + "_review.png"
    x, rate, channels = read(path)
    seconds = len(x) / rate
    peak = max((abs(v) for v in x), default=0.0)
    clipped = sum(1 for v in x if abs(v) >= 0.999)
    rms = math.sqrt(sum(v * v for v in x) / max(len(x), 1))
    print("%s: %.1f s, %d Hz, %d channel(s)" % (path, seconds, rate, channels))
    print("peak %.1f dB, RMS %.1f dB, clipped samples %d" % (db(peak), db(rms), clipped))

    # Second by second.
    print("\n  s   RMS dB  peak dB")
    per = []
    for s in range(int(math.ceil(seconds))):
        seg = x[s * rate:(s + 1) * rate]
        if not seg:
            break
        r = math.sqrt(sum(v * v for v in seg) / len(seg))
        p = max(abs(v) for v in seg)
        per.append(db(r))
        print("%4d  %6.1f  %6.1f  %s" % (s, db(r), db(p), "#" * max(0, int((db(r) + 60) / 2))))

    # Spectrogram.
    step = DECIMATE
    low = [sum(x[i:i + step]) / step for i in range(0, len(x) - step + 1, step)]
    lrate = rate / step
    hann = [0.5 - 0.5 * math.cos(2 * math.pi * i / (WINDOW - 1)) for i in range(WINDOW)]
    frames = []
    for start in range(0, max(len(low) - WINDOW, 0) + 1, HOP):
        seg = low[start:start + WINDOW]
        if len(seg) < WINDOW:
            break
        spec = fft([v * w for v, w in zip(seg, hann)])
        frames.append([abs(spec[k]) for k in range(WINDOW // 2)])
    # Onsets: a frame much louder than the second before it.
    energy = [sum(m * m for m in f) for f in frames]
    print("\nSounds starting (louder by 9 dB+ than the second before):")
    fps = lrate / HOP
    last = -10.0
    for i in range(len(energy)):
        before = energy[max(0, i - int(fps)):i]
        if not before:
            continue
        avg = sum(before) / len(before)
        t = i / fps
        if energy[i] > avg * 8.0 and energy[i] > 1e-3 and t - last > 1.0:
            spec = frames[i]
            k = max(range(1, len(spec)), key=lambda j: spec[j])
            print("  %6.2f s  (strongest near %d Hz)" % (t, int(k * lrate / WINDOW)))
            last = t
    # The picture.
    width = min(len(frames), MAX_WIDTH)
    if width == 0:
        sys.exit("too short")
    per_col = len(frames) / width
    fmin, fmax = 30.0, lrate / 2
    rows = []
    for r in range(ROWS):
        f = fmin * (fmax / fmin) ** (1.0 - r / (ROWS - 1))
        rows.append(min(WINDOW // 2 - 1, max(1, int(f * WINDOW / lrate))))
    pixels = [[(0, 0, 0)] * width for _ in range(ROWS + LOUD_ROWS)]
    top = max(max(f) for f in frames) or 1.0
    for c in range(width):
        group = frames[int(c * per_col):max(int((c + 1) * per_col), int(c * per_col) + 1)]
        for r, k in enumerate(rows):
            m = max(g[k] for g in group)
            pixels[r][c] = colour((db(m / top) + 70.0) / 70.0)
        e = max(math.sqrt(sum(v * v for v in g) / len(g)) for g in group)
        level = (db(e / top) + 60.0) / 60.0
        h = int(max(0.0, min(1.0, level)) * (LOUD_ROWS - 1))
        for r in range(LOUD_ROWS):
            pixels[ROWS + r][c] = (90, 200, 120) if LOUD_ROWS - 1 - r <= h else (20, 20, 24)
    # Second marks along the bottom.
    for s in range(int(seconds) + 1):
        c = int(s * fps / per_col)
        if c < width:
            for r in range(ROWS + LOUD_ROWS - (8 if s % 10 == 0 else 3), ROWS + LOUD_ROWS):
                pixels[r][c] = (255, 255, 255)
    write_png(out_png, width, ROWS + LOUD_ROWS, pixels)
    print("\npicture: %s (%d x %d; %.2f s per pixel column)" % (out_png, width, ROWS + LOUD_ROWS, seconds / width))


if __name__ == "__main__":
    main()
