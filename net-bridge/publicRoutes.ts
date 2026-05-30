"use strict";

import type { IncomingMessage, ServerResponse } from "http";

type RuntimeStats = {
  sessions: number;
  nativeConnected: number;
};

type PublicRouteContext = {
  getStats?: () => RuntimeStats;
};

type LobbyTuple = [
  lobbyId: string,
  regionId: string,
  players: number,
  maxPlayers: number,
  meta?: Record<string, unknown>,
];

const JSON_HEADERS = Object.freeze({
  "Content-Type": "application/json",
  "Cache-Control": "no-cache, no-store, must-revalidate",
  Pragma: "no-cache",
  Expires: "0",
});

const DEFAULT_HOST = "127.0.0.1";
const DEFAULT_PORT = 8080;
const LOBBY_ID = process.env.KRUNK_NATIVE_LOBBY_ID || "KN:local";
const REGION_ID = process.env.KRUNK_NATIVE_REGION || "local";
const MAX_PLAYERS = Number.parseInt(process.env.KRUNK_NATIVE_MAX_PLAYERS || "8", 10) || 8;

function makeId(length: number): string {
  const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  let out = "";
  for (let i = 0; i < length; i++) {
    out += chars[Math.floor(Math.random() * chars.length)];
  }
  return out;
}

function writeJson(res: ServerResponse, status: number, body: unknown): boolean {
  res.writeHead(status, JSON_HEADERS);
  res.end(JSON.stringify(body));
  return true;
}

function routeUrl(req: IncomingMessage): URL {
  return new URL(req.url || "/", "http://localhost");
}

function requestEndpoint(req: IncomingMessage): { host: string; port: number } {
  const headerHost = typeof req.headers.host === "string" ? req.headers.host : "";
  let parsedHost = DEFAULT_HOST;
  let parsedPort = DEFAULT_PORT;

  if (headerHost) {
    try {
      const base = new URL(`http://${headerHost}`);
      parsedHost = base.hostname || parsedHost;
      parsedPort = Number.parseInt(base.port || "", 10) || parsedPort;
    } catch (_err) {
      const [host, port] = headerHost.split(":", 2);
      parsedHost = host || parsedHost;
      parsedPort = Number.parseInt(port || "", 10) || parsedPort;
    }
  }

  return {
    host: process.env.KRUNK_NATIVE_PUBLIC_HOST || process.env.MM_PUBLIC_HOST || parsedHost,
    port:
      Number.parseInt(
        process.env.KRUNK_NATIVE_PUBLIC_PORT ||
          process.env.MM_PUBLIC_PORT ||
          process.env.KRUNK_NATIVE_BRIDGE_PORT ||
          process.env.MM_PORT ||
          "",
        10,
      ) || parsedPort,
  };
}

function stats(ctx: PublicRouteContext): RuntimeStats {
  return ctx.getStats?.() || { sessions: 0, nativeConnected: 0 };
}

function buildLobbyTuple(ctx: PublicRouteContext): LobbyTuple {
  const current = stats(ctx).nativeConnected;
  return [
    LOBBY_ID,
    REGION_ID,
    current,
    MAX_PLAYERS,
    {
      map: "KrunkNative",
      mode: "FFA",
      custom: true,
      native: true,
    },
  ];
}

function emptyPagedResponse(index: number, pos = 0) {
  return {
    data: [],
    index,
    pos,
    error: null,
  };
}

export function handle(
  req: IncomingMessage,
  res: ServerResponse,
  ctx: PublicRouteContext = {},
): boolean {
  const url = routeUrl(req);
  const pathname = url.pathname || "";
  const params = url.searchParams;

  switch (pathname) {
    case "/ping":
      return writeJson(res, 200, { success: "true" });

    case "/mm/seek-game": {
      const endpoint = requestEndpoint(req);
      return writeJson(res, 200, {
        changeReason: null,
        gameId: LOBBY_ID,
        lobbyId: LOBBY_ID,
        host: endpoint.host,
        port: endpoint.port,
        clientId: makeId(30),
      });
    }

    case "/mm/game-list": {
      const tuple = buildLobbyTuple(ctx);
      return writeJson(res, 200, {
        success: true,
        totalPlayerCount: tuple[2],
        games: [tuple],
      });
    }

    case "/mm/game-info":
      return writeJson(res, 200, buildLobbyTuple(ctx));

    case "/mm/ping-list": {
      const endpoint = requestEndpoint(req);
      const value = endpoint.port && endpoint.port !== 443
        ? `${endpoint.host}:${endpoint.port}`
        : endpoint.host;
      return writeJson(res, 200, { [REGION_ID]: value });
    }

    case "/api/maps": {
      const index = Number(params.get("index") || 0);
      const pos = Number(params.get("pos") || 0);
      return writeJson(res, 200, emptyPagedResponse(index, pos));
    }

    case "/api/mods":
    case "/api/assets": {
      const index = Number(params.get("index") || 0);
      return writeJson(res, 200, emptyPagedResponse(index));
    }

    case "/api/search":
      return writeJson(res, 200, { data: [], index: 0, error: null });

    case "/social/player-count":
    case "/api/player-count":
      return writeJson(res, 200, { playerCount: stats(ctx).nativeConnected });

    case "/api/streams": {
      const index = Number(params.get("index"));
      return writeJson(
        res,
        200,
        index === 1
          ? { data: { streams: [], promo: false, dropCampaigns: [] }, n: null }
          : {
              data: { streams: [], promo: false, dropCampaigns: [] },
              c: 0,
              d: [],
              n: null,
            },
      );
    }

    case "/prices": {
      const id = params.get("id");
      if (id != null) return writeJson(res, 200, 0);
      return writeJson(res, 200, {});
    }

    default:
      return false;
  }
}

