#!/usr/bin/env python3
"""Krunker Strike screen templates (ui/strike/screens/*.html) from the sandbox captures.

    python3 -I tools/strike/strike_screens.py [--captures DIR] [--out DIR] [--img DIR]

FRVR's Krunker Strike 1.37.2 builds its whole UI with Mithril from its app bundle; the static
index.html only has the boot loading screen and #frvrMenuMobileHolder. KrunkNative has no JS in
the native client, so each screen's markup is taken from the DOM FRVR's own code rendered in the
offline sandbox (tools/uidiff/capture-strike.js, doc/ref/krunker-strike-1.37.2/<state>.html.gz)
and stored as a template that client/src/ui/strike_*.c inserts and then drives over the DOM-ops
API. The markup is FRVR's, unchanged, except:
  - data: URI images (FRVR renders weapon silhouettes / class faces into canvases at run time)
    become files under ui/strike/img/kn-captured/<sha256-12>.<ext>;
  - three.js canvases (the avatar viewers) keep their element, id and class, without the
    data-engine / width / height attributes the renderer put on them;
  - the KrunkNative hooks: onclick="strikeAction('<name>')" on the elements FRVR bound click
    handlers to (ACTIONS below; Mithril's handlers are JS properties, invisible in the DOM) and
    data-kn="<slot>" on the elements C fills with live values (SLOTS below).
Everything else (classes, inline styles, text, order) is what FRVR rendered. The script never
executes anything it reads; run it with python3 -I.
"""
import argparse, gzip, hashlib, html, os, re, sys
from html.parser import HTMLParser

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr'}


class Node:
    __slots__ = ('tag', 'attrs', 'kids', 'text', 'parent')

    def __init__(self, tag, attrs=None, text=None):
        self.tag, self.attrs, self.kids, self.text, self.parent = tag, list(attrs or []), [], text, None

    def attr(self, name):
        for k, v in self.attrs:
            if k == name: return v
        return None

    def set_attr(self, name, value):
        for i, (k, _) in enumerate(self.attrs):
            if k == name:
                if value is None: del self.attrs[i]
                else: self.attrs[i] = (k, value)
                return
        if value is not None: self.attrs.append((name, value))

    def classes(self):
        return (self.attr('class') or '').split()

    def walk(self):
        yield self
        for k in self.kids:
            if k.tag: yield from k.walk()


class Parser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node('#root')
        self.cur = self.root

    def handle_starttag(self, tag, attrs):
        n = Node(tag, [(k, v if v is not None else '') for k, v in attrs])
        n.parent = self.cur
        self.cur.kids.append(n)
        if tag not in VOID: self.cur = n

    def handle_startendtag(self, tag, attrs):
        n = Node(tag, [(k, v if v is not None else '') for k, v in attrs])
        n.parent = self.cur
        self.cur.kids.append(n)

    def handle_endtag(self, tag):
        c = self.cur
        while c is not self.root and c.tag != tag: c = c.parent
        if c is not self.root: self.cur = c.parent

    def handle_data(self, data):
        t = Node(None, text=data)
        t.parent = self.cur
        self.cur.kids.append(t)


def parse(path):
    with gzip.open(path, 'rt', encoding='utf-8') as f:
        src = f.read()
    p = Parser()
    p.feed(src)
    p.close()
    return p.root


def find(root, pred):
    for n in root.walk():
        if n.tag and pred(n): return n
    return None


def by_id(root, i):
    return find(root, lambda n: n.attr('id') == i)


def has_class(c):
    return lambda n: c in n.classes()


# ---------------------------------------------------------------- serialisation

def esc_attr(v):
    return v.replace('&', '&amp;').replace('"', '&quot;').replace('<', '&lt;')


def esc_text(t):
    return t.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def ser(n, out):
    if n.tag is None:
        out.append(esc_text(n.text))
        return
    a = ''.join(' %s="%s"' % (k, esc_attr(v)) for k, v in n.attrs)
    out.append('<%s%s>' % (n.tag, a))
    if n.tag in VOID: return
    for k in n.kids: ser(k, out)
    out.append('</%s>' % n.tag)


# ---------------------------------------------------------------- rewriting

DATA_RE = re.compile(r'data:image/(png|jpeg|gif|webp|svg\+xml);base64,([A-Za-z0-9+/=]+)')


def save_data_uri(m, img_dir, rel):
    import base64
    ext = {'png': 'png', 'jpeg': 'jpg', 'gif': 'gif', 'webp': 'webp', 'svg+xml': 'svg'}[m.group(1)]
    data = base64.b64decode(m.group(2))
    name = hashlib.sha256(data).hexdigest()[:12] + '.' + ext
    os.makedirs(img_dir, exist_ok=True)
    path = os.path.join(img_dir, name)
    if not os.path.exists(path):
        with open(path, 'wb') as f: f.write(data)
    return rel + name


def rewrite(root, img_dir, img_rel):
    for n in list(root.walk()):
        for k, v in list(n.attrs):
            if 'data:image/' in v:
                n.set_attr(k, DATA_RE.sub(lambda m: save_data_uri(m, img_dir, img_rel), v))
        if n.tag == 'canvas':
            for k in ('data-engine', 'width', 'height'): n.set_attr(k, None)
            if n.attr('id') == '': n.set_attr('id', None)
            if n.attr('class') == '': n.set_attr('class', None)


def strip(root, pred):
    for n in list(root.walk()):
        if n is not root and pred(n) and n.parent: n.parent.kids.remove(n)


# KrunkNative hooks: (css-like predicate, action) -> onclick="strikeAction('action')"
def sel(s):
    """'.a.b' / '#id' / 'tag.a' / '.a .b' (descendant, two levels max) -> predicate"""
    parts = s.split()
    def simple(p):
        tag = re.match(r'^[a-z0-9]+', p)
        ids = re.findall(r'#([\w-]+)', re.sub(r'\[.*?\]', '', p))
        cls = re.findall(r'\.([\w-]+)', re.sub(r'\[.*?\]', '', p))
        attrs = re.findall(r'\[([\w-]+)=([\w-]+)\]', p)
        def f(n):
            if any(n.attr(k) != v for k, v in attrs): return False
            if tag and n.tag != tag.group(0): return False
            if ids and n.attr('id') != ids[0]: return False
            c = n.classes()
            return all(x in c for x in cls)
        return f
    if len(parts) == 1: return simple(parts[0])
    anc, leaf = simple(parts[0]), simple(parts[1])
    def g(n):
        if not leaf(n): return False
        p = n.parent
        while p is not None and p.tag:
            if anc(p): return True
            p = p.parent
        return False
    return g


def hook_row(root, label, action):
    """The first .frvr-button of the settings row whose .settName reads `label` (FRVR's Mithril rows)"""
    for row in root.walk():
        if not row.tag or 'setting' not in row.classes(): continue
        name = find(row, has_class('settName'))
        text = ''.join(k.text for k in name.kids if k.tag is None).strip() if name else ''
        if text != label: continue
        btn = find(row, has_class('frvr-button'))
        if btn is None: print('  warning: no button in row %s' % label, file=sys.stderr); return 0
        btn.set_attr('onclick', "strikeAction('%s')" % action)
        return 1
    print('  warning: no settings row %s' % label, file=sys.stderr)
    return 0


def hook(root, rules, attr):
    """rules: [(selector, value, 'all'|'first'|index)]; returns the number of hooks set"""
    count = 0
    for s, value, which in rules:
        hits = [n for n in root.walk() if n.tag and sel(s)(n)]
        if which == 'first': hits = hits[:1]
        elif isinstance(which, int): hits = hits[which:which + 1]
        for i, n in enumerate(hits):
            v = value.replace('{i}', str(i))
            if attr == 'onclick': n.set_attr('onclick', "strikeAction('%s')" % v)
            else: n.set_attr(attr, v)
            count += 1
        if not hits and (s, value, which) not in COMMON_ACTIONS: print('  warning: no element for %s' % s, file=sys.stderr)
    return count


COMMON_ACTIONS = [
    ('.frvr-button-back', 'back', 'all'),
    ('.frvr-button-home', 'home', 'all'),
]

# view id -> (capture state, extraction, click hooks, value slots)
SCREENS = {
    'MAIN_MENU': ('strike-main-menu', 'menu', [
        ('[trackingid=play_krunker_main_menu]', 'play', 'first'),
        ('[trackingid=store_main_menu]', 'store', 'first'),
        ('[trackingid=customize_main_menu]', 'customize', 'first'),
        ('[trackingid=feedback_main_menu]', 'feedback', 'first'),
        ('#mobMenuSett', 'settings', 'first'),
        ('#mobMenuAcc', 'profile', 'first'),
        ('.frvr-kr-limit__info', 'kr-limit-info', 'first'),
    ], []),
    'CLASS_SELECTION': ('strike-class-selection', 'menu', [
        ('.class-selection__class-bullet-holder', 'class:{i}', 'all'),
        ('.class-selection__select-class .frvr-button', 'select-class', 'first'),
    ], []),
    'CUSTOMIZATION_SCREEN': ('strike-customization', 'menu', [
        ('.customization-screen__class-selection', 'class-selection', 'first'),
    ], []),
    'SETTINGS': ('strike-settings', 'menu', [
        ('.tabSelectorItem', 'settings-tab:{i}', 'all'),
        ('.frvr-preset__button', 'preset:{i}', 'all'),
    ], []),
    'TAP_TO_PLAY': ('strike-tap-to-play', 'menu', [
        ('[trackingid=tap_to_play_join_match]', 'play', 'first'),
        ('[trackingid=store_main_menu]', 'store', 'first'),
        ('#mobMenuCustomize', 'customize', 'first'),
        ('#mobMenuSett', 'settings', 'first'),
        ('#mobMenuAcc', 'profile', 'first'),
    ], []),
    'PROFILE': ('strike-profile', 'menu', [], []),
    'CREATE_MATCH': ('strike-create-match', 'menu', [], []),
    'BATTLEPASS_SCREEN': ('strike-battlepass', 'menu', [], []),
    'LEAN_END_SCREEN': ('strike-end', 'menu', [
        ('.lean-end-screen__bottom-buttons .frvr-button', 'match-details', 'first'),
    ], []),
    'STORE': ('strike-store', 'menu', [], []),                 # fg: Extras (SPINS / MARKET / CURRENCY)
    'SPIN': ('strike-spin', 'menu', [], []),                   # SA: Pt(SA, {spinnerIndex}), the prize wheel
    'CUSTOMIZE_CONTROLS': ('strike-customize-controls', 'menu', [], []), # wb: Pt(ZC, {onHide}), the controls editor
    'LEADERBOARD_PLAYERS': ('strike-leaderboard-players', 'menu', [], []), # YB: the tournament board rows
    'POPUP': ('strike-popup', 'menu', [], []),                 # In: the popup frame (its builder fills .popup__holder)
    'IN_GAME_UI': ('strike-ingame', 'ingame', [], []),
    'KILL_CARD': ('strike-killcard', 'killcard', [], []),
}
# the settings tabs' content (.tabsContent of each tab)
TABS = {'SETTINGS-presets': 'strike-settings', 'SETTINGS-display': 'strike-settings-display', 'SETTINGS-game': 'strike-settings-game',
        'SETTINGS-sound': 'strike-settings-sound', 'SETTINGS-other': 'strike-settings-other'}

HOLDER_SKIP = ('frvr-notifications-holder', 'ftue-overlay', 'frvr-loading-holder', 'frvr-in-game-ui', 'joystick-zones', 'control-buttons')


def extract(doc, how):
    holder = by_id(doc, 'frvrMenuMobileHolder')
    if holder is None: raise SystemExit('no #frvrMenuMobileHolder')
    if how == 'menu':
        kids = [k for k in holder.kids if k.tag and not any(c in k.classes() for c in HOLDER_SKIP)]
        kids = kids[-1:]  # the top screen (a keepAlive nu() leaves a stale copy of it before)
    elif how == 'ingame':
        ig = find(holder, has_class('frvr-in-game-ui'))
        strip(ig, has_class('frvr-in-game-ui__kill-card-container'))
        kids = [ig]
    elif how == 'killcard':
        kids = [find(holder, has_class('frvr-in-game-ui__kill-card-container'))]
    else:
        raise SystemExit('bad extraction ' + how)
    return [k for k in kids if k is not None]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--captures', default=os.path.join(REPO, 'doc', 'ref', 'krunker-strike-1.37.2'))
    ap.add_argument('--out', default=os.path.join(REPO, 'ui', 'strike', 'screens'))
    ap.add_argument('--img', default=os.path.join(REPO, 'ui', 'strike', 'img', 'kn-captured'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    img_rel = './img/kn-captured/'
    done = 0
    for view, (state, how, actions, slots) in SCREENS.items():
        src = os.path.join(a.captures, state + '.html.gz')
        if not os.path.exists(src):
            print('skip %s: no %s' % (view, src), file=sys.stderr)
            continue
        doc = parse(src)
        roots = extract(doc, how)
        wrap = Node('#frag')
        for r in roots: wrap.kids.append(r); r.parent = wrap
        rewrite(wrap, a.img, img_rel)
        hook(wrap, COMMON_ACTIONS + actions, 'onclick')
        hook(wrap, slots, 'data-kn')
        out = []
        for r in roots: ser(r, out)
        body = ''.join(out)
        with open(os.path.join(a.out, view + '.html'), 'w', encoding='utf-8') as f:
            f.write('<!-- FRVR Krunker Strike 1.37.2 %s, rendered by FRVR\'s own code in the sandbox (%s.html.gz); generated by '
                    'tools/strike/strike_screens.py: do not edit by hand -->\n' % (view, state))
            f.write(body + '\n')
        done += 1
        print('%s <- %s (%d bytes)' % (view, state, len(body)))
    for name, state in TABS.items():
        src = os.path.join(a.captures, state + '.html.gz')
        if not os.path.exists(src): continue
        doc = parse(src)
        content = find(doc, has_class('tabsContent'))
        if content is None: continue
        wrap = Node('#frag')
        wrap.kids = content.kids
        rewrite(wrap, a.img, img_rel)
        hook(wrap, [a for a in SCREENS['SETTINGS'][2] if a[1].startswith('preset')] if name.endswith('presets') else [], 'onclick')
        if name == 'SETTINGS-game': hook_row(wrap, 'Customize Controls', 'customize-controls')
        out = []
        for k in content.kids: ser(k, out)
        with open(os.path.join(a.out, name + '.html'), 'w', encoding='utf-8') as f:
            f.write('<!-- FRVR Krunker Strike 1.37.2 settings tab content (.tabsContent) from %s.html.gz; generated by '
                    'tools/strike/strike_screens.py: do not edit by hand -->\n' % state)
            f.write(''.join(out) + '\n')
        done += 1
    print('%d templates' % done)


if __name__ == '__main__':
    main()
