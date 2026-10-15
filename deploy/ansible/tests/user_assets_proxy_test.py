#!/usr/bin/env python3
"""Role kn_brain's user-asset proxy (roles/kn_brain/tasks/user_assets_proxy.yml) on fake krunkbox
.env files, run locally with ansible-playbook (connection local, a temp kn_etc_dir, no root):

  python3 -I deploy/ansible/tests/user_assets_proxy_test.py

Checks: PROXY (plain, double-quoted, single-quoted, `export PROXY=`, after other keys and comments)
becomes KN_USER_ASSETS_PROXY="<value>" in <kn_etc_dir>/user-assets.env (mode 0640) and nothing else of
the .env does; the value never appears in ansible's output (no_log); a .env without PROXY and a
missing .env change nothing and do not fail. Exit 77 when ansible-playbook is not installed.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TASKS = os.path.join(HERE, "..", "roles", "kn_brain", "tasks", "user_assets_proxy.yml")
SECRET = "http://user:s3cr3t-%d@10.9.8.7:3128" % os.getpid()
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def run(work, env_text):
    etc = os.path.join(work, "etc")
    os.makedirs(etc, exist_ok=True)
    env_path = os.path.join(work, "krunkbox.env")
    if env_text is None:
        if os.path.exists(env_path):
            os.remove(env_path)
    else:
        with open(env_path, "w") as f:
            f.write(env_text)
    import grp
    import pwd
    user = pwd.getpwuid(os.getuid()).pw_name
    group = grp.getgrgid(os.getgid()).gr_name
    play = os.path.join(work, "play.yml")
    with open(play, "w") as f:
        f.write("""- hosts: localhost
  connection: local
  gather_facts: false
  vars:
    kn_etc_dir: %s
    kn_group: %s
    kn_brain_env_owner: %s
    kn_brain_user_assets_proxy_env: %s
  tasks:
    - ansible.builtin.include_tasks: %s
  handlers:
    - name: Restart kn_brain
      ansible.builtin.debug:
        msg: restart
""" % (json.dumps(etc), json.dumps(group), json.dumps(user), json.dumps(env_path), json.dumps(os.path.abspath(TASKS))))
    env = dict(os.environ, ANSIBLE_NOCOLOR="1", ANSIBLE_RETRY_FILES_ENABLED="0", ANSIBLE_LOCALHOST_WARNING="0",
               ANSIBLE_INVENTORY_UNPARSED_WARNING="0")
    r = subprocess.run(["ansible-playbook", "-i", "localhost,", play], capture_output=True, text=True, env=env, cwd=work)
    out_file = os.path.join(etc, "user-assets.env")
    content = open(out_file).read() if os.path.exists(out_file) else None
    return r.returncode, r.stdout + r.stderr, content, out_file


def main():
    if not shutil.which("ansible-playbook"):
        print("ansible-playbook not installed: skipped")
        return 77
    work = tempfile.mkdtemp(prefix="kn-ua-proxy-")
    try:
        cases = [
            ("plain", "# krunkbox\nOTHER=1\nPROXY=%s\nTOKEN=abc\n" % SECRET),
            ("double-quoted", 'PROXY="%s"\n' % SECRET),
            ("single-quoted", "PROXY='%s'\n" % SECRET),
            ("export", "export PROXY=%s\n" % SECRET),
        ]
        for name, text in cases:
            shutil.rmtree(os.path.join(work, "etc"), ignore_errors=True)
            rc, out, content, path = run(work, text)
            check(rc == 0, "%s: the play passes" % name)
            check(content is not None and ("KN_USER_ASSETS_PROXY=%s" % json.dumps(SECRET)) in content,
                  "%s: KN_USER_ASSETS_PROXY written" % name)
            check(content is not None and "OTHER" not in content and "TOKEN" not in content, "%s: nothing else of the .env" % name)
            check(content is not None and oct(os.stat(path).st_mode & 0o777) == "0o640", "%s: mode 0640" % name)
            check(SECRET not in out and "s3cr3t" not in out, "%s: the value is never in ansible's output" % name)
        # an existing file stays as it is without a PROXY / without the .env
        for name, text in (("no PROXY", "OTHER=1\nPROXY_URL=x\n"), ("no .env", None)):
            rc, out, content, _ = run(work, text)
            check(rc == 0, "%s: the play passes" % name)
            check(content is not None and SECRET in content, "%s: user-assets.env left as it is" % name)
            check("No PROXY in" in out, "%s: says so" % name)
        shutil.rmtree(os.path.join(work, "etc"), ignore_errors=True)
        rc, out, content, _ = run(work, None)
        check(rc == 0 and content is None, "no .env, no file: nothing written")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if FAILS:
        print("user assets proxy test: %d FAILED" % len(FAILS))
        return 1
    print("user assets proxy test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
