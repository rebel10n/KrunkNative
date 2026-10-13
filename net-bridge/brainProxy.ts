"use strict";

/**
 * Same-origin brain for the web build (KrunkNative backend): `net-bridge --web`
 * reverse-proxies the brain's public routes (`/mm/*`, `/api/*`, `/social/*`, `/ai/*`) and its
 * social WebSocket (`/social/ws`, `/api/ws`) to kn_brain, like the nginx deployment
 * does, so the page's same-origin brain URL works. When no brain answers, HTTP
 * requests fall back to the bridge's own routes (publicRoutes.ts) and WebSocket
 * upgrades are refused with 503; the client probes `/mm/status` (kn_brain only) to
 * tell the two apart.
 *
 * Brain URL, first match wins: `--brain URL`, env KN_BRAIN_URL, config/config.json
 * "brain" (public_url, else host:port), else http://127.0.0.1:5050.
 */

import fs from "fs";
import http, { type IncomingMessage, type ServerResponse } from "http";
import net from "net";
import path from "path";
import type { Duplex } from "stream";

export type BrainTarget = { host: string; port: number; url: string };

const PROXIED_PREFIXES = ["/mm/", "/api/", "/social/", "/ai/"];
const WS_PATHS = new Set(["/social/ws", "/api/ws"]);

export function resolveBrain(cliUrl?: string | null): BrainTarget {
  let url = cliUrl || process.env.KN_BRAIN_URL || "";
  if (!url) {
    const candidates = [
      process.env.KN_CONFIG,
      path.resolve(__dirname, "..", "config", "config.json"),
      path.resolve(process.cwd(), "config", "config.json"),
    ];
    for (const file of candidates) {
      if (!file || !fs.existsSync(file)) continue;
      try {
        const brain = JSON.parse(fs.readFileSync(file, "utf8")).brain || {};
        if (typeof brain.public_url === "string" && brain.public_url.startsWith("http://")) url = brain.public_url;
        else if (brain.port) url = `http://${brain.host && brain.host !== "0.0.0.0" ? brain.host : "127.0.0.1"}:${brain.port}`;
      } catch (_err) {
        /* unreadable config: default below */
      }
      break;
    }
  }
  if (!url) url = "http://127.0.0.1:5050";
  const parsed = new URL(url);
  return { host: parsed.hostname, port: Number(parsed.port) || 80, url: parsed.origin };
}

export function isBrainPath(pathname: string): boolean {
  return PROXIED_PREFIXES.some((p) => pathname.startsWith(p));
}

export function isBrainWsPath(pathname: string): boolean {
  return WS_PATHS.has(pathname);
}

/**
 * Forwards one HTTP request to the brain. `fallback` runs (instead of an error
 * response) when the brain cannot be reached, so the bridge's own routes answer.
 */
export function proxyHttp(
  brain: BrainTarget,
  req: IncomingMessage,
  res: ServerResponse,
  fallback: () => void,
): void {
  const headers = { ...req.headers, host: `${brain.host}:${brain.port}` };
  const upstream = http.request(
    { host: brain.host, port: brain.port, method: req.method, path: req.url, headers, timeout: 10000 },
    (up) => {
      res.writeHead(up.statusCode || 502, up.headers);
      up.pipe(res);
    },
  );
  let done = false;
  upstream.on("error", (err: NodeJS.ErrnoException) => {
    if (done) return;
    done = true;
    if (!res.headersSent && (err.code === "ECONNREFUSED" || err.code === "EHOSTUNREACH" || err.code === "ENOTFOUND")) {
      fallback();
      return;
    }
    if (!res.headersSent) {
      res.writeHead(502, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "brain_unreachable" }));
    } else {
      res.destroy();
    }
  });
  upstream.on("timeout", () => upstream.destroy(Object.assign(new Error("timeout"), { code: "ETIMEDOUT" })));
  req.pipe(upstream);
}

/** Pipes a WebSocket upgrade to the brain byte for byte (the handshake included). */
export function proxyUpgrade(brain: BrainTarget, req: IncomingMessage, socket: Duplex, head: Buffer): void {
  const upstream = net.connect({ host: brain.host, port: brain.port });
  upstream.setNoDelay(true);
  upstream.on("connect", () => {
    const lines = [`${req.method} ${req.url} HTTP/${req.httpVersion}`];
    for (let i = 0; i < req.rawHeaders.length; i += 2) {
      const name = req.rawHeaders[i];
      lines.push(`${name}: ${name.toLowerCase() === "host" ? `${brain.host}:${brain.port}` : req.rawHeaders[i + 1]}`);
    }
    upstream.write(lines.join("\r\n") + "\r\n\r\n");
    if (head && head.length) upstream.write(head);
    upstream.pipe(socket);
    socket.pipe(upstream);
  });
  upstream.on("error", () => {
    if (socket.writable) {
      socket.end("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"brain_unreachable\"}");
    }
    socket.destroy();
  });
  socket.on("error", () => upstream.destroy());
  socket.on("close", () => upstream.destroy());
  upstream.on("close", () => socket.destroy());
}
