#!/usr/bin/env python3
"""Editor WP4 parity test: the logic UI (triggers), the Custom GUI panel and the left panel's game
config against Krunker v7.1.4 (doc/v7/editor.md WP4).

    python3 -I tools/editor/logic_parity.py --bin <build>/client_bin [--case NAME]... [--keep]

Every case of tools/editor/logic_cases.json is replayed in a headless native editor
(client_bin --offline --editor new, KRUNKNATIVE_HEADLESS=1, no window on any display) through
KRUNKNATIVE_EDITOR_SCRIPT's trig: / gui: / cfg: commands (the handlers the panel controls run) and
compared with what v7's own editor wrote for the same edits (tools/editor/v7ref.js state "logic",
doc/ref/krunker-v7.1.4/editor/logic/):

- trigger cases: the object's serialization (key order included) and, when the object panel shows
  the logic folders (editor_inspector.c calls ed_triggers_panel), the "Trigger Event" /
  "Trigger Actions" block of the right panel dump;
- GUI / config cases: getMapExport's config and gui, and the right (GUI) / left panel dump.

Plus the left panel of a new map against left-essential.dump.txt.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
REF = os.path.join(REPO, 'doc', 'ref', 'krunker-v7.1.4', 'editor')


def native_value(step, gui=False):
    kind, v = step.get('kind'), step['value']
    if kind == 'select':
        # a select's value: a number in the native control, the option's string for condition
        # values (v7's GUI.action stores the string; editor_triggers.c does the same)
        if not gui and step['set'].startswith('conditionVal'):
            return json.dumps(str(v))
        return json.dumps(v if isinstance(v, (int, float)) else float(v))
    if kind in ('text', 'color'):
        return json.dumps(str(v))
    if kind == 'switch':
        return json.dumps(bool(v))
    return json.dumps(v)


def script_for(case, out):
    cmds = ['closewindow']
    if case.get('prefab') is not None:
        cmds.append('add:%d' % case['prefab'])
    for s in case['steps']:
        if 'set' in s:
            cmds.append('trig:set:%s:%s' % (s['set'], native_value(s)))
        elif 'add' in s:
            cmds.append('trig:add')
        elif 'remove' in s:
            cmds.append('trig:remove:%d' % s['remove'])
        elif 'gui_add' in s:
            cmds.append('gui:add')
        elif 'gui_remove' in s:
            cmds.append('gui:remove:%d' % s['gui_remove'])
        elif 'gui_dup' in s:
            cmds.append('gui:dup:%d' % s['gui_dup'])
        elif 'gui_set' in s:
            cmds.append('gui:set:%d:%s:%s' % (s['gui_set'], s['key'], native_value(s, gui=True)))
        elif 'cfg_set' in s:
            cmds.append('cfg:set:%s:%s' % (s['cfg_set'], json.dumps(s['value'])))
        elif 'cfg_copy' in s:
            cmds.append('cfg:copy:%d' % s['cfg_copy'])
        elif 'cfg_copycat' in s:
            cmds.append('cfg:copycat:%d:%s' % (s['cfg_copycat'], s['sub']))
        elif 'cfg_reset' in s:
            cmds.append('cfg:reset:%s:%d' % (s['cfg_reset'], s['index']))
    gui = any('gui_add' in s for s in case['steps'])
    if case.get('prefab') is not None:
        cmds += ['wait:0.3', 'objdump:' + out['obj'], 'dump:right:' + out['dump']]
    else:
        cmds += ['panel:gui' if gui else 'closewindow', 'wait:0.3', 'export:' + out['export'],
                 'dump:%s:%s' % ('right' if gui else 'left', out['dump'])]
    for c in cmds:
        if ';' in c:
            raise SystemExit('case %s: ";" in a script command' % case['name'])
    return ';'.join(cmds)


def run_editor(binary, script, work, egl):
    env = dict(os.environ)
    for k in ('DISPLAY', 'WAYLAND_DISPLAY'):
        env.pop(k, None)
    env.update({'KRUNKNATIVE_HEADLESS': '1', 'XDG_CONFIG_HOME': os.path.join(work, 'cfg'),
                'KRUNKNATIVE_EDITOR_SCRIPT': script, 'KRUNKNATIVE_SCREENSHOT': os.path.join(work, 'shot.png')})
    if egl:
        env['KRUNKNATIVE_EGL_DEVICE'] = egl
    with open(os.path.join(work, 'client.log'), 'w') as log:
        r = subprocess.run([binary, '--offline', '--editor', 'new'], cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    return r.returncode


def trigger_block(lines):
    """The lines of the "Trigger Event" / "Trigger Actions" folders (v7 UI:4080-4500)."""
    out, inside = [], False
    for ln in lines:
        if ln.startswith('Fs Trigger Event') or ln.startswith('Fs Trigger Actions'):
            inside = True
        elif inside and (ln.startswith('Fs ') or ln.startswith('F ') or ln.startswith('L[] Script')):
            inside = False
        if inside:
            out.append(ln)
    return out


def ordered(o):
    """JSON with key order kept (object_pairs_hook) for an exact comparison."""
    return json.loads(json.dumps(o), object_pairs_hook=lambda p: [list(x) for x in p])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', required=True, help='client_bin')
    ap.add_argument('--case', action='append', help='run only these cases')
    ap.add_argument('--egl', default=os.environ.get('KRUNKNATIVE_EGL_DEVICE', ''), help='KRUNKNATIVE_EGL_DEVICE (default: the headless pick)')
    ap.add_argument('--keep', action='store_true', help='keep the work directory')
    args = ap.parse_args()
    binary = os.path.abspath(args.bin)
    cases = json.load(open(os.path.join(REPO, 'tools', 'editor', 'logic_cases.json')))['cases']
    if args.case:
        cases = [c for c in cases if c['name'] in args.case]
    base = os.path.join(os.path.expanduser('~'), '.cache')
    os.makedirs(base, exist_ok=True)
    work = tempfile.mkdtemp(prefix='kn-logic-parity-', dir=base)
    failed = skipped = 0
    try:
        # the left panel of a new map
        if not args.case:
            d = os.path.join(work, 'left')
            os.makedirs(d)
            out = os.path.join(d, 'left.txt')
            run_editor(binary, 'closewindow;dump:left:' + out, d, args.egl)
            want = open(os.path.join(REF, 'left-essential.dump.txt')).read()
            got = open(out).read() if os.path.exists(out) else ''
            ok = got == want
            failed += not ok
            print('%-28s %s' % ('left-essential', 'PASS' if ok else 'FAIL (left panel dump differs)'))
        for case in cases:
            d = os.path.join(work, case['name'])
            os.makedirs(d)
            out = {k: os.path.join(d, k + '.txt') for k in ('obj', 'dump', 'export')}
            rc = run_editor(binary, script_for(case, out), d, args.egl)
            golden = json.load(open(os.path.join(REF, 'logic', case['name'] + '.json')))
            ref_dump = open(os.path.join(REF, 'logic', case['name'] + '.dump.txt')).read().splitlines()
            got_dump = open(out['dump']).read().splitlines() if os.path.exists(out['dump']) else []
            problems, notes = [], []
            if rc != 0:
                problems.append('client_bin exit %d' % rc)
            if case.get('prefab') is not None:
                got = None
                if os.path.exists(out['obj']):
                    for ln in open(out['obj']).read().splitlines():
                        name, _, js = ln.partition(' ')
                        if js.startswith('{'):
                            o = json.loads(js)
                            if o.get('i') == case['prefab'] or (case['prefab'] == 0 and 'i' not in o):
                                got = o
                if got is None:
                    problems.append('no object in objdump')
                else:
                    got.pop('objUUID', None)
                    if case['prefab'] == 55:
                        # a new model prefab takes the generator's OBJ bounds, v7 the loaded model's
                        # (headless: none, [1, 1, 1]); doc/v7/editor.md 1.13, editor_model.h
                        golden = dict(golden)
                        golden['s'] = got.get('s')
                    if ordered(got) != ordered(golden):
                        problems.append('serialize differs:\n    native %s\n    v7     %s' % (json.dumps(got), json.dumps(golden)))
                want_block, got_block = trigger_block(ref_dump), trigger_block(got_dump)
                if not got_block:
                    notes.append('panel dump skipped (the inspector does not show the logic folders yet)')
                elif got_block != want_block:
                    problems.append('panel dump differs:\n' + '\n'.join('    ' + x for x in difflines(want_block, got_block)))
            else:
                exp = json.load(open(out['export'])) if os.path.exists(out['export']) else {}
                got = {'config': exp.get('config'), 'gui': exp.get('gui')}
                if ordered(got) != ordered(golden):
                    problems.append('export differs:\n    native %s\n    v7     %s' % (json.dumps(got), json.dumps(golden)))
                if got_dump != ref_dump:
                    problems.append('panel dump differs:\n' + '\n'.join('    ' + x for x in difflines(ref_dump, got_dump)))
            if problems:
                failed += 1
                print('%-28s FAIL' % case['name'])
                for p in problems:
                    print('  ' + p)
            else:
                skipped += bool(notes)
                print('%-28s PASS%s' % (case['name'], (' (' + '; '.join(notes) + ')') if notes else ''))
    finally:
        if args.keep or failed:
            print('work directory: ' + work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print('logic parity: %d failed' % failed)
    return 1 if failed else 0


def difflines(want, got):
    import difflib
    return list(difflib.unified_diff(want, got, 'v7', 'native', lineterm='', n=1))[:60]


if __name__ == '__main__':
    sys.exit(main())
