#!/usr/bin/env node
// Dump Krunker v7.1.4 data tables (src/data/*.js) as JSON (shared/tests/data/v7/): the source the v7
// weapon / class files in shared/src/ were generated from. The tables the tests check are v10.0.3's
// (shared/tests/weapons_v10_test.c, tools/kref/v10-fixtures.py). The v7 tree is read-only: the modules are
// required from it under a minimal browser polyfill (they read location / localStorage at
// load time). Functions (req, anim, onPickup, ...) are dropped by JSON.stringify.
//
//   node tools/kref/v7-dump.js [v7_root] [out_dir]
//
// Defaults: /home/user/jails/kru/src/Krunker_v7.1.4 and shared/tests/data/v7.
'use strict';
const fs = require('fs');
const path = require('path');

const root = process.argv[2] || '/home/user/jails/kru/src/Krunker_v7.1.4';
const out = process.argv[3] || path.join(__dirname, '../../shared/tests/data/v7');

const store = {};
global.window = global;
global.self = global;
global.location = {hostname: 'localhost', href: 'http://localhost/', search: '', protocol: 'http:',
                   host: 'localhost', origin: 'http://localhost', pathname: '/'};
global.localStorage = {getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); },
                       removeItem: k => { delete store[k]; }};
Object.defineProperty(global, 'navigator', {value: {userAgent: 'node', platform: 'linux', language: 'en'}, configurable: true});
global.document = {createElement: () => ({getContext: () => null, style: {}}), getElementById: () => null,
                   addEventListener: () => {}, body: {}, documentElement: {style: {}}};

const data = name => require(path.join(root, 'src/data', name + '.js'));

// Plain data only: functions become their presence (true) under "<key>$fn" so tests can
// tell "has an onPickup / req" apart from "none".
function plain(value) {
    if (Array.isArray(value)) return value.map(plain);
    if (value && typeof value === 'object') {
        const o = {};
        for (const k of Object.keys(value)) {
            const v = value[k];
            if (typeof v === 'function') o[k + '$fn'] = true;
            else if (v !== undefined) o[k] = plain(v);
        }
        return o;
    }
    return value;
}

const tables = {
    weapons: () => data('weapons'),
    upgradeWeapons: () => data('upgradeWeapons'),
    classes: () => data('classes'),
    streaks: () => data('streaks'),
    effects: () => data('effects'),
    attach: () => data('attach'),
    aimRecticles: () => data('aimRecticles'),
    medals: () => data('medals'),
    projectiles: () => {
        const P = data('projectiles');
        const m = new P({}, {});
        return m.stored || [];
    },
};

fs.mkdirSync(out, {recursive: true});
for (const [name, load] of Object.entries(tables)) {
    let value;
    try { value = load(); } catch (e) { console.error(`${name}: ${e.message.split('\n')[0]}`); continue; }
    const file = path.join(out, name + '.json');
    fs.writeFileSync(file, JSON.stringify(plain(value), null, 1) + '\n');
    console.log(`${file}: ${Array.isArray(value) ? value.length + ' entries' : typeof value}`);
}
