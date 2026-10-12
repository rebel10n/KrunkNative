#!/usr/bin/env node
// Krunker v7.1.4 store skin table for assets/skins.json: store.skins as the v7 client holds it
// after src/data/store.js loaded (v7_store.js): jspck.unpack(public/skins.jspck) with the
// templates merged and every load-time fixup applied (index, seas || 1, assetType/keyW of
// BACK/HEAD, creators, and the noSpin / noSale / noQuickSell / limited / limT flags store.js
// derives from free, itemShop, ranked, drop, mapReward, package, challenge, craftable, bundle,
// opensea, collectible, ticket and battlepass). kn_brain (services/brain/store.c), the bots
// (shared/src/bots.c) and the client read those flags as-is, so they must be in the file.
// Only `thumbnail` (an assets.krunker.io URL) is dropped.
//
// usage: node tools/ui-import/v7_skins.js <Krunker_v7.1.4 root>   (prints JSON on stdout)
'use strict';
const { loadStore } = require('./v7_store.js');

const root = process.argv[2];
if (!root) {
  process.stderr.write('usage: v7_skins.js <Krunker_v7.1.4 root>\n');
  process.exit(2);
}
const { store } = loadStore(root);
process.stdout.write(JSON.stringify(store.skins));
