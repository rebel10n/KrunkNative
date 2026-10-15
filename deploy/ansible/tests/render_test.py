#!/usr/bin/env python3
"""Renders the edge game-server deploy (role kn_gameserver, the edge firewall drop-in, net-bridge's
allowlist) with ansible-playbook against the real inventory (deploy/ansible/inventory/hosts.yml) and
given host facts, entirely on this machine: connection local, facts set by a stub module (no
setup), every file written under a temp dir. Nothing connects to a host.

  python3 -I deploy/ansible/tests/render_test.py        (make deploy-test)

Checks the Krunker Strike instances (tasks/strike.yml, strike_plan.yml) and that the Box Office ones
are what they were:
  - each edge plans its Strike instances (2 with 8+ vcpus, else 1) and starts only what its memory
    holds next to the Box Office servers; a tight host starts none;
  - strike-<n>.cfg: --flavor strike's config (flavor strike, the Strike rotation, a STRIKE node id,
    the Strike port, the <edge>-1 name, the same coordinator link + secrets, its own state / stop
    reason paths); kn-strike@.service: --flavor strike, MemoryHigh / MemoryMax, a lower CPUWeight;
  - server-<n>.cfg / kn-gameserver@.service carry nothing of Strike, one per vcpu - 1 as before;
  - ports never overlap; the firewall drop-in opens both ranges; net-bridge allows the Strike span;
  - ansible-playbook --syntax-check of edges.yml and central.yml.
Prints PASS/FAIL lines; exit 0 only when every check passed, 0 with "skipped" when ansible-playbook
is not installed.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ANSIBLE = os.path.dirname(HERE)
results = []

# (inventory host, vcpus, MemTotal MB). vcpus as the live instance counts say (README "Edge
# firewall": clara / chitown / ashburn 21015-21017, angeles 21015-21025); memory: a test value.
HOSTS = {
    "com.clara": (4, 3915),
    "com.angeles": (12, 7940),
    "com.chitown": (4, 3915),
    "amph.ashburn": (4, 3915),
}
TIGHT = ("com.clara", 4, 1700)  # 1024 + 3 x 160 = 1504: no room for a 256 MB Strike instance
SECRET = "render-test-internal-secret-0123456789"

PLAYBOOK = """
- name: Render the edges' game servers
  hosts: edges
  gather_facts: false
  vars:
    kn_svc_base: /opt/krunknative-svc
    kn_svc_current: /opt/krunknative-svc/current
    kn_svc_release_id: render-test
    kn_gs_internal_secret: "%(secret)s"
    kn_gs_join_secret: ""
    kn_gameserver_active: true
    out: "%(out)s/{{ inventory_hostname }}"
  tasks:
    - kn_render_facts: {facts: "{{ kn_render_host_facts[inventory_hostname] }}"}
    - ansible.builtin.include_role:
        name: kn_gameserver
        tasks_from: strike_plan.yml
        public: true
    - ansible.builtin.file: {path: "{{ out }}", state: directory, mode: "0755"}
    - ansible.builtin.template: {src: "%(roles)s/kn_gameserver/templates/server.cfg.j2", dest: "{{ out }}/server-{{ n }}.cfg", mode: "0644"}
      loop: "{{ range(1, (kn_gameserver_count | int) + 1) | list }}"
      loop_control: {loop_var: n}
    - ansible.builtin.template: {src: "%(roles)s/kn_gameserver/templates/strike.cfg.j2", dest: "{{ out }}/strike-{{ n }}.cfg", mode: "0644"}
      loop: "{{ range(1, (kn_strike_run | int) + 1) | list }}"
      loop_control: {loop_var: n}
    - ansible.builtin.template: {src: "%(roles)s/kn_gameserver/templates/kn-gameserver@.service.j2", dest: "{{ out }}/kn-gameserver@.service", mode: "0644"}
    - ansible.builtin.template: {src: "%(roles)s/kn_gameserver/templates/kn-strike@.service.j2", dest: "{{ out }}/kn-strike@.service", mode: "0644"}
    - ansible.builtin.template: {src: "%(roles)s/edge_firewall/templates/dropin.nft.j2", dest: "{{ out }}/60-krunknative.nft", mode: "0644"}
      vars: {edge_firewall_dropin: "{{ edge_firewall_dropins[0] }}"}
    - ansible.builtin.copy:
        dest: "{{ out }}/plan.json"
        mode: "0644"
        content: "{{ {'bo': kn_gameserver_count | int, 'strike_planned': kn_strike_planned | int, 'strike': kn_strike_run | int,
                     'active': kn_strike_active | bool, 'mem': kn_strike_mem_total | int, 'bo_ports': kn_gameserver_port_range,
                     'strike_ports': kn_strike_port_range} | to_json }}"

- name: Render net-bridge's allowlist
  hosts: central
  gather_facts: false
  tasks:
    - ansible.builtin.include_role:
        name: kn_bridge
        tasks_from: targets.yml
        public: true
    - ansible.builtin.copy: {dest: "%(out)s/bridge-targets.json", content: "{{ kn_bridge_targets | to_json }}", mode: "0644"}
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def cfg_value(text, key):
    m = re.search(r"^%s\s+(.*)$" % re.escape(key), text, re.M)
    return m.group(1).strip().strip('"') if m else None


def render(work, hosts):
    """Runs the render playbook with facts hosts {name: (vcpus, mem)}; returns the out dir."""
    out = os.path.join(work, "out")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    facts = {host: {"ansible_processor_vcpus": vcpus, "ansible_processor_nproc": vcpus, "ansible_memtotal_mb": mem,
                    "ansible_distribution_release": "trixie", "ansible_distribution_major_version": "13"}
             for host, (vcpus, mem) in hosts.items()}
    with open(os.path.join(work, "facts.json"), "w") as f:
        json.dump({"kn_render_host_facts": facts}, f)
    # the facts a setup run would gather, from a stub module next to the playbook (library/)
    os.makedirs(os.path.join(work, "library"), exist_ok=True)
    with open(os.path.join(work, "library", "kn_render_facts.py"), "w") as f:
        f.write("from ansible.module_utils.basic import AnsibleModule\n"
                "m = AnsibleModule(argument_spec={'facts': {'type': 'dict', 'required': True}})\n"
                "m.exit_json(changed=False, ansible_facts=m.params['facts'])\n")
    play = os.path.join(work, "render.yml")
    with open(play, "w") as f:
        f.write(PLAYBOOK % {"secret": SECRET, "out": out, "roles": os.path.join(ANSIBLE, "roles")})
    # the playbook's dir must hold group_vars (playbook-relative): link the repo's
    for name in ("group_vars", "roles"):
        link = os.path.join(work, name)
        if not os.path.lexists(link):
            os.symlink(os.path.join(ANSIBLE, name), link)
    env = dict(os.environ, ANSIBLE_CONFIG=os.path.join(work, "none.cfg"), ANSIBLE_GATHERING="explicit",
               ANSIBLE_ROLES_PATH=os.path.join(ANSIBLE, "roles"), ANSIBLE_HOST_KEY_CHECKING="false",
               ANSIBLE_LOCAL_TEMP=os.path.join(work, "tmp"), ANSIBLE_REMOTE_TEMP=os.path.join(work, "tmp"),
               ANSIBLE_NOCOLOR="1", ANSIBLE_RETRY_FILES_ENABLED="false")
    limit = ",".join(list(hosts) + ["kru"])
    r = subprocess.run(["ansible-playbook", "-i", os.path.join(ANSIBLE, "inventory", "hosts.yml"), play, "--limit", limit,
                        "-e", "ansible_connection=local", "-e", "ansible_python_interpreter=" + sys.executable,
                        "-e", "@" + os.path.join(work, "facts.json")],
                       cwd=work, env=env, capture_output=True, text=True)
    check("ansible-playbook renders (%s)" % ", ".join(f"{h} {v}vcpu/{m}MB" for h, (v, m) in hosts.items()), r.returncode == 0,
          (r.stdout + r.stderr)[-2500:])
    return out


def check_host(out, host, vcpus, mem):
    d = os.path.join(out, host)
    plan = json.loads(read(os.path.join(d, "plan.json")) or "{}")
    edge = host.split(".", 1)[1]
    bo = max(vcpus - 1, 1)
    planned = 2 if vcpus >= 8 else 1
    fit = max((mem - 1024 - bo * 160) // 256, 0)
    want = min(planned, fit)
    check(f"{edge}: {bo} Box Office instances (vcpus - 1), as before", plan.get("bo") == bo, plan)
    check(f"{edge}: plans {planned} Strike instance(s), starts {want} in {mem} MB", plan.get("strike_planned") == planned and plan.get("strike") == want, plan)
    bo_cfgs = sorted(f for f in os.listdir(d) if f.startswith("server-"))
    st_cfgs = sorted(f for f in os.listdir(d) if f.startswith("strike-"))
    check(f"{edge}: server-1..{bo}.cfg, strike-1..{want}.cfg", len(bo_cfgs) == bo and len(st_cfgs) == want, (bo_cfgs, st_cfgs))
    bo_ports = set()
    for f in bo_cfgs:
        t = read(os.path.join(d, f))
        n = int(re.search(r"(\d+)", f).group(1))
        bo_ports.add(int(cfg_value(t, "port")))
        ok = (cfg_value(t, "node_id") == f"{edge.upper()}{n}" and cfg_value(t, "public_host") == f"{edge}-{n}.krunk.io"
              and cfg_value(t, "port") == str(21015 + n - 1) and "flavor" not in t and "strike" not in t.lower()
              and cfg_value(t, "modes") == "rotation" and cfg_value(t, "max_players") == "8"
              and cfg_value(t, "coordinator_secret") == SECRET and cfg_value(t, "bans_file") == f"/var/lib/krunknative/gs-{n}/bans.json")
        check(f"{edge}: {f} is the Box Office config (no flavor, {edge.upper()}{n}, :{21015 + n - 1})", ok, t)
    st_ports = set()
    for f in st_cfgs:
        t = read(os.path.join(d, f))
        n = int(re.search(r"(\d+)", f).group(1))
        st_ports.add(int(cfg_value(t, "port")))
        ok = (cfg_value(t, "flavor") == "strike" and cfg_value(t, "node_id") == f"{edge.upper()}STRIKE{n}"
              and cfg_value(t, "port") == str(21115 + n - 1) and cfg_value(t, "public_host") == f"{edge}-1.krunk.io"
              and cfg_value(t, "modes") == "rotation" and cfg_value(t, "max_players") == "10"
              and cfg_value(t, "region") == cfg_value(read(os.path.join(d, "server-1.cfg")), "region")
              and cfg_value(t, "coordinator_url") == "wss://coordinator.krunk.io/mm/internal/ws"
              and cfg_value(t, "coordinator_secret") == SECRET and cfg_value(t, "join_secret") == SECRET
              and cfg_value(t, "finder_url") == "https://matchmaker.krunk.io"
              and cfg_value(t, "bans_file") == f"/var/lib/krunknative/strike-{n}/bans.json"
              and cfg_value(t, "shutdown_reason_file") == "/run/krunknative-deploy/strike-stop-reason"
              and cfg_value(t, "hostname").startswith("Krunker Strike "))
        check(f"{edge}: {f} is a Krunker Strike config ({edge.upper()}STRIKE{n}, :{21115 + n - 1})", ok, t)
    check(f"{edge}: Strike ports never overlap the Box Office ones", not (bo_ports & st_ports) and all(p >= 21115 for p in st_ports),
          (bo_ports, st_ports))
    unit = read(os.path.join(d, "kn-strike@.service"))
    check(f"{edge}: kn-strike@.service runs --flavor strike from strike-%i.cfg, capped",
          "--flavor strike --cfg /etc/krunknative/strike-%i.cfg" in unit and "MemoryMax=256M" in unit and "MemoryHigh=192M" in unit
          and "CPUWeight=50" in unit and "StateDirectory=krunknative/strike-%i" in unit and "MemorySwapMax=0" in unit, unit)
    bunit = read(os.path.join(d, "kn-gameserver@.service"))
    check(f"{edge}: kn-gameserver@.service is the Box Office unit", "--flavor" not in bunit and "MemoryMax" not in bunit
          and "CPUWeight=80" in bunit and "/server-%i.cfg" in bunit, bunit)
    nft = read(os.path.join(d, "60-krunknative.nft"))
    want_tcp = plan.get("bo_ports", "?") + (", " + plan.get("strike_ports", "?") if plan.get("strike_planned") else "")
    check(f"{edge}: firewall drop-in opens {want_tcp} (tcp + udp)",
          f"tcp_accept {{ {want_tcp} }}" in nft and f"udp_accept {{ {want_tcp} }}" in nft, nft)
    return plan


def main():
    if not shutil.which("ansible-playbook"):
        print("render_test: ansible-playbook not installed: skipped")
        return 0
    work = tempfile.mkdtemp(prefix="kn-deploy-render-", dir=os.environ.get("TMPDIR"))
    with open(os.path.join(work, "none.cfg"), "w") as f:
        f.write("[defaults]\n")
    try:
        out = render(work, HOSTS)
        plans = {h: check_host(out, h, v, m) for h, (v, m) in HOSTS.items()}
        targets = json.loads(read(os.path.join(out, "bridge-targets.json")) or "[]")
        for host in HOSTS:
            edge = host.split(".", 1)[1]
            check(f"net-bridge allows {edge}-1.krunk.io:21115-21118 (Strike) and still {edge}-1..11:21015-21025",
                  f"{edge}-1.krunk.io:21115-21118" in targets and f"{edge}-11.krunk.io:21015-21025" in targets
                  and f"{edge}-1.krunk.io:21015-21025" in targets, targets)
        total = sum(p.get("strike", 0) for p in plans.values())
        print("plan: " + "; ".join(f"{h.split('.', 1)[1]} {p.get('bo')} Box Office + {p.get('strike')} Strike" for h, p in plans.items())
              + f" ({total} Strike lobbies)", flush=True)
        # a host whose memory cannot hold a Strike instance next to its Box Office servers starts none
        host, vcpus, mem = TIGHT
        out = render(work, {host: (vcpus, mem)})
        plan = check_host(out, host, vcpus, mem)
        check("a tight host starts no Strike instance (inactive)", plan.get("strike") == 0 and plan.get("active") is False, plan)
        # the playbooks use ansible.posix (synchronize): an ansible-core without it cannot parse them
        posix = subprocess.run(["ansible-doc", "-t", "module", "ansible.posix.synchronize"], capture_output=True).returncode == 0
        if not posix:
            print("syntax-check skipped: the ansible.posix collection is not installed (deploy/ansible/requirements.yml)")
        for book in ("edges.yml", "central.yml") if posix else ():
            r = subprocess.run(["ansible-playbook", "--syntax-check", book], cwd=ANSIBLE, capture_output=True, text=True,
                               env=dict(os.environ, ANSIBLE_LOCAL_TEMP=os.path.join(work, "tmp")))
            check(f"ansible-playbook --syntax-check {book}", r.returncode == 0, (r.stdout + r.stderr)[-1500:])
    finally:
        if results and all(results):
            shutil.rmtree(work, ignore_errors=True)
        else:
            print("kept: " + work)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
