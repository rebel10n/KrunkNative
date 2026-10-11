#!/usr/bin/env python3
"""Server-info query client (doc/backend.md 8.8): kn-query.py HOST[:PORT] [...]

Sends FF FF FF FF "KNINFO" 00 <nonce> over UDP and prints the JSON reply, one line per
server. Exit code 1 when any server does not answer within --timeout seconds.
"""
import argparse
import json
import os
import socket
import sys

PREFIX = b"\xff\xff\xff\xff"


def query(host, port, timeout):
    nonce = os.urandom(8).hex()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(PREFIX + b"KNINFO\x00" + nonce.encode(), (host, port))
        data, _ = s.recvfrom(2048)
    if not data.startswith(PREFIX + b"KNinfo\x00"):
        raise ValueError("not a KNinfo reply")
    info = json.loads(data[len(PREFIX) + 7:])
    if info.get("nonce") != nonce:
        raise ValueError("nonce mismatch")
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("servers", nargs="+", help="HOST[:PORT] (default port 21015)")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--json", action="store_true", help="raw JSON per line")
    args = ap.parse_args()
    rc = 0
    for target in args.servers:
        host, _, port = target.rpartition(":") if ":" in target else (target, "", "21015")
        try:
            info = query(host or target, int(port), args.timeout)
        except (OSError, ValueError) as e:
            print(f"{target}: no answer ({e})", file=sys.stderr)
            rc = 1
            continue
        if args.json:
            print(json.dumps(info))
        else:
            tags = ",".join(info.get("tags", []))
            print(f"{target}  {info.get('name')!r}  {info.get('map')} / {info.get('mode')}  "
                  f"{info.get('players')}/{info.get('max')}  [{tags}]  community={info.get('community')!r}  "
                  f"online_mode={info.get('online_mode')}  v{info.get('version')}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
