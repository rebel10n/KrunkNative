"""PNG helpers for the headless client tests (stdlib only: the frames client_bin writes with
`shot` / `frames` / KRUNKNATIVE_SCREENSHOT, client/src/png_write.c)."""
import struct
import zlib


def read_rgba(path):
    """(width, height, channels, rows) of an 8-bit RGB / RGBA PNG; rows are bytes per row."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h, channels = 8, b"", 0, 0, 4
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, _depth, color = struct.unpack(">IIBB", body[:10])
            channels = {2: 3, 6: 4}[color]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        filt = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 255
            elif filt == 2:
                line[i] = (line[i] + b) & 255
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, channels, rows


def is_magenta(r, g, b):
    """magenta, also lit or fogged: red and blue alike, little green"""
    return r > 60 and b > 60 and g < 0.45 * min(r, b) and abs(r - b) < 0.4 * max(r, b)


def magenta_count(path, box=None, step=1):
    """magenta pixels in the frame (or the box x0, y0, x1, y1), sampling every `step` px; -1 when
    the file cannot be read"""
    try:
        w, h, ch, rows = read_rgba(path)
    except (OSError, KeyError, ValueError, zlib.error, struct.error):
        return -1
    x0, y0, x1, y1 = box if box else (0, 0, w, h)
    hits = 0
    for y in range(max(y0, 0), min(y1, h), step):
        row = rows[y]
        for x in range(max(x0, 0), min(x1, w), step):
            if is_magenta(row[x * ch], row[x * ch + 1], row[x * ch + 2]):
                hits += 1
    return hits


def magenta_share(path, step=4):
    """share of sampled pixels that read as magenta; -1.0 when the file cannot be read"""
    try:
        w, h, ch, rows = read_rgba(path)
    except (OSError, KeyError, ValueError, zlib.error, struct.error):
        return -1.0
    hits = total = 0
    for row in rows[::step]:
        for x in range(0, w * ch, ch * step):
            total += 1
            hits += is_magenta(row[x], row[x + 1], row[x + 2])
    return hits / max(total, 1)


def write_solid(path, rgba, size=8):
    """a size x size PNG of one colour"""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    raw = b"".join(b"\0" + bytes(rgba) * size for _ in range(size))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
