// The harness route table: where each request to the local stack goes (krunker-local.mjs asks routeRequest per request).
//
//   brain   account, matchmaker and API hosts (cfg.hostRoutes), and /mm/ on the local origin: our brain, on loopback.
//   static  textures, models, images, sounds, fonts and the manifest. They are fetched from the official CDN
//           (cfg.staticHosts) or the page's own site (cfg.staticPageOrigin, for the page-relative files the local
//           tree lacks) through the local HTTP proxy (cfg.staticProxy, KN_FETCH_PROXY), cached under cfg.staticCache,
//           and served from the local origin.
//   refuse  anything else. hostRules() maps every other host to NOTFOUND, so the page cannot reach production.
//
// THE PROXY IS FOR STATIC ASSETS ONLY. A request that is not a static asset type is never sent to it: no account,
// matchmaking, API, game, script or data request. The routes_test.mjs test pins that.
import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";

export const MIME = {
  ".html": "text/html; charset=utf-8", ".js": "application/javascript", ".mjs": "application/javascript",
  ".css": "text/css", ".json": "application/json", ".wasm": "application/wasm", ".png": "image/png",
  ".jpg": "image/jpeg", ".gif": "image/gif", ".svg": "image/svg+xml", ".ttf": "font/ttf",
  ".woff": "font/woff", ".woff2": "font/woff2", ".mp3": "audio/mpeg", ".ogg": "audio/ogg",
  ".webm": "video/webm", ".mp4": "video/mp4", ".jspck": "application/octet-stream", ".txt": "text/plain",
  ".dat": "application/octet-stream", ".webp": "image/webp", ".ico": "image/x-icon", ".otf": "font/otf",
  ".wav": "audio/wav", ".obj": "application/octet-stream", ".mtl": "text/plain", ".gltf": "model/gltf+json",
};

// The asset types the static route fetches: images, models, textures, sounds and fonts. No scripts, pages or data.
export const STATIC_EXT = new Set([".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico", ".woff", ".woff2",
  ".ttf", ".otf", ".obj", ".mtl", ".gltf", ".mp3", ".ogg", ".wav"]);
// The page-relative files that are not in STATIC_EXT: the web manifest, on the page's own site only.
const PAGE_STATIC_EXACT = new Set(["/manifest.json"]);
const SAFE_REL = /^\/[A-Za-z0-9._/-]+$/;
const BUILD = /^[A-Za-z0-9_-]{1,64}$/;

// host: the official host name the request came in on, or "" for the local origin (the page itself).
// pathname and search are the request's, as received (the static route decodes and checks the path).
export function routeRequest(cfg, host, pathname, search = "") {
  if (host && Object.hasOwn(cfg.hostRoutes, host)) return { kind: "brain", prefix: cfg.hostRoutes[host] };
  if (!host && pathname.startsWith("/mm/")) return { kind: "brain", prefix: "" };
  const target = staticTarget(cfg, host, pathname, search);
  return target || { kind: "refuse", reason: `no route for ${host || "the local origin"}${pathname}` };
}

function staticTarget(cfg, host, pathname, search) {
  let rel;
  try { rel = decodeURIComponent(pathname); } catch (e) { return null; }
  if (!SAFE_REL.test(rel) || rel.split("/").some((seg) => seg === "." || seg === "..")) return null;
  const ext = path.posix.extname(rel).toLowerCase();
  let origin;
  if (!host) {
    if (!cfg.staticPageOrigin || !(STATIC_EXT.has(ext) || PAGE_STATIC_EXACT.has(rel))) return null;
    origin = cfg.staticPageOrigin;
  } else {
    if (!cfg.staticHosts || !Object.hasOwn(cfg.staticHosts, host) || !STATIC_EXT.has(ext)) return null;
    origin = cfg.staticHosts[host];
  }
  // only the build id of the query is kept (the CDN's version key); any other parameter is dropped
  const params = new URLSearchParams(search);
  const build = params.get("build") || "";
  if (build && !BUILD.test(build)) return null;
  return {
    kind: "static",
    origin,
    originHost: new URL(origin).hostname,
    rel,
    build,
    url: origin + rel + (build ? `?build=${build}` : ""),
  };
}

// The static files: serve(route) answers from the cache, or fetches the file once through the proxy and caches it.
// A file the proxy cannot give (not 200, empty, or an HTML page for an image path) is a 404, remembered for the run.
export function createStaticFiles(cfg, log = () => {}) {
  const cacheDir = cfg.staticCache;
  const proxy = cfg.staticProxy || "";
  const proxyLabel = proxy ? hostPort(proxy) : "none";
  const inFlight = new Map();
  const missing = new Set();
  return {
    async serve(route) {
      if (!route || route.kind !== "static") return { status: 403, reason: "not a static route" };
      const file = path.join(cacheDir, route.originHost, route.build ? `build-${route.build}` : "", route.rel);
      if (fs.existsSync(file)) return { status: 200, file, type: contentType(route.rel) };
      if (!proxy) return { status: 404, reason: "no static proxy configured" };
      if (missing.has(file)) return { status: 404, reason: "missing" };
      let job = inFlight.get(file);
      if (!job) {
        job = fetchInto(route, file, cacheDir, proxy, proxyLabel, missing, log)
          .catch((e) => { log(`[static] ${route.originHost}${route.rel}: ${e.message}`); missing.add(file); return false; });
        inFlight.set(file, job);
        job.finally(() => inFlight.delete(file));
      }
      if (await job) return { status: 200, file, type: contentType(route.rel) };
      return { status: 404, reason: "missing" };
    },
  };
}

async function fetchInto(route, file, cacheDir, proxy, proxyLabel, missing, log) {
  fs.mkdirSync(cacheDir, { recursive: true });
  const part = path.join(cacheDir, `.part-${process.pid}-${Math.random().toString(36).slice(2)}`);
  const got = await curlVia(route.url, part, proxy);
  const ext = path.posix.extname(route.rel).toLowerCase();
  const htmlForImage = got.type.startsWith("text/html") && ext !== ".html" && ext !== ".htm";
  if (got.status === 200 && got.size > 0 && !htmlForImage) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.renameSync(part, file);
    log(`[static] cached ${route.originHost}${route.build ? ` build ${route.build}` : ""}${route.rel} (proxy ${proxyLabel})`);
    return true;
  }
  fs.rmSync(part, { force: true });
  missing.add(file);
  log(`[static] missing ${route.originHost}${route.rel} (proxy ${proxyLabel} answered ${got.status})`);
  return false;
}

// One GET through the proxy with curl. The proxy goes in the child's environment, not its arguments, and no
// redirect is followed (a redirect would leave the static route). Only http and https are allowed.
function curlVia(url, out, proxy) {
  const env = { ...process.env, http_proxy: proxy, https_proxy: proxy, all_proxy: proxy,
    HTTP_PROXY: proxy, HTTPS_PROXY: proxy, ALL_PROXY: proxy, no_proxy: "", NO_PROXY: "" };
  const args = ["-sS", "-m", "60", "--proto", "=http,https", "--max-redirs", "0", "-o", out,
    "-w", "%{http_code}\n%{content_type}\n%{size_download}", url];
  return new Promise((resolve) => {
    const child = spawn("curl", args, { env, stdio: ["ignore", "pipe", "ignore"] });
    let text = "";
    child.stdout.on("data", (d) => { text += d; });
    child.on("error", () => resolve({ status: 0, type: "", size: 0 }));
    child.on("close", (code) => {
      const [status, type, size] = text.split("\n");
      resolve({ status: code === 0 ? Number(status) : 0, type: type || "", size: Number(size) || 0 });
    });
  });
}

function contentType(rel) {
  return MIME[path.posix.extname(rel).toLowerCase()] || "application/octet-stream";
}

// the proxy's host:port for the log; a URL with credentials in it is never printed
function hostPort(proxy) {
  try { const u = new URL(proxy); return `${u.hostname}:${u.port}`; } catch (e) { return "(unparsed)"; }
}

// Chromium's host rules: the page's own name and the official hosts we answer resolve to the local listeners
// (the port in a MAP rule is the listener's); every other host, the official CDNs, analytics, consent vendors and the
// real krunker.io hosts included, does not resolve. Static hosts go to the TLS listener, which serves them from the cache.
export function hostRules(cfg) {
  return [`MAP ${cfg.localHost} 127.0.0.1:${cfg.port}`,
    ...Object.keys(cfg.hostRoutes).map((h) => `MAP ${h} 127.0.0.1:${cfg.tlsPort}`),
    ...Object.keys(cfg.staticHosts || {}).map((h) => `MAP ${h} 127.0.0.1:${cfg.tlsPort}`),
    ...(cfg.bridge ? [`MAP ${cfg.bridge.host} 127.0.0.1:${cfg.tlsPort}`] : []),
    "MAP * ~NOTFOUND"];
}
