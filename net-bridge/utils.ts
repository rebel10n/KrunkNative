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
