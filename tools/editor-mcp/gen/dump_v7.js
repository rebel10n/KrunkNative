// Prints the Krunker v7.1.4 editor data kn_editor_mcp's schema is built from (prefabs,
// textures, triggers, config lists, limits) as JSON. Run by gen_schema.py:
//   node tools/editor-mcp/gen/dump_v7.js <Krunker_v7.1.4/src>
// The reference tree is read-only; this only require()s it under browser stubs.
const path = require('path');
const root = process.argv[2] || '/home/user/jails/kru/src/Krunker_v7.1.4/src';
global.location = {href: 'http://localhost/', hostname: 'localhost', host: 'localhost', protocol: 'http:', search: '', pathname: '/'};
global.window = global;
global.document = {createElement: () => ({getContext: () => ({})}), addEventListener() {}, getElementById() { return null; }};
global.navigator = {userAgent: ''};
global.localStorage = {getItem() { return null; }, setItem() {}};

const plain = (v) => typeof v === 'boolean' || typeof v === 'number' || typeof v === 'string' ||
    (Array.isArray(v) && v.every((n) => typeof n === 'number'));
const out = {};
const t = require(path.join(root, 'data/triggers.js'));
out.events = t.events.map((e) => e.name);
out.actions = t.actions.map((a) => ({name: a.name, type: a.type || null, custom: !!a.custom}));
out.conditions = (t.conditions || []).map((c) => c.name);
const P = require(path.join(root, 'data/prefabs.js'));
const p = new P(false);
out.prefabs = p.prefabs.map((x) => {
    const o = {};
    for (const k of Object.keys(x)) if (plain(x[k])) o[k] = x[k];
    return o;
});
out.textures = p.textures.map((x) => ({name: x.name, src: x.src, vars: x.vars ? x.vars.length : 0}));
const c = require(path.join(root, 'config.js'));
out.serverConfig = c.serverConfig;
out.limits = {};
for (const k of ['objectLimit', 'objectLimitF', 'objectColLimit', 'objectColLimitF', 'spawnLimit', 'eventObjectLimit',
    'guiLimit', 'signLimit', 'signTextLimit', 'lbLimit', 'wheelLimit', 'triggerLimit', 'triggerActionLimit',
    'interfaceLimit', 'triangleLimit', 'anchorLimit', 'aiLimit', 'botLimit', 'positionRange', 'interfaceIDMax']) {
    if (typeof c[k] === 'number') out.limits[k] = c[k];
}
out.emitterSounds = c.emitterSounds || [];
console.log(JSON.stringify(out));
