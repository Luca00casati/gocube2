"""Makes .flac files from .wav files: lossless (the game plays back exactly
what was recorded) at about half the size. Writes X.flac next to each
X.wav and removes the .wav (--keep keeps it).

    python tools/to_flac.py FILE.wav [FILE.wav ...] [--keep]
    python tools/to_flac.py content/sounds              (every .wav under it)

Plain Python (no packages). Mono or stereo, 8/16/24-bit PCM in. Each block
of 4096 samples: each channel predicted by the best fixed polynomial
(order 0 to 4), the rest Rice-coded in partitions; stereo as left/right,
left/side, side/right or mid/side, whichever is smallest.
"""
import hashlib
import os
import struct
import sys

BLOCK = 4096
MAX_PARTITION_ORDER = 6


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, v, n):
        self.acc = (self.acc << n) | v
        self.n += n
        if self.n >= 64:
            whole = self.n >> 3
            rest = self.n & 7
            self.buf += (self.acc >> rest).to_bytes(whole, "big")
            self.acc &= (1 << rest) - 1
            self.n = rest

    def signed(self, v, n):
        self.write(v & ((1 << n) - 1), n)

    def finish(self):
        """Pads to a whole byte; the bytes."""
        if self.n & 7:
            self.write(0, 8 - (self.n & 7))
        if self.n:
            self.buf += self.acc.to_bytes(self.n >> 3, "big")
            self.acc = self.n = 0
        return bytes(self.buf)


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def read_wav(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError("not a .wav file")
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
    if fmt is None or data is None:
        raise ValueError("no format or no samples")
    tag, ch, rate, _, _, bits = fmt
    if tag != 1 or bits not in (8, 16, 24) or ch not in (1, 2):
        raise ValueError("not mono or stereo 8/16/24-bit PCM")
    width = bits // 8
    n = len(data) // (width * ch)
    data = data[:n * width * ch]
    if bits == 16:
        x = list(struct.unpack("<%dh" % (n * ch), data))
    elif bits == 8:
        x = [b - 128 for b in data]
    else:
        x = [int.from_bytes(data[i:i + 3], "little", signed=True) for i in range(0, len(data), 3)]
    return [x[c::ch] for c in range(ch)], rate, bits, data


def residual_of(x, order):
    if order == 0:
        return x[:]
    if order == 1:
        return [x[i] - x[i - 1] for i in range(1, len(x))]
    if order == 2:
        return [x[i] - 2 * x[i - 1] + x[i - 2] for i in range(2, len(x))]
    if order == 3:
        return [x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3] for i in range(3, len(x))]
    return [x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4] for i in range(4, len(x))]


def rice_param(total, count):
    """The Rice parameter for values summing to `total` over `count`."""
    k = 0
    while k < 14 and (count << (k + 1)) < total:
        k += 1
    return k


def rice_bits(total, count, k):
    return count * (k + 1) + (total >> k) + 4


def plan_residual(res, order, block):
    """(bits, partition order, parameters) of the cheapest partitioning."""
    u = [(v << 1) if v >= 0 else (-v << 1) - 1 for v in res]
    best = None
    top = 0
    while top < MAX_PARTITION_ORDER and block % (2 << top) == 0 and (block >> (top + 1)) > order:
        top += 1
    for p in range(top + 1):
        per = block >> p
        bits, params = 6, []
        start = 0
        for part in range(1 << p):
            count = per - order if part == 0 else per
            total = sum(u[start:start + count])
            start += count
            k = rice_param(total, count)
            params.append(k)
            bits += rice_bits(total, count, k)
        if best is None or bits < best[0]:
            best = (bits, p, params)
    return best, u


def plan_subframe(x, bps):
    """The cheapest coding of one channel: (bits, kind, details)."""
    block = len(x)
    if all(v == x[0] for v in x):
        return (8 + bps, "constant", None)
    best = (8 + bps * block, "verbatim", None)
    # The fixed order with the smallest residual, then its exact plan.
    orders = [o for o in range(5) if o < block]
    sums = [(sum(abs(v) for v in residual_of(x, o)), o) for o in orders]
    order = min(sums)[1]
    res = residual_of(x, order)
    (bits, p, params), u = plan_residual(res, order, block)
    bits += 8 + order * bps
    if bits < best[0]:
        best = (bits, "fixed", (order, p, params, u))
    return best


def write_subframe(w, x, bps, plan):
    _, kind, details = plan
    if kind == "constant":
        w.write(0, 8)
        w.signed(x[0], bps)
    elif kind == "verbatim":
        w.write(1 << 1, 8)
        for v in x:
            w.signed(v, bps)
    else:
        order, p, params, u = details
        w.write((8 + order) << 1, 8)
        for v in x[:order]:
            w.signed(v, bps)
        w.write(0, 2)  # 4-bit Rice parameters
        w.write(p, 4)
        per = len(x) >> p
        start = 0
        for part, k in enumerate(params):
            count = per - order if part == 0 else per
            w.write(k, 4)
            mask = (1 << k) - 1
            for v in u[start:start + count]:
                w.write((1 << k) | (v & mask), (v >> k) + 1 + k)
            start += count


def utf8_number(n):
    if n < 0x80:
        return bytes([n])
    out = []
    while True:
        out.insert(0, 0x80 | (n & 0x3F))
        n >>= 6
        lead_bits = 6 - len(out)  # bits left in the first byte
        if n < (1 << lead_bits):
            mark = (0xFF << (7 - len(out))) & 0xFF
            return bytes([mark | n] + out)


def encode(chans, rate, bits):
    ch = len(chans)
    n = len(chans[0])
    out = bytearray()
    frame = 0
    min_frame, max_frame = 1 << 24, 0
    for start in range(0, n, BLOCK):
        block = min(BLOCK, n - start)
        parts = [c[start:start + block] for c in chans]
        # The channel layout: the code and each subframe's (samples, bits).
        if ch == 1:
            choice = (0, [(parts[0], bits)])
        else:
            left, right = parts
            side = [a - b for a, b in zip(left, right)]
            mid = [(a + b) >> 1 for a, b in zip(left, right)]
            plans = {}

            def cost(name, xs, b):
                if name not in plans:
                    plans[name] = plan_subframe(xs, b)
                return plans[name][0]
            options = [
                (1, [(left, bits), (right, bits)], ["l", "r"]),
                (8, [(left, bits), (side, bits + 1)], ["l", "s"]),
                (9, [(side, bits + 1), (right, bits)], ["s", "r"]),
                (10, [(mid, bits), (side, bits + 1)], ["m", "s"]),
            ]
            best = min(options, key=lambda o: sum(cost(nm, xs, b) for nm, (xs, b) in zip(o[2], o[1])))
            choice = (best[0], best[1])
        code, subs = choice
        head = BitWriter()
        head.write(0x3FFE, 14)
        head.write(0, 2)
        head.write(12 if block == BLOCK else 7, 4)  # 4096, or 16 bits of size - 1 at the end
        head.write(0, 4)  # the rate: as STREAMINFO says
        head.write(code, 4)
        head.write(0, 3)  # the sample size: as STREAMINFO says
        head.write(0, 1)
        header = head.finish() + utf8_number(frame)
        if block != BLOCK:
            header += struct.pack(">H", block - 1)
        header += bytes([crc8(header)])
        body = BitWriter()
        for xs, b in subs:
            write_subframe(body, xs, b, plan_subframe(xs, b))
        data = header + body.finish()
        data += struct.pack(">H", crc16(data))
        out += data
        min_frame, max_frame = min(min_frame, len(data)), max(max_frame, len(data))
        frame += 1
    return bytes(out), min_frame, max_frame


def to_flac(src, dst):
    chans, rate, bits, raw = read_wav(src)
    n = len(chans[0])
    if n == 0:
        raise ValueError("no samples")
    frames, min_frame, max_frame = encode(chans, rate, bits)
    if bits == 8:  # (the MD5 is of signed samples)
        raw = bytes((b - 128) & 0xFF for b in raw)
    info = BitWriter()
    blk = max(16, min(BLOCK, n))
    info.write(blk, 16)
    info.write(blk, 16)
    info.write(min_frame, 24)
    info.write(max_frame, 24)
    info.write(rate, 20)
    info.write(len(chans) - 1, 3)
    info.write(bits - 1, 5)
    info.write(n, 36)
    info = info.finish() + hashlib.md5(raw).digest()
    head = b"fLaC" + bytes([0x80]) + len(info).to_bytes(3, "big") + info
    with open(dst, "wb") as f:
        f.write(head + frames)
    return len(head) + len(frames)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    keep = "--keep" in sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    paths = []
    for a in args:
        if os.path.isdir(a):
            for root, _, files in os.walk(a):
                paths += [os.path.join(root, f) for f in sorted(files) if f.lower().endswith(".wav")]
        else:
            paths.append(a)
    for p in paths:
        dst = os.path.splitext(p)[0] + ".flac"
        try:
            size = to_flac(p, dst)
        except ValueError as e:
            print("%s: %s" % (p, e))
            continue
        before = os.path.getsize(p)
        print("%s: %d -> %d bytes (%d%%)" % (dst, before, size, round(100 * size / before)))
        if not keep:
            os.remove(p)


if __name__ == "__main__":
    main()
