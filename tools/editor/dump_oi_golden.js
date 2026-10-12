// node tools/editor/dump_oi_golden.js [--defaults] [map.json ...]: v7.1.4's own serialization of
// map objects, the golden for client/tests/editor_serialize_test.c. Runs the readable v7 sources
// (libs/ObjectInstance.js with three.js r150 from the v7 tree; read-only) with the browser parts
// stubbed (no textures or models load). One JSON document on stdout:
//   {"defaults": [<ObjectInstance.defaultFromType(i).serialize()>, ...97],
//    "maps": {"<file>": [[<input object>, <serialize()>], ...]}}
// (maps are keyed by file name.) Each map's objects go through loadObjects' pool expansion (ci / ei / si) first, and update(0)
// runs before serialize(), as in the editor's frame loop. Model prefabs whose size comes from the
// loaded model serialize with the size given (their defaultFromType size is [1, 1, 1] here).
// KN_V7_ROOT overrides the v7 tree (default /home/user/jails/kru/src/Krunker_v7.1.4).
const fs = require("fs");
const path = require("path");
const Module = require("module");
const R = process.env.KN_V7_ROOT || "/home/user/jails/kru/src/Krunker_v7.1.4";

globalThis.localStorage = {getItem() { return null; }, setItem() {}};
globalThis.location = {hostname: "127.0.0.1", protocol: "http:", host: "127.0.0.1", href: "http://127.0.0.1/", search: ""};
globalThis.window = globalThis;
globalThis.navigator = {userAgent: ""};
const ctx2d = new Proxy({}, {
    get(t, k) {
        if (k === "measureText") return () => ({width: 1});
        if (k in t) return t[k];
        return () => {};
    },
    set(t, k, v) { t[k] = v; return true; },
});
const element = () => ({getContext() { return ctx2d; }, style: {}, width: 1, height: 1, addEventListener() {}, setAttribute() {}});
globalThis.document = {createElement: element, createElementNS: element, getElementById() { return null; }, body: {appendChild() {}, removeChild() {}}};
globalThis.Image = function () {};
globalThis.XMLHttpRequest = function () {
    this.open = (m, url) => { this.url = url.split("?")[0]; };
    this.send = () => {
        try {
            this.responseText = fs.readFileSync(R + "/public/" + this.url, "utf8");
            this.status = 200;
        } catch (e) {
            this.status = 404;
        }
    };
};

const req = Module.createRequire(R + "/src/libs/ObjectInstance.js");
const THREE = req("three");
THREE.StrippedLambertMaterial = THREE.MeshLambertMaterial;
class StubLoader { load() { return new THREE.Texture(); } setMeshoptDecoder() {} setPath() { return this; } }
THREE.OBJLoader = THREE.GLTFLoader = THREE.FBXLoader = THREE.TextureLoader = StubLoader;
THREE.MeshoptDecoder = {};
THREE.RectAreaLightHelper = class extends THREE.Object3D { update() {} };
THREE.BufferGeometryUtils = {mergeGeometries: () => new THREE.BufferGeometry()};
process.on("unhandledRejection", () => {});

const OI = req("./ObjectInstance.js");

const serialize = (data) => {
    const o = OI.deserialize(JSON.parse(JSON.stringify(data)));
    // the model's bounds (promises()) are never computed here: the size the map was exported with
    if ((!o.prefab.defaultSize || o.assetID) && Array.isArray(data.s)) o.defaultSize = data.s.slice();
    try { o.update(0); } catch (e) { /* update() helpers need a scene */ }
    return o.serialize();
};

const out = {defaults: [], maps: {}};
const args = process.argv.slice(2);
if (args.includes("--defaults")) {
    for (let i = 0; i < 97; i++) {
        try { out.defaults.push(OI.defaultFromType(i).serialize()); }
        catch (e) { out.defaults.push({error: String(e)}); }
    }
}
for (const file of args.filter((a) => !a.startsWith("--"))) {
    let map = JSON.parse(fs.readFileSync(file, "utf8"));
    if (map.states || map.id || map.map) map = map.map;
    if (typeof map === "string") map = JSON.parse(map);
    const colors = map.colors || [], xyz = map.xyz || [];
    const list = [];
    for (const raw of map.objects || []) {
        const obj = JSON.parse(JSON.stringify(raw));
        for (const k of ["e", "c"]) {
            if (obj[k + "i"] != null && colors[obj[k + "i"]]) {
                obj[k] = colors[obj[k + "i"]];
                delete obj[k + "i"];
            }
        }
        const v = obj.si * 3;
        if (obj.si != null && xyz.length > v) {
            obj.s = xyz.slice(v, v + 3);
            delete obj.si;
        }
        let ser;
        try { ser = serialize(obj); } catch (e) { ser = {error: String(e)}; }
        list.push([obj, ser]);
    }
    out.maps[path.basename(file)] = list;
}
process.stdout.write(JSON.stringify(out));
