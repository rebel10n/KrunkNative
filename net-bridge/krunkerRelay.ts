"use strict";

/**
 * krunker.io through an internal proxy (net-bridge --web and --bridge-only).
 *
 * A browser cannot use an HTTP proxy, so the bridge can relay the krunker.io traffic a web
 * client needs (its HTTP calls and its game WebSockets) and send it out through a proxy on
 * the server's internal network. krunker.io is otherwise not reachable from the servers, so
 * the whole relay is gated on one variable:
 *
 *   KN_BRIDGE_KRUNKER_PROXY=socks5h://user:password@PROXY_HOST:PORT
 *
 * Unset or empty = OFF: no `/krunker-relay/*` route exists (the bridge answers 404 and closes
 * a WebSocket upgrade there, exactly as before this module) and nothing is sent to krunker.io.
 * Set = ON: every relayed connection goes through that proxy and nowhere else. A proxy that
 * does not answer fails the request with 502 (krunker_proxy_unreachable); there is no fallback
 * to a direct connection. A value that is not a proxy URL stops the bridge at startup.
 *
 * Where the value goes on the server (the bridge reads it once, at startup; restart kn-bridge):
 *   - deploy/ansible role kn_bridge: /etc/krunknative/bridge-proxy.env, an optional EnvironmentFile
 *     that the role does not template (mode 0640, root:kn), so the credentials stay out of the
 *     repository and out of ansible runs. The kn-bridge unit reads it (templates/kn-bridge.service.j2).
 *   - deploy/systemd/kn-net-bridge.service (no ansible): /etc/krunknative/net-bridge.env, the unit's
 *     EnvironmentFile already.
 *
 * The URI shapes are the native client's (client/include/http_proxy.h, Settings > Client > Network):
 * http:// (CONNECT), https:// (TLS to the proxy, then CONNECT), socks5:// (this bridge resolves the
 * target) and socks5h:// (the proxy resolves it); a bare host[:port] is http://; user:password@ with
 * percent-encoding; no path. The default ports are the client's (80, 443, 1080, 1080).
 *
 * Scope: only krunker.io, its subdomains and crucible.frvr.com (the krunker.io account service; the
 * client's http_proxy_host_is_krunker), on port 443. Every other host is refused with 403 before any
 * connection. Credentials are never logged: the log label is scheme://host:port.
 *
 * Routes (when ON):
 *   GET|HEAD|POST /krunker-relay/http?url=https://<krunker host>/<path>   HTTP(S) request, body and status passed through
 *   GET /krunker-relay/ws?url=wss://<krunker host>/<path>  (Upgrade)       WebSocket: the handshake and frames are piped
 *                                                                           byte for byte through a TLS tunnel (the browser
 *                                                                           negotiates with the krunker.io server itself)
 */

import dns from "dns";
import type { IncomingMessage, ServerResponse } from "http";
import https from "https";
import net from "net";
import tls from "tls";

export const PROXY_ENV = "KN_BRIDGE_KRUNKER_PROXY";
export const RELAY_HTTP_PATH = "/krunker-relay/http";
export const RELAY_WS_PATH = "/krunker-relay/ws";

const KRUNKER_DOMAINS = ["krunker.io", "crucible.frvr.com"];
const RELAY_PORT = 443;
const TIMEOUT_MS = 15000;
const MAX_BODY = 256 << 10;
const MAX_URL = 2048;
const MAX_HEAD = 8192;
// The game's own origin: the native client sends the same Origin on its WebSocket (krunker_net.c)
const ORIGIN = "https://krunker.io";
const FORWARD_HEADERS = [
  "accept",
  "accept-encoding",
  "accept-language",
  "authorization",
  "content-type",
  "if-modified-since",
  "if-none-match",
  "user-agent",
];
const WS_HEADERS: Array<[string, string]> = [
  ["sec-websocket-key", "Sec-WebSocket-Key"],
  ["sec-websocket-version", "Sec-WebSocket-Version"],
  ["sec-websocket-extensions", "Sec-WebSocket-Extensions"],
  ["sec-websocket-protocol", "Sec-WebSocket-Protocol"],
  ["user-agent", "User-Agent"],
];
const HOP_BY_HOP = ["connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "trailer", "upgrade"];
const SOCKS_REPLIES = [
  "succeeded",
  "general failure",
  "connection not allowed",
  "network unreachable",
  "host unreachable",
  "connection refused",
  "TTL expired",
  "command not supported",
  "address type not supported",
];

export type ProxyKind = "http" | "https" | "socks5" | "socks5h";
export type ProxyTarget = {
  kind: ProxyKind;
  host: string;
  port: number;
  user: string; // percent-decoded, "" = none
  pass: string;
  label: string; // scheme://host:port, never the credentials
};

const SCHEMES: Array<{ prefix: string; kind: ProxyKind; port: number }> = [
  { prefix: "http://", kind: "http", port: 80 },
  { prefix: "https://", kind: "https", port: 443 },
  { prefix: "socks5://", kind: "socks5", port: 1080 },
  { prefix: "socks5h://", kind: "socks5h", port: 1080 },
];

// The message names the rule, never the input: the input may carry a password.
function badProxy(why: string): Error {
  return new Error(
    `${PROXY_ENV} is not a proxy URL (${why}). Use http://, https://, socks5:// or socks5h://, then host[:port] ` +
      "with user:password@ optional (percent-encode special characters).",
  );
}

function decodePart(text: string): string {
  try {
    return decodeURIComponent(text);
  } catch (_err) {
    throw badProxy("bad percent-encoding in the credentials");
  }
}

/** A proxy URL in the client's shapes (http_proxy.h). Throws with a message that holds no credentials. */
export function parseProxyUrl(input: string): ProxyTarget {
  let text = input.trim();
  while (text.endsWith("/")) text = text.slice(0, -1);
  if (!text) throw badProxy("empty");
  let kind: ProxyKind = "http";
  let port = 80;
  let rest = text;
  if (text.includes("://")) {
    const scheme = SCHEMES.find((s) => text.slice(0, s.prefix.length).toLowerCase() === s.prefix);
    if (!scheme) throw badProxy("unsupported scheme");
    kind = scheme.kind;
    port = scheme.port;
    rest = text.slice(scheme.prefix.length);
  }
  if (rest.includes("/")) throw badProxy("a path is not allowed");

  let user = "";
  let pass = "";
  const at = rest.lastIndexOf("@");
  if (at >= 0) {
    const info = rest.slice(0, at);
    const colon = info.indexOf(":");
    user = decodePart(colon >= 0 ? info.slice(0, colon) : info);
    pass = colon >= 0 ? decodePart(info.slice(colon + 1)) : "";
    rest = rest.slice(at + 1);
  }

  let host: string;
  let portText = "";
  let hasPort = false;
  if (rest.startsWith("[")) {
    const close = rest.indexOf("]");
    if (close < 0) throw badProxy("unclosed [ in the host");
    host = rest.slice(1, close);
    if (net.isIP(host) !== 6) throw badProxy("bad IPv6 host");
    const after = rest.slice(close + 1);
    if (after !== "") {
      if (!after.startsWith(":")) throw badProxy("bad host");
      portText = after.slice(1);
      hasPort = true;
    }
  } else {
    const colon = rest.indexOf(":");
    host = colon >= 0 ? rest.slice(0, colon) : rest;
    if (colon >= 0) {
      portText = rest.slice(colon + 1);
      hasPort = true;
    }
    if (!/^[A-Za-z0-9._-]+$/.test(host)) throw badProxy("bad host name");
  }
  if (hasPort) {
    if (!/^[0-9]{1,5}$/.test(portText)) throw badProxy("bad port");
    port = Number(portText);
    if (port < 1 || port > 65535) throw badProxy("bad port");
  }
  const shown = host.includes(":") ? `[${host}]` : host;
  return { kind, host, port, user, pass, label: `${kind}://${shown}:${port}` };
}

/** The proxy from the environment: null when the variable is unset or empty (the gate is off). */
export function proxyFromEnv(env: NodeJS.ProcessEnv = process.env): ProxyTarget | null {
  const value = (env[PROXY_ENV] || "").trim();
  return value ? parseProxyUrl(value) : null;
}

/** krunker.io, its subdomains and crucible.frvr.com (no other host is ever relayed). */
export function isKrunkerHost(host: string): boolean {
  const name = host.toLowerCase().replace(/\.$/, "");
  return KRUNKER_DOMAINS.some((domain) => name === domain || name.endsWith(`.${domain}`));
}

/** The `url` parameter of a relay request: an absolute URL of the given scheme on a krunker host, port 443. */
export function relayTarget(raw: string | null, scheme: "https:" | "wss:"): { host: string; path: string } | { error: string } {
  if (!raw || raw.length > MAX_URL) return { error: "url parameter missing or too long" };
  let url: URL;
  try {
    url = new URL(raw);
  } catch (_err) {
    return { error: "url is not an absolute URL" };
  }
  if (url.protocol !== scheme) return { error: `url must use ${scheme.slice(0, -1)}://` };
  if (url.username || url.password) return { error: "credentials in the url are refused" };
  if (url.port !== "") return { error: "only the default port 443 is relayed" };
  const host = url.hostname.toLowerCase();
  if (!isKrunkerHost(host)) return { error: `${host} is not krunker.io (only krunker.io hosts are relayed)` };
  return { host, path: `${url.pathname}${url.search}` || "/" };
}

function waitFor(socket: net.Socket, event: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const done = (err?: Error) => {
      socket.removeListener(event, onEvent);
      socket.removeListener("error", onError);
      socket.removeListener("close", onClose);
      if (err) reject(err);
      else resolve();
    };
    const onEvent = () => done();
    const onError = (err: Error) => done(err);
    const onClose = () => done(new Error("connection closed"));
    socket.on(event, onEvent);
    socket.on("error", onError);
    socket.on("close", onClose);
  });
}

// Exactly n bytes from the socket's read buffer; bytes past them stay buffered for the next reader.
function readExact(socket: net.Socket, n: number): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const attempt = () => {
      const chunk = socket.read(n);
      if (chunk !== null) {
        done();
        resolve(chunk);
      }
    };
    const fail = (err: Error) => {
      done();
      reject(err);
    };
    const closed = () => fail(new Error("the proxy closed the connection"));
    const done = () => {
      socket.removeListener("readable", attempt);
      socket.removeListener("error", fail);
      socket.removeListener("end", closed);
      socket.removeListener("close", closed);
    };
    socket.on("readable", attempt);
    socket.on("error", fail);
    socket.on("end", closed);
    socket.on("close", closed);
    attempt();
  });
}

async function readHead(socket: net.Socket): Promise<string> {
  let head = "";
  while (!head.endsWith("\r\n\r\n")) {
    head += (await readExact(socket, 1)).toString("latin1");
    if (head.length > MAX_HEAD) throw new Error("answered with a head that is too long");
  }
  return head;
}

async function httpConnect(socket: net.Socket, proxy: ProxyTarget, host: string, port: number): Promise<void> {
  const target = `${net.isIP(host) === 6 ? `[${host}]` : host}:${port}`;
  const auth =
    proxy.user || proxy.pass
      ? `Proxy-Authorization: Basic ${Buffer.from(`${proxy.user}:${proxy.pass}`).toString("base64")}\r\n`
      : "";
  socket.write(`CONNECT ${target} HTTP/1.1\r\nHost: ${target}\r\n${auth}\r\n`);
  const head = await readHead(socket);
  const status = /^HTTP\/1\.[01] (\d{3})/.exec(head);
  if (!status) throw new Error("did not answer the CONNECT with HTTP");
  if (status[1] !== "200") throw new Error(`refused the CONNECT (HTTP ${status[1]})`);
}

function ipv4Bytes(text: string): Buffer {
  return Buffer.from(text.split(".").map((part) => Number(part)));
}

function ipv6Bytes(text: string): Buffer {
  const halves = text.split("::");
  const groups = (part: string) => (part ? part.split(":") : []);
  const left = groups(halves[0]);
  const right = halves.length > 1 ? groups(halves[1]) : [];
  const all = [...left, ...new Array(8 - left.length - right.length).fill("0"), ...right];
  const out = Buffer.alloc(16);
  all.forEach((group, i) => out.writeUInt16BE(Number.parseInt(group, 16) || 0, i * 2));
  return out;
}

// The target address field of a SOCKS5 request (ATYP + address).
function socksAddress(address: string, family: number): Buffer {
  return family === 6 ? Buffer.concat([Buffer.from([4]), ipv6Bytes(address)]) : Buffer.concat([Buffer.from([1]), ipv4Bytes(address)]);
}

async function socksConnect(socket: net.Socket, proxy: ProxyTarget, host: string, port: number): Promise<void> {
  const wantAuth = proxy.user !== "" || proxy.pass !== "";
  socket.write(wantAuth ? Buffer.from([5, 2, 0, 2]) : Buffer.from([5, 1, 0]));
  const [version, method] = await readExact(socket, 2);
  if (version !== 5) throw new Error("is not a SOCKS5 proxy");
  if (method === 2 && wantAuth) {
    const user = Buffer.from(proxy.user, "utf8");
    const pass = Buffer.from(proxy.pass, "utf8");
    if (user.length > 255 || pass.length > 255) throw new Error("the username or password is longer than 255 bytes");
    socket.write(Buffer.concat([Buffer.from([1, user.length]), user, Buffer.from([pass.length]), pass]));
    const [, status] = await readExact(socket, 2);
    if (status !== 0) throw new Error("rejected the username and password");
  } else if (method !== 0) {
    throw new Error(
      method === 255 ? "accepts none of the authentication methods the bridge offers" : `chose authentication method ${method} the bridge does not offer`,
    );
  }

  let address: Buffer;
  const literal = net.isIP(host);
  if (literal) {
    address = socksAddress(host, literal);
  } else if (proxy.kind === "socks5h") {
    const name = Buffer.from(host, "utf8");
    if (name.length > 255) throw new Error("the host name is longer than 255 bytes");
    address = Buffer.concat([Buffer.from([3, name.length]), name]);
  } else {
    // socks5: this bridge resolves the name, the proxy gets the address
    const found = await dns.promises.lookup(host);
    address = socksAddress(found.address, found.family);
  }
  const portBytes = Buffer.alloc(2);
  portBytes.writeUInt16BE(port, 0);
  socket.write(Buffer.concat([Buffer.from([5, 1, 0]), address, portBytes]));

  const [reply, code, , atyp] = await readExact(socket, 4);
  if (reply !== 5) throw new Error("answered with a bad SOCKS5 reply");
  if (code !== 0) throw new Error(`refused the connection (SOCKS5 reply ${code}: ${SOCKS_REPLIES[code] || "unknown"})`);
  let bound: number;
  if (atyp === 1) bound = 4;
  else if (atyp === 4) bound = 16;
  else if (atyp === 3) bound = (await readExact(socket, 1))[0];
  else throw new Error("answered with a bad SOCKS5 address type");
  await readExact(socket, bound + 2); // the proxy's bound address and port
}

/**
 * A connected socket to host:port through the proxy (no TLS to the target yet). The proxy's
 * answer is consumed; bytes it sends after that stay in the socket. Errors name the proxy's
 * label and never the credentials.
 */
export async function openTunnel(proxy: ProxyTarget, host: string, port: number, timeoutMs = TIMEOUT_MS): Promise<net.Socket> {
  const socket: net.Socket =
    proxy.kind === "https"
      ? tls.connect({ host: proxy.host, port: proxy.port, servername: net.isIP(proxy.host) ? undefined : proxy.host })
      : net.connect({ host: proxy.host, port: proxy.port });
  const timer = setTimeout(() => socket.destroy(new Error(`no answer within ${timeoutMs} ms`)), timeoutMs);
  try {
    socket.setNoDelay(true);
    await waitFor(socket, proxy.kind === "https" ? "secureConnect" : "connect");
    if (proxy.kind === "socks5" || proxy.kind === "socks5h") await socksConnect(socket, proxy, host, port);
    else await httpConnect(socket, proxy, host, port);
    return socket;
  } catch (err) {
    socket.destroy();
    throw new Error(`${proxy.label}: ${(err as Error).message}`);
  } finally {
    clearTimeout(timer);
  }
}

/** TLS to a krunker.io host through the proxy (certificate checked against ca, default: the system store). */
export async function dialKrunker(proxy: ProxyTarget, host: string, ca?: string | Buffer): Promise<tls.TLSSocket> {
  const tunnel = await openTunnel(proxy, host, RELAY_PORT);
  const secure = tls.connect({ socket: tunnel, servername: host, ca });
  const timer = setTimeout(() => secure.destroy(new Error("no TLS answer")), TIMEOUT_MS);
  try {
    await waitFor(secure, "secureConnect");
    return secure;
  } catch (err) {
    secure.destroy();
    throw new Error(`${proxy.label}: TLS to ${host} failed: ${(err as Error).message}`);
  } finally {
    clearTimeout(timer);
  }
}

// The one connection an upstream request may use: the tunnel's TLS socket. (https.request with
// agent:false ignores createConnection and dials the host itself, which would skip the proxy.)
class TunnelAgent extends https.Agent {
  constructor(private readonly tunnel: tls.TLSSocket) {
    super({ keepAlive: false });
  }

  createConnection(): net.Socket {
    return this.tunnel;
  }
}

function queryParam(url: string | undefined, name: string): string | null {
  try {
    return new URL(url || "/", "http://bridge").searchParams.get(name);
  } catch (_err) {
    return null;
  }
}

function sendJson(res: ServerResponse, status: number, body: Record<string, string>): void {
  if (res.headersSent) {
    res.destroy();
    return;
  }
  res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  res.end(JSON.stringify(body));
}

function rejectUpgrade(socket: net.Socket, status: number, reason: string, body: Record<string, string>): void {
  const text = JSON.stringify(body);
  socket.end(
    `HTTP/1.1 ${status} ${reason}\r\nContent-Type: application/json\r\nContent-Length: ${Buffer.byteLength(text)}\r\nConnection: close\r\n\r\n${text}`,
  );
}

function readBody(req: IncomingMessage): Promise<Buffer | null> {
  return new Promise((resolve, reject) => {
    const parts: Buffer[] = [];
    let size = 0;
    let tooLarge = false;
    req.on("data", (chunk: Buffer) => {
      size += chunk.length;
      if (size > MAX_BODY) tooLarge = true;
      else parts.push(chunk);
    });
    req.on("end", () => resolve(tooLarge ? null : Buffer.concat(parts)));
    req.on("error", reject);
  });
}

function unreachable(err: unknown): Record<string, string> {
  return {
    error: "krunker_proxy_unreachable",
    message: `krunker.io is reached only through the internal proxy (${PROXY_ENV}): ${(err as Error).message}`,
  };
}

export type KrunkerRelayOptions = {
  /** Trust anchors for the krunker.io certificate (tests; the gate itself never sets it). */
  ca?: string | Buffer;
};

export type KrunkerRelay = {
  enabled: boolean;
  handlesHttp(pathname: string): boolean;
  handlesUpgrade(pathname: string): boolean;
  handleHttp(req: IncomingMessage, res: ServerResponse): void;
  handleUpgrade(req: IncomingMessage, socket: net.Socket, head: Buffer): void;
};

const OFF: KrunkerRelay = {
  enabled: false,
  handlesHttp: () => false,
  handlesUpgrade: () => false,
  handleHttp: () => {},
  handleUpgrade: () => {},
};

/** The relay through proxy; null = OFF (no route is handled, so the bridge answers as it always did). */
export function createKrunkerRelay(proxy: ProxyTarget | null, options: KrunkerRelayOptions = {}): KrunkerRelay {
  if (!proxy) return OFF;
  console.info(
    `[krunker-relay] krunker.io routed through the internal proxy ${proxy.label}` +
      `${proxy.user || proxy.pass ? " (credentials set)" : ""}: ${RELAY_HTTP_PATH}, ${RELAY_WS_PATH}`,
  );

  const relayHttp = async (req: IncomingMessage, res: ServerResponse, method: string, host: string, path: string) => {
    const body = method === "POST" ? await readBody(req) : Buffer.alloc(0);
    if (body === null) {
      res.setHeader("Connection", "close");
      sendJson(res, 413, { error: "body_too_large", message: "the relayed request body is over 256 KB" });
      return;
    }
    let secure: tls.TLSSocket;
    try {
      secure = await dialKrunker(proxy, host, options.ca);
    } catch (err) {
      console.error(`[krunker-relay] ${method} ${host}${path.split("?")[0]} not sent: ${(err as Error).message}`);
      sendJson(res, 502, unreachable(err));
      return;
    }
    const headers: Record<string, string | string[]> = { host, origin: ORIGIN };
    for (const name of FORWARD_HEADERS) {
      const value = req.headers[name];
      if (value !== undefined) headers[name] = value as string | string[];
    }
    const up = https.request({
      host,
      port: RELAY_PORT,
      method,
      path,
      headers,
      agent: new TunnelAgent(secure),
      timeout: TIMEOUT_MS,
    });
    up.on("response", (upRes: IncomingMessage) => {
      const out: Record<string, string | string[]> = {};
      for (const [name, value] of Object.entries(upRes.headers)) {
        if (value !== undefined && !HOP_BY_HOP.includes(name)) out[name] = value as string | string[];
      }
      res.writeHead(upRes.statusCode || 502, out);
      upRes.on("error", () => res.destroy());
      upRes.pipe(res);
    });
    up.on("timeout", () => up.destroy(new Error("no answer from krunker.io")));
    up.on("error", (err: Error) => {
      console.error(`[krunker-relay] ${method} ${host}${path.split("?")[0]} failed: ${err.message}`);
      sendJson(res, 502, { error: "krunker_upstream_error", message: err.message });
    });
    up.end(body);
  };

  const relayWs = async (req: IncomingMessage, socket: net.Socket, head: Buffer, host: string, path: string) => {
    let secure: tls.TLSSocket;
    try {
      secure = await dialKrunker(proxy, host, options.ca);
    } catch (err) {
      console.error(`[krunker-relay] ws ${host}${path.split("?")[0]} not opened: ${(err as Error).message}`);
      rejectUpgrade(socket, 502, "Bad Gateway", unreachable(err));
      return;
    }
    if (socket.destroyed || !socket.writable) {
      secure.destroy();
      return;
    }
    const lines = [`GET ${path} HTTP/1.1`, `Host: ${host}`, "Upgrade: websocket", "Connection: Upgrade", `Origin: ${ORIGIN}`];
    for (const [name, canonical] of WS_HEADERS) {
      const value = req.headers[name];
      if (value) lines.push(`${canonical}: ${Array.isArray(value) ? value.join(", ") : value}`);
    }
    secure.write(`${lines.join("\r\n")}\r\n\r\n`);
    if (head.length) secure.write(head);
    console.info(`[krunker-relay] ws ${host}${path.split("?")[0]} opened via ${proxy.label}`);

    const close = () => {
      secure.destroy();
      socket.destroy();
    };
    secure.on("error", (err: Error) => {
      console.error(`[krunker-relay] ws ${host} upstream error: ${err.message}`);
      close();
    });
    socket.on("error", close);
    secure.once("close", () => socket.destroy());
    socket.once("close", () => secure.destroy());
    secure.pipe(socket);
    socket.pipe(secure);
  };

  return {
    enabled: true,
    handlesHttp: (pathname) => pathname === RELAY_HTTP_PATH,
    handlesUpgrade: (pathname) => pathname === RELAY_WS_PATH,
    handleHttp(req, res) {
      const method = req.method || "GET";
      if (method !== "GET" && method !== "HEAD" && method !== "POST") {
        sendJson(res, 405, { error: "method_not_allowed", message: "GET, HEAD and POST are relayed" });
        return;
      }
      const target = relayTarget(queryParam(req.url, "url"), "https:");
      if ("error" in target) {
        console.warn(`[krunker-relay] ${method} refused: ${target.error}`);
        sendJson(res, 403, { error: "krunker_only", message: target.error });
        return;
      }
      relayHttp(req, res, method, target.host, target.path).catch((err) => {
        console.error(`[krunker-relay] ${method} ${target.host} threw:`, err);
        sendJson(res, 500, { error: "internal" });
      });
    },
    handleUpgrade(req, socket, head) {
      const target = relayTarget(queryParam(req.url, "url"), "wss:");
      if ("error" in target) {
        console.warn(`[krunker-relay] ws refused: ${target.error}`);
        rejectUpgrade(socket, 403, "Forbidden", { error: "krunker_only", message: target.error });
        return;
      }
      if (!/^websocket$/i.test(String(req.headers.upgrade || "")) || !req.headers["sec-websocket-key"]) {
        rejectUpgrade(socket, 400, "Bad Request", { error: "not_a_websocket", message: "the relay answers WebSocket upgrades only" });
        return;
      }
      relayWs(req, socket, head, target.host, target.path).catch((err) => {
        console.error(`[krunker-relay] ws ${target.host} threw:`, err);
        socket.destroy();
      });
    },
  };
}

/** The relay for this process: OFF unless KN_BRIDGE_KRUNKER_PROXY is set; a bad value throws. */
export function relayFromEnv(env: NodeJS.ProcessEnv = process.env): KrunkerRelay {
  return createKrunkerRelay(proxyFromEnv(env));
}
