// node dump_data.js: Krunker v3.1.1 modes/classes/weapons/triggers + config limits as JSON (gen_editor_data.py)
globalThis.localStorage={};globalThis.location={hostname:"127.0.0.1",protocol:"http:",host:"127.0.0.1",href:"http://127.0.0.1/",search:""};globalThis.window=globalThis;globalThis.navigator={userAgent:""};globalThis.document={};
const S=''+(process.env.KN_EDITOR_SRC||'/tmp/kn-editor-src/src')+'/';
const pick=a=>a.map(x=>{const o={};for(const k in x) if(typeof x[k]!=='function'&&typeof x[k]!=='object') o[k]=x[k]; return o;});
const modes=require(S+'data/modes.js'), classes=require(S+'data/classes.js'), weps=require(S+'data/weapons.js');
const c=require(S+'config.js');
const trig=require(S+'data/triggers.js');
console.log(JSON.stringify({modes:pick(modes).map(m=>({name:m.name,cost:m.cost,pcg:m.preventCustomGames})),classes:pick(classes).map(m=>({name:m.name,cost:m.cost})),weapons:pick(weps),
 particleIDS:c.particleIDS, emitterSounds:c.emitterSounds, particleBlending:c.particleBlending, spawnLimit:c.spawnLimit, signLimit:c.signLimit, objectLimit:c.objectLimit, objectColLimit:c.objectColLimit,
 shadowRes:c.shadowRes, shadowDst:c.shadowDst, lightDistance:c.lightDistance, lightIntensity:c.lightIntensity, ambIntensity:c.ambIntensity, sunAngleX:c.sunAngleX, sunAngleY:c.sunAngleY,
 triggers: trig && {events: trig.events, actions: (trig.actions||[]).map(a=>({name:a.name,type:a.type,custom:!!a.custom}))}}));
