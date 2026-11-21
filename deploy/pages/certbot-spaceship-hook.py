#!/usr/bin/env python3
"""certbot DNS-01 hooks for the krunk.io zone at Spaceship (OWNED_HOST.md "Certificate").

Installed on divide's edge relay CT 201 as /etc/letsencrypt/kn-spaceship-hook.py and named by
certbot's --manual-auth-hook / --manual-cleanup-hook, so the renewal config keeps them and
certbot.timer renews without a person:

  certbot certonly --manual --preferred-challenges dns --cert-name krunk.io -d krunk.io -d www.krunk.io \
      --manual-auth-hook '/etc/letsencrypt/kn-spaceship-hook.py auth' \
      --manual-cleanup-hook '/etc/letsencrypt/kn-spaceship-hook.py cleanup' \
      --deploy-hook 'systemctl reload openresty'

The API key and secret come from /etc/letsencrypt/kn-spaceship.env (root 0600, lines
SPACESHIP_API_KEY=... and SPACESHIP_API_SECRET=...); they are never printed. Only TXT records
named _acme-challenge[.<sub>] are ever written or deleted. API: https://docs.spaceship.dev/
(PUT /v1/dns/records/{domain} upserts items, DELETE removes the listed records; 204 on success).
Standard library only.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

ZONE = "krunk.io"
API = "https://spaceship.dev/api/v1/dns/records/" + ZONE
ENV_FILE = os.environ.get("KN_SPACESHIP_ENV", "/etc/letsencrypt/kn-spaceship.env")
AUTHORITATIVE = ("launch1.spaceship.net", "launch2.spaceship.net")
TTL = 60


def creds():
    out = {}
    with open(ENV_FILE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.removeprefix("export ").strip()] = v.strip().strip("\"'")
    return out["SPACESHIP_API_KEY"], out["SPACESHIP_API_SECRET"]


def call(method, body):
    key, secret = creds()
    req = urllib.request.Request(API, method=method, data=json.dumps(body).encode(),
                                 headers={"X-API-Key": key, "X-API-Secret": secret, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status
    except urllib.error.HTTPError as e:  # the body may echo the request, never the credentials
        sys.exit("spaceship %s: HTTP %d %s" % (method, e.code, e.read()[:300].decode("utf-8", "replace")))


def record_name(domain):
    domain = domain.rstrip(".").lower()
    if domain != ZONE and not domain.endswith("." + ZONE):
        sys.exit("%s is not in %s" % (domain, ZONE))
    sub = domain[: -len(ZONE)].rstrip(".")
    return "_acme-challenge" + ("." + sub if sub else "")


def visible(fqdn, value):
    """Every authoritative server answers the TXT value."""
    for ns in AUTHORITATIVE:
        r = subprocess.run(["dig", "+short", "+norecurse", "TXT", fqdn, "@" + ns], capture_output=True, text=True, timeout=15)
        if ('"%s"' % value) not in r.stdout:
            return False
    return True


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("auth", "cleanup"):
        sys.exit("usage: kn-spaceship-hook.py auth|cleanup (run by certbot)")
    domain, value = os.environ["CERTBOT_DOMAIN"], os.environ["CERTBOT_VALIDATION"]
    name = record_name(domain)
    rec = {"type": "TXT", "name": name, "value": value}
    if sys.argv[1] == "cleanup":
        call("DELETE", [rec])
        return
    # force: add beside an existing TXT of the same name (www and the apex can be validated at once)
    status = call("PUT", {"force": True, "items": [dict(rec, ttl=TTL)]})
    if status != 204:
        sys.exit("spaceship PUT: HTTP %d" % status)
    fqdn = name + "." + ZONE
    for _ in range(60):  # up to 10 minutes for both authoritative servers
        if visible(fqdn, value):
            time.sleep(10)
            return
        time.sleep(10)
    sys.exit("%s TXT never appeared on %s" % (fqdn, ", ".join(AUTHORITATIVE)))


if __name__ == "__main__":
    main()
