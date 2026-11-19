# DittoSR's F8 capture (capture_key) as PNGs, and its flicker measured.
#   python test/capture.py <the game's folder>\dittosr-capture [out folder]
# Each fNN_<output|relight>.raw ("DSRCAP1", width, height, DXGI format, row pitch, then the rows) -> fNN_<...>.png
# (float colour tone-mapped x / (1 + x), then gamma 2.2; UNORM as it is). Per set (output, relight), on the perceptual
# 0 - 255 image: the mean |frame - previous| over the 7 pairs, and flicker_<set>.png - that per pixel x 8 (where it
# changes), plus the 8 x 8 grid's worst cells. Needs numpy.
import os, struct, sys, zlib

import numpy as np

FORMATS = {2: 'rgba32f', 10: 'rgba16f', 26: 'r11g11b10f', 24: 'rgb10a2', 28: 'rgba8', 29: 'rgba8', 87: 'bgra8', 91: 'bgra8'}


def load(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:7] != b'DSRCAP1':
        raise ValueError(path + ': not a DittoSR capture')
    w, h, fmt, pitch = struct.unpack('<4I', d[8:24])
    rows = np.frombuffer(d, np.uint8, pitch * h, 24).reshape(h, pitch)
    kind = FORMATS.get(fmt)
    if kind == 'rgba16f':
        px = rows[:, :w * 8].copy().view(np.float16).reshape(h, w, 4)[..., :3].astype(np.float32)
    elif kind == 'rgba32f':
        px = rows[:, :w * 16].copy().view(np.float32).reshape(h, w, 4)[..., :3]
    elif kind == 'r11g11b10f':
        v = rows[:, :w * 4].copy().view(np.uint32).reshape(h, w)
        def uf(bits, mbits):  # (an unsigned small float: 5 exponent bits, mbits mantissa)
            e = (bits >> mbits) & 31
            m = (bits & ((1 << mbits) - 1)).astype(np.float32)
            return np.where(e == 0, m / (1 << mbits) * 2.0 ** -14, (1 + m / (1 << mbits)) * 2.0 ** (e.astype(np.float32) - 15))
        px = np.stack([uf(v & 0x7ff, 6), uf((v >> 11) & 0x7ff, 6), uf(v >> 22, 5)], -1).astype(np.float32)
    elif kind == 'rgb10a2':
        v = rows[:, :w * 4].copy().view(np.uint32).reshape(h, w)
        return np.stack([v & 1023, (v >> 10) & 1023, (v >> 20) & 1023], -1).astype(np.float32) / 1023.0 * 255.0
    elif kind in ('rgba8', 'bgra8'):
        px = rows[:, :w * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
        return px[..., ::-1] if kind == 'bgra8' else px
    else:
        raise ValueError('%s: DXGI format %d not handled' % (path, fmt))
    px = np.nan_to_num(np.maximum(px, 0.0))
    return (px / (1.0 + px)) ** (1.0 / 2.2) * 255.0  # (float colour: tone-mapped for the eye)


def save_png(path, img):
    img = np.clip(img, 0, 255).astype(np.uint8)
    if img.ndim == 2:
        img = np.stack([img] * 3, -1)
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(h))
    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src
    os.makedirs(out, exist_ok=True)
    for kind in ('output', 'relight'):
        frames = []
        for i in range(8):
            p = os.path.join(src, 'f%02d_%s.raw' % (i, kind))
            if not os.path.exists(p):
                break
            img = load(p)
            save_png(os.path.join(out, 'f%02d_%s.png' % (i, kind)), img)
            frames.append(img.mean(-1))
        if len(frames) < 2:
            continue
        diffs = [np.abs(frames[i] - frames[i - 1]) for i in range(1, len(frames))]
        mean = np.mean(diffs, 0)
        save_png(os.path.join(out, 'flicker_%s.png' % kind), mean * 8.0)
        h, w = mean.shape
        cells = [(mean[y * h // 8:(y + 1) * h // 8, x * w // 8:(x + 1) * w // 8].mean(), x, y) for y in range(8) for x in range(8)]
        cells.sort(reverse=True)
        print('%s: %d frames %dx%d, mean |frame - previous| %.3f (per pair: %s); worst 8 x 8 cells (column, row): %s' %
              (kind, len(frames), w, h, mean.mean(), ' '.join('%.2f' % d.mean() for d in diffs),
               ', '.join('(%d, %d) %.2f' % (x, y, v) for v, x, y in cells[:5])))


if __name__ == '__main__':
    main()
