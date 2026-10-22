// The harness route table (tools/krunker-local/routes.mjs). A static asset is fetched once through the local
// proxy, then served from the cache; an account, matchmaker or API request never takes the static route and never
// reaches the proxy; any other host is refused, and Chromium resolves nothing but the local listeners.
// Run: node --test tools/krunker-local/routes_test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { createStaticFiles, hostRules, routeRequest } from "./routes.mjs";

const PNG = Buffer.from("89504e470d0a1a0a0000000d4948445200000001", "hex");

// The config the harness builds, with the static origins on plain http: the fake proxy below then sees the
// absolute-form requests that curl sends to an http proxy, with no network and no real host.
function cfgFor(proxy, cache) {
  return {
    port: 8094, tlsPort: 8443, localHost: "krunker-local.test",
    hostRoutes: { "matchmaker.krunker.io": "/mm", "gapi.beta.krunker.io": "", "api.krunker.io": "" },
    staticPageOrigin: "http://krunker.io",
    staticHosts: { "assets.krunker.io": "http://assets.krunker.io" },
    staticProxy: proxy,
    staticCache: cache,
    bridge: { host: "kn-bridge.krunker-local.test" },
  };
}

// A fake proxy on loopback: it records each request it is sent and answers from a table (default: 404, as the CDN does)
function fakeProxy(table) {
  const seen = [];
  const server = http.createServer((req, res) => {
    seen.push(req.url);
    const hit = table[req.url];
    if (!hit) { res.writeHead(404, { "content-type": "application/xml" }); return res.end("<Error>NoSuchKey</Error>"); }
    res.writeHead(hit.status, { "content-type": hit.type });
    res.end(hit.body);
  });
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve({
    url: `http://127.0.0.1:${server.address().port}`,
    seen,
    close: () => new Promise((r) => server.close(r)),
  })));
}

function tmpDir() { return fs.mkdtempSync(path.join(os.tmpdir(), "kn-routes-")); }

function filesUnder(dir) {
  if (!fs.existsSync(dir)) return [];
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? filesUnder(path.join(dir, e.name)) : [path.join(dir, e.name)]);
}

test("a static asset is fetched once through the proxy, then served from the cache", async () => {
  const url = "http://assets.krunker.io/textures/ammo_0.png?build=OpCIg";
  const proxy = await fakeProxy({ [url]: { status: 200, type: "image/png", body: PNG } });
  const cache = tmpDir();
  try {
    const cfg = cfgFor(proxy.url, cache);
    const files = createStaticFiles(cfg);
    const route = routeRequest(cfg, "assets.krunker.io", "/textures/ammo_0.png", "?build=OpCIg");
    assert.equal(route.kind, "static");
    const first = await files.serve(route);
    assert.equal(first.status, 200);
    assert.equal(first.type, "image/png");
    assert.deepEqual(fs.readFileSync(first.file), PNG);
    const again = await files.serve(route);
    assert.equal(again.status, 200);
    assert.deepEqual(fs.readFileSync(again.file), PNG);
    assert.deepEqual(proxy.seen, [url], "the second request is served from the cache, not the proxy");
  } finally {
    await proxy.close();
    fs.rmSync(cache, { recursive: true, force: true });
  }
});

test("a page-relative file the local tree lacks (img/vic.png) comes from the page origin through the proxy", async () => {
  const proxy = await fakeProxy({ "http://krunker.io/img/vic.png": { status: 200, type: "image/png", body: PNG } });
  const cache = tmpDir();
  try {
    const cfg = cfgFor(proxy.url, cache);
    const files = createStaticFiles(cfg);
    const route = routeRequest(cfg, "", "/img/vic.png", "");
    assert.equal(route.kind, "static");
    assert.equal(route.originHost, "krunker.io");
    const r = await files.serve(route);
    assert.equal(r.status, 200);
    assert.deepEqual(fs.readFileSync(r.file), PNG);
    assert.deepEqual(proxy.seen, ["http://krunker.io/img/vic.png"]);
  } finally {
    await proxy.close();
    fs.rmSync(cache, { recursive: true, force: true });
  }
});

test("a file the proxy reports missing is a 404: nothing is written, nothing is invented, the proxy is asked once", async () => {
  const proxy = await fakeProxy({});
  const cache = tmpDir();
  try {
    const cfg = cfgFor(proxy.url, cache);
    const files = createStaticFiles(cfg);
    const route = routeRequest(cfg, "", "/img/nope.png", "");
    assert.equal((await files.serve(route)).status, 404);
    assert.equal((await files.serve(route)).status, 404);
    assert.deepEqual(proxy.seen, ["http://krunker.io/img/nope.png"]);
    assert.deepEqual(filesUnder(cache), []);
  } finally {
    await proxy.close();
    fs.rmSync(cache, { recursive: true, force: true });
  }
});

test("a 200 HTML answer for an image path (a site's fallback page) is not a static file", async () => {
  const proxy = await fakeProxy({ "http://krunker.io/img/spa.png": { status: 200, type: "text/html", body: "<!doctype html>" } });
  const cache = tmpDir();
  try {
    const cfg = cfgFor(proxy.url, cache);
    const files = createStaticFiles(cfg);
    assert.equal((await files.serve(routeRequest(cfg, "", "/img/spa.png", ""))).status, 404);
    assert.deepEqual(filesUnder(cache), []);
  } finally {
    await proxy.close();
    fs.rmSync(cache, { recursive: true, force: true });
  }
});

test("account, matchmaking and API requests are never static and never reach the proxy", async () => {
  const proxy = await fakeProxy({});
  const cache = tmpDir();
  try {
    const cfg = cfgFor(proxy.url, cache);
    const files = createStaticFiles(cfg);
    const cases = [
      ["gapi.beta.krunker.io", "/guest/profile", ""],
      ["gapi.beta.krunker.io", "/maps/logo.png", ""], // an image path on an account host is still the brain's
      ["matchmaker.krunker.io", "/mm/seek-game", ""],
      ["api.krunker.io", "/data/skins/hat.png", ""],
      ["", "/mm/game-list", ""], // the local origin's matchmaker path
    ];
    for (const [host, p, search] of cases) {
      const route = routeRequest(cfg, host, p, search);
      assert.equal(route.kind, "brain", `${host}${p} goes to the brain, not the static route`);
      const r = await files.serve(route);
      assert.equal(r.status, 403, `${host}${p} is refused by the static route`);
    }
    assert.deepEqual(proxy.seen, []);
  } finally {
    await proxy.close();
    fs.rmSync(cache, { recursive: true, force: true });
  }
});

test("any other host or path is refused: third-party scripts, scripts and data on the CDN, traversal", () => {
  const cfg = cfgFor("http://127.0.0.1:1", "/unused");
  const refused = [
    ["www.googletagmanager.com", "/gtag/js", "?id=UA-1"],
    ["unpkg.com", "/web3@latest/dist/web3.min.js", ""],
    ["challenges.cloudflare.com", "/turnstile/v0/api.js", ""],
    ["assets.krunker.io", "/js/app.js", ""], // a script is not a static asset type
    ["assets.krunker.io", "/textures/%2e%2e/x.png", ""], // traversal, decoded then refused
    ["", "/guest/profile", ""], // not a static type on the local origin
    ["", "/", ""],
  ];
  for (const [host, p, search] of refused) {
    assert.equal(routeRequest(cfg, host, p, search).kind, "refuse", `${host}${p}${search}`);
  }
});

test("with no proxy configured a static request is refused, and nothing is fetched directly", async () => {
  const cache = tmpDir();
  try {
    const cfg = cfgFor("", cache);
    const r = await createStaticFiles(cfg).serve(routeRequest(cfg, "assets.krunker.io", "/textures/ammo_0.png", ""));
    assert.equal(r.status, 404);
    assert.deepEqual(filesUnder(cache), []);
  } finally {
    fs.rmSync(cache, { recursive: true, force: true });
  }
});

test("the harness config routes the user-assets CDN (user textures and models) as static, through the proxy", () => {
  const example = JSON.parse(fs.readFileSync(new URL("./config.example.json", import.meta.url), "utf8"));
  const cfg = { ...example, localHost: "krunker-local.test", staticProxy: "http://127.0.0.1:8888", staticCache: "/unused" };
  for (const [host, p, search] of [
    ["user-assets.krunker.io", "/632/model.obj", "?build=1S33"],
    ["user-assets.krunker.io", "/27432/model.gltf", "?build=1S33"],
    ["user-assets.krunker.io", "/52272/texture.png", ""],
  ]) {
    const route = routeRequest(cfg, host, p, search);
    assert.equal(route.kind, "static", `${host}${p}`);
    assert.equal(route.originHost, "user-assets.krunker.io");
  }
  assert.ok(hostRules(cfg).includes("MAP user-assets.krunker.io 127.0.0.1:8443"));
});

test("Chromium resolves the local listeners and the static hosts; every other host does not resolve", () => {
  const cfg = cfgFor("http://127.0.0.1:1", "/unused");
  cfg.bridge = { host: "kn-bridge.krunker-local.test" };
  assert.deepEqual(hostRules(cfg), [
    "MAP krunker-local.test 127.0.0.1:8094",
    "MAP matchmaker.krunker.io 127.0.0.1:8443",
    "MAP gapi.beta.krunker.io 127.0.0.1:8443",
    "MAP api.krunker.io 127.0.0.1:8443",
    "MAP assets.krunker.io 127.0.0.1:8443",
    "MAP kn-bridge.krunker-local.test 127.0.0.1:8443",
    "MAP * ~NOTFOUND",
  ]);
});
