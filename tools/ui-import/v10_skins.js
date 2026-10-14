#!/usr/bin/env node
// Krunker v10.0.2 store skin table for assets/skins.json: store.skins as the v10 client holds it
// after its store module loaded (v10_store.js): the gapi /data/skins table (jspck) with the
// templates merged and every applyShopMutations fixup applied (index, seas || 1, assetType/keyW
// of BACK/HEAD, creators, and the noSpin / noSale / noQuickSell / limited / limT flags it derives
// from free, itemShop, ranked, drop, mapReward, eventReward, package, challenge, craftable, bundle,
// opensea, collectible / ticket / paint types, battlepass and premiumSpin). kn_brain
// (services/brain/store.c), the bots (shared/src/bots.c) and the client read those flags as-is.
// Only `thumbnail` (an assets.krunker.io URL) is dropped.
//
// usage: node tools/ui-import/v10_skins.js [skins.jspck]   (prints JSON on stdout)
'use strict';
const { rawSkins, loadStore } = require('./v10_store.js');

const { store } = loadStore(process.argv[2] ? rawSkins(process.argv[2]) : null);
process.stdout.write(JSON.stringify(store.skins));
