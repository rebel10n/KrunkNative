#!/usr/bin/env node
// Generates client/src/viewer/viewer_store_table.c: the Krunker v10.0.2 store data the client
// reads (viewer page, store / inventory / spin wheel / market / loadout UI, weapon and cosmetic
// skins): every store.skins entry, rarities, types, previews (the v10 store module) and
// currentSeason / skinColors (v10 config), plus the viewer's dropdown option lists computed
// with v7 src/viewer.js's own sorting (f460 / f459 / the melee list); and
// shared/src/store_spins_table.c: v10's prize wheels and item pools (gapi /data/spins), which
// kn_brain (services/brain/store.c) and the client parse (shared/include/store_spins.h).
//
// The skins are assets/skins.json (the table kn_brain and the bots use, written by
// game_assets.py from v10_skins.js), run through v10's applyShopMutations again (idempotent) so
// the table is exactly v10's store.skins; everything else is v10's store module (v10_store.js).
//   node tools/ui-import/gen_viewer_store.js [assets/skins.json] [out.c] [spins out.c]
// (The v7.1.4 table came from v7_store.js, the v3.1.1 one from the v3 viewer.js.map; see git history.)
'use strict';
const fs = require('fs');
const path = require('path');
const { loadStore, loadConfig, loadSpins } = require('./v10_store.js');

const REPO = path.resolve(__dirname, '..', '..');
// viewer_page.py (tools/ui-import/import.py) still passes the v3.1.1 viewer.js.map: ignore a .map
// argument.
const args = process.argv.slice(2).filter((a) => !a.endsWith('.map'));
const SKINS = args[0] || path.join(REPO, 'assets/skins.json');
const OUT = args[1] || path.join(REPO, 'client/src/viewer/viewer_store_table.c');
const SPINS_OUT = args[2] || path.join(REPO, 'shared/src/store_spins_table.c');

const { store } = loadStore(JSON.parse(fs.readFileSync(SKINS, 'utf8')));
const config = loadConfig();
const spins = loadSpins();

const cstr = (s) => s == null ? 'NULL' : '"' + String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/[^\x20-\x7e]/g, (c) => {
  return Array.from(Buffer.from(c, 'utf8')).map((b) => '\\' + b.toString(8).padStart(3, '0')).join('');
}) + '"';
// v10's table has a few numbers as strings ("mScl": "0.0005", P90 7371); JS arithmetic takes them
const num = (v, d) => (typeof v === 'number' && isFinite(v)) ? v
  : (typeof v === 'string' && v.trim() !== '' && isFinite(Number(v))) ? Number(v) : d;
const truthy = (v) => !!v && v !== 'false';
const f = (v) => { const s = String(v); return (s.includes('.') || s.includes('e') ? s : s + '.0') + 'f'; };
const col = (v) => typeof v === 'number' ? v : (typeof v === 'string' && /^#?[0-9a-f]{6}$/i.test(v) ? parseInt(v.replace('#', ''), 16) : -1);

const rows = store.skins.map((s, i) => {
  if (s.index !== i) throw new Error('skin index mismatch at ' + i);
  const tex = s.tex == null ? null : String(s.tex);
  const flags = [];
  const flag = (cond, name) => { if (cond) flags.push(name); };
  flag(s.glow, 'VS_GLOW'); flag(s.sameGlow, 'VS_SAME_GLOW'); flag(s.noEmis, 'VS_NO_EMIS');
  flag(s.trans, 'VS_TRANS'); flag(s.wire, 'VS_WIRE'); flag(s.blocked, 'VS_BLOCKED');
  flag(s.is3D, 'VS_IS_3D'); flag(s.flip, 'VS_FLIP'); flag(s.doubleSide, 'VS_DOUBLE_SIDE');
  flag(s.lowR, 'VS_LOW_R'); flag(s.noHead, 'VS_NO_HEAD'); flag(s.noLegs, 'VS_NO_LEGS');
  flag(s.pat != null, 'VS_PAT'); flag(typeof s.tex === 'number', 'VS_TEX_NUM'); flag(s.rgb, 'VS_RGB');
  flag(s.noSpin, 'VS_NO_SPIN');
  // store.getSpnItems: limited / blackM / opensea / limT (limType) / limG items are never in the
  // pool of a wheel without limType / limGroup / tag (illicit is v3.1.1's)
  flag(s.limited || s.illicit || s.limT || s.blackM || s.opensea || s.limG, 'VS_NOT_IN_WHEEL');
  flag(s.free, 'VS_FREE'); flag(s.hasAnim, 'VS_HAS_ANIM');
  const fields = [
    `.name = ${cstr(s.name)}`,
    `.type = ${num(s.type, 0)}`, `.weapon = ${num(s.weapon, 0)}`, `.id = ${typeof s.id === 'string' && num(s.id, null) === null ? -1 : num(s.id, -1)}`,
    `.rarity = ${num(s.rarity, 0)}`, `.seas = ${num(s.seas, 0)}`,
    `.tex = ${cstr(tex)}`, `.pat = ${num(s.pat, -1)}`, `.mid = ${num(s.mid, -1)}`, `.mid_t = ${num(s.midT, -1)}`,
    `.flags = ${flags.length ? flags.join(' | ') : '0'}`,
  ];
  if (typeof s.id === 'string' && num(s.id, null) === null) fields.push(`.id_name = ${cstr(s.id)}`); // paints: "red"
  if (s.creator) fields.push(`.creator = ${cstr(s.creator)}`);
  if (s.assetType != null) fields.push(`.asset_type = ${s.assetType}`);
  if (s.texExt) fields.push(`.tex_ext = ${cstr(s.texExt)}`);
  if (s.prevExt) fields.push(`.prev_ext = ${cstr(s.prevExt)}`);
  if (s.fileExt) fields.push(`.file_ext = ${cstr(s.fileExt)}`);
  if (s.hasAnim) fields.push(`.anim_s = ${f(num(s.animS, 1))}`);
  if (s.env || s.sameEnv) fields.push(`.env = ${f(num(s.env, 3))}`);
  const opt = (key, cname, d) => { if (s[key] !== undefined && s[key] !== null && num(s[key], d) !== d) fields.push(`.${cname} = ${f(num(s[key], d))}`); };
  opt('xOff', 'x_off', 0); opt('yOff', 'y_off', 0); opt('zOff', 'z_off', 0);
  opt('mScl', 'm_scl', 0); opt('scl', 'scl', 0); opt('spnScl', 'spn_scl', 0);
  opt('xRot', 'x_rot', 0); opt('zRota', 'z_rota', 0); opt('sclMlt', 'scl_mlt', 0);
  opt('sitOff', 'sit_off', 0); opt('sitOffZ', 'sit_off_z', 0);
  opt('sxOff', 'sx_off', 0); opt('syOff', 'sy_off', 0); opt('szOff', 'sz_off', 0); opt('sxRot', 'sx_rot', 0);
  opt('alpha', 'alpha', 0); opt('prevRot', 'prev_rot', 0); opt('tileS', 'tile_s', 0);
  if (s.opac != null) fields.push(`.opac = ${f(s.opac)}`);
  if (s.opacity != null) fields.push(`.opacity = ${f(s.opacity)}`);
  fields.push(`.emiss = ${s.emiss != null ? col(s.emiss) : -1}`, `.color = ${s.color != null ? col(s.color) : -1}`);
  if (s.side) fields.push(`.side = ${s.side}`);
  if (s.frames) fields.push(`.frames = ${s.frames}`, `.frame_t = ${num(s.frameT, 0)}`);
  if (s.rAnim) fields.push(`.r_anim = ${f(s.rAnim)}`, `.r_anim_axis = '${(s.rAnimA || 'y').toLowerCase()}'`);
  if (s.movT != null) fields.push(`.mov_t = ${f(s.movT)}`);
  if (s.pulsT != null) fields.push(`.puls_t = ${f(s.pulsT === true ? 1 : num(s.pulsT, 0))}`); // one skin has pulsT: true
  // v7.1.4 player-model and material-effect fields (players.js generateMeshes / generateMeshData,
  // playerMesh.js, pets.js, render.js getMat): flags2 + the values below
  const flags2 = [];
  const flag2 = (cond, name) => { if (cond) flags2.push(name); };
  flag2(s.noBody, 'VS2_NO_BODY'); flag2(s.noHair, 'VS2_NO_HAIR'); flag2(s.randomLeg, 'VS2_RANDOM_LEG');
  flag2(s.noSleeve, 'VS2_NO_SLEEVE'); flag2(s.noLeftSleeve, 'VS2_NO_LEFT_SLEEVE'); flag2(s.noRightHand, 'VS2_NO_RIGHT_HAND');
  flag2(s.noCharm, 'VS2_NO_CHARM'); flag2(s.projection, 'VS2_PROJECTION'); flag2(s.useSkin, 'VS2_USE_SKIN');
  flag2(s.shaderNum != null, 'VS2_SHADER'); flag2(s.video != null, 'VS2_VIDEO'); flag2(s.particle != null, 'VS2_PARTICLE');
  flag2(s.snd, 'VS2_SND'); flag2(s.killSnd, 'VS2_KILL_SND'); flag2(s.stepSnd, 'VS2_STEP_SND'); flag2(s.jumpSnd, 'VS2_JUMP_SND');
  flag2(s.reloadSnd, 'VS2_RELOAD_SND'); flag2(s.spraySnd != null, 'VS2_SPRAY_SND'); flag2(s.explode, 'VS2_EXPLODE');
  flag2(s.sameEnv, 'VS2_SAME_ENV'); flag2(truthy(s.forceAttach), 'VS2_FORCE_ATTACH');
  flag2(truthy(s.noSnd), 'VS2_NO_SND'); // a remodel that keeps the weapon's shot sound (v10.0.3 shoot, deob-10.0.3.js:196403)
  // v10.0.2: the server's tradable bit, limitedSpin (a noSpin item a wheel may still draw),
  // premiumSpin, paint consumables / paintable items, item sets
  flag2(s.tradable, 'VS2_TRADABLE'); flag2(s.limitedSpin, 'VS2_LIMITED_SPIN'); flag2(s.premiumSpin, 'VS2_PREMIUM_SPIN');
  flag2(s.paintTargets, 'VS2_PAINTABLE'); flag2(s.noComp, 'VS2_NO_COMP');
  if (flags2.length) fields.push(`.flags2 = ${flags2.join(' | ')}`);
  if (s.shaderNum != null) fields.push(`.shader_num = ${s.shaderNum}`);
  if (s.video != null) fields.push(`.video = ${s.video}`);
  if (s.particle != null) fields.push(`.particle = ${s.particle}`);
  if (s.spraySnd != null) fields.push(`.spray_snd = ${num(s.spraySnd, 0)}`);
  if (s.desc) fields.push(`.desc = ${cstr(s.desc)}`);
  if (s.paintColor) fields.push(`.paint_color = ${cstr(s.paintColor)}`);
  // v10.0.3 paintable items: the paintMask texture (loadMesh's paintMaskMap) and a paintTargets mesh list
  if (s.paintMask && s.paintMask.src) {
    fields.push(`.paint_mask = ${cstr(s.paintMask.src)}`);
    const ext = s.paintMask.data && s.paintMask.data.texExt;
    if (ext && ext !== 'png') fields.push(`.paint_mask_ext = ${cstr(ext)}`);
  }
  if (Array.isArray(s.paintTargets)) fields.push(`.paint_targets = ${cstr(s.paintTargets.map(String).join(','))}`);
  if (s.title != null) fields.push(`.title = ${num(s.title, 0)}`);
  if (s.set) fields.push(`.set = ${cstr(s.set)}`, `.set_min = ${num(s.setMin, 0)}`);
  opt('legLen', 'leg_len', 0); opt('sitOffX', 'sit_off_x', 0); opt('syRot', 'sy_rot', 0); opt('szRot', 'sz_rot', 0);
  opt('glowInt', 'glow_int', 0); opt('minGlo', 'min_glo', 0); opt('rgbS', 'rgb_s', 0); opt('rgbT', 'rgb_t', 0);
  opt('rAnimT', 'r_anim_t', 0); opt('rAnimM', 'r_anim_m', 0); opt('movD', 'mov_d', 0); opt('movE', 'mov_e', 0);
  opt('rotT', 'rot_t', 0); opt('spd', 'spd', 0);
  const hold = (k) => num(s[k], 0) || 0;
  if (['leftHoldX', 'leftHoldY', 'leftHoldZ', 'rightHoldX', 'rightHoldY', 'rightHoldZ'].some((k) => num(s[k], 0))) {
    fields.push(`.left_hold = {${f(hold('leftHoldX'))}, ${f(hold('leftHoldY'))}, ${f(hold('leftHoldZ'))}}`);
    fields.push(`.right_hold = {${f(hold('rightHoldX'))}, ${f(hold('rightHoldY'))}, ${f(hold('rightHoldZ'))}}`);
  }
  return `    /* ${i} */ {${fields.join(', ')}},`;
});

// Dyes (type 5): the colour overrides of players.js generateMeshes. A colour may be a list the
// client picks from at random per model (UTILS.getRandom); up to 4 entries are kept.
const DYE_PARTS = ['skinCol', 'shirtCol', 'sleeveCol', 'pantsCol', 'waistCol', 'shoeCol', 'hairCol'];
const dyeRows = store.skins.filter((s) => s.type === 5).map((s) => {
  const parts = DYE_PARTS.map((k) => {
    const v = s[k];
    const list = v == null ? [] : (Array.isArray(v) ? v : [v]).map(col).filter((c) => c >= 0).slice(0, 4);
    return `{${list.length}, {${[...list, ...[-1, -1, -1, -1]].slice(0, 4).join(', ')}}}`;
  });
  return `    {${s.index}, {${parts.join(', ')}}},`;
});

// The weapons' charm mounts (weapons.js charm, charmM{X,Y,Z}Off, charmMScl, charm{X,Y,Z}Off,
// charmScl) from the v10 weapon dump shared/tests/data/v10/weapons.json, by weapon id.
const weaponsJson = JSON.parse(fs.readFileSync(path.join(REPO, 'shared/tests/data/v10/weapons.json'), 'utf8'));
const charmRows = weaponsJson.map((w, id) => w && w.charm ? `    [${id}] = {.set = 1, .mount = {${f(num(w.charmMXOff, 0))}, ${f(num(w.charmMYOff, 0))}, ${f(num(w.charmMZOff, 0))}}, .mount_scl = ${f(num(w.charmMScl, 1))}, .offset = {${f(num(w.charmXOff, 0))}, ${f(num(w.charmYOff, 0))}, ${f(num(w.charmZOff, 0))}}, .scl = ${f(num(w.charmScl, 1))}},` : null).filter(Boolean);

const previewInit = (p) => `.set = 1, .x_off = ${f(num(p.xOff, 0))}, .y_off = ${f(num(p.yOff, 0))}, .z_rota = ${f(num(p.zRota, 0))}, .scl = ${f(num(p.scl, 0))}, .x_rot = ${f(num(p.xRot, 0))}`;
const previews = Object.keys(store.previews).filter((w) => /^\d+$/.test(w)).map((w) => `    [${w}] = {${previewInit(store.previews[w])}},`);
// v10 getPreview: store.previews["<weapon>_<mid>"] for a remodel first
const remodelPreviews = Object.keys(store.previews).filter((w) => /^\d+_\d+$/.test(w)).map((w) => {
  const [weapon, mid] = w.split('_');
  return `    {${weapon}, ${mid}, {${previewInit(store.previews[w])}}},`;
});

// The option lists of the viewer's dat.gui dropdowns, computed like v7 src/viewer.js (f460(type),
// f459(weapon), the melee list; not dev mode) for every season value, so the order (three
// stable sorts, then reverse) and the duplicate-key collapsing of the options object are V8's.
// Each option is {label skin index, value}: the label is the first skin with that
// "name [rarity|Free]" key, the value (skin index) the last one, as object assignment does.
function optionList(items) {
  const o = { Default: -1 };
  const label = {};
  for (const n of items) {
    const key = n.name + ' [' + (n.free ? 'Free' : store.rarities[n.rarity].name) + ']';
    if (!(key in o)) label[key] = n.index;
    o[key] = n.index;
  }
  return Object.keys(o).map((k) => k === 'Default' ? [-1, -1] : [label[k], o[k]]);
}
function sorted(filter, season) {
  return store.skins.filter((e) => e && filter(e) && (season == 0 || season == (e.seas || 1)) && !e.blocked)
    .sort((e, t) => (e.id > t.id ? 1 : t.id > e.id ? -1 : 0))
    .sort((e, t) => ((e.rarity || 0) > (t.rarity || 0) ? 1 : (t.rarity || 0) > (e.rarity || 0) ? -1 : 0))
    .sort((e, t) => (e.seas > t.seas ? 1 : t.seas > e.seas ? -1 : 0))
    .reverse();
}
const lists = [];
const seasons = Array.from({ length: config.currentSeason + 1 }, (_, i) => i);
// v7 viewer folders: hat 1, body 2, dye 5, waist 6, face 7, shoes 8, pet 9, wrist 11, back 14, head 15
for (const type of [1, 2, 5, 6, 7, 8, 9, 11, 14, 15]) for (const season of seasons) {
  lists.push({ kind: 'VIEWER_LIST_TYPE', key: type, season, opts: optionList(sorted((t) => t.type == type, season)) });
}
for (const season of seasons) {
  lists.push({ kind: 'VIEWER_LIST_MELEE', key: 3, season, opts: optionList(sorted((e) => e.type == 3, season)) });
}
const weaponIds = [...new Set(store.skins.filter((s) => s.weapon).map((s) => s.weapon))].sort((a, b) => a - b);
for (const w of weaponIds) for (const season of seasons) {
  lists.push({ kind: 'VIEWER_LIST_WEAPON', key: w, season, opts: optionList(sorted((e) => e.weapon == w, season)) });
}
const listData = lists.map((l, i) => `static const short list_${i}[] = {${l.opts.map((o) => o[0] + ', ' + o[1]).join(', ')}};`);
const listIndex = lists.map((l, i) => `    {${l.kind}, ${l.key}, ${l.season}, ${l.opts.length}, list_${i}},`);

const out = `// Generated by tools/ui-import/gen_viewer_store.js from assets/skins.json and the Krunker v10.0.2
// store module (tools/ui-import/v10_store.js; viewer lists: v7 src/viewer.js). Do not edit: re-run the generator.
#include "viewer_store.h"

const ViewerSkin g_viewer_skins[] = {
${rows.join('\n')}
};
const int g_viewer_skin_count = ${rows.length};

// store.rarities: name, colour
const ViewerRarity g_viewer_rarities[] = {
${store.rarities.map((r) => `    {${cstr(r.name)}, ${cstr(r.color)}},`).join('\n')}
};
const int g_viewer_rarity_count = ${store.rarities.length};

// store.types (model directory + prefix per skin type)
const char *const g_viewer_types[] = {${store.types.map(cstr).join(', ')}};
const int g_viewer_type_count = ${store.types.length};

// store.previews[weapon]: offsets of a weapon's preview model (cosmetic / showcase modes)
const ViewerPreview g_viewer_previews[VIEWER_PREVIEW_SLOTS] = {
${previews.join('\n')}
};

// store.previews["<weapon>_<mid>"]: a remodel's own offsets (v10 getPreview)
const ViewerPreviewRemodel g_viewer_preview_remodels[] = {
${remodelPreviews.join('\n')}
};
const int g_viewer_preview_remodel_count = ${remodelPreviews.length};

// config.skinColors
const ViewerSkinColor g_viewer_skin_colors[] = {
${config.skinColors.map((c) => `    {${c.id}, ${c.col}, ${cstr(c.name)}},`).join('\n')}
};
const int g_viewer_skin_color_count = ${config.skinColors.length};

const int g_viewer_current_season = ${config.currentSeason};

// Dyes: store index, then {count, colours} for skin, shirt, sleeve, pants, waist, shoe, hair
const ViewerDye g_viewer_dyes[] = {
${dyeRows.join('\n')}
};
const int g_viewer_dye_count = ${dyeRows.length};

// Weapon charm mounts by weapon id (weapons.js)
const ViewerCharmMount g_viewer_charm_mounts[VIEWER_CHARM_WEAPONS] = {
${charmRows.join('\n')}
};

// Dropdown option lists: pairs of (label skin index, value), -1/-1 = "Default".
${listData.join('\n')}

const ViewerOptionList g_viewer_lists[] = {
${listIndex.join('\n')}
};
const int g_viewer_list_count = ${lists.length};
`;
// shared/src/store_spins_table.c: gapi /data/spins as one JSON text (store_spins.c parses it), the
// wheels in store.wheels order (the index the "spin" frame and a ticket's spinWheelId name) and
// the item pools (store indices). Season: the wheels without one roll config.currentSeason.
const spinsJson = JSON.stringify({ season: config.currentSeason, wheels: spins.wheels, itemPools: spins.itemPools });
const spinsOut = `// Generated by tools/ui-import/gen_viewer_store.js from tools/ui-import/v10_spins.jspck (Krunker
// v10.0.2 gapi /data/spins: prize wheels and item pools) and v10's config.currentSeason.
// Do not edit: re-run the generator. Parsed by shared/src/store_spins.c.
#include "store_spins.h"

const char g_store_spins_json[] =
${spinsJson.match(/.{1,100}/g).map((c) => '    ' + cstr(c)).join('\n')};
`;

const outputs = [[OUT, out, rows.length + ' skins'], [SPINS_OUT, spinsOut, spins.wheels.length + ' wheels, ' + Object.keys(spins.itemPools).length + ' pools']];
if (process.env.GEN_CHECK) {
  let stale = 0;
  for (const [file, text, what] of outputs) {
    const cur = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
    if (cur !== text) { console.log('[gen_viewer_store] ' + path.relative(REPO, file) + ' is out of date'); stale++; }
    else console.log('[gen_viewer_store] ' + path.relative(REPO, file) + ' up to date (' + what + ')');
  }
  process.exit(stale ? 1 : 0);
}
for (const [file, text, what] of outputs) {
  fs.writeFileSync(file, text);
  console.log('[gen_viewer_store] wrote ' + path.relative(REPO, file) + ' (' + what + ')');
}
