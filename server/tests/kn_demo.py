"""Reader of K.P.D demo files (shared/include/demo.h): header, meta, records and their framed packets."""
import json
import struct

MAGIC = b"KNDEMO\0"
STREAM, KEYFRAME = 0, 1


def parse(data):
    """-> (version, meta dict, [(t_ms, kind, [(type, payload), ...]), ...]); raises ValueError."""
    if len(data) < 12 or data[:7] != MAGIC:
        raise ValueError("not a demo file")
    version = data[7]
    (meta_len,) = struct.unpack_from("<I", data, 8)
    meta = json.loads(data[12:12 + meta_len])
    at = 12 + meta_len
    records = []
    while at < len(data):
        t, kind, n = struct.unpack_from("<IBI", data, at)
        at += 9
        body = data[at:at + n]
        if len(body) != n:
            raise ValueError("truncated record")
        at += n
        frames, i = [], 0
        while i < n:
            ptype, plen = body[i], (body[i + 1] << 8) | body[i + 2]
            frames.append((ptype, body[i + 3:i + 3 + plen]))
            i += 3 + plen
        if i != n:
            raise ValueError("bad frames")
        records.append((t, kind, frames))
    return version, meta, records
