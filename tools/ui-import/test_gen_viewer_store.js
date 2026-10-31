#!/usr/bin/env node
// Checks client/src/viewer/viewer_store_table.c and shared/src/store_spins_table.c
// (gen_viewer_store.js), the client's store table that the player model, the store / inventory UI
// and the viewer read (doc/v7/player-viewer.md WP-F), and the v10 prize wheels kn_brain rolls:
//   - 8153 skins (the live table of v10.0.6, 2026-10-30: Scarecrow Peg added) in v10's 19 item types, with v10's per-type counts;
//   - index stability: every store index of v7.1.4's 6448 skins keeps its item (id, weapon, type,
//     tex / mid; names too except one v10 rename) so owned inventory rows keep their meaning, and
//     every index of v3.1.1's 1917 keeps its item and type, except v7's HAT 1 -> HEAD 15 and
//     BODY 2 -> BACK 14 retypes;
//   - the assetType remap: BACK (14) items load from body/ (2), HEAD (15) items from hats/ (1);
//   - the player-model fields: dyes, shoes' legLen / randomLeg, pets, charm mounts;
//   - the v10 fields (paint ids, prevExt, tradable, remodel previews) and the wheels: Universal
//     tickets, s10Pool pools of v10 items.
// Reads the generated C files, assets/skins.json and the v3.1.1 / v7.1.4 reference trees (read-only).
//   node tools/ui-import/test_gen_viewer_store.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const REPO = path.resolve(__dirname, '..', '..');
const V3_STORE = '/home/user/jails/kru/src/Krunker_v3.1.1/src/data/store.js';
const TABLE = path.join(REPO, 'client/src/viewer/viewer_store_table.c');
const SPINS = path.join(REPO, 'shared/src/store_spins_table.c');

let failures = 0, checks = 0;
const check = (cond, msg) => { checks++; if (!cond) { failures++; console.log('FAIL: ' + msg); } };

const c = fs.readFileSync(TABLE, 'utf8');
const rowRe = /^    \/\* (\d+) \*\/ \{(.*)\},$/gm;
const rows = [];
for (let m; (m = rowRe.exec(c));) rows.push({ index: +m[1], body: m[2] });
const field = (row, name) => { const m = row.body.match(new RegExp('\\.' + name + ' = ("(?:[^"\\\\]|\\\\.)*"|[^,]+)')); return m ? m[1] : undefined; };
const int = (row, name, d) => { const v = field(row, name); return v === undefined ? d : parseFloat(v); };
// a C string literal back to text: \ooo escapes are UTF-8 bytes (gen_viewer_store.js cstr)
const cname = (row) => {
  const v = field(row, 'name');
  if (v === undefined) return null;
  const latin1 = v.slice(1, -1).replace(/\\([0-7]{3})/g, (_, o) => String.fromCharCode(parseInt(o, 8))).replace(/\\(["\\])/g, '$1');
  return Buffer.from(latin1, 'latin1').toString('utf8');
};

// ---- counts
check(rows.length === 8153, `8153 skins (${rows.length})`);
check(rows.every((r, i) => r.index === i), 'rows are in store-index order');
check(/const int g_viewer_skin_count = 8153;/.test(c), 'g_viewer_skin_count 8153');
check(/const int g_viewer_current_season = 10;/.test(c), 'config.currentSeason 10');
const V10_COUNTS = { 0: 4833, 1: 312, 2: 181, 3: 405, 4: 556, 5: 45, 6: 104, 7: 668, 8: 88, 9: 27, 10: 31, 11: 13, 12: 49, 13: 22, 14: 277, 15: 410, 16: 85, 17: 28, 18: 19 };
const byType = {};
for (const r of rows) { const t = int(r, 'type', 0); byType[t] = (byType[t] || 0) + 1; }
check(Object.keys(byType).length === 19, `19 item types (${Object.keys(byType).length})`);
for (const t of Object.keys(V10_COUNTS)) check(byType[t] === V10_COUNTS[t], `type ${t}: ${V10_COUNTS[t]} items (${byType[t]})`);
const types = c.match(/const char \*const g_viewer_types\[\] = \{(.*)\};/);
check(types && types[1].split(',').length === 19 && types[1].includes('"back/back_"') && types[1].includes('"emoji/emoji_"') && types[1].includes('"paints/paint_"'), 'store.types: 19 asset dirs');

// ---- index stability against v7.1.4 (inventory skin_id = store index): same id, weapon, type,
// tex, pat, mid, midT, assetType; names too, but for v10's one rename
const { loadStore: loadV7 } = require('./v7_store.js');
const v7skins = loadV7().store.skins;
check(v7skins.length === 6448, `v7.1.4 has 6448 skins (${v7skins.length})`);
const V10_RENAMES = { 5815: ['Tehchy Trikot', 'Trikot'] };
let v7moved = 0, v7renamed = 0;
const skinsNow = JSON.parse(fs.readFileSync(path.join(REPO, 'assets/skins.json'), 'utf8'));
for (let i = 0; i < v7skins.length; i++) {
  const a = v7skins[i], b = skinsNow[i] || {};
  const same = ['id', 'weapon', 'tex', 'pat', 'mid', 'midT', 'assetType'].every((k) => JSON.stringify(a[k]) === JSON.stringify(b[k])) && (a.type || 0) === (b.type || 0);
  if (!same) { v7moved++; if (v7moved <= 5) console.log(`  index ${i}: v7 ${a.name} -> v10 ${b.name}`); }
  if (a.name !== b.name) { v7renamed++; if (!V10_RENAMES[i] || V10_RENAMES[i][0] !== a.name || V10_RENAMES[i][1] !== b.name) { v7moved++; console.log(`  index ${i}: renamed ${a.name} -> ${b.name}`); } }
}
check(v7moved === 0, `indices 0-6447 keep v7.1.4's item (${v7moved} differ)`);
check(v7renamed === 1, `one v10 rename (5815 Tehchy Trikot -> Trikot) (${v7renamed})`);

// ---- index stability against v3.1.1 (store.js module.exports.skins)
const v3 = { exports: {} };
vm.runInNewContext(fs.readFileSync(V3_STORE, 'utf8'), { module: v3, exports: v3.exports, require: () => ({}), Math, JSON, console });
const v3skins = v3.exports.skins;
check(Array.isArray(v3skins) && v3skins.length === 1917, `v3.1.1 has 1917 skins (${v3skins && v3skins.length})`);
let moved = 0, retyped = 0;
for (let i = 0; i < 1914; i++) {
  const a = v3skins[i] || {}, r = rows[i];
  const t3 = a.type || 0, t7 = int(r, 'type', 0);
  const same = (a.id == null ? -1 : a.id) === int(r, 'id', -1) && (a.weapon || 0) === int(r, 'weapon', 0);
  const typeOk = t3 === t7 || (t3 === 1 && t7 === 15) || (t3 === 2 && t7 === 14);
  if (t3 !== t7) retyped++;
  if (!same || !typeOk) { moved++; if (moved <= 5) console.log(`  index ${i}: v3 ${a.name}/${t3}/${a.id} -> ${cname(r)}/${t7}/${int(r, 'id', -1)}`); }
}
check(moved === 0, `indices 0-1913 keep their item (${moved} moved)`);
check(retyped === 185, `185 v3.1.1 items retyped HAT -> HEAD / BODY -> BACK (${retyped})`);

// ---- assetType remap
const bad = rows.filter((r) => {
  const t = int(r, 'type', 0), a = int(r, 'asset_type', 0);
  return t === 14 ? a !== 2 : t === 15 ? a !== 1 : a !== 0;
});
check(bad.length === 0, `assetType: BACK -> 2, HEAD -> 1, none elsewhere (${bad.length} wrong)`);

// ---- the same table as assets/skins.json (names, types)
const skins = JSON.parse(fs.readFileSync(path.join(REPO, 'assets/skins.json'), 'utf8'));
check(skins.length === rows.length, 'assets/skins.json has the same length');
let diff = 0;
for (let i = 0; i < skins.length; i++) if ((skins[i].type || 0) !== int(rows[i], 'type', 0) || skins[i].name !== cname(rows[i])) diff++;
check(diff === 0, `every row matches assets/skins.json name / type (${diff} differ)`);

// ---- v7 player-model fields
const dyeRows = (c.match(/const ViewerDye g_viewer_dyes\[\] = \{([\s\S]*?)\n\};/) || [, ''])[1].trim().split('\n').filter(Boolean);
check(dyeRows.length === 45, `45 dye entries (${dyeRows.length})`);
check(/\{2897, \{\{0, [^}]*\}\}, \{3, \{3882027, 6315847, 5131065, -1\}\}/.test(c), 'Woodland dye keeps its 3 shirt colours (random pick)');
check(/\/\* 2860 \*\/.*VS2_RANDOM_LEG.*\.leg_len = 0\.22f/.test(c), 'Peg shoes: randomLeg, legLen 0.22');
check(/\/\* 4249 \*\/.*\.type = 11,.*VS_HAS_ANIM.*VS2_NO_LEFT_SLEEVE/.test(c), 'Kronix Gold wrist: hasAnim, noLeftSleeve');
check(/\/\* 1735 \*\/.*VS2_SHADER.*\.shader_num = 0/.test(c), 'shaderNum 0 kept (VS2_SHADER)');
check(rows.filter((r) => int(r, 'type', 0) === 9).length === 27, '27 pets');
const mounts = (c.match(/const ViewerCharmMount g_viewer_charm_mounts\[VIEWER_CHARM_WEAPONS\] = \{([\s\S]*?)\n\};/) || [, ''])[1].trim().split('\n').filter(Boolean);
check(mounts.length === 23, `23 weapons take a charm (${mounts.length})`);

// ---- v10 fields
check(/\/\* 8050 \*\/ \{\.name = "Red Paint"|\.name = "Red Paint", \.type = 18,.*\.id_name = "red"/.test(c), 'paints keep their string id (Red Paint: id_name "red")');
check(/\/\* 7803 \*\/.*\.prev_ext = "webp"/.test(c), 'prevExt (KrunkCup band preview is .webp)');
check(/\/\* 7937 \*\/.*\.file_ext = "glb".*VS2_TRADABLE/.test(c), 'V-CORE 01: glb model, tradable');
check(/\/\* 7371 \*\/.*VS2_FORCE_ATTACH/.test(c) && /\/\* 7371 \*\/.*\.m_scl = 0\.000574897587939697f/.test(c), 'string numbers / "true" of the v10 table (P90 7371)');
check(/\{6, 0, \{\.set = 1, .*\.scl = 1\.2f/.test(c), 'store.previews["6_0"]: the remodel preview');
check(rows.filter((r) => /VS2_LIMITED_SPIN/.test(r.body)).length === 117, '117 limitedSpin items');

// ---- v10 wheels and pools (gapi /data/spins)
const sc = fs.readFileSync(SPINS, 'utf8');
const lit = sc.slice(sc.indexOf('g_store_spins_json[] =') + 22).split('\n').map((l) => l.trim()).filter((l) => l.startsWith('"'))
  .map((l) => l.replace(/;$/, '').slice(1, -1)).join('');
const spins = JSON.parse(Buffer.from(lit.replace(/\\([0-7]{3})/g, (_, o) => String.fromCharCode(parseInt(o, 8))).replace(/\\(["\\])/g, '$1'), 'latin1').toString('utf8'));
check(spins.season === 10 && spins.wheels.length === 14, `14 wheels, season 10 (${spins.wheels.length}, ${spins.season})`);
const w = (n) => spins.wheels.find((x) => x.name === n) || {};
check(spins.wheels[0].name === 'Starter' && w('Starter').ticketIndex === 8067 && w('Starter').price === 50, 'Starter: 50 KR or the Universal Starter Ticket 8067');
check(spins.wheels[1].name === 'Elite' && w('Elite').ticketIndex === 8068 && w('Elite').ticketOnly, 'Elite: ticket only (Universal Elite Ticket 8068)');
check(spins.wheels[2].name === 'Heroic' && w('Heroic').ticketIndex === 7993 && w('Heroic').price === 500, 'Heroic: 500 KR or the Universal Spin Ticket 7993');
check(w('Ultra').ticketIndex === 7510 && w('Ultra').ticketOnly, 'Ultra: Season 9 Ultra Ticket 7510');
for (const [t, id] of [[8067, 0], [8068, 1], [7993, 2]]) check(skinsNow[t].type === 13 && skinsNow[t].spinWheelId === id, `ticket ${t} -> store.wheels[${id}]`);
const s10 = Object.keys(spins.itemPools).filter((k) => k.startsWith('s10Pool'));
check(s10.length === 7, `7 s10Pool pools (${s10.length})`);
const s10items = s10.flatMap((k) => spins.itemPools[k].items);
check(s10items.every((i) => i >= 6448 && i < 8153 && skinsNow[i]), `every s10Pool item is a v10 store item (${s10items.length})`);
check(s10.every((k) => spins.itemPools[k].items.every((i) => skinsNow[i].rarity === ['Uncommon', 'Rare', 'Epic', 'Legendary', 'Relic', 'Contraband', 'Unobtainable'].indexOf(spins.itemPools[k].name))),
  "an s10Pool's items have its rarity");
check(Object.values(spins.itemPools).every((p) => p.items.every((i) => i >= 0 && i < 8153)), 'every pool item is in the table');

console.log(failures ? `test_gen_viewer_store: ${failures} of ${checks} checks failed` : `test_gen_viewer_store: OK (${checks} checks)`);
process.exit(failures ? 1 : 0);
