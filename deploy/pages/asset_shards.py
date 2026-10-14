#!/usr/bin/env python3
"""The asset CDN's split over Pages projects: deploy/asset-shards.json (deploy/pages/README.md
"Asset CDN"). Loads and checks the file and derives from it what each consumer needs:

    route(path)        the shard a path below assets/ is deployed to (None: excluded, kru only)
    redirects()        the default shard's _redirects: the other shards' prefixes (301), kru_only
                       and exclude (302 to download), longest prefix first
    page_split()       kn-config.json "asset_split" for loader.js (per base URL: prefix -> URL)
    mirrors()          the launcher manifest's assets.mirrors with only/skip filters (kn-updates.py
                       has its own copy of this: it runs on kru without the repository)

usage: asset_shards.py [--config FILE] check | env | self-test
       env: shell assignments (KN_SHARDS, KN_SHARD_<name>_{PROJECT,URL,PAGES_DEV,PREFIXES}, KN_SHARD_DEFAULT,
            KN_SHARDS_LIVE, KN_ASSET_DL_URL_DEFAULT) for assets-cdn.sh and assets-cdn.yml.
Standard library only.
"""
import argparse
import json
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(os.path.dirname(HERE), "asset-shards.json")
NAME_RE = re.compile(r"^[a-z][a-z0-9]{0,15}$")
PROJECT_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,57}$")
URL_RE = re.compile(r"^https?://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._-]+)*/$")
PREFIX_RE = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*/?$")
SITE_PATH_RE = re.compile(r"^/[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*/$")
PAGES_MAX_DYNAMIC_REDIRECTS = 100


class ShardError(ValueError):
    pass


def _prefixes(value, what):
    if not isinstance(value, list):
        raise ShardError("%s: not a list" % what)
    for p in value:
        if not isinstance(p, str) or not PREFIX_RE.match(p) or ".." in p.split("/"):
            raise ShardError("%s: %r is not a path prefix below assets/" % (what, p))
    return list(value)


class Shards:
    def __init__(self, cfg):
        if not isinstance(cfg, dict):
            raise ShardError("not a JSON object")
        self.live = cfg.get("live") is True
        self.max_files = int(cfg.get("max_files", 16000))
        self.download = cfg.get("download", "")
        if not isinstance(self.download, str) or not URL_RE.match(self.download):
            raise ShardError("download: %r is not https://host/" % self.download)
        self.kru_only = cfg.get("kru_only", [])
        for p in self.kru_only:
            if not isinstance(p, str) or not SITE_PATH_RE.match(p):
                raise ShardError("kru_only: %r is not a site path like /updates/" % p)
        self.exclude = _prefixes(cfg.get("exclude", []), "exclude")
        self.shards = cfg.get("shards")
        if not isinstance(self.shards, list) or not self.shards:
            raise ShardError("shards: none")
        names, seen_prefixes = set(), {}
        self.default = None
        for s in self.shards:
            name = s.get("name", "")
            if not NAME_RE.match(name) or name in names:
                raise ShardError("shards: bad or repeated name %r" % name)
            names.add(name)
            if not PROJECT_RE.match(s.get("project", "")):
                raise ShardError("shard %s: bad project %r" % (name, s.get("project")))
            for k in ("url", "pages_dev"):
                if not isinstance(s.get(k), str) or not URL_RE.match(s[k]):
                    raise ShardError("shard %s: %s %r is not https://host/" % (name, k, s.get(k)))
            if "prefixes" in s:
                s["prefixes"] = _prefixes(s["prefixes"], "shard %s prefixes" % name)
                if not s["prefixes"]:
                    raise ShardError("shard %s: empty prefixes (leave the key out for the default shard)" % name)
                for p in s["prefixes"]:
                    if p in seen_prefixes:
                        raise ShardError("prefix %s in shards %s and %s" % (p, seen_prefixes[p], name))
                    seen_prefixes[p] = name
            elif self.default is None:
                self.default = s
            else:
                raise ShardError("shards %s and %s both lack prefixes: only one default" % (self.default["name"], name))
        if self.default is None:
            raise ShardError("no default shard (one without prefixes)")
        self.others = [s for s in self.shards if s is not self.default]
        # longest prefix first: a more specific rule wins (route, _redirects, the clients)
        self._routes = sorted([(p, s) for s in self.others for p in s["prefixes"]] + [(p, None) for p in self.exclude],
                              key=lambda r: (-len(r[0]), r[0]))
        if len(self._routes) + len(self.kru_only) > PAGES_MAX_DYNAMIC_REDIRECTS:
            raise ShardError("%d redirect rules: Pages takes %d dynamic ones" % (
                len(self._routes) + len(self.kru_only), PAGES_MAX_DYNAMIC_REDIRECTS))

    def by_name(self, name):
        return next((s for s in self.shards if s["name"] == name), None)

    def route(self, path):
        """The shard (dict) a path below assets/ lives on; None when excluded (kru only)."""
        for prefix, shard in self._routes:
            if path.startswith(prefix):
                return shard
        return self.default

    def redirects(self):
        """The default shard's _redirects."""
        lines = ["# deploy/pages/asset_shards.py from deploy/asset-shards.json: old clients ask this host",
                 "# (assets.krunk.io) for everything; the other shards' directories are a 301 away, the files",
                 "# only kru serves (the updates store, exclude) a 302."]
        for prefix, shard in self._routes:
            target = shard["url"] if shard else self.download
            lines.append("/assets/%s* %sassets/%s:splat %d" % (prefix, target, prefix, 301 if shard else 302))
        for path in self.kru_only:
            lines.append("%s* %s%s:splat 302" % (path, self.download, path[1:]))
        return "\n".join(lines) + "\n"

    def _split(self, key):
        return {"base": self.default[key],
                "routes": [[p, s[key] if s else self.download] for p, s in self._routes]}

    def page_split(self):
        """kn-config.json asset_split: the custom domains once live, the pages.dev names always."""
        return ([self._split("url")] if self.live else []) + [self._split("pages_dev")]

    def page_origins(self):
        """kn-config.json asset_origins this file adds: dl, <shard>-dev, and <shard> once live."""
        out = {"dl": self.download}
        for s in self.shards:
            out[s["name"] + "-dev"] = s["pages_dev"]
            if self.live:
                out[s["name"]] = s["url"]
        return out

    def mirrors(self, live=None):
        """The launcher manifest's assets.mirrors (by path, with only/skip filters)."""
        key = "url" if (self.live if live is None else live) else "pages_dev"
        out = [{"url": self.default[key] + "assets/", "layout": "path",
                "skip": sorted({p for p, _s in self._routes})}]
        for s in self.others:
            out.append({"url": s[key] + "assets/", "layout": "path", "only": list(s["prefixes"])})
        return out


def load(path=None):
    path = path or CONFIG
    try:
        with open(path) as f:
            return Shards(json.load(f))
    except (OSError, ValueError) as e:
        raise ShardError("%s: %s" % (path, e))


def shell_env(sh):
    out = {"KN_SHARDS": " ".join(s["name"] for s in sh.shards), "KN_SHARD_DEFAULT": sh.default["name"],
           "KN_SHARDS_LIVE": "1" if sh.live else "0", "KN_ASSET_DL_URL_DEFAULT": sh.download,
           "KN_SHARDS_MAX_FILES": str(sh.max_files)}
    for s in sh.shards:
        n = s["name"]
        out["KN_SHARD_%s_PROJECT" % n] = s["project"]
        out["KN_SHARD_%s_URL" % n] = s["url"]
        out["KN_SHARD_%s_PAGES_DEV" % n] = s["pages_dev"]
        out["KN_SHARD_%s_PREFIXES" % n] = " ".join(s.get("prefixes", []))
    return "".join("%s=%s\n" % (k, shlex.quote(v)) for k, v in out.items())


def self_test():
    fails = []

    def check(cond, what):
        print(("  ok    " if cond else "  FAIL  ") + what)
        if not cond:
            fails.append(what)

    base = {"live": False, "download": "https://dl.example/", "kru_only": ["/updates/"], "exclude": ["textures/previews/big/"],
            "shards": [{"name": "a", "project": "p-a", "url": "https://a.example/", "pages_dev": "https://p-a.pages.dev/"},
                       {"name": "b", "project": "p-b", "url": "https://b.example/", "pages_dev": "https://p-b.pages.dev/",
                        "prefixes": ["textures/previews/", "textures/hats/"]}]}
    sh = Shards(json.loads(json.dumps(base)))
    check(sh.route("textures/previews/x.png")["name"] == "b", "a prefix routes to its shard")
    check(sh.route("textures/weapons/skins/w_1.png")["name"] == "a", "anything else to the default shard")
    check(sh.route("textures/previews/big/x.png") is None, "a longer exclude prefix wins over the shard's")
    check(sh.route("textures/hatsX/y.png")["name"] == "a", "prefixes match whole directory names")
    red = sh.redirects().splitlines()
    rules = [l for l in red if not l.startswith("#")]
    check(rules[0] == "/assets/textures/previews/big/* https://dl.example/assets/textures/previews/big/:splat 302",
          "exclude first, 302 to download (%s)" % rules[0])
    check("/assets/textures/previews/* https://b.example/assets/textures/previews/:splat 301" in rules, "shard prefix: 301")
    check(rules[-1] == "/updates/* https://dl.example/updates/:splat 302", "kru_only: 302 to download")
    split = sh.page_split()
    check(len(split) == 1 and split[0]["base"] == "https://p-a.pages.dev/", "not live: only the pages.dev split")
    check(["textures/hats/", "https://p-b.pages.dev/"] in split[0]["routes"], "pages.dev split routes to the pages.dev shard")
    check("b" not in sh.page_origins() and sh.page_origins()["b-dev"] == "https://p-b.pages.dev/", "origins: -dev names only")
    m = sh.mirrors()
    check(m[0]["url"] == "https://p-a.pages.dev/assets/" and "textures/hats/" in m[0]["skip"], "mirror a skips b's prefixes")
    check(m[1] == {"url": "https://p-b.pages.dev/assets/", "layout": "path", "only": ["textures/previews/", "textures/hats/"]},
          "mirror b only its prefixes")
    live = json.loads(json.dumps(base))
    live["live"] = True
    sh = Shards(live)
    check(sh.page_split()[0]["base"] == "https://a.example/" and sh.mirrors()[1]["url"] == "https://b.example/assets/",
          "live: the custom domains")
    for bad, what in ((lambda c: c["shards"][1].pop("prefixes"), "two default shards"),
                      (lambda c: c["shards"][1].update(prefixes=["../x/"]), "a prefix with .."),
                      (lambda c: c["shards"][1].update(prefixes=["textures/hats/", "textures/hats/"]), "a repeated prefix"),
                      (lambda c: c.update(download="ftp://x/"), "a download URL that is not http(s)")):
        c = json.loads(json.dumps(base))
        bad(c)
        try:
            Shards(c)
            check(False, "refused: " + what)
        except ShardError:
            check(True, "refused: " + what)
    try:
        real = load()
        check(True, "deploy/asset-shards.json loads (%d shards, default %s)" % (len(real.shards), real.default["name"]))
    except ShardError as e:
        check(False, "deploy/asset-shards.json: %s" % e)
        real = None
    # kn-updates.py (on kru, without this module) derives the same manifest mirrors
    import importlib.util
    import tempfile
    spec = importlib.util.spec_from_file_location(
        "kn_updates", os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "release", "kn-updates.py"))
    kn_updates = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kn_updates)
    for cfg in (base, live) + ((json.load(open(CONFIG)),) if real else ()):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(cfg, f)
        try:
            same = kn_updates.shard_mirrors(f.name) == Shards(json.loads(json.dumps(cfg))).mirrors()
        finally:
            os.unlink(f.name)
        check(same, "kn-updates.py shard_mirrors == mirrors() (live %s)" % cfg.get("live"))
    print("asset_shards self-test: %s" % ("FAILED: " + "; ".join(fails) if fails else "all passed"))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=CONFIG)
    ap.add_argument("cmd", choices=("check", "env", "self-test"))
    a = ap.parse_args()
    if a.cmd == "self-test":
        return self_test()
    try:
        sh = load(a.config)
    except ShardError as e:
        sys.exit("asset_shards: %s" % e)
    if a.cmd == "env":
        sys.stdout.write(shell_env(sh))
    else:
        print("asset_shards: %s ok: %s, default %s, download %s, live %s" % (
            a.config, ", ".join("%s=%s (%s)" % (s["name"], s["url"], s["project"]) for s in sh.shards),
            sh.default["name"], sh.download, sh.live))
    return 0


if __name__ == "__main__":
    sys.exit(main())
