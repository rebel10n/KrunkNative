#!/usr/bin/env node
// Checks client/src/viewer/viewer_store_table.c (gen_viewer_store.js), the client's store table
// that the player model, the store / inventory UI and the viewer read (doc/v7/player-viewer.md WP-F):
//   - 6448 skins in v7's 17 item types, with v7.1.4's per-type counts;
//   - index stability: every store index of v3.1.1's 1917 skins keeps its item (name, weapon,
//     id) and type, except v7's HAT 1 -> HEAD 15 and BODY 2 -> BACK 14 retypes;
//   - the assetType remap: BACK (14) items load from body/ (2), HEAD (15) items from hats/ (1);
//   - the v7 player-model fields: dyes, shoes' legLen / randomLeg, pets, charm mounts.
// Reads the generated C file, assets/skins.json and the v3.1.1 / v7.1.4 reference trees (read-only).
//   node tools/ui-import/test_gen_viewer_store.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const REPO = path.resolve(__dirname, '..', '..');
const V3_STORE = '/home/user/jails/kru/src/Krunker_v3.1.1/src/data/store.js';
const TABLE = path.join(REPO, 'client/src/viewer/viewer_store_table.c');

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
check(rows.length === 6448, `6448 skins (${rows.length})`);
check(rows.every((r, i) => r.index === i), 'rows are in store-index order');
check(/const int g_viewer_skin_count = 6448;/.test(c), 'g_viewer_skin_count 6448');
const V7_COUNTS = { 0: 3904, 1: 270, 2: 131, 3: 280, 4: 466, 5: 30, 6: 71, 7: 544, 8: 59, 9: 13, 10: 26, 11: 1, 12: 7, 13: 9, 14: 229, 15: 362, 16: 46 };
const byType = {};
for (const r of rows) { const t = int(r, 'type', 0); byType[t] = (byType[t] || 0) + 1; }
check(Object.keys(byType).length === 17, `17 item types (${Object.keys(byType).length})`);
for (const t of Object.keys(V7_COUNTS)) check(byType[t] === V7_COUNTS[t], `type ${t}: ${V7_COUNTS[t]} items (${byType[t]})`);
const types = c.match(/const char \*const g_viewer_types\[\] = \{(.*)\};/);
check(types && types[1].split(',').length === 17 && types[1].includes('"back/back_"') && types[1].includes('"head/head_"'), 'store.types: 17 asset dirs');

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
check(dyeRows.length === 30, `30 dye entries (${dyeRows.length})`);
check(/\{2897, \{\{0, [^}]*\}\}, \{3, \{3882027, 6315847, 5131065, -1\}\}/.test(c), 'Woodland dye keeps its 3 shirt colours (random pick)');
check(/\/\* 2860 \*\/.*VS2_RANDOM_LEG.*\.leg_len = 0\.22f/.test(c), 'Peg shoes: randomLeg, legLen 0.22');
check(/\/\* 4249 \*\/.*\.type = 11,.*VS_HAS_ANIM.*VS2_NO_LEFT_SLEEVE/.test(c), 'Kronix Gold wrist: hasAnim, noLeftSleeve');
check(/\/\* 1735 \*\/.*VS2_SHADER.*\.shader_num = 0/.test(c), 'shaderNum 0 kept (VS2_SHADER)');
check(rows.filter((r) => int(r, 'type', 0) === 9).length === 13, '13 pets');
const mounts = (c.match(/const ViewerCharmMount g_viewer_charm_mounts\[VIEWER_CHARM_WEAPONS\] = \{([\s\S]*?)\n\};/) || [, ''])[1].trim().split('\n').filter(Boolean);
check(mounts.length === 23, `23 weapons take a charm (${mounts.length})`);

console.log(failures ? `test_gen_viewer_store: ${failures} of ${checks} checks failed` : `test_gen_viewer_store: OK (${checks} checks)`);
process.exit(failures ? 1 : 0);
