// node tools/editor/dump_editor_v7.js: the Krunker v7.1.4 data only the map editor needs, as one
// JSON document on stdout, for tools/editor/gen_editor_data.py (the shared prefab / config tables
// come from tools/maps/dump_v7.js). Reads the readable v7 sources (read-only):
//   config.js                  mapCats, assetCat, custom GUI lists, perks (zombiePerks), editor limits
//   data/colors.js             zones.sky / zones.BR (map defaults)
//   data/classes.js, effects.js  every class / effect value of the Class / Effect Config panels
//   data/krunkScriptSyntax.js  client / server templates, autocomplete, aliases
//   data/gameTemplates.js, data/scriptTemplates.js, data/defaultMaps.js  the Templates window
// KN_V7_SRC overrides the source root (default /home/user/jails/kru/src/Krunker_v7.1.4/src).
globalThis.localStorage = {getItem() { return null; }, setItem() {}};
globalThis.location = {hostname: "127.0.0.1", protocol: "http:", host: "127.0.0.1", href: "http://127.0.0.1/", search: ""};
globalThis.window = globalThis;
globalThis.navigator = {userAgent: ""};
globalThis.document = {createElement() { return {getContext() { return null; }}; }};
globalThis.Image = function () {};

const S = (process.env.KN_V7_SRC || "/home/user/jails/kru/src/Krunker_v7.1.4/src") + "/";
const config = require(S + "config.js");
const colors = require(S + "data/colors.js");
const classes = require(S + "data/classes.js");
const effects = require(S + "data/effects.js");
const kss = require(S + "data/krunkScriptSyntax.js");
const gameTemplates = require(S + "data/gameTemplates.js");
const scriptTemplates = require(S + "data/scriptTemplates.js");
const defaultMaps = require(S + "data/defaultMaps.js")();

const pick = (o, keys) => {
    const r = {};
    for (const k of keys) if (o[k] !== undefined && typeof o[k] !== "function") r[k] = o[k];
    return r;
};
const list = (x) => (Array.isArray(x) ? x : Object.values(x || {}));
const classKeys = ["name", "cost"].concat(config.classConfig.map((c) => c.varN));
const effectKeys = ["name", "id"].concat(config.effectConfig.map((c) => c.varN));

console.log(JSON.stringify({
    mapCats: config.mapCats,
    assetCat: config.assetCat,
    custGUITypes: config.custGUITypes,
    perks: config.zombiePerks,
    inputTypesGUI: config.inputTypesGUI,
    limits: pick(config, ["eventObjectLimit", "guiLimit", "guiLimitF", "teamLimit", "custValLimit", "triggerLimit",
                          "objectLimit", "objectLimitF", "objectColLimit", "objectColLimitF"]),
    zoneColors: {sky: colors.zones.sky, BR: colors.zones.BR},
    classes: list(classes).map((c) => pick(c, classKeys)),
    effects: list(effects).map((e) => pick(e, effectKeys)),
    kss: {templates: kss.templates, autoComplete: kss.autoComplete, alias: kss.alias},
    templates: {
        game: gameTemplates.map((t) => pick(t, ["name", "icon", "data"])),
        script: scriptTemplates.map((t) => pick(t, ["name", "icon", "data"])),
        defaultMaps: defaultMaps.map((m) => pick(m, ["name", "fileName"])),
    },
}));
