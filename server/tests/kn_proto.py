"""Minimal KrunkNative game-protocol client for server tests (TCP framing: u8 type, u16 BE length).

Only the packets the dedicated-server tests need: HELLO, JOIN, ENT, and reading SPAWN /
ERROR / CHAT / REDIRECT / AUTH_CHALLENGE. Layouts mirror shared/include/shared.h (packed).
"""
import socket
import struct
import time

HELLO, ENT, SPAWN, INIT, CHAT, ERROR = 1, 3, 4, 2, None, 30
JOIN, AUTH_CHALLENGE, REDIRECT = 72, 73, 74


def cstr(s, n):
    b = s.encode()[: n - 1]
    return b + b"\0" * (n - len(b))


def uncstr(b):
    return b.split(b"\0", 1)[0].decode(errors="replace")


class Conn:
    def __init__(self, host, port, timeout=5.0):
        self.s = socket.create_connection((host, port), timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.packets = []

    def send(self, ptype, payload=b""):
        self.s.sendall(struct.pack(">BH", ptype, len(payload)) + payload)

    def hello(self):
        self.send(HELLO, bytes([1, 0, 0]))

    def join(self, client_key="", session="", name="", password="", game_id=""):
        self.send(JOIN, cstr(game_id, 48) + cstr(client_key, 352) + cstr(session, 64) + cstr(name, 32) + cstr(password, 16))

    def ent(self, class_index=0, secondary=-1, cosmetics=(-1, -1, -1, -1, -1, -1, -1), spectate=0, slid=1):
        self.send(ENT, struct.pack("<ii7iBB", class_index, secondary, *cosmetics, spectate, slid))

    def read(self, until=None, timeout=5.0):
        """Reads packets for up to timeout s; returns the first matching `until` type (or None)."""
        end = time.time() + timeout
        while time.time() < end:
            while len(self.buf) >= 3:
                t, n = struct.unpack(">BH", self.buf[:3])
                if len(self.buf) < 3 + n:
                    break
                payload, self.buf = self.buf[3 : 3 + n], self.buf[3 + n :]
                self.packets.append((t, payload))
                if until is not None and t == until:
                    return payload
            try:
                self.s.settimeout(max(0.05, end - time.time()))
                data = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return None
            if not data:
                return None
            self.buf += data
        return None

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def spawn_info(payload):
    """NetSpawnPacket: uid i, is_you B, active B, class i, team i, health f, max_health i, pos 3f, dir 2f,
    name 32s, secondary i, cosmetics 7i."""
    fmt = "<iBBiifi3f2f32si7i"  # 0 uid 1 is_you 2 active 3 class 4 team 5 hp 6 max 7-9 pos 10-11 dir 12 name 13 sec 14-20 cos
    if len(payload) < struct.calcsize(fmt):
        return None
    v = struct.unpack(fmt, payload[: struct.calcsize(fmt)])
    return {"uid": v[0], "is_you": v[1], "name": uncstr(v[12]), "cosmetics": list(v[14:21])}


def error_info(payload):
    return uncstr(payload[: len(payload) - 8]), uncstr(payload[-8:])
