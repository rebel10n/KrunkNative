#!/usr/bin/env node
// WP-E: reference results of Krunker v7.1.4's own custom game config code for
// shared/tests/config_test.c. Runs the real data/customGameConfig.js setupGameConfig and the
// real applyConfig body cut out of data/game.js (the v7 tree is read-only; modules are required
// from it under the same minimal browser polyfill as v7-dump.js) over a list of requests, and
// writes each case's request plus what v7 ends up with: every serverConfig value, config.classes,
// and the weaponConfig / classConfig / effectConfig values of the entries the case overrides.
//
//   node tools/kref/v7-config-dump.js [v7_root] [out_file]
//
// Defaults: /home/user/jails/kru/src/Krunker_v7.1.4 and shared/tests/data/v7/config_cases.json.
'use strict';
const fs = require('fs');
const path = require('path');

const root = process.argv[2] || '/home/user/jails/kru/src/Krunker_v7.1.4';
const out = process.argv[3] || path.join(__dirname, '../../shared/tests/data/v7/config_cases.json');

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

const src = p => path.join(root, 'src', p);
const config = require(src('config.js'));
const UTILS = require(src('libs/utils.js'));
const gameModes = require(src('data/gameModes.js'));
const {setupGameConfig} = require(src('data/customGameConfig.js'));
const weapons = require(src('data/weapons.js'));
const classes = require(src('data/classes.js'));
const effects = require(src('data/effects.js'));

// applyConfig, verbatim from data/game.js
const gameSrc = fs.readFileSync(src('data/game.js'), 'utf8');
const start = gameSrc.indexOf('this.applyConfig = function (cfg, hostType, hostIndex) {');
const end = gameSrc.indexOf('this.applyConfig();', start);
if (start < 0 || end < 0) throw new Error('applyConfig not found in data/game.js');
const body = gameSrc.slice(start, end);
const install = new Function('config', 'UTILS', body.replace(/^this\.applyConfig = /, 'return '));

function newGame() {
    const game = {
        weapons, classes, effects, upgradeWeapons: [],
        map: {maps: new Array(32).fill(null).map((_, i) => ({name: 'map' + i})), modes: gameModes},
        isSandbox: false, isCustom: true, isOfficial: false,
    };
    game.resetClasses = function () {
        this.config.classes = [];
        for (let i = 0; i < this.classConfig.length; i++) {
            if (!this.classConfig[i].hide || (this.isSandbox || this.isCustom) && (!this.isOfficial || !this.isOfficial.rotationMaps)) this.config.classes.push(i);
        }
    };
    game.applyConfig = install(config, UTILS);
    return game;
}

// A map "config" as a flat host request (game_config_from_map).
function fromMap(c) {
    const r = Object.assign({}, c.settings || {});
    for (const k of ['modes', 'classes', 'weps', 'cls', 'prks']) if (c[k] != null) r[k] = c[k];
    return r;
}

function plain(v) { return typeof v === 'number' || typeof v === 'boolean' || typeof v === 'string' ? v : null; }

function pick(table, entry) {
    const o = {};
    for (const opt of config[table]) {
        const v = plain(entry[opt.varN]);
        if (v !== null) o[opt.varN] = v;
    }
    return o;
}

function run(name, hostIndex, clamp, request, setup) {
    const cfg = setup ? setupGameConfig(hostIndex, JSON.parse(JSON.stringify(request)), undefined, false) : request;
    const game = newGame();
    game.applyConfig(JSON.parse(JSON.stringify(cfg)), clamp, hostIndex);
    const settings = {};
    for (const opt of config.serverConfig) settings[opt.varN] = game.config[opt.varN];
    const result = {name, host: hostIndex, clamp, setup: !!setup, request, settings, classes: game.config.classes,
                    isComp: !!cfg.isComp, mimicPubs: !!cfg.mimicPubs, private: !!cfg.private};
    if (setup) result.setupMaps = cfg.maps == null ? null : cfg.maps;
    if (setup) result.setupModes = cfg.modes == null ? null : cfg.modes;
    for (const [key, table, list] of [['weps', 'weaponConfig', 'weaponConfig'], ['cls', 'classConfig', 'classConfig'], ['prks', 'effectConfig', 'effectConfig']]) {
        if (!cfg[key]) continue;
        result[key] = {};
        for (const id of Object.keys(cfg[key])) if (game[list][id]) result[key][id] = pick(table, game[list][id]);
    }
    return result;
}

const maps = d => JSON.parse(fs.readFileSync(path.join(__dirname, '../../assets/maps', d), 'utf8'));
const templates = require(src('data/gameTemplates.js'));
const tpl = i => JSON.parse(templates[i].data).config;

const cases = [];
cases.push(run('defaults custom', 0, 3, {}, true));
cases.push(run('defaults comp', 1, 3, {}, true));
cases.push(run('defaults public clamp', 0, 1, {}, false));
for (const clamp of [-2, 0, 1, 2, 3]) {
    cases.push(run('clamps ' + clamp, 0, clamp, {maxPlayers: 100, minPlayers: -3, gravMlt: -5, jumpMlt: 0, hitBoxPad: 9, warmupTime: 'x',
        deltaMlt: 0.05, scoreLimit: 123456, tmSize: '2', nameTeam1: 'Reds', nameTeam2: 7, autoJump: 1, canSlide: 0, kCams: '',
        frFire: 'yes', t3Dmg: 5, classLim4: 99, intermTmr: 1, objtvTime: 99}, false));
}
cases.push(run('select invalid', 0, 3, {tmSize: 9, nameTeam1: '', bDrop: null}, false));
cases.push(run('comp request', 1, 3, {maxPlayers: 6, tmSize: 1, specSlots: 3, gameTime: 7, gamRounds: 5, jumpMlt: 2, nameTeam1: 'Reds'}, true));
cases.push(run('zombies host', 3, 3, {maxPlayers: 10, maps: [5, 27], modes: [0, 35], gameTime: 9}, true));
cases.push(run('raid host allowed', 4, 3, {maps: [29, 3], modes: [23]}, true));
cases.push(run('raid host fallback', 4, 3, {maps: [3]}, true));
cases.push(run('custom prevented modes', 0, 3, {modes: [0, 35, 2], classes: [0, 99]}, true));
for (const i of [4, 6, 7, 16, 28]) cases.push(run('template ' + i + ' ' + templates[i].name, 0, 3, fromMap(tpl(i)), true));
cases.push(run('Krunk_Plaza', 0, 3, fromMap(maps('custom/Krunk_Plaza.json').config), true));
cases.push(run('eterno_jump', 0, 3, fromMap(maps('custom/eterno_jump.json').config), true));
cases.push(run('overrides', 0, 3, {
    weps: {0: {dmg: 500, rate: 10, reload: 900, recoil: 0.0123, name: 'Big Iron', burst: 1, burstC: 3}, 12: {ammo: 50, dmg: -300, range: 30},
           13: {pj_spd: 9, pj_dmg: 77, pj_grav: 0.5, chrgTime: 2500}, 1: {movSpread: 0, lsMlt: 0, spread: 777, zoom: 0}},
    cls: {0: {health: 120, segs: 6, regen: 0.2, speed: 1.05, throwChrg: 600, wallJM: 100, camType: '3', canShoot: false, movSch: '7',
              yCmLim: 0.62, camOffY: 3, name: 'Tank', speedX: 0, grA: 0.5, canCrch: false, lckCmYR: -0.5, aID: '48931cq', mdl: 2},
          3: {health: 9999, hbS: 3, bkSlid: true, slT: 9, melRate: 0.05, secondary: false, wallJ: true}},
    prks: {0: {max: 20, timer: 30, mlt: 0.5, hp: 50, name: 'Fast'}, 3: {hp: 40, max: 0}},
}, true));

fs.mkdirSync(path.dirname(out), {recursive: true});
fs.writeFileSync(out, JSON.stringify(cases, null, 1) + '\n');
console.log('wrote ' + cases.length + ' cases to ' + out);
