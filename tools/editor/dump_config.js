// node dump_config.js: Krunker v3.1.1 config.js gameConfig/serverConfig/weaponConfig as JSON (gen_editor_data.py)
globalThis.localStorage={};globalThis.location={hostname:"127.0.0.1",protocol:"http:",host:"127.0.0.1",href:"http://127.0.0.1/",search:""};globalThis.window=globalThis;globalThis.navigator={userAgent:""};globalThis.document={};
const c=require(''+(process.env.KN_EDITOR_SRC||'/tmp/kn-editor-src/src')+'/config.js');
const pick=a=>a.map(x=>{const o={};for(const k in x) if(typeof x[k]!=='function') o[k]=x[k]; return o;});
console.log(JSON.stringify({gameConfig:pick(c.gameConfig||[]),serverConfig:pick(c.serverConfig||[]),weaponConfig:pick(c.weaponConfig||[]),interfaceIDMax:c.interfaceIDMax,xyzKeys:c.xyzKeys}));
