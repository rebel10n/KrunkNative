#!/usr/bin/env node
// Lists the asset-host files of the store items (assets/skins.json, v10_store.js itemAssetPaths)
// that are in neither assets/ nor assets/v10/: previews, models and textures as the client
// resolves them. Required files are reported; optional ones (a skin texture the client falls back
// from, glow maps) only with --all. --tsv prints "category<TAB>path" lines for
// `fetch_live_assets.py fetch --list` (then add the 200s to live_assets_v10.lock and `install`).
// Exit 1 when a required file is missing and not in KNOWN_ABSENT.
//   node tools/ui-import/v10_store_assets.js [--assets DIR] [--from INDEX] [--all] [--tsv]
'use strict';
const fs = require('fs');
const path = require('path');
const { itemAssetPaths } = require('./v10_store.js');

const REPO = path.resolve(__dirname, '..', '..');
const argv = process.argv.slice(2);
const opt = (name, d) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : d; };
const ASSETS = opt('--assets', path.join(REPO, 'assets'));
const FROM = +opt('--from', 0);
const ALL = argv.includes('--all'), TSV = argv.includes('--tsv');

// Files the live asset host does not have either (HTTP 404, 2026-10-13): Bald Cap (replaceHair) has
// no model of its own
const KNOWN_ABSENT = new Set(['models/hats/hat_483.obj', 'textures/hats/hat_483.png']);

const skins = JSON.parse(fs.readFileSync(path.join(ASSETS, 'skins.json'), 'utf8'));
const have = (p) => fs.existsSync(path.join(ASSETS, p)) || fs.existsSync(path.join(ASSETS, 'v10', p));
let required = 0, optional = 0, absent = 0, total = 0;
for (let i = FROM; i < skins.length; i++) {
  for (const e of itemAssetPaths(skins[i])) {
    total++;
    if (have(e.path)) continue;
    if (e.optional) optional++;
    else if (KNOWN_ABSENT.has(e.path)) absent++;
    else required++;
    if (!e.optional || ALL) {
      if (TSV) console.log(`store-${e.kind}${e.optional ? '-opt' : ''}\t${e.path}`);
      else console.log(`${e.optional ? 'optional' : KNOWN_ABSENT.has(e.path) ? 'absent upstream' : 'MISSING'}\t${e.path}\t${i} ${skins[i].name}`);
    }
  }
}
console.error(`[v10_store_assets] items ${FROM}-${skins.length - 1}: ${total} files, ${required} required missing, ` +
  `${absent} absent upstream, ${optional} optional (fallback) missing`);
process.exit(required ? 1 : 0);
