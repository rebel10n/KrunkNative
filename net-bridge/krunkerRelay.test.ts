// Unit checks of the krunker.io relay (krunkerRelay.ts): the KN_BRIDGE_KRUNKER_PROXY gate, the proxy URI
// rules, the host allowlist, the CONNECT / SOCKS5 protocols against fake proxies, the routes through a
// real HTTP server, and that the password never reaches a log or a response. No network is used: every
// proxy is a local fake, and no krunker.io name is ever resolved directly.
//   npm test            (node -r ts-node/register/transpile-only krunkerRelay.test.ts)
import assert from "assert";
import { execFileSync } from "child_process";
import fs from "fs";
import http from "http";
import https from "https";
import net from "net";
import os from "os";
import path from "path";
import { WebSocket, WebSocketServer } from "ws";
import {
  createKrunkerRelay,
  isKrunkerHost,
  openTunnel,
  parseProxyUrl,
  proxyFromEnv,
  relayFromEnv,
  relayTarget,
  RELAY_HTTP_PATH,
  RELAY_WS_PATH,
  type ProxyTarget,
} from "./krunkerRelay";

const PASS = "s3cret-PW-test";
const USER = "kn-relay";
const AUTH = `${USER}:${PASS}`;

// every console line of the run, checked at the end for the password
const logged: string[] = [];
for (const level of ["log", "info", "warn", "error"] as const) {
  const original = console[level];
  console[level] = (...args: unknown[]) => {
    logged.push(args.map((a) => (a instanceof Error ? `${a.message} ${a.stack || ""}` : String(a))).join(" "));
    if (process.env.KRUNKER_RELAY_TEST_VERBOSE) original.apply(console, args);
  };
}

function listen(server: net.Server | http.Server): Promise<number> {
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve((server.address() as net.AddressInfo).port)));
}

type Fake = { port: number; conns: number; server: net.Server };

// A fake proxy: onConn gets every accepted connection; conns counts them (a test that expects no
// connection checks that the count did not move).
async function fakeProxy(onConn: (s: net.Socket) => void): Promise<Fake> {
  const fake: Fake = { port: 0, conns: 0, server: net.createServer() };
  fake.server.on("connection", (s) => {
    fake.conns++;
    s.on("error", () => {});
    onConn(s);
  });
  fake.port = await listen(fake.server);
  return fake;
}

// Reads bytes from a socket in flowing mode: take(n) resolves with exactly n bytes.
function reader(s: net.Socket) {
  let buf = Buffer.alloc(0);
  let waiter: (() => void) | null = null;
  s.on("data", (chunk: Buffer) => {
    buf = Buffer.concat([buf, chunk]);
    if (waiter) {
      const wake = waiter;
      waiter = null;
      wake();
    }
  });
  return {
    async take(n: number): Promise<Buffer> {
      while (buf.length < n) await new Promise<void>((resolve) => (waiter = resolve));
      const out = buf.subarray(0, n);
      buf = buf.subarray(n);
      return out;
    },
    async head(): Promise<string> {
      while (!buf.toString("latin1").includes("\r\n\r\n")) await new Promise<void>((resolve) => (waiter = resolve));
      const end = buf.indexOf("\r\n\r\n") + 4;
      const out = buf.subarray(0, end).toString("latin1");
      buf = buf.subarray(end);
      return out;
    },
  };
}

function proxyOf(fake: Fake, kind: ProxyTarget["kind"], user = "", pass = ""): ProxyTarget {
  return { kind, host: "127.0.0.1", port: fake.port, user, pass, label: `${kind}://127.0.0.1:${fake.port}` };
}

function bodyOf(res: http.IncomingMessage): Promise<{ status: number; body: string }> {
  return new Promise((resolve, reject) => {
    let body = "";
    res.setEncoding("utf8");
    res.on("data", (c) => (body += c));
    res.on("end", () => resolve({ status: res.statusCode || 0, body }));
    res.on("error", reject);
  });
}

// The raw answer of a WebSocket upgrade attempt (status line + head), read from the bridge.
function upgradeAnswer(port: number, target: string): Promise<string> {
  return new Promise((resolve, reject) => {
    const s = net.connect({ host: "127.0.0.1", port });
    s.on("error", reject);
    s.write(
      `GET ${target} HTTP/1.1\r\nHost: bridge\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
    );
    let text = "";
    s.on("data", (c) => {
      text += c.toString("latin1");
      if (text.includes("\r\n\r\n") || text.length > 4096) {
        s.destroy();
        resolve(text);
      }
    });
    s.on("close", () => resolve(text));
  });
}

async function main() {
  // 1. proxy URIs: the client's shapes, default ports, credentials percent-decoded
  assert.deepStrictEqual(parseProxyUrl("socks5h://kn-relay:s3cret-PW-test@10.1.2.3:1080/"), {
    kind: "socks5h", host: "10.1.2.3", port: 1080, user: "kn-relay", pass: PASS, label: "socks5h://10.1.2.3:1080",
  });
  assert.deepStrictEqual(parseProxyUrl("http://proxy.internal"), {
    kind: "http", host: "proxy.internal", port: 80, user: "", pass: "", label: "http://proxy.internal:80",
  });
  assert.strictEqual(parseProxyUrl("https://proxy.internal").port, 443);
  assert.strictEqual(parseProxyUrl("socks5://proxy.internal").port, 1080);
  assert.strictEqual(parseProxyUrl("proxy.internal:3128").kind, "http"); // bare host:port is http://
  assert.strictEqual(parseProxyUrl("SOCKS5://proxy.internal:9").kind, "socks5");
  const encoded = parseProxyUrl("http://kn%40relay:p%40ss%3Aword@proxy.internal:8888");
  assert.strictEqual(encoded.user, "kn@relay");
  assert.strictEqual(encoded.pass, "p@ss:word");
  assert.strictEqual(parseProxyUrl("http://[::1]:3128").label, "http://[::1]:3128");
  for (const bad of [
    "", "   ", "ftp://proxy:21", "socks4://proxy:1080", "http://proxy/path", "http://proxy:0", "http://proxy:70000",
    "http://:8080", "http://proxy:", "http://proxy:80x", "http://u%zz@proxy:1", "http://[::1:8080",
    "socks5h://kn-relay:s3cret-PW-test@bad host:1080", "https://kn-relay:s3cret-PW-test@proxy:1/x",
  ]) {
    assert.throws(() => parseProxyUrl(bad), (err: Error) => !err.message.includes(PASS), `bad: ${bad}`);
  }

  // 2. the gate: unset or empty is OFF, a value turns it on, a bad value stops the bridge
  assert.strictEqual(proxyFromEnv({}), null);
  assert.strictEqual(proxyFromEnv({ KN_BRIDGE_KRUNKER_PROXY: "" }), null);
  assert.strictEqual(proxyFromEnv({ KN_BRIDGE_KRUNKER_PROXY: "  " }), null);
  assert.strictEqual(proxyFromEnv({ KN_BRIDGE_KRUNKER_PROXY: "socks5h://proxy.internal:1080" })?.kind, "socks5h");
  assert.throws(() => relayFromEnv({ KN_BRIDGE_KRUNKER_PROXY: "ftp://proxy:21" }), /not a proxy URL/);
  const off = relayFromEnv({});
  assert.strictEqual(off.enabled, false);
  assert.strictEqual(off.handlesHttp(RELAY_HTTP_PATH), false); // no route: the bridge answers as before
  assert.strictEqual(off.handlesUpgrade(RELAY_WS_PATH), false);

  // 3. the host allowlist: krunker.io and its subdomains only, port 443
  for (const host of ["krunker.io", "api.krunker.io", "Krunker.IO.", "play-1.krunker.io", "crucible.frvr.com"]) {
    assert.strictEqual(isKrunkerHost(host), true, host);
  }
  for (const host of ["example.com", "krunker.io.evil.com", "evilkrunker.io", "krunker.ioo", "frvr.com", "localhost"]) {
    assert.strictEqual(isKrunkerHost(host), false, host);
  }
  assert.deepStrictEqual(relayTarget("https://api.krunker.io/x/y?z=1", "https:"), { host: "api.krunker.io", path: "/x/y?z=1" });
  assert.deepStrictEqual(relayTarget("wss://play-1.krunker.io/ws", "wss:"), { host: "play-1.krunker.io", path: "/ws" });
  for (const [raw, scheme] of [
    ["https://example.com/", "https:"], ["https://krunker.io.evil.com/", "https:"], ["https://krunker.io:8443/", "https:"],
    ["http://krunker.io/", "https:"], ["https://krunker.io/", "wss:"], ["https://user:pw@krunker.io/", "https:"],
    ["https://krunker.io@example.com/", "https:"], ["krunker.io/", "https:"], ["", "https:"], [null, "https:"],
  ] as Array<[string | null, "https:" | "wss:"]>) {
    assert.ok("error" in relayTarget(raw, scheme), `refused: ${raw}`);
  }

  // 4. the protocols against fake proxies: SOCKS5 with user/password (domain and IPv4), CONNECT, refusals, timeout, https
  const socks = await fakeProxy(async (s) => {
    const r = reader(s);
    assert.deepStrictEqual([...(await r.take(4))], [5, 2, 0, 2]); // offers no-auth and user/password
    s.write(Buffer.from([5, 2]));
    assert.strictEqual((await r.take(1))[0], 1);
    const user = (await r.take((await r.take(1))[0])).toString();
    const pass = (await r.take((await r.take(1))[0])).toString();
    assert.strictEqual(`${user}:${pass}`, AUTH);
    s.write(Buffer.from([1, 0]));
    assert.deepStrictEqual([...(await r.take(4))], [5, 1, 0, 3]); // socks5h: the name goes to the proxy
    const name = (await r.take((await r.take(1))[0])).toString();
    assert.strictEqual(name, "www.krunker.io");
    assert.strictEqual((await r.take(2)).readUInt16BE(0), 443);
    // the reply and the tunnel's first bytes in one write: the bytes past the reply must still be read
    s.write(Buffer.concat([Buffer.from([5, 0, 0, 1, 0, 0, 0, 0, 0, 0]), Buffer.from("HELLO")]));
  });
  const tunnel = await openTunnel(proxyOf(socks, "socks5h", USER, PASS), "www.krunker.io", 443, 3000);
  const got = await new Promise<string>((resolve) => {
    let text = "";
    tunnel.on("data", (c) => {
      text += c.toString();
      if (text.length >= 5) resolve(text);
    });
  });
  assert.strictEqual(got, "HELLO");
  tunnel.destroy();

  const socksIp = await fakeProxy(async (s) => {
    const r = reader(s);
    assert.deepStrictEqual([...(await r.take(3))], [5, 1, 0]); // no credentials: no-auth only
    s.write(Buffer.from([5, 0]));
    assert.deepStrictEqual([...(await r.take(4))], [5, 1, 0, 1]);
    assert.deepStrictEqual([...(await r.take(4))], [127, 0, 0, 1]);
    assert.strictEqual((await r.take(2)).readUInt16BE(0), 443);
    s.write(Buffer.from([5, 0, 0, 1, 0, 0, 0, 0, 0, 0]));
  });
  (await openTunnel(proxyOf(socksIp, "socks5"), "127.0.0.1", 443, 3000)).destroy();

  const socksRefused = await fakeProxy(async (s) => {
    const r = reader(s);
    await r.take(3);
    s.write(Buffer.from([5, 0]));
    await r.take(10);
    s.write(Buffer.from([5, 5, 0, 1, 0, 0, 0, 0, 0, 0]));
  });
  await assert.rejects(openTunnel(proxyOf(socksRefused, "socks5h"), "www.krunker.io", 443, 3000), (err: Error) =>
    err.message.includes("refused the connection (SOCKS5 reply 5: connection refused)") && err.message.startsWith("socks5h://127.0.0.1:"),
  );

  const socksBadAuth = await fakeProxy(async (s) => {
    const r = reader(s);
    await r.take(4);
    s.write(Buffer.from([5, 2]));
    await r.take(3 + USER.length + PASS.length);
    s.write(Buffer.from([1, 1]));
  });
  await assert.rejects(openTunnel(proxyOf(socksBadAuth, "socks5h", USER, PASS), "www.krunker.io", 443, 3000), (err: Error) =>
    err.message.includes("rejected the username and password") && !err.message.includes(PASS),
  );

  const connect = await fakeProxy(async (s) => {
    const r = reader(s);
    const head = await r.head();
    assert.ok(head.startsWith("CONNECT www.krunker.io:443 HTTP/1.1\r\n"), head);
    assert.ok(head.includes(`Proxy-Authorization: Basic ${Buffer.from(AUTH).toString("base64")}\r\n`), head);
    s.write("HTTP/1.1 200 Connection established\r\n\r\nHELLO");
  });
  const viaConnect = await openTunnel(proxyOf(connect, "http", USER, PASS), "www.krunker.io", 443, 3000);
  const hello = await new Promise<string>((resolve) => viaConnect.once("data", (c) => resolve(c.toString())));
  assert.strictEqual(hello, "HELLO");
  viaConnect.destroy();

  const refused407 = await fakeProxy(async (s) => {
    await reader(s).head();
    s.end("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n");
  });
  await assert.rejects(openTunnel(proxyOf(refused407, "http", USER, PASS), "www.krunker.io", 443, 3000), (err: Error) =>
    err.message.endsWith("refused the CONNECT (HTTP 407)"),
  );

  const silent = await fakeProxy(() => {});
  await assert.rejects(openTunnel(proxyOf(silent, "socks5h"), "www.krunker.io", 443, 300), (err: Error) =>
    err.message.includes("no answer within 300 ms"),
  );

  const plainForTls = await fakeProxy((s) => s.destroy()); // an https:// proxy that is not TLS
  await assert.rejects(openTunnel(proxyOf(plainForTls, "https"), "www.krunker.io", 443, 3000), (err: Error) =>
    err.message.startsWith("https://127.0.0.1:"),
  );

  // 5. the routes through a real bridge server, with an HTTP CONNECT fake that answers 502 or 200 + a TLS hello
  let lastHead = "";
  let firstTlsByte = -1;
  let answer: "502" | "tls" = "502";
  const connectFake = await fakeProxy(async (s) => {
    const r = reader(s);
    lastHead = await r.head();
    if (answer === "502") {
      s.end("HTTP/1.1 502 Bad Gateway\r\n\r\n");
      return;
    }
    s.write("HTTP/1.1 200 Connection established\r\n\r\n");
    const hello = await r.take(1);
    firstTlsByte = hello[0];
    s.destroy();
  });
  const relay = createKrunkerRelay(proxyOf(connectFake, "http", USER, PASS));
  assert.strictEqual(relay.enabled, true);
  assert.strictEqual(relay.handlesHttp(RELAY_HTTP_PATH), true);
  assert.strictEqual(relay.handlesUpgrade(RELAY_WS_PATH), true);
  assert.strictEqual(relay.handlesHttp("/krunker-relay/other"), false);

  const bridge = http.createServer((req, res) => {
    const pathname = (req.url || "/").split("?")[0];
    if (relay.handlesHttp(pathname)) relay.handleHttp(req, res);
    else {
      res.writeHead(404);
      res.end();
    }
  });
  bridge.on("upgrade", (req, socket: net.Socket, head) => {
    const pathname = (req.url || "/").split("?")[0];
    if (relay.handlesUpgrade(pathname)) relay.handleUpgrade(req, socket, head);
    else socket.destroy();
  });
  const bridgePort = await listen(bridge);
  const get = (target: string, method = "GET") =>
    new Promise<{ status: number; body: string }>((resolve, reject) => {
      const req = http.request({ host: "127.0.0.1", port: bridgePort, path: target, method }, (res) => {
        bodyOf(res).then(resolve, reject);
      });
      req.on("error", reject);
      req.end();
    });

  const before = connectFake.conns;
  assert.strictEqual((await get(`${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://example.com/")}`)).status, 403);
  assert.strictEqual((await get(`${RELAY_HTTP_PATH}?url=${encodeURIComponent("http://krunker.io/")}`)).status, 403);
  assert.strictEqual((await get(`${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://krunker.io:8443/")}`)).status, 403);
  assert.strictEqual((await get(RELAY_HTTP_PATH)).status, 403);
  assert.strictEqual((await get(`${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://krunker.io/")}`, "PUT")).status, 405);
  assert.strictEqual(connectFake.conns, before, "refused hosts never reach a proxy");

  answer = "502";
  const down = await get(`${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://api.krunker.io/v1/seek-game")}`);
  assert.strictEqual(down.status, 502);
  assert.ok(down.body.includes("krunker_proxy_unreachable"), down.body);
  assert.ok(down.body.includes("http://127.0.0.1:"), down.body);
  assert.strictEqual(connectFake.conns, before + 1);
  assert.ok(lastHead.startsWith("CONNECT api.krunker.io:443 HTTP/1.1\r\n"), lastHead);
  assert.ok(!down.body.includes(PASS));

  answer = "tls";
  const tlsSeen = await get(`${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://api.krunker.io/")}`);
  assert.strictEqual(tlsSeen.status, 502); // the tunnel opened, the TLS handshake failed
  assert.strictEqual(firstTlsByte, 0x16, "the TLS ClientHello went through the tunnel");

  // 6. WebSocket relay: refused hosts and bad upgrades before any connection; krunker goes through the tunnel
  const wsBefore = connectFake.conns;
  assert.match(await upgradeAnswer(bridgePort, `${RELAY_WS_PATH}?url=${encodeURIComponent("wss://example.com/ws")}`), /^HTTP\/1\.1 403 /);
  assert.match(await upgradeAnswer(bridgePort, `${RELAY_WS_PATH}?url=${encodeURIComponent("ws://play.krunker.io/ws")}`), /^HTTP\/1\.1 403 /);
  assert.strictEqual(connectFake.conns, wsBefore, "refused websockets never reach a proxy");
  answer = "502";
  const wsDown = await upgradeAnswer(bridgePort, `${RELAY_WS_PATH}?url=${encodeURIComponent("wss://play-1.krunker.io/game")}`);
  assert.match(wsDown, /^HTTP\/1\.1 502 /);
  assert.ok(wsDown.includes("krunker_proxy_unreachable") && !wsDown.includes(PASS));
  assert.strictEqual(connectFake.conns, wsBefore + 1);
  answer = "tls";
  await upgradeAnswer(bridgePort, `${RELAY_WS_PATH}?url=${encodeURIComponent("wss://play-1.krunker.io/game")}`);
  assert.strictEqual(firstTlsByte, 0x16);

  // 7. a dead proxy: 502 naming the proxy, no fallback to a direct connection
  const probe = net.createServer();
  const freePort = await listen(probe);
  await new Promise<void>((resolve) => probe.close(() => resolve())); // a port nothing listens on
  const dead = createKrunkerRelay({ kind: "socks5h", host: "127.0.0.1", port: freePort, user: USER, pass: PASS, label: `socks5h://127.0.0.1:${freePort}` });
  const deadBridge = http.createServer((req, res) => dead.handleHttp(req, res));
  const deadPort = await listen(deadBridge);
  const deadAnswer = await new Promise<{ status: number; body: string }>((resolve, reject) => {
    http.get({ host: "127.0.0.1", port: deadPort, path: `${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://krunker.io/")}` }, (res) => {
      bodyOf(res).then(resolve, reject);
    }).on("error", reject);
  });
  assert.strictEqual(deadAnswer.status, 502);
  assert.ok(deadAnswer.body.includes(`socks5h://127.0.0.1:${freePort}`), deadAnswer.body);
  assert.ok(!deadAnswer.body.includes(PASS));

  // 8. the gate is off: the bridge's own 404 (no relay route), and nothing is dialled
  const offBridge = http.createServer((req, res) => {
    if (relayFromEnv({}).handlesHttp((req.url || "/").split("?")[0])) return;
    res.writeHead(404);
    res.end();
  });
  const offPort = await listen(offBridge);
  const offAnswer = await new Promise<{ status: number; body: string }>((resolve, reject) => {
    http.get({ host: "127.0.0.1", port: offPort, path: `${RELAY_HTTP_PATH}?url=${encodeURIComponent("https://krunker.io/")}` }, (res) => {
      bodyOf(res).then(resolve, reject);
    }).on("error", reject);
  });
  assert.strictEqual(offAnswer.status, 404);

  // 10. a full round trip: TLS to a local origin under krunker.io's names, reached only through a CONNECT
  // fake that tunnels to it (the origin's certificate is made by openssl and trusted through the relay's
  // test ca option). Proves the request, the body, the response and the WebSocket all use the tunnel.
  const certDir = fs.mkdtempSync(path.join(os.tmpdir(), "kn-relay-tls-"));
  const keyFile = path.join(certDir, "key.pem");
  const certFile = path.join(certDir, "cert.pem");
  let cert: Buffer | null = null;
  try {
    execFileSync(
      "openssl",
      ["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", keyFile, "-out", certFile,
        "-days", "1", "-subj", "/CN=api.krunker.io", "-addext", "subjectAltName=DNS:api.krunker.io,DNS:play-1.krunker.io"],
      { stdio: "ignore" },
    );
    cert = fs.readFileSync(certFile);
  } catch (_err) {
    process.stdout.write("krunkerRelay: TLS round trip skipped (openssl not available)\n");
  }
  if (cert) {
    const origin = https.createServer({ key: fs.readFileSync(keyFile), cert }, (req, res) => {
      let body = "";
      req.on("data", (c) => (body += c));
      req.on("end", () => {
        res.writeHead(201, { "Content-Type": "application/json", "X-Upstream": "yes" });
        res.end(JSON.stringify({ method: req.method, url: req.url, host: req.headers.host, origin: req.headers.origin, auth: req.headers.authorization, body }));
      });
    });
    const wsOrigin = new WebSocketServer({ server: origin, path: "/game" });
    wsOrigin.on("connection", (ws: WebSocket, req: http.IncomingMessage) => {
      ws.on("message", (m) => ws.send(`echo:${m}:${req.headers.origin}`));
    });
    const originPort = await listen(origin);
    const connectedTo: string[] = [];
    const tunnelFake = await fakeProxy((s) => {
      let head = "";
      s.on("data", function onHead(chunk: Buffer) {
        head += chunk.toString("latin1");
        if (!head.includes("\r\n\r\n")) return;
        s.removeListener("data", onHead);
        connectedTo.push(head.split("\r\n")[0]);
        assert.ok(head.includes(`Proxy-Authorization: Basic ${Buffer.from(AUTH).toString("base64")}\r\n`), head);
        const up = net.connect(originPort, "127.0.0.1", () => {
          s.write("HTTP/1.1 200 Connection established\r\n\r\n");
          up.pipe(s);
          s.pipe(up);
        });
        up.on("error", () => s.destroy());
      });
    });
    const tlsRelay = createKrunkerRelay(proxyOf(tunnelFake, "http", USER, PASS), { ca: cert });
    const tlsBridge = http.createServer((req, res) => {
      if (tlsRelay.handlesHttp((req.url || "/").split("?")[0])) tlsRelay.handleHttp(req, res);
      else res.writeHead(404).end();
    });
    tlsBridge.on("upgrade", (req, socket: net.Socket, head) => {
      if (tlsRelay.handlesUpgrade((req.url || "/").split("?")[0])) tlsRelay.handleUpgrade(req, socket, head);
      else socket.destroy();
    });
    const tlsPort = await listen(tlsBridge);
    const json = (method: string, target: string, payload?: string) =>
      new Promise<{ status: number; body: any; headers: http.IncomingHttpHeaders }>((resolve, reject) => {
        const req = http.request({ host: "127.0.0.1", port: tlsPort, method, path: `${RELAY_HTTP_PATH}?url=${encodeURIComponent(target)}`,
          headers: { "User-Agent": "relay-test", Authorization: "Bearer tok" } }, (res) => {
          let text = "";
          res.on("data", (c) => (text += c));
          res.on("end", () => resolve({ status: res.statusCode || 0, body: JSON.parse(text), headers: res.headers }));
        });
        req.on("error", reject);
        req.end(payload);
      });
    const got = await json("GET", "https://api.krunker.io/v1/seek-game?region=eu");
    assert.strictEqual(got.status, 201);
    assert.strictEqual(got.headers["x-upstream"], "yes");
    assert.deepStrictEqual(
      { method: got.body.method, url: got.body.url, host: got.body.host, origin: got.body.origin, auth: got.body.auth },
      { method: "GET", url: "/v1/seek-game?region=eu", host: "api.krunker.io", origin: "https://krunker.io", auth: "Bearer tok" },
    );
    const posted = await json("POST", "https://api.krunker.io/post", '{"q":"hello"}');
    assert.strictEqual(posted.status, 201);
    assert.strictEqual(posted.body.method, "POST");
    assert.strictEqual(posted.body.body, '{"q":"hello"}');
    const socket = new WebSocket(`ws://127.0.0.1:${tlsPort}${RELAY_WS_PATH}?url=${encodeURIComponent("wss://play-1.krunker.io/game")}`);
    const echoed = await new Promise<string>((resolve, reject) => {
      socket.on("open", () => socket.send("ping"));
      socket.on("message", (m) => resolve(m.toString()));
      socket.on("error", reject);
      setTimeout(() => reject(new Error("no echo")), 5000).unref();
    });
    assert.strictEqual(echoed, "echo:ping:https://krunker.io");
    socket.close();
    assert.deepStrictEqual(connectedTo.map((l) => l.split(" ")[1]).sort(), ["api.krunker.io:443", "api.krunker.io:443", "play-1.krunker.io:443"]);
    tlsBridge.close();
    origin.close();
    wsOrigin.close();
    tunnelFake.server.close();
  }
  fs.rmSync(certDir, { recursive: true, force: true });

  // 9. credentials: the startup line and every error and log of the run are free of the password
  assert.ok(logged.some((line) => line.includes("socks5h://") && line.includes("(credentials set)")));
  for (const line of logged) assert.ok(!line.includes(PASS), `logged a credential: ${line}`);
  assert.ok(!logged.some((line) => line.includes(Buffer.from(AUTH).toString("base64"))), "the Basic credentials were logged");

  bridge.close();
  deadBridge.close();
  offBridge.close();
  process.stdout.write("krunkerRelay tests OK\n");
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
