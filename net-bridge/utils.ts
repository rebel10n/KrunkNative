"use strict";

const msgpack = require("msgpack-lite") as {
  encode(value: unknown): Buffer | Uint8Array;
  decode(value: Uint8Array | Buffer): unknown;
};

export type DecodedPacket = [ahNum: number, label: string, args: unknown[]];

export function serializeNetworkMessage(label: string, ...args: unknown[]): Buffer {
  const encoded = msgpack.encode([label, ...args]);
  return Buffer.isBuffer(encoded) ? encoded : Buffer.from(encoded);
}

export function encodeNetworkMessage(payload: Uint8Array | Buffer, ahNum: number): Buffer {
  const framed = Buffer.alloc(payload.length + 2);
  Buffer.from(payload).copy(framed, 0);
  const [hi, lo] = getPadding(ahNum);
  framed[framed.length - 2] = hi;
  framed[framed.length - 1] = lo;
  return framed;
}

function bytesFromMessage(message: unknown): Uint8Array | null {
  if (Array.isArray(message)) {
    const chunks = message.filter(Buffer.isBuffer) as Buffer[];
    return new Uint8Array(Buffer.concat(chunks));
  }
  if (typeof message === "string") return new Uint8Array(Buffer.from(message));
  if (message instanceof ArrayBuffer) return new Uint8Array(message);
  if (ArrayBuffer.isView(message)) {
    return new Uint8Array(message.buffer, message.byteOffset, message.byteLength);
  }
  if (Buffer.isBuffer(message)) return new Uint8Array(message);
  return null;
}

export function decodePacket(message: unknown): DecodedPacket {
  const bytes = bytesFromMessage(message);
  if (!bytes || bytes.length < 3) throw new Error("Invalid message");

  const ahNum = combineNibbles(bytes[bytes.length - 2], bytes[bytes.length - 1]);
  const decoded = msgpack.decode(bytes.slice(0, bytes.length - 2));
  if (!Array.isArray(decoded) || decoded.length < 1 || typeof decoded[0] !== "string") {
    throw new Error("Invalid message");
  }

  return [ahNum, decoded[0], decoded.slice(1)];
}

export function rotateAhNum(current: number, ahk: number): number {
  return (current + ahk) & 255;
}

export function getPadding(value: number): [number, number] {
  return [(value >> 4) & 15, value & 15];
}

export function combineNibbles(left: number, right: number): number {
  return (left << 4) + right;
}

// The browser's real address for the native server (NET_PACKET_FORWARDED = 85). The bridge
// itself reaches server_bin from 127.0.0.1, which the server would otherwise treat as a
// player on its own machine (cheats allowed). Forwarding headers are trusted only when the
// TCP peer is loopback (a reverse proxy on this box), like v3.1.1 getWsAddress.
export const NET_PACKET_FORWARDED = 85;
const NET_FORWARDED_ADDRESS_SIZE = 48;

function isLoopback(addr: string): boolean {
  return addr === "::1" || addr.startsWith("127.") || addr.startsWith("::ffff:127.");
}

export function peerAddress(req: { socket: { remoteAddress?: string }; headers: Record<string, unknown> }): string {
  let addr = req.socket.remoteAddress || "";
  if (isLoopback(addr)) {
    const xff = req.headers["x-forwarded-for"];
    // The rightmost entry: the hop our own proxy appended. v3.1.1 takes the leftmost, which
    // the browser can set itself ("X-Forwarded-For: 127.0.0.1" would make it local here).
    const list = (Array.isArray(xff) ? xff.join(",") : typeof xff === "string" ? xff : "").split(",");
    const last = list[list.length - 1]?.trim();
    const xri = req.headers["x-real-ip"];
    if (last) addr = last;
    else if (typeof xri === "string" && xri.trim()) addr = xri.trim();
  }
  if (addr.startsWith("::ffff:")) addr = addr.slice(7); // IPv4-mapped: the server parses dotted IPv4
  return addr.slice(0, NET_FORWARDED_ADDRESS_SIZE) || "?";
}

// [type, len_hi, len_lo, address bytes] (the native frame header, shared/include/shared.h).
export function forwardedFrame(address: string): Buffer {
  const body = Buffer.from(address, "utf8").subarray(0, NET_FORWARDED_ADDRESS_SIZE);
  const frame = Buffer.alloc(3 + body.length);
  frame[0] = NET_PACKET_FORWARDED;
  frame.writeUInt16BE(body.length, 1);
  body.copy(frame, 3);
  return frame;
}
