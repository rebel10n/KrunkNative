#!/usr/bin/env node
// Krunker v7.1.4 store skin table, the way the v7 client loads it (src/data/skins.js):
// jspck.unpack(public/skins.jspck) with v7's own src/shim/jspck.js (read from the reference
// tree, never copied). Prints the array as compact JSON on stdout: the raw store.skins entries
// in index order with store.js's `templates` merged in (src/data/store.js: a skin with
// `template: n` gets every field of templates[n], which is where 301 skins get their `id` and
// `type`). The other load-time fixups (index, seas || 1, assetType/keyW of BACK/HEAD, creators,
// free/itemShop/ranked/drop flags) are left to the consumers, as assets/skins.json always held
// the raw v3.1.1 store.js array.
//
// usage: node tools/ui-import/v7_skins.js <Krunker_v7.1.4 root>
"use strict";
const fs = require("fs");
const path = require("path");

const root = process.argv[2];
if (!root) {
    process.stderr.write("usage: v7_skins.js <Krunker_v7.1.4 root>\n");
    process.exit(2);
}
const shim = fs.readFileSync(path.join(root, "src/shim/jspck.js"), "utf8");
const module_ = {exports: {}};
// The shim is a UMD-ish module that require()s "./define.js" for its AMD argument list and
// assigns module.exports; nothing else is touched.
new Function("module", "exports", "require", shim)(module_, module_.exports, () => ({}));
const jspck = module_.exports;
if (!jspck || typeof jspck.unpack !== "function") {
    process.stderr.write("v7_skins.js: src/shim/jspck.js did not export unpack()\n");
    process.exit(1);
}
const skins = jspck.unpack(fs.readFileSync(path.join(root, "public/skins.jspck"), "utf8"));
if (!Array.isArray(skins)) {
    process.stderr.write("v7_skins.js: skins.jspck is not an array\n");
    process.exit(1);
}
// store.js: `module.exports.templates = [...];` (a plain literal), merged per skin like
// `for (k in templates[skin.template]) skin[k] = templates[skin.template][k]`.
const store = fs.readFileSync(path.join(root, "src/data/store.js"), "utf8");
const m = /module\.exports\.templates\s*=\s*(\[[\s\S]*?\]);\s*module\.exports\.skins\s*=/.exec(store);
if (!m) {
    process.stderr.write("v7_skins.js: templates not found in src/data/store.js\n");
    process.exit(1);
}
const templates = new Function("return " + m[1])();
for (const skin of skins) {
    if (skin.template == null) continue;
    const t = templates[skin.template];
    if (!t) {
        process.stderr.write("v7_skins.js: unknown template " + skin.template + " (" + skin.name + ")\n");
        process.exit(1);
    }
    for (const k in t) skin[k] = t[k];
}
process.stdout.write(JSON.stringify(skins));
