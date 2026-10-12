// node tools/maps/dump_v7.js: the Krunker v7.1.4 map data tables as one JSON document on stdout,
// for tools/maps/gen_prefab_data.py (shared/src/prefab_data.c) and tools/editor/gen_editor_data.py
// (client/src/editor/editor_data.c). Reads the readable v7 sources (read-only):
//   data/prefabs.js   prefabs (97) + textures (20, variants)
//   data/triggers.js  events (32), conditions (12), actions (72)
//   config.js         serverConfig / classConfig / weaponConfig / effectConfig, limits, sound tables
//   data/gameModes.js, data/classes.js, data/weapons.js, data/effects.js
// KN_V7_SRC overrides the source root (default /home/user/jails/kru/src/Krunker_v7.1.4/src).
globalThis.localStorage = {getItem() { return null; }, setItem() {}};
globalThis.location = {hostname: "127.0.0.1", protocol: "http:", host: "127.0.0.1", href: "http://127.0.0.1/", search: ""};
globalThis.window = globalThis;
globalThis.navigator = {userAgent: ""};
globalThis.document = {createElement() { return {getContext() { return null; }}; }};
globalThis.Image = function () {};

const S = (process.env.KN_V7_SRC || "/home/user/jails/kru/src/Krunker_v7.1.4/src") + "/";
const config = require(S + "config.js");
const Prefabs = require(S + "data/prefabs.js");
const triggers = require(S + "data/triggers.js");

const plain = (x) => {
    if (x == null || typeof x !== "object") return x;
    if (Array.isArray(x)) return x.map(plain);
    const o = {};
    for (const k in x) {
        if (typeof x[k] === "function") continue;
        o[k] = plain(x[k]);
    }
    return o;
};
const safe = (fn, fallback) => { try { return fn(); } catch (e) { return fallback; } };

const p = new Prefabs(null);
const prefabs = p.prefabs.map((x, i) => {
    const o = {id: i};
    for (const k in x) {
        const v = x[k];
        if (typeof v === "function") {
            o["has_" + k] = true;
            if (k === "gen") o.gen = v.toString();
        } else {
            o[k] = plain(v);
        }
    }
    return o;
});

const vals = (list) => (list || []).map(plain);
const actions = (triggers.actions || []).map((a) => {
    const o = {};
    for (const k in a) if (typeof a[k] !== "function") o[k] = plain(a[k]);
    return o;
});
const conditions = (triggers.conditions || []).map((c) => ({name: c.name, vals: vals(c.vals)}));

const modes = safe(() => require(S + "data/gameModes.js"), []);
const classes = safe(() => require(S + "data/classes.js"), []);
const weapons = safe(() => require(S + "data/weapons.js"), []);
const effects = safe(() => require(S + "data/effects.js"), []);
const pickNames = (list) => (Array.isArray(list) ? list : Object.values(list || {})).map((m) => {
    const o = {};
    for (const k in m) if (typeof m[k] !== "function" && typeof m[k] !== "object") o[k] = m[k];
    return o;
});

const C = (k) => plain(config[k]);
console.log(JSON.stringify({
    prefabs,
    textures: plain(p.textures),
    triggers: {events: vals(triggers.events), conditions, actions},
    config: {
        serverConfig: C("serverConfig"), classConfig: C("classConfig"), weaponConfig: C("weaponConfig"),
        effectConfig: C("effectConfig"),
        interfaceIDMax: C("interfaceIDMax"), interfaceLimit: C("interfaceLimit"), triggerLimit: C("triggerLimit"),
        triggerActionLimit: C("triggerActionLimit"), custValLimit: C("custValLimit"),
        objectLimit: C("objectLimit"), objectLimitF: C("objectLimitF"), objectColLimit: C("objectColLimit"),
        objectColLimitF: C("objectColLimitF"), spawnLimit: C("spawnLimit"), signLimit: C("signLimit"),
        signFontMax: C("signFontMax"), signTextLimit: C("signTextLimit"), lbLimit: C("lbLimit"),
        anchorLimit: C("anchorLimit"), wheelLimit: C("wheelLimit"), triangleLimit: C("triangleLimit"),
        aiLimit: C("aiLimit"), botLimit: C("botLimit"), eventObjectLimit: C("eventObjectLimit"),
        guiLimit: C("guiLimit"), guiLimitF: C("guiLimitF"), buildMax: C("buildMax"), teamLimit: C("teamLimit"),
        positionRange: C("positionRange"), borderH: C("borderH"),
        emitterSounds: C("emitterSounds"), networkSounds: C("networkSounds"), stepSounds: C("stepSounds"),
        particleIDS: C("particleIDS"), particleBlending: C("particleBlending"), encodingTypes: C("encodingTypes"),
        inputTypes: C("inputTypes"), cameraTypes: C("cameraTypes"), pathTypes: C("pathTypes"),
        soundScapes: C("soundScapes"), billboards: safe(() => config.billboards.length, null),
        emitterVolMin: C("emitterVolMin"), emitterVolMax: C("emitterVolMax"), emitterRateMin: C("emitterRateMin"),
        emitterRateMax: C("emitterRateMax"), emitterDistMax: C("emitterDistMax"),
        shadowRes: C("shadowRes"), shadowDst: C("shadowDst"), lightDistance: C("lightDistance"),
        lightIntensity: C("lightIntensity"), ambIntensity: C("ambIntensity"), sunAngleX: C("sunAngleX"),
        sunAngleY: C("sunAngleY"), deathY: C("deathY"),
    },
    modes: pickNames(modes),
    classes: pickNames(classes),
    weapons: pickNames(weapons),
    effects: pickNames(effects),
}));
