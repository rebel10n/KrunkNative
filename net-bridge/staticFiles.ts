"use strict";

import fs from "fs";
import path from "path";
import type { IncomingMessage, ServerResponse } from "http";

type ServeOptions = {
  root?: string;
  /**
   * URL path prefixes (e.g. "/assets/") whose files are immutable game
   * content and may be cached for a long time. Everything else is served
   * with `no-cache` so a rebuilt client.js/client.wasm is picked up at once.
   */
  longCachePrefixes?: string[];
  /** Extra headers added to every successful response (e.g. COOP/COEP). */
  extraHeaders?: Record<string, string>;
};

type FsErr = NodeJS.ErrnoException & { statusCode?: number };

const LOCAL_PUBLIC_DIR = path.resolve(__dirname, "..", "public");
const V31_PUBLIC_DIR = path.resolve(__dirname, "..", "..", "Krunker_v3.1.1", "public");

export const PUBLIC_DIR = path.resolve(
  process.env.KRUNK_NATIVE_PUBLIC_DIR ||
    (fs.existsSync(LOCAL_PUBLIC_DIR) ? LOCAL_PUBLIC_DIR : V31_PUBLIC_DIR),
);

export function contentTypeFor(filePath: string): string {
  const ext = path.extname(filePath).toLowerCase();
  switch (ext) {
    case ".html":
      return "text/html; charset=utf-8";
    case ".js":
    case ".mjs":
      return "application/javascript; charset=utf-8";
    case ".css":
      return "text/css; charset=utf-8";
    case ".json":
    case ".map":
      return "application/json; charset=utf-8";
    case ".wasm":
      return "application/wasm";
    case ".data":
      // Emscripten --preload-file bundle.
      return "application/octet-stream";
    case ".obj":
    case ".mtl":
    case ".txt":
      return "text/plain; charset=utf-8";
    case ".glsl":
    case ".vert":
    case ".frag":
      return "text/plain; charset=utf-8";
    case ".ogg":
      return "audio/ogg";
    case ".png":
      return "image/png";
    case ".jpg":
    case ".jpeg":
      return "image/jpeg";
    case ".gif":
      return "image/gif";
    case ".webp":
      return "image/webp";
    case ".svg":
      return "image/svg+xml";
    case ".ico":
      return "image/x-icon";
    case ".wav":
      return "audio/wav";
    case ".mp3":
      return "audio/mpeg";
    case ".woff":
      return "font/woff";
    case ".woff2":
      return "font/woff2";
    case ".ttf":
      return "font/ttf";
    default:
      return "application/octet-stream";
  }
}

const NO_CACHE_HEADERS = Object.freeze({
  "Cache-Control": "no-cache, no-store, must-revalidate",
  Pragma: "no-cache",
  Expires: "0",
});

const LONG_CACHE_HEADERS = Object.freeze({
  "Cache-Control": "public, max-age=31536000, immutable",
});

function cacheHeadersFor(
  urlPath: string,
  prefixes: string[] | undefined,
): Record<string, string> {
  if (prefixes) {
    for (const prefix of prefixes) {
      if (urlPath.startsWith(prefix)) return { ...LONG_CACHE_HEADERS };
    }
  }
  return { ...NO_CACHE_HEADERS };
}

export async function serve(
  req: IncomingMessage,
  res: ServerResponse,
  opts: ServeOptions = {},
): Promise<boolean> {
  if (req.method !== "GET" && req.method !== "HEAD") return false;
  const root = opts.root || PUBLIC_DIR;

  let requestPath: string;
  try {
    requestPath = new URL(req.url || "/", "http://localhost").pathname;
  } catch (_err) {
    res.writeHead(400);
    res.end("Invalid URL");
    return true;
  }

  let normalized: string;
  try {
    normalized = decodeURIComponent(requestPath);
  } catch (_err) {
    res.writeHead(400);
    res.end("Invalid URL");
    return true;
  }

  const resolvedPath = path.resolve(root, `.${normalized}`);
  const resolvedRoot = path.resolve(root);
  const publicRoot = `${resolvedRoot}${path.sep}`;
  if (
    !(resolvedPath + path.sep).startsWith(publicRoot) &&
    resolvedPath !== resolvedRoot
  ) {
    res.writeHead(403);
    res.end("Forbidden");
    return true;
  }

  let filePath = resolvedPath;
  if (normalized === "/") filePath = path.join(root, "index.html");

  try {
    // fs.stat follows symlinks, so a `dist/web/assets -> ../../assets` link
    // is served transparently. The path check above is done on the logical
    // (pre-realpath) path, which is what keeps `..` escapes out.
    let stat = await fs.promises.stat(filePath);
    if (stat.isDirectory()) {
      // `/ui/` -> `/ui/index.html`
      filePath = path.join(filePath, "index.html");
      stat = await fs.promises.stat(filePath);
    }
    if (!stat.isFile()) {
      res.writeHead(404);
      res.end("Not Found");
      return true;
    }

    res.writeHead(200, {
      "Content-Type": contentTypeFor(filePath),
      "Content-Length": stat.size,
      ...cacheHeadersFor(normalized, opts.longCachePrefixes),
      ...(opts.extraHeaders || {}),
    });

    if (req.method === "HEAD") {
      res.end();
      return true;
    }

    const stream = fs.createReadStream(filePath);
    stream.on("error", (err) => {
      console.error("[staticFiles] stream error:", err);
      if (!res.headersSent) res.writeHead(500);
      res.end("Internal Server Error");
    });
    stream.pipe(res);
    return true;
  } catch (err) {
    const fsErr = err as FsErr;
    if (fsErr && (fsErr.code === "ENOENT" || fsErr.code === "ENOTDIR")) {
      res.writeHead(404);
      res.end("Not Found");
      return true;
    }
    console.error("[staticFiles] serve error:", fsErr);
    res.writeHead(fsErr?.statusCode || 500);
    res.end("Internal Server Error");
    return true;
  }
}
