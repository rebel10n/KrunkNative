// node dump_prefabs.js: Krunker v3.1.1 data/prefabs.js (prefab list + textures) as JSON (gen_editor_data.py)
globalThis.localStorage={};globalThis.location={hostname:"127.0.0.1",protocol:"http:",host:"127.0.0.1",href:"http://127.0.0.1/",search:""};globalThis.window=globalThis;globalThis.navigator={userAgent:""};globalThis.document={};
const S=(process.env.KN_EDITOR_SRC||'/tmp/kn-editor-src/src')+'/';
const config=require(S+'config.js');
const Mod=require(S+'data/prefabs.js');
const p=new Mod(config,{},false,null);
const out=p.prefabs.map((x,i)=>{const o={id:i};for(const k in x){const v=x[k]; o[k]=typeof v==='function'?(k==='gen'?'<fn>'+v.toString():'<fn>'):v;} return o;});
console.log(JSON.stringify({prefabs:out,textures:p.textures}));
