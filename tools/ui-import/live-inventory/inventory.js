#!/usr/bin/env node
// Static inventory of every asset path Krunker v7.1.4's client can request, from v7's own data
// tables (required in node with browser stubs, read-only) and the KrunkNative map catalogue.
// Output TSV: category \t path (relative to assets/, i.e. the asset host layout) \t source
//   node -r v7env.js inventory.js > candidates.tsv
'use strict';
const fs = require('fs');
const path = require('path');
const V7 = '/home/user/jails/kru/src/Krunker_v7.1.4';
const KN = process.env.KN_REPO || path.resolve(__dirname, '..', '..', '..');
const D = (m) => require(path.join(V7, 'src/data', m));
const out = new Map(); // path -> [category, source]
function add(cat, p, src) {
  if (!p || /undefined|null|NaN/.test(p)) return;
  p = p.replace(/^\/+/, '').replace(/\?.*$/, '');
  if (!out.has(p)) out.set(p, [cat, src]);
}
const snd = (name, src, cat = 'sound') => { if (name && typeof name === 'string') add(cat, `sound/${name}.mp3`, src); };
const read = (f) => fs.readFileSync(path.join(V7, f), 'utf8');

// ---------------------------------------------------------------- sounds
const soundJs = read('src/libs/sound.js');
const cats = soundJs.match(/this\.soundCats = \{([\s\S]*?)\};/)[1];
for (const m of cats.matchAll(/^\s*"?([A-Za-z0-9_]+)"?:/gm)) snd(m[1], 'sound.js soundCats');
const config = require(path.join(V7, 'src/config.js'));
for (const n of config.networkSounds || []) snd(n, 'config.networkSounds');
for (const n of config.emitterSounds || []) snd(n, 'config.emitterSounds');
for (const s of config.stepSounds || []) for (const n of s.ids) snd(n, 'config.stepSounds');
for (const v of Object.values(config.soundScapes || {})) snd('ambient_' + v, 'config.soundScapes');
// literal SOUND.play("x") / play3D("x") / playSound("x") in every v7 client source file
const srcFiles = [];
(function walk(d) { for (const f of fs.readdirSync(d)) { const p = path.join(d, f); const st = fs.statSync(p); if (st.isDirectory()) walk(p); else if (f.endsWith('.js')) srcFiles.push(p); } })(path.join(V7, 'src'));
for (const f of srcFiles) {
  const s = fs.readFileSync(f, 'utf8');
  for (const m of s.matchAll(/\b(?:SOUND|vVO\w*|p\d+\.SOUND|this\.SOUND|game\.SOUND)?\.?(?:play|play3D|play3DL|playAt|getSound|playSound)\(\s*"([A-Za-z0-9_]+)"\s*[,)]/g)) snd(m[1], 'literal play() in ' + path.relative(V7, f));
}

// ---------------------------------------------------------------- weapons, attachments
const weapons = D('weapons.js');
const melee = weapons.find((w) => w.melee) || {};
weapons.forEach((w, i) => {
  const s = `weapons.js[${i}] ${w.name}`;
  if (!w.melee) snd(w.sound, s); snd(w.swapSnd, s); snd(w.aimSound, s);
  if (w.src) {
    add('weapon-model', `models/weapons/${w.src}.obj`, s);
    add('weapon-texture', `textures/weapons/${w.src}.png`, s);
    add('preview', `textures/previews/weapons/${w.src}.png`, s);
  }
  if (w.icon) add('weapon-icon', `textures/weapons/${w.icon}.png`, s);
  if (w.flap && w.flap.src) { add('weapon-model', `models/attach/${w.flap.src}.obj`, s + ' flap'); add('weapon-texture', `textures/attach/${w.flap.src}.png`, s + ' flap'); }
  if (w.sound && fs.existsSync(`${KN}/assets/sound/${w.sound}_blast.mp3`)) snd(w.sound + '_blast', s);
});
for (const w of D('upgradeWeapons.js')) if (w && w.sound) snd(w.sound, 'upgradeWeapons.js ' + w.name);
for (const a of D('attach.js')) {
  add('attach', `models/attach/${a.src}.obj`, 'attach.js ' + a.name);
  add('attach', `textures/attach/${a.src}.png`, 'attach.js ' + a.name);
  add('preview', `textures/previews/attach/attach_${a.id}.png`, 'attach.js ' + a.name);
}

// ---------------------------------------------------------------- prefabs (map props), textures
const prefabs = new (D('prefabs.js'))();
for (const p of prefabs.prefabs) {
  if (!p.src || p.src === 'cube_0' || p.src === 'model' || p.src === 'default') continue;
  const s = 'prefabs.js ' + p.name;
  add('prop-model', `models/${p.src}.obj`, s);
  if (!p.noTexture || /machine|weaponbox/.test(p.src)) add('prop-texture', `textures/${p.src}.png`, s);
  if (p.emiss) add('prop-texture', `textures/${p.src}_e.png`, s);
  for (let r = 1; r <= (p.texRange || 0); r++) add('prop-texture', `textures/${p.src}_${r}.png`, s + ' texRange');
}
for (const t of prefabs.textures) {
  if (!t.src || t.src === 'default') continue;
  const s = 'prefabs.js textures ' + t.name;
  add('map-texture', `textures/${t.src}.png`, s);
  for (const v of t.variants || []) if (v.id) add('map-texture', `textures/${t.src}_${v.id}.png`, s + ' variant ' + v.name);
}
for (const p of prefabs.props || []) add('ui-img', `img/props/prop_${p.id}.png`, 'prefabs.js props');
// special props (MapManager.js): weapon box, krum machine, perk machine with the zombie perk textures
add('prop-texture', 'textures/weaponbox_0_e.png', 'MapManager.js weaponbox emissive');
add('prop-texture', 'textures/krummachine_0_e.png', 'MapManager.js krummachine emissive');
const nPerks = (config.zombiePerks || []).length - 1;
for (let i = 1; i <= nPerks; i++) {
  add('zombies', `textures/perkmachine_0_${i}.png`, 'MapManager.js perk machine perk ' + i);
  add('zombies', `textures/perkmachine_0_${i}_e.png`, 'MapManager.js perk machine perk ' + i + ' emissive');
}

// ---------------------------------------------------------------- medals (spawned pickups), AI, airdrops, streaks
D('medals.js').forEach((m, i) => {
  const s = `medals.js[${i}] ${m.name}`;
  if (!m.model) return;
  add('pickup-model', `models/${m.model}.${m.fileExt || 'obj'}`, s);
  if (m.fileExt) return; // loadAnimMesh: embedded materials
  add('pickup-texture', `textures/${m.texSrc || m.model}.png`, s);
  if (m.emisTex) add('pickup-texture', `textures/${m.model}_e.png`, s);
  for (let r = 1; r <= (m.texRange || 0); r++) add('pickup-texture', `textures/${m.model}_${r}.png`, s + ' texRange');
  if (m.snd) snd(m.snd, s);
});
// AI presets (data/AI.js): asset models with flag letters (q glb, g gltf, f fbx, else obj; e/n/s/a/... maps)
const aiSrc = read('src/data/AI.js');
const assetModel = (aid, s, cat) => {
  aid = String(aid); const id = aid.replace(/[^\d]/g, ''); if (!id || id === '0') return;
  const ext = aid.includes('q') ? 'glb' : aid.includes('g') ? 'gltf' : aid.includes('f') ? 'fbx' : 'obj';
  add(cat, `user-assets/${id}/model.${ext}`, s);
  if (ext === 'obj') {
    add(cat, `user-assets/${id}/model.png`, s);
    const maps = { e: '_e', n: '_n', s: '_s', a: '_a', m: '_m', r: '_r', l: '_l', p: '_d', b: '_b' };
    for (const [k, suf] of Object.entries(maps)) if (aid.replace(/^\d+/, '').includes(k)) add(cat, `user-assets/${id}/model${suf}.png`, s + ' flag ' + k);
  }
};
for (const m of aiSrc.matchAll(/asset:\s*"([0-9a-z]+)"/g)) assetModel(m[1], 'AI.js preset', 'ai');
add('ai', 'models/ghost_0.obj', 'AI.js default'); add('ai', 'textures/ghost_0.png', 'AI.js default');
add('ai', 'models/orb_0.obj', 'AI.js projectile'); add('ai', 'textures/orb_0.png', 'AI.js projectile');
// airdrop (airDrop.js) + package icons
add('airdrop', 'models/adrp_0.obj', 'airDrop.js'); add('airdrop', 'textures/adrp_0.png', 'airDrop.js');
add('airdrop', 'textures/adrp_1.png', 'airDrop.js'); snd('adrop_0', 'airDrop.js'); snd('adrop_1', 'airDrop.js');
D('airDropPackages.js').forEach((p) => add('ui-img', `img/streaks/package/${p.icon || 0}.png`, 'airDropPackages.js'));
D('streaks.js').forEach((s) => add('ui-img', `img/streaks/${s.index}.png`, 'streaks.js ' + s.name));
D('badges.js').forEach((b) => add('ui-texture', `textures/badges/badge_${b.id}${b.tex ? '_' + b.tex : ''}.png`, 'badges.js'));
D('aimRecticles.js').forEach((r) => add('ui-texture', `textures/scopes/scope_${r.id}.${r.type || 'png'}`, 'aimRecticles.js'));
D('reticles.js').forEach((r) => add('ui-texture', `textures/reticles/reticle_${r.id}.${r.type || 'png'}`, 'reticles.js'));
D('classes.js').forEach((c, i) => add('ui-texture', `textures/classes/icon_${i}.png`, 'classes.js ' + c.name));
(config.zombiePerks || []).forEach((n, i) => add('ui-img', `img/perks/zombie/${i}.png`, 'config.zombiePerks ' + n + ' (io_updateZombiePerks)'));
D('effects.js').forEach((e, i) => add('ui-img', `img/perks/${i}.png`, 'effects.js ' + e.name));
for (let i = 0; i <= 2; i++) add('ui-img', `img/death_${i}_1.png`, 'hud death markers (game-overlay.js)');
for (let i = 0; i <= 6; i++) add('ui-img', `img/drops/drop_${i}.png`, 'drops rarity ' + i);
for (const b of D('buildObjs.js')) add('ui-img', `img/build/build_${b.icon}.png`, 'buildObjs.js ' + b.name);
for (const m of D('medals.js')) if (m.icon != null) add('ui-img', `img/medals/medal_${m.icon}.png`, 'medals.js ' + m.name);
for (const g of D('gameModes.js')) if (g && g.id) add('ui-img', `img/modes/${g.id}.png`, 'gameModes.js ' + g.id);
for (const r of config.purchServRegions || []) add('ui-img', `img/flags/3x2/${r.f}.svg`, 'config.purchServRegions');
for (const b of D('backgrounds.js')) add('ui-img', `img/backgrounds/${b.id}.${b.type || 'png'}`, 'backgrounds.js');

// ---------------------------------------------------------------- store skins (assets/skins.json == v7 store.skins)
const types = ['weapons/weapon_', 'hats/hat_', 'body/body_', 'melee/melee_', 'sprays/', 'dyes/', 'waist/waist_', 'faces/face_', 'shoes/shoe_', 'pets/pet_', 'collectibles/collect_', 'wrist/wrist_', 'charms/charm_', 'tickets/ticket_', 'back/back_', 'head/head_', 'playercards/card_'];
const skins = JSON.parse(fs.readFileSync(KN + '/assets/skins.json', 'utf8'));
const MELEE = 3, SPRAY = 4, DYE = 5, CARD = 16;
skins.forEach((k, idx) => {
  const s = `skins.json[${idx}] ${k.name}`;
  const t = k.assetType || k.type || 0;
  // UTILS.getPreview (utils.js:698)
  let prev;
  if (k.type && (k.type == CARD || k.type == SPRAY)) prev = types[k.type] + k.id;
  else if (k.type && k.type != MELEE) prev = 'previews/cosmetics/' + t + '_' + k.id + (k.tex ? '_' + k.tex : '');
  else prev = 'previews/' + types[t] + (k.type && k.type == MELEE ? k.id + (k.pat == null ? (k.tex == null ? '' : '_' + k.tex) : '_c' + k.pat)
    : (k.weapon || 0) + '_' + (k.mid == null ? (k.pat == null ? (k.tex ? k.tex : k.id) : 'c' + k.pat) : 'm' + k.mid + (k.midT == null ? '' : '_' + k.midT)));
  add('preview', `textures/${prev}.${k.texExt || 'png'}`, s);
  if (t == SPRAY || t == CARD) { add('cosmetic-texture', `textures/${types[t]}${k.id}.${k.texExt || 'png'}`, s); return; }
  if (k.video != null) add('video', `videos/video_${k.video || 0}.mp4`, s);
  if (t == DYE) return;
  // mesh: game.js:3168 / players.js:562
  const src = types[t] + (k.assetType || k.type ? k.id : k.weapon) + (k.mid == null ? '' : '_' + k.mid);
  const anim = !!k.hasAnim;
  if (k.type || k.mid != null) add(anim ? 'cosmetic-gltf' : 'cosmetic-model', `models/${src}.${anim ? 'gltf' : 'obj'}`, s);
  if (!anim) {
    let tex;
    if (k.mid == null) {
      if (k.type && k.type == MELEE) tex = k.pat ? k.tex : 'melee/melee_' + (k.id || 0) + (k.tex ? '_' + k.tex : '');
      else if (k.type) tex = k.tex ? types[t] + k.id + '_' + k.tex : null;
      else tex = k.tex ? k.tex : 'weapons/skins/weapon_' + k.weapon + '_' + k.id;
    } else tex = types[t] + (k.weapon || 0) + '_' + (k.midT || k.mid);
    const base = tex || src;
    add('cosmetic-texture', `textures/${base}.${k.texExt || 'png'}`, s);
    if (k.glow || k.sameGlow) { if (!k.sameGlow) add('cosmetic-texture', `textures/${base}_e.png`, s + ' glow'); }
  }
  // sounds (players.js getKillSnd / getReloadSnd / getStepSound / getJumpSound / spray)
  const w = t == MELEE ? melee : (weapons[(k.weapon || 1) - 1] || {});
  if (k.killSnd && k.type) snd((w.sound || 'melee') + (k.id != null ? '_' + k.id : ''), s + ' killSnd', 'skin-sound');
  if (k.reloadSnd) for (const n of [1, 2]) snd(w.sound + (k.mid == null ? '' : '_' + k.mid) + '_r_' + n, s + ' reloadSnd', 'skin-sound');
  if (k.stepSnd) snd('shoe_' + k.id, s + ' stepSnd', 'skin-sound');
  if (k.spraySnd) snd('spray_' + k.spraySnd, s + ' spraySnd', 'skin-sound');
  if (!k.type && k.mid != null && !k.noSnd && w.sound) {
    const n = w.sound + '_' + k.mid;
    if ((config.networkSounds || []).includes(n)) snd(n, s + ' remodel shot sound', 'skin-sound');
  }
});
// pets (pets.js): pets/pet_<id> handled by the PET type above

// ---------------------------------------------------------------- literal asset paths in v7 code / page / css
const pageFiles = srcFiles.concat([path.join(V7, 'public/index.html'), path.join(V7, 'public/editor.html'), path.join(V7, 'public/viewer.html'), path.join(V7, 'public/social.html')]);
(function walk(d) { if (!fs.existsSync(d)) return; for (const f of fs.readdirSync(d)) { const p = path.join(d, f); const st = fs.statSync(p); if (st.isDirectory()) walk(p); else if (f.endsWith('.css')) pageFiles.push(p); } })(path.join(V7, 'public/css'));
for (const f of pageFiles) {
  if (!fs.existsSync(f)) continue;
  const s = fs.readFileSync(f, 'utf8');
  for (const m of s.matchAll(/["'(\/]((?:img|textures|models|sound|videos|scares)\/[A-Za-z0-9_\-\/.]+\.(?:png|svg|gif|jpe?g|webp|mp3|ogg|obj|gltf|glb|mp4|webm))["')?]/g)) {
    const p = m[1]; const cat = p.startsWith('img/') ? 'ui-img' : p.startsWith('sound/') ? 'sound' : p.startsWith('models/') ? 'literal-model' : 'literal-texture';
    add(cat, p, 'literal in ' + path.relative(V7, f));
  }
}

// ---------------------------------------------------------------- maps (KN catalogue = v7 public/maps + custom)
const mapFiles = [];
for (const d of [KN + '/assets/maps', KN + '/assets/maps/custom']) for (const f of fs.readdirSync(d)) if (f.endsWith('.json')) mapFiles.push(path.join(d, f));
const prefabByIdx = prefabs.prefabs;
const texByIdx = prefabs.textures;
const SOUND_KEYS = ['esa', 'idl', 'dth', 'atk', 'atkh', 'mel', 'melh', 'agr'];
function scanAssetKeys(o, s, depth = 0) {
  if (!o || typeof o !== 'object' || depth > 8) return;
  if (Array.isArray(o)) { for (const x of o) scanAssetKeys(x, s, depth + 1); return; }
  for (const [k, v] of Object.entries(o)) {
    if (SOUND_KEYS.includes(k) && v != null && parseInt(v) > 0) add('map-asset-sound', `user-assets/${parseInt(v)}/sound.mp3`, s + ' ' + k);
    else if (typeof v === 'object') scanAssetKeys(v, s, depth + 1);
  }
}
for (const f of mapFiles) {
  const name = path.relative(KN + '/assets/maps', f);
  let m; try { m = JSON.parse(fs.readFileSync(f, 'utf8')); } catch (e) { continue; }
  const s = 'map ' + name;
  if (m.mmId) add('map-asset-texture', `user-assets/${String(m.mmId).replace(/[^\d]/g, '')}/texture.png`, s + ' minimap');
  for (const k of ['skyDomeTex', 'skyDomeTexA', 'skyDomeEmisTex']) if (m[k] && /\d/.test(String(m[k])) && m[k] !== true) add('map-asset-texture', `user-assets/${String(m[k]).replace(/[^\d]/g, '')}/texture.png`, s + ' ' + k);
  if (m.ambInd != null) snd('ambient_' + m.ambInd, s + ' ambInd', 'map-sound');
  scanAssetKeys(m.objects, s); scanAssetKeys(m.config, s);
  // map GUI images (overlayHelpers.js: t == 2 -> userAssetsUrl(s + "/texture.png"))
  for (const g of Array.isArray(m.gui) ? m.gui : []) if (g && g.t == 2 && /\d/.test(String(g.s || ''))) add('map-asset-texture', `user-assets/${String(g.s).replace(/[^\d]/g, '')}/texture.png`, s + ' gui image');
  if (m.modURL && /user-assets\.krunker\.io\/(md\d+\/mod\.zip)/.test(m.modURL)) add('map-mod', 'user-assets/' + m.modURL.match(/user-assets\.krunker\.io\/(md\d+\/mod\.zip)/)[1], s + ' modURL');
  // custom classes / weapons (config.cls / config.weps): class model asset, projectile asset, pickup icon
  for (const [grp, tbl] of [['cls', (m.config || {}).cls], ['weps', (m.config || {}).weps]]) for (const [k, v] of Object.entries(tbl || {})) {
    if (!v || typeof v !== 'object') continue;
    if (v.aID) assetModel(v.aID, s + ' config.' + grp + '[' + k + '].aID', 'map-asset-model');
    if (v.pj_asset) assetModel(v.pj_asset, s + ' config.' + grp + '[' + k + '].pj_asset', 'map-asset-model');
    if (v.assetIcon) add('map-asset-texture', `user-assets/${String(v.assetIcon).replace(/[^\d]/g, '')}/texture.png`, s + ' config.' + grp + '[' + k + '].assetIcon');
  }
  for (const o of m.objects || []) {
    const pf = prefabByIdx[o.i || o.id || 0];
    if (!pf) continue;
    if (o.aid) assetModel(o.aid, s + ' ' + pf.name + ' aid', 'map-asset-model');
    if (o.at != null && /\d/.test(String(o.at))) {
      const id = parseInt(String(o.at).replace(/[^\d]/g, ''));
      if (id) add('map-asset-texture', `user-assets/${id}/${String(o.at).includes('v') ? 'video.mp4' : 'texture.png'}`, s + ' ' + pf.name + ' at');
    }
    if (pf.name === 'SPRAY') { const sk = skins[parseInt(o.spt) || 2482]; if (sk) add('map-texture', `textures/sprays/${sk.id}.${sk.texExt || 'png'}`, s + ' spray store[' + (parseInt(o.spt) || 2482) + ']'); }
    if (pf.name === 'SOUND_EMITTER' && o.esa == null) snd(config.emitterSounds[parseInt(o.es) || 0] || config.emitterSounds[0], s + ' SOUND_EMITTER es', 'map-sound');
    for (const a of (Array.isArray(o.tas) ? o.tas : [])) if (a && a.esa == null && a.es != null) snd(config.emitterSounds[parseInt(a.es) || 0], s + ' trigger es', 'map-sound');
    // block / object textures (map.js:300): texture name + "_0" (GRASS "_1") + variant
    if (o.at == null && pf.texturable !== false && !pf.noTexture) {
      let tn = typeof o.t === 'string' ? o.t : ((texByIdx[o.t == null ? 0 : o.t] || {}).name || 'DEFAULT');
      if (tn && tn !== 'DEFAULT') {
        const tx = prefabs.getTextureByName(tn);
        let src = tn.toLowerCase() + '_' + (tn == 'GRASS' ? 1 : 0);
        const tv = parseInt(o.tv) || 0;
        if (tx && tx.variants && tv && tx.variants[tv]) src += '_' + tx.variants[tv].id;
        add('map-texture', `textures/${src}.png`, s + ' t=' + tn);
      }
    }
    if (pf.src && !['cube_0', 'model', 'default'].includes(pf.src)) add('prop-model', `models/${pf.src}.obj`, s + ' ' + pf.name);
  }
  // KrunkScript: asset ids passed to the asset APIs (base64 KS or compiled JS)
  for (const code of Object.values(m.scripts || {})) {
    let t = String(code); try { const b = Buffer.from(t, 'base64').toString('utf8'); if (/GAME\.|num |str /.test(b)) t = b; } catch (e) { }
    for (const mm of t.matchAll(/(?:addAsset|addCustomAsset|loadAsset)\s*\(\s*"?(\d{2,7}[a-z]*)"?/g)) assetModel(mm[1], s + ' KrunkScript addAsset', 'ks-asset');
    for (const mm of t.matchAll(/(?:play2D|play3D|playSound|SOUND\.play)\s*\(\s*"?(\d{2,7})"?/g)) add('ks-asset', `user-assets/${mm[1]}/sound.mp3`, s + ' KrunkScript sound');
    for (const mm of t.matchAll(/(?:addImage|addSprite|drawImage|setTexture|texture)\s*\(\s*"?(\d{2,7})"?/g)) add('ks-asset', `user-assets/${mm[1]}/texture.png`, s + ' KrunkScript image');
  }
}

for (const [p, [cat, src]] of [...out.entries()].sort((a, b) => a[1][0].localeCompare(b[1][0]) || a[0].localeCompare(b[0]))) process.stdout.write(`${cat}\t${p}\t${src}\n`);
