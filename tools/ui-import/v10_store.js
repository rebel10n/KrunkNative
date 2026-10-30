// Krunker v10.0.2 store data: the store module of the live v10 client (~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js, the
// 10.0.2 bundle, lines 102800-103420; read, never executed) transcribed as data, plus its
// load-time loop applyShopMutations (v10.js:103066) ported to plain JS.
//
// v10 has no store table in the bundle: the client fetches it from gapi at load time
// (v10.js:28030 `GET <gapi>/data/skins`, a jspck text) and the prize wheels and their item pools
// the same way (v10.js:102788 `GET <gapi>/data/spins`). Both responses are jspck (the packer of
// v7.1.4's src/shim/jspck.js, which unpacks them byte-exactly):
//   skins: ~/.cache/kn-v10/live/api/skins.jspck (live, 2026-10-30, v10.0.5; 8152 items), KN_V10_SKINS=
//   spins: tools/ui-import/v10_spins.jspck (live, 2026-10-30, v10.0.5; 14 wheels, 43 pools), KN_V10_SPINS=
// Used by v10_skins.js (assets/skins.json) and gen_viewer_store.js (viewer_store_table.c and
// shared/src/store_spins_table.c).
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');

const V7_JSPCK = '/home/user/jails/kru/src/Krunker_v7.1.4/src/shim/jspck.js';
const V10_SKINS = process.env.KN_V10_SKINS || path.join(os.homedir(), '.cache/kn-v10/live/api/skins.jspck');
const V10_SPINS = process.env.KN_V10_SPINS || path.join(__dirname, 'v10_spins.jspck');
// sha256 of the pinned live responses (a different file is reported, not refused)
const V10_SKINS_SHA256 = 'c361e31d430aa48843e9969d8ca0100c304a8befd91df16559bcc8cf8f2c13ad';
const V10_SPINS_SHA256 = '1e35774dee2656ad4b78f5352c9f3ef39553089723f315927bfd8546cdab4479';

// itemTypes (v10.js:20116): v7's 17 plus EMOJI 17 and PAINT 18
const T = { WEAPON: 0, HAT: 1, BODY: 2, MELEE: 3, SPRAY: 4, DYE: 5, WAIST: 6, FACE: 7, SHOE: 8, PET: 9, COLLECTIBLE: 10,
  WRIST: 11, CHARM: 12, TICKET: 13, BACK: 14, HEAD: 15, PLAYER_CARD: 16, EMOJI: 17, PAINT: 18 };

// store.types (v10.js:102835)
const types = ['weapons/weapon_', 'hats/hat_', 'body/body_', 'melee/melee_', 'sprays/', 'dyes/', 'waist/waist_',
  'faces/face_', 'shoes/shoe_', 'pets/pet_', 'collectibles/collect_', 'wrist/wrist_', 'charms/charm_', 'tickets/ticket_',
  'back/back_', 'head/head_', 'playercards/card_', 'emoji/emoji_', 'paints/paint_'];

// store.rarities (v10.js:102836); confCols / language / dark / animate are UI-only
const rarities = [
  { name: 'Uncommon', language: 'windows.spin.rarity.uncommon', rar: 60, color: '#b2f252', confC: 30, sell: 1, fee: 5 },
  { name: 'Rare', language: 'windows.spin.rarity.rare', rar: 40, color: '#2196F3', confC: 30, sell: 3, fee: 5 },
  { name: 'Epic', language: 'windows.spin.rarity.epic', rar: 25, color: '#E040FB', confC: 30, sell: 10, fee: 5 },
  { name: 'Legendary', language: 'windows.spin.rarity.legendary', rar: 10, color: '#FBC02D', confC: 40, sell: 100, fee: 10 },
  { name: 'Relic', language: 'windows.spin.rarity.relic', rar: 2.5, color: '#ed4242', confC: 50, sell: 500, fee: 10 },
  { name: 'Contraband', language: 'windows.spin.rarity.contraband', rar: 0.5, color: '#292929', confC: 70, dark: true, sell: 2500, fee: 25, scrap: 5 },
  { name: 'Unobtainable', language: 'windows.spin.rarity.unobtainable', rar: 0, color: '#fff53d', confC: 100, animate: true, sell: 10000, fee: 200, scrap: 50 },
  { name: 'NFT', language: 'windows.spin.rarity.nft', rar: 0, color: '#f3b8ff', confC: 0, sell: 10, fee: 500, noSpin: true },
];

// store.freeKR (v10.js:102911)
const freeKR = [{ kr: [5], rarity: 0 }, { kr: [10], rarity: 1 }, { kr: [20], rarity: 2 }, { kr: [500], rarity: 3 }];

// store.previews (v10.js:102924): by weapon, or "<weapon>_<mid>" for a remodel (getPreview)
const previews = {
  1: { xOff: -2, yOff: -0.6, zRota: 0.2, scl: 0.001376794727638135 },
  2: { xOff: -1.4, yOff: -0.8, scl: 0.001148941748743716 },
  3: { xOff: 0, yOff: -0.2, scl: 0.000676917226130652 },
  4: { xOff: -1.5, yOff: 0.5, scl: 0.000940741507537686 },
  5: { xOff: -0.8, yOff: 0.5, scl: 0.00079312502512562 },
  6: { xOff: -1, yOff: -1.1, scl: 0.00099521137688442 },
  '6_0': { xOff: -1, yOff: -1.1, scl: 1.2 },
  7: { xOff: -1, yOff: -0.9, scl: 0.001221518472361808 },
  8: { xOff: -1, yOff: -0.6, scl: 0.0012179208743718641 },
  9: { xOff: -1.2, yOff: -1, scl: 0.0010676876984924638 },
  10: { scl: 1.6 },
  14: { yOff: -2.5, scl: 0.0007874150753768881 },
  15: { scl: 0.00098426884422111 },
  16: { scl: 1.4, yOff: 1, xOff: -1 },
  17: { xOff: 0, yOff: -0.2, scl: 0.0003 },
  19: { xOff: -1.4, yOff: -0.8, scl: 0.0009095788844221084 },
  22: { xOff: 0, yOff: -0.2, scl: 0.0003158947055276376 },
  24: { xOff: 0.7, yOff: -0.87, zOff: -3.33, scl: 0.0010531966030150732 },
  25: { xOff: 0, yOff: 0, scl: 1 },
  28: { xOff: 0, yOff: -0.2, scl: 0.000676917226130652 },
  29: { xOff: -2, yOff: -2.6, zRota: 0.2, scl: 0.001376794727638135 },
};

// store.templates (v10.js:103040), the same four as v7.1.4
const templates = [
  { mid: 0, scl: 0.9, mScl: 1, zOff: 0.28, yOff: -0.5 },
  { mid: 0, noMuz: true, spnScl: 1.3, scl: 0.9, mScl: 0.84, zOff: 0, yOff: 0 },
  { flipAnim: true, id: 3, type: 3, scl: 2.6, sclMlt: 1, yOff: -1, xOff: -1.5, sxRot: 0.3, sxOff: 0.35, szOff: -0.25 },
  { id: 0, type: 7, sitOff: 1, sitOffZ: 0.005, trans: true, yOff: -0.7, xOff: 1.6, xRot: -0.5 },
];

// store.tags (v10.js:102800): limT -> label colour (applyShopMutations adds every other limT as null)
const tags = {
  Debug: '#000000', Easter: null, Twitch: '#6441a5', 'Clan Wars': null, Charity: '#6441a5', Raid: '#eb347a', Canteen: '#fe4002',
  Gift: null, Free: null, Christmas: null, Burg: '#ADE4F9', Sandstorm: '#ADE4F9', Littletown: '#ADE4F9', Site: '#ADE4F9',
  Industry: '#ADE4F9', Subzero: '#ADE4F9', Freight: '#ADE4F9', Undergrowth: '#ADE4F9', Lostworld: '#ADE4F9', Kanji: '#ADE4F9',
  Citadel: '#ADE4F9', Oasis: '#ADE4F9', Evacuation: '#ADE4F9', Craftable: '#cc51f8', NFT: '#f3b8ff', Ranked: '#ff5500',
  FRVR: 'linear-gradient(132deg, rgba(0,189,255,1) 0%, rgba(155,0,255,1) 25%, rgba(255,0,41,1) 50%, rgba(255,150,3,1) 75%, rgba(255,199,0,1) 100%)',
  Halloween: '#ff5500', 'Battle Pass': '#00BCD4', 'Premium Spin': '#fc038d',
};

// config (v10.js:16499 currentSeason, 18798 skinColors: v7.1.4's eight)
const config = {
  currentSeason: 10,
  skinColors: [
    { id: 0, col: 8412234, name: 'Bronze' }, { id: 1, col: 10975328, name: 'Sienna' }, { id: 2, col: 13864303, name: 'Rose' },
    { id: 3, col: 13408638, name: 'Tan' }, { id: 4, col: 15581094, name: 'Pale' }, { id: 5, col: 8492161, name: 'Infected' },
    { id: 6, col: 14919767, name: 'Yellowish' }, { id: 7, col: 2492161, name: 'Darker Color' },
  ],
};

function jspck() {
  const module = { exports: {} };
  vm.runInNewContext(fs.readFileSync(V7_JSPCK, 'utf8'), {
    module, exports: module.exports, require: () => ({}), Math, JSON, Object, Array, String, Number, parseInt, parseFloat,
    isNaN, RegExp, Error, TypeError,
  }, { filename: V7_JSPCK });
  if (typeof module.exports.unpack !== 'function') throw new Error(V7_JSPCK + ' did not export unpack()');
  return module.exports;
}

// JSON round trip: plain objects of this realm (the shim builds them in its own context)
function unpackFile(file, sha) {
  const text = fs.readFileSync(file, 'utf8');
  const got = require('crypto').createHash('sha256').update(text).digest('hex');
  if (sha && got !== sha) process.stderr.write(`[v10_store] ${file}: sha256 ${got}, pinned ${sha} (a newer live table?)\n`);
  return JSON.parse(JSON.stringify(jspck().unpack(text)));
}

// The raw store.skins array as the v10 client gets it from gapi /data/skins.
function rawSkins(file = V10_SKINS) {
  const skins = unpackFile(file, file === V10_SKINS && !process.env.KN_V10_SKINS ? V10_SKINS_SHA256 : null);
  if (!Array.isArray(skins)) throw new Error(file + ' is not an array');
  return skins;
}

// gapi /data/spins: {wheels, itemPools} (store.applySpinData)
function loadSpins(file = V10_SPINS) {
  const spins = unpackFile(file, file === V10_SPINS && !process.env.KN_V10_SPINS ? V10_SPINS_SHA256 : null);
  if (!spins || !Array.isArray(spins.wheels) || typeof spins.itemPools !== 'object') throw new Error(file + ': no wheels / itemPools');
  return spins;
}

// applyShopMutations(skin, index) (v10.js:103066). Idempotent, like v7's loop, so an already
// processed table (assets/skins.json) comes out unchanged. `thumbnail` (the asset host URL the
// server ships and the loop recomputes with getPreview) is dropped: it is not data.
function applyShopMutations(s, i, emojiMap) {
  s.index = i;
  s.seas = s.seas || 1;
  if (s.type == T.BACK) { s.assetType = 2; s.keyW = 'Back'; }
  if (s.type == T.HEAD) { s.assetType = 1; s.keyW = 'Head'; }
  if (s.creator) s.creators = s.creator.toLowerCase().split(', ');
  if (s.streamer) s.streamers = s.streamer.toLowerCase().split(', ');
  if (s.template != null) {
    const t = templates[s.template];
    for (const k in t) if (!(k == 'mid' && s[k] !== undefined)) s[k] = t[k]; // v10: a template keeps the item's own mid
  }
  if (s.free) { s.limT = 'Free'; s.noSale = true; s.noSpin = true; }
  if (s.itemShop) { s.noSale = s.itemShop == 1; s.noSpin = true; s.limited = true; s.limT = 'Item Shop'; }
  if (s.ranked) { s.noSale = true; s.noSpin = true; s.limited = true; s.noQuickSell = true; s.limT = 'Ranked'; }
  if (s.drop) { s.noSale = true; s.noQuickSell = true; if (!s.limT) s.limT = 'Twitch'; }
  if (s.mapReward != null) { s.noSpin = true; s.noSale = true; s.noQuickSell = true; if (!s.limT) s.limT = 'Map Reward'; }
  if (s.eventReward != null) { s.noSpin = true; s.noSale = true; s.noQuickSell = true; if (!s.limT) s.limT = 'Event Reward'; }
  if (s.package) { s.noSpin = true; s.noQuickSell = true; }
  if (s.challenge) { s.noSale = true; s.noSpin = true; s.noQuickSell = true; }
  if (s.craftable) { s.noSale = true; s.noSpin = true; s.noQuickSell = true; s.limT = 'Craftable'; }
  if (s.bundle != null) { s.noSale = true; s.noSpin = true; s.noQuickSell = true; s.limT = 'Bundle #' + (s.bundle || 0); }
  if (s.opensea) { s.noSpin = true; s.limT = 'NFT'; }
  if (s.type == T.COLLECTIBLE) s.noSpin = true;
  if (s.type == T.TICKET) { s.noSpin = true; s.side = 2; }
  if (s.type == T.PAINT) s.noSpin = true;
  if (s.battlepass) { s.limT = 'Battle Pass'; s.noSale = true; s.noSpin = true; s.noQuickSell = true; }
  if (s.premiumSpin) { s.limT = 'Premium Spin'; s.noSale = true; s.noSpin = true; s.noQuickSell = true; }
  if (s.type == T.PLAYER_CARD || s.type == T.EMOJI) s.texExt = 'webp';
  if (s.limT && !tags[s.limT]) tags[s.limT] = null;
  if (s.type == T.EMOJI && emojiMap) for (const t of (typeof s.text === 'string' ? [s.text] : s.text || [])) emojiMap[t] = i;
  delete s.thumbnail;
  if (s.noLegs || s.noBody || s.noHead) s.noComp = true;
}

// The v10 store over `skins` (default: the raw gapi table).
function loadStore(skins = null) {
  const list = skins || rawSkins();
  const emojiMap = {};
  list.forEach((s, i) => applyShopMutations(s, i, emojiMap));
  return { store: { skins: list, rarities, types, previews, templates, tags, freeKR, emojiMap }, itemTypes: T };
}

function loadConfig() { return config; }

// The asset-host files a store item needs, as the KrunkNative client resolves them
// (client/src/viewer/viewer_store.c preview = v10 getPreview v10.js:24328; viewer_scene.c item_keys
// / player_mesh.c weapon_skin_keys = players.js generateMeshData / getReModel). Each entry is
// {kind, path, optional}: optional files are ones the client tries and falls back from (a skin
// texture -> the model's own, the glow map, a shaderNum item's texture).
function itemAssetPaths(s) {
  const out = [];
  const add = (kind, path, optional) => out.push({ kind, path, optional: !!optional });
  const type = s.type || 0, dir = s.assetType || type;
  const ext = s.prevExt || s.texExt || 'png';
  if (type == T.PLAYER_CARD || type == T.SPRAY || type == T.EMOJI || type == T.PAINT) {
    add('preview', `textures/${types[type]}${s.id}.${ext}`);
    return out; // the texture is the preview
  }
  const tail = s.pat != null ? '_c' + s.pat : '';
  if (type && type != T.MELEE) add('preview', `textures/previews/cosmetics/${dir}_${s.id}${tail || (s.tex ? '_' + s.tex : '')}.${ext}`);
  else if (type == T.MELEE) add('preview', `textures/previews/${types[3]}${s.id}${tail || (s.tex != null ? '_' + s.tex : '')}.${ext}`);
  else {
    const t = s.mid != null ? 'm' + s.mid + (s.midT != null ? '_' + s.midT : '') : s.pat != null ? 'c' + s.pat : s.tex ? s.tex : s.id;
    add('preview', `textures/previews/${types[0]}${s.weapon || 0}_${t}.${ext}`);
  }
  if (type == T.DYE) return out;
  const model = types[dir] + (dir ? (s.id > 0 ? s.id : 0) : s.weapon) + (s.mid != null ? '_' + s.mid : '');
  if (s.hasAnim) {
    add('model', `models/${model}.${s.fileExt || 'gltf'}`);
    return out;
  }
  if (type || s.mid != null) add('model', `models/${model}.obj`); // else the weapon's own model
  let tex = null;
  if (s.mid != null) tex = `weapons/weapon_${s.weapon}_${s.midT > 0 ? s.midT : s.mid}`;
  else if (!type) tex = s.tex ? String(s.tex) : `weapons/skins/weapon_${s.weapon}_${s.id}`;
  else if (type == T.MELEE) tex = s.pat != null && s.tex ? String(s.tex) : `melee/melee_${s.id > 0 ? s.id : 0}${s.tex ? '_' + s.tex : ''}`;
  else if (type) tex = s.tex ? `${types[dir]}${s.id}_${s.tex}` : model;
  const texOptional = s.shaderNum != null || s.video != null;
  if (tex) {
    add('texture', `textures/${tex}.${s.texExt || 'png'}`, texOptional || tex !== model);
    if (s.glow && !s.sameGlow) add('texture', `textures/${tex}_e.png`, true);
  }
  return out;
}

module.exports = { V10_SKINS, V10_SPINS, T, rawSkins, loadSpins, loadStore, loadConfig, applyShopMutations, itemAssetPaths };
