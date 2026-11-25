"""Makes a sound loop without a seam: keeps FROM..TO seconds of a recording
and crossfades its last FADE seconds into the FADE seconds just before FROM
(equal power), so the end flows into the start. Fade-ins and fade-outs at
the recording's ends are left out.

    python tools/make_loop.py IN.wav OUT.wav FROM TO [FADE]

16-bit PCM in (any channels), the same out (tools/to_flac.py then makes it
the .flac the game plays). Plain Python (no packages).
"""
import math
import struct
import sys
import wave


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    start, end = float(sys.argv[3]), float(sys.argv[4])
    fade = float(sys.argv[5]) if len(sys.argv) > 5 else 2.0
    with wave.open(src, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        if width != 2:
            sys.exit("16-bit PCM only")
        x = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    a, b, f = int(start * rate), int(end * rate), int(fade * rate)
    if a < f or b > n or b - a <= 2 * f:
        sys.exit("FROM must be at least FADE in, TO within the file, and the loop longer than two fades")
    frames = [x[i * ch:(i + 1) * ch] for i in range(n)]
    out = frames[a:b]
    # The end fades out while what came just before the start fades in:
    # played around, the last frame leads straight into the first.
    for k in range(f):
        t = (k + 0.5) / f
        g_out, g_in = math.cos(t * math.pi / 2), math.sin(t * math.pi / 2)
        tail = out[len(out) - f + k]
        lead = frames[a - f + k]
        out[len(out) - f + k] = tuple(int(max(-32768, min(32767, round(p * g_out + q * g_in)))) for p, q in zip(tail, lead))
    with wave.open(dst, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % (len(out) * ch), *(v for fr in out for v in fr)))
    print("%s: %.2f s loop (%.2f..%.2f s of %s, %.1f s crossfade)" % (dst, len(out) / rate, start, end, src, fade))


if __name__ == "__main__":
    main()
