#!/usr/bin/env python3
"""Krunker's public live data that Box Office serves from its own copies, pinned so drift is caught.

Krunker v10 fetches these from gapi at run time (deob-10.0.7.js: the gapi client is `new iiíïíiï(GAPI_URI)`, :27773,
GAPI_URI https://gapi.svc.krunker.io, :16427). kn_brain serves the same data from repo files:

  titles   GET /titles (titles.getTitles, :27650)                       -> assets/titles.json (services/brain/v10_api.c)
  limited  GET /shop/displays?tag=limited (shop.getDisplayByTag, :26627;
           the Eterno Shop :129566-129580, store.catalog :220848)       -> shop_official_products / _contents rows of
                                                                           services/db/migrations/*/03xx (shop.c)

Other gapi data has its own pin: the store items and spin wheels (/data/skins, /data/spins:
tools/ui-import/v10_store.js, `make store-check`) and the official maps (/maps, /maps/<i>: tools/krunker/fetch_maps.py).
Everything else the client asks gapi for is per-account (an Authorization header: /ranked/shop, /market/black-market,
/kpd/reviewers, ...) or live game state Box Office keeps itself (territorial wars, KrunkCup, creator events, news).

usage:
  python3 -I tools/krunker/live_data.py fetch    # download, store the copies under tools/krunker/live_data/, pin them
  python3 -I tools/krunker/live_data.py verify   # offline: KN's copies match the pinned responses
  python3 -I tools/krunker/live_data.py check    # online: the live responses still match the pins (exit 1 on drift)

environment: KN_FETCH_PROXY (default http://127.0.0.1:8888: Krunker's hosts answer 403 to some networks), KN_FETCH_UA.
"""

import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAPI = 'https://gapi.svc.krunker.io'
LOCK = os.path.join(ROOT, 'tools', 'krunker', 'live_data.lock.json')
COPIES = os.path.join(ROOT, 'tools', 'krunker', 'live_data')
ENDPOINTS = {
    'titles': GAPI + '/titles',
    'limited': GAPI + '/shop/displays?tag=limited',
}
# the purchase counts in a display are krunker.io's own and change with every sale: not part of the pin
VOLATILE = ('purchase_count',)


def opener():
    proxy = os.environ.get('KN_FETCH_PROXY', 'http://127.0.0.1:8888')
    handlers = [urllib.request.ProxyHandler({'http': proxy, 'https': proxy})] if proxy else []
    return urllib.request.build_opener(*handlers)


def get(url):
    ua = os.environ.get('KN_FETCH_UA', 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131 Safari/537.36')
    req = urllib.request.Request(url, headers={'User-Agent': ua, 'Origin': 'https://krunker.io', 'Referer': 'https://krunker.io/'})
    last = None
    for attempt in range(4):
        try:
            with opener().open(req, timeout=30) as r:
                return r.read()
        except Exception as e:  # 429 / 5xx / network: back off and retry
            last = e
            time.sleep(2 * (attempt + 1))
    raise SystemExit(f'live_data: {url}: {last}')


def strip_volatile(o):
    if isinstance(o, dict):
        return {k: strip_volatile(v) for k, v in o.items() if k not in VOLATILE}
    if isinstance(o, list):
        return [strip_volatile(v) for v in o]
    return o


def canonical(raw):
    """The pinned form: the response's data without the volatile fields, sorted keys, compact."""
    return json.dumps(strip_volatile(json.loads(raw)['data']), sort_keys=True, separators=(',', ':'), ensure_ascii=False)


def sha(s):
    return hashlib.sha256(s.encode('utf-8')).hexdigest()


def load_lock():
    with open(LOCK) as f:
        return json.load(f)


def copy_path(name):
    return os.path.join(COPIES, name + '.json')


def cmd_fetch(_args):
    os.makedirs(COPIES, exist_ok=True)
    lock = {'comment': 'tools/krunker/live_data.py: Krunker\'s public gapi data KN serves from its own copies',
            'endpoints': {}}
    for name, url in ENDPOINTS.items():
        c = canonical(get(url))
        with open(copy_path(name), 'w') as f:
            f.write(json.dumps(json.loads(c), indent=1, sort_keys=True, ensure_ascii=False) + '\n')
        lock['endpoints'][name] = {'url': url, 'sha256': sha(c), 'bytes': len(c.encode('utf-8')),
                                   'fetched': datetime.date.today().isoformat()}
        print(f'{name}: {url} sha256 {sha(c)[:16]}')
        time.sleep(1)
    with open(LOCK, 'w') as f:
        json.dump(lock, f, indent=2)
        f.write('\n')
    return 0


def pinned(name, lock):
    with open(copy_path(name)) as f:
        data = json.load(f)
    c = json.dumps(data, sort_keys=True, separators=(',', ':'), ensure_ascii=False)
    if sha(c) != lock['endpoints'][name]['sha256']:
        raise SystemExit(f'live_data: {copy_path(name)} does not match its pin in {LOCK}')
    return data


# ---- KN's side of each endpoint

def kn_titles():
    with open(os.path.join(ROOT, 'assets', 'titles.json')) as f:
        return json.load(f)


SQL_VALUE = re.compile(r"'(?:[^']|'')*'|[^,\s][^,]*")


def sql_values(s):
    out = []
    for v in SQL_VALUE.findall(s):
        v = v.strip()
        if v.startswith("'"):
            out.append(v[1:-1].replace("''", "'"))
        elif v == 'NULL':
            out.append(None)
        else:
            out.append(float(v) if '.' in v else int(v))
    return out


def kn_display(tag, dialect='postgres'):
    """The products of one display after every migration of a dialect, in file order (INSERT and node_order UPDATE)."""
    prods, contents = {}, {}
    ins = re.compile(r'INSERT INTO shop_official_(products|contents) \(([^)]*)\) VALUES \((.*)\);$')
    upd = re.compile(r"UPDATE shop_official_products SET node_order = (\d+) WHERE id = (\d+) AND display_tag = '([a-z]+)';$")
    for path in sorted(glob.glob(os.path.join(ROOT, 'services', 'db', 'migrations', dialect, '*.sql'))):
        with open(path) as f:
            for line in f:
                line = line.strip()
                m = ins.match(line)
                if m:
                    row = dict(zip([c.strip() for c in m.group(2).split(',')], sql_values(m.group(3))))
                    if m.group(1) == 'products':
                        prods[row['id']] = row
                    else:
                        contents.setdefault(row['product_id'], []).append(row)
                    continue
                m = upd.match(line)
                if m and int(m.group(2)) in prods and prods[int(m.group(2))]['display_tag'] == m.group(3):
                    prods[int(m.group(2))]['node_order'] = int(m.group(1))
    return {i: (p, contents.get(i, [])) for i, p in prods.items() if p['display_tag'] == tag and p.get('active', 1) == 1}


def diff_display(live, kn):
    """Differences between a pinned display and KN's rows: what the brain would serve differently."""
    out = []
    nodes = {c['product_id']: c for c in live['children'] if c.get('product_id') is not None}
    lp = {int(k): v for k, v in live['products'].items()}
    for pid in sorted(set(lp) | set(kn)):
        if pid not in kn:
            out.append(f'product {pid} {lp[pid]["name"]!r}: in krunker.io\'s display, not in KN\'s migrations')
            continue
        if pid not in lp:
            # Box Office's own catalogue rows (a kn.* price, such as the 50 KR Paint Ticket) are not
            # on krunker.io's display. A krunker.io product that left the display still differs.
            ref = str(kn[pid][0].get("price_ref") or "")
            if not ref.startswith("kn."):
                out.append(f'product {pid} {kn[pid][0]["name"]!r}: in KN\'s migrations, gone from krunker.io\'s display')
            continue
        (row, cont), p, n = kn[pid], lp[pid], nodes.get(pid)
        pr = p['prices'][0]
        want = {'name': p['name'], 'description': p['description'], 'image_url': p['image_url'],
                'price_id': pr['id'], 'price_ref': pr['price_ref'], 'price_type': pr['type'],
                'price_usd': (pr.get('real') or {}).get('price_usd'), 'price_amount': (pr.get('virtual') or {}).get('amount'),
                'purchase_limit': p.get('purchase_limit'), 'purchase_limit_scope': p.get('purchase_limit_scope'),
                'display_id': (p.get('display_ids') or [None])[0]}
        if n:
            want.update({'node_id': n['id'], 'node_order': n['order'], 'node_type': n['type']})
        for k, v in want.items():
            got = row.get(k)
            if isinstance(v, (int, float)) and isinstance(got, (int, float)) and abs(v - got) < 1e-9:
                continue
            if got != v:
                out.append(f'product {pid} {p["name"]!r} {k}: KN {got!r}, krunker.io {v!r}')
        lc = [(c['id'], c['type'], c['ref'], c['quantity']) for c in p['contents']]
        kc = [(c['content_id'], c['content_type'], c['ref'], c['quantity']) for c in sorted(cont, key=lambda c: c['ord'])]
        if lc != kc:
            out.append(f'product {pid} {p["name"]!r} contents: KN {kc}, krunker.io {lc}')
    return out


def cmd_verify(_args):
    lock = load_lock()
    bad = []
    lt, kt = pinned('titles', lock), kn_titles()
    if json.dumps(lt, sort_keys=True) != json.dumps(kt, sort_keys=True):
        lm, km = {t['id']: t for t in lt}, {t['id']: t for t in kt}
        for i in sorted(set(lm) | set(km)):
            if json.dumps(lm.get(i), sort_keys=True) != json.dumps(km.get(i), sort_keys=True):
                bad.append(f'title {i}: assets/titles.json {km.get(i, {}).get("name")!r}, krunker.io {lm.get(i, {}).get("name")!r}')
        if not bad:
            bad.append('assets/titles.json: the titles are in a different order than krunker.io\'s')
    live = pinned('limited', lock)
    for dialect in ('postgres', 'sqlite', 'mysql'):
        bad += [f'{dialect}: {d}' for d in diff_display(live, kn_display('limited', dialect))]
    for b in bad:
        print('live_data:', b)
    print(f'live_data: {"%d difference(s)" % len(bad) if bad else "titles and the limited display match their pins"}')
    return 1 if bad else 0


def cmd_check(_args):
    lock = load_lock()
    drift = 0
    for name, ent in lock['endpoints'].items():
        c = canonical(get(ent['url']))
        if sha(c) != ent['sha256']:
            drift += 1
            print(f'live_data: {name} changed on krunker.io ({ent["url"]}): pinned {ent["sha256"][:16]} '
                  f'({ent["fetched"]}), live {sha(c)[:16]}; run `make live-data-fetch`, then port the change')
        time.sleep(1)
    print(f'live_data: {"%d endpoint(s) drifted" % drift if drift else "no drift"}')
    return 1 if drift else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=('fetch', 'verify', 'check'))
    a = ap.parse_args()
    return {'fetch': cmd_fetch, 'verify': cmd_verify, 'check': cmd_check}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
