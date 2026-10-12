// Krunker v7.1.4 store data, evaluated from the reference tree's own sources (read-only):
// src/data/store.js (wheels, rarities, types, previews, templates and the per-skin load-time
// fixups), src/data/itemTypes.js, src/config.js (currentSeason, skinColors) and the skin table
// public/skins.jspck (unpacked with src/shim/jspck.js, as src/data/skins.js does).
// Used by v7_skins.js (assets/skins.json) and gen_viewer_store.js (viewer_store_table.c).
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const V7_ROOT = '/home/user/jails/kru/src/Krunker_v7.1.4';
const inert = new Proxy(function () {}, { get: () => inert, apply: () => inert, construct: () => inert });

function evalModule(file, requireMap) {
  const module = { exports: {} };
  const req = (name) => {
    for (const key of Object.keys(requireMap)) if (name.endsWith(key)) return requireMap[key];
    return inert;
  };
  vm.runInNewContext(fs.readFileSync(file, 'utf8'), {
    module, exports: module.exports, require: req, console, Math, JSON, Date, parseInt, parseFloat, isNaN, isFinite,
    Object, Array, String, Number, Boolean, RegExp, Error, TypeError, Infinity, NaN, Symbol, Map, Set,
  }, { filename: file });
  return module.exports;
}

// The raw store.skins array exactly as the v7 client gets it (jspck.unpack(skins.jspck)).
function rawSkins(root = V7_ROOT) {
  const shim = evalModule(path.join(root, 'src/shim/jspck.js'), {});
  if (!shim || typeof shim.unpack !== 'function') throw new Error('src/shim/jspck.js did not export unpack()');
  const skins = shim.unpack(fs.readFileSync(path.join(root, 'public/skins.jspck'), 'utf8'));
  if (!Array.isArray(skins)) throw new Error('skins.jspck is not an array');
  return skins;
}

// store.js evaluated over `skins` (default: the raw table). Its loop merges the templates and
// applies every load-time fixup (index, seas || 1, BACK/HEAD assetType + keyW, creators,
// free / itemShop / ranked / drop / mapReward / package / challenge / craftable / bundle /
// opensea / collectible / ticket / battlepass flags, player-card texExt, thumbnail). The loop is
// idempotent, so an already processed table (assets/skins.json) comes out unchanged.
function loadStore(root = V7_ROOT, skins = null) {
  const itemTypes = evalModule(path.join(root, 'src/data/itemTypes.js'), {});
  const utils = { getPreview: () => '' }; // the only UTILS call store.js makes at load time
  const store = evalModule(path.join(root, 'src/data/store.js'), {
    '../libs/utils.js': utils, './itemTypes.js': itemTypes, './skins.js': skins || rawSkins(root),
  });
  for (const s of store.skins) delete s.thumbnail; // the asset host URL (getPreview), not data
  return { store, itemTypes };
}

function loadConfig(root = V7_ROOT) {
  return evalModule(path.join(root, 'src/config.js'), {
    'shim/process.js': { env: {}, release: { name: 'node' } }, // config.isNode: no `location`
    'data/movement.js': [],
  });
}

module.exports = { V7_ROOT, rawSkins, loadStore, loadConfig };
