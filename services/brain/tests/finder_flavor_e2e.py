#!/usr/bin/env python3
"""End-to-end test of the Krunker Strike flavor pool in the server finder (services/brain/finder.c,
finder_table.c): a base game server and a Krunker Strike server both register and are verified, and

  - a base client (GET /servers, no ?flavor) sees only the base server, never the Strike one, and
    the base entry is byte-identical to before Krunker Strike (no "flavor" key) -> base listing
    unchanged;
  - a Strike client (GET /servers?flavor=strike) sees only the Strike server, carrying
    flavor:"strike";
  - ?flavor=all lists both (dashboards);
  - ?exclude=host:port,... (a seek's current / recent servers) leaves them out while another server
    matches, and lists them when none does.

No database: kn_brain --role finder --no-db. Ports come from server/tests/kn_proto.free_ports();
the fake game servers are small UDP KNINFO responders here, so nothing links libpq and no real
server_bin is needed. Nothing touches a display.

  services/brain/tests/finder_flavor_e2e.py <build dir>

Exit 77 (CTest skip) when kn_brain is missing.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

KNINFO_PREFIX = b"\xff\xff\xff\xff"


def log(msg):
    print("[finder-flavor] " + msg, flush=True)


class FakeServer:
    """A minimal KNINFO responder: answers the finder's UDP poll with a server-info JSON reply that
    carries the given flavor (None = the base game, so the reply omits the field exactly like a base
    server_bin)."""

    def __init__(self, name, query_port, flavor):
        self.name = name
        self.query_port = query_port
        self.flavor = flavor
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", query_port))
        self.sock.settimeout(0.2)
        self.stop = False
        self.polls = 0
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _reply(self, nonce):
        o = {
            "v": 1, "game": "krunknative", "name": self.name, "map": "Sandstorm",
            "mode": "ffa", "modeId": 0, "players": 1, "max": 8, "tags": [],
            "community": "", "online_mode": False, "password": False, "region": "eu",
            "version": "1", "port": self.query_port, "ruleset": "v10", "nonce": nonce,
        }
        if self.flavor:
            o["flavor"] = self.flavor
        return KNINFO_PREFIX + b"KNinfo\x00" + json.dumps(o).encode()

    def _run(self):
        while not self.stop:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data.startswith(KNINFO_PREFIX + b"KNINFO\x00"):
                continue
            nonce = data[11:].decode(errors="replace")
            self.polls += 1
            try:
                self.sock.sendto(self._reply(nonce), addr)
            except OSError:
                pass

    def close(self):
        self.stop = True
        self.thread.join(timeout=1)
        self.sock.close()


def http_json(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())


def heartbeat(finder_url, port, query_port):
    body = json.dumps({"host": "127.0.0.1", "port": port, "query_port": query_port}).encode()
    req = urllib.request.Request(finder_url + "/servers/heartbeat", data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode())


def names(servers):
    return sorted(s.get("name", "") for s in servers)


def wait_for(fn, timeout=15, interval=0.25):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(interval)
    return False


def main():
    if len(sys.argv) < 2:
        print("usage: finder_flavor_e2e.py <build dir>", file=sys.stderr)
        return 2
    build = sys.argv[1]
    brain = os.path.join(build, "services", "brain", "kn_brain")
    if not os.path.exists(brain):
        log("kn_brain missing: skipped")
        return 77

    work = tempfile.mkdtemp(prefix="kn-finder-flavor.")
    base_q, strike_q = free_ports(2)
    env = dict(os.environ)
    env["KN_LOG_LEVEL"] = "info"
    env["XDG_CONFIG_HOME"] = work
    logpath = os.path.join(work, "finder.log")
    logf = open(logpath, "w")
    proc = subprocess.Popen(
        [brain, "--role", "finder", "--no-db", "--brain.port=0", "--brain.id=finder",
         "--finder.poll_interval_s=1"],
        stdout=logf, stderr=subprocess.STDOUT, env=env)
    base = strike = None
    fails = 0

    def fail(msg):
        nonlocal fails
        fails += 1
        log("FAIL " + msg)

    def ok(msg):
        log("ok   " + msg)

    try:
        # read the ephemeral HTTP port from the log
        port = None
        end = time.time() + 10
        while time.time() < end and port is None:
            if proc.poll() is not None:
                break
            try:
                text = open(logpath).read()
            except OSError:
                text = ""
            m = re.search(r"listening on http://[^:]+:(\d+)", text)
            if m:
                port = int(m.group(1))
            else:
                time.sleep(0.1)
        if port is None:
            fail("kn_brain did not start")
            return 1
        finder_url = "http://127.0.0.1:%d" % port
        log("finder at %s (base q=%d, strike q=%d)" % (finder_url, base_q, strike_q))

        base = FakeServer("Base Srv", base_q, None)
        strike = FakeServer("Strike Srv", strike_q, "strike")

        # register both; the finder polls them over UDP and verifies the KNINFO reply
        heartbeat(finder_url, base_q, base_q)
        heartbeat(finder_url, strike_q, strike_q)

        def both_listed():
            srv = http_json(finder_url + "/servers?flavor=all").get("servers", [])
            return names(srv) == ["Base Srv", "Strike Srv"]

        if not wait_for(both_listed):
            fail("both servers never verified (base polls=%d, strike polls=%d)"
                 % (base.polls, strike.polls))
            return 1
        ok("both servers verified and listed under ?flavor=all")

        # ?exclude= (a seek leaving its current / recent servers, client recent_lobbies.h): never
        # listed, so a seek is not sent back to a server it leaves
        ex_one = http_json(finder_url + "/servers?flavor=all&exclude=127.0.0.1:%d" % base_q).get("servers", [])
        if names(ex_one) == ["Strike Srv"]:
            ok("?exclude= leaves the excluded server out")
        else:
            fail("?exclude=<base> listing is %s, expected ['Strike Srv']" % names(ex_one))
        ex_all = http_json(finder_url + "/servers?flavor=strike&exclude=127.0.0.1:%d,127.0.0.1:%d"
                           % (strike_q, base_q)).get("servers", [])
        if names(ex_all) == []:
            ok("?exclude= lists nothing when every matching server is excluded")
        else:
            fail("?exclude=<all> strike listing is %s, expected []" % names(ex_all))

        # base client: no ?flavor -> only the base server, and the entry carries no "flavor" key
        base_list = http_json(finder_url + "/servers").get("servers", [])
        if names(base_list) == ["Base Srv"]:
            ok("base client (no flavor) sees only the base server")
        else:
            fail("base client listing is %s, expected ['Base Srv']" % names(base_list))
        base_entry = next((s for s in base_list if s["name"] == "Base Srv"), {})
        if "flavor" not in base_entry:
            ok("base entry has no 'flavor' key (base listing unchanged)")
        else:
            fail("base entry unexpectedly carries flavor=%r" % base_entry.get("flavor"))

        # ?flavor=base is identical to no flavor
        base_list2 = http_json(finder_url + "/servers?flavor=base").get("servers", [])
        if names(base_list2) == ["Base Srv"]:
            ok("?flavor=base matches the implicit base view")
        else:
            fail("?flavor=base listing is %s" % names(base_list2))

        # Strike client: only the Strike server, carrying flavor:"strike"
        strike_list = http_json(finder_url + "/servers?flavor=strike").get("servers", [])
        if names(strike_list) == ["Strike Srv"]:
            ok("strike client sees only the Strike server")
        else:
            fail("strike client listing is %s, expected ['Strike Srv']" % names(strike_list))
        strike_entry = next((s for s in strike_list if s["name"] == "Strike Srv"), {})
        if strike_entry.get("flavor") == "strike":
            ok("strike entry carries flavor='strike'")
        else:
            fail("strike entry flavor=%r, expected 'strike'" % strike_entry.get("flavor"))

        # base listing unchanged by the existence of the Strike server: the finder entry is identical
        # once the volatile per-poll fields (ping_ms / last_seen) and the async release annotations
        # (outdated / incompatible, added by releases.c as release metadata loads) are set aside.
        def stable(entry):
            volatile = ("ping_ms", "last_seen", "outdated", "incompatible")
            return {k: v for k, v in entry.items() if k not in volatile}

        strike.close()
        strike = None

        def strike_gone():
            return names(http_json(finder_url + "/servers?flavor=all").get("servers", [])) == ["Base Srv"]

        wait_for(strike_gone, timeout=10)
        alone = http_json(finder_url + "/servers").get("servers", [])
        alone_entry = next((s for s in alone if s["name"] == "Base Srv"), {})
        if stable(alone_entry) == stable(base_entry):
            ok("base entry identical with and without a Strike server present")
        else:
            fail("base entry changed: %s vs %s" % (stable(base_entry), stable(alone_entry)))

    finally:
        if base:
            base.close()
        if strike:
            strike.close()
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        logf.close()

    if fails:
        log("%d check(s) failed; log kept at %s" % (fails, logpath))
        return 1
    log("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
