// Screenshot + DOM-dump our ui/index.html mock page in a given HUD state.
// usage: node mockshot.js <name> "<query>" [w h] [js-after-load]
const { CDP, sleep } = require('./cdp.js'); const fs=require('fs');
(async () => {
  const [name, query, w, h, js] = process.argv.slice(2);
  const W=+w||1280, H=+h||720;
  const c = await CDP.connect(9335); await c.send('Page.enable'); await c.send('Runtime.enable');
  const logs=[]; c.on(m=>{ if(m.method==='Runtime.consoleAPICalled') logs.push(m.params.args.map(a=>a.value!==undefined?String(a.value):a.description).join(' ').slice(0,200)); if(m.method==='Runtime.exceptionThrown') logs.push('EXC '+JSON.stringify(m.params.exceptionDetails).slice(0,400)); });
  await c.setViewport(W,H);
  await c.send('Page.navigate',{url:'http://127.0.0.1:8778/index.html?'+query+'&_=' + Date.now()});
  await sleep(2500);
  if (js) { try { console.log('js ->', JSON.stringify(await c.eval(js))); } catch(e){ console.log('js error', e.message); } await sleep(process.env.AFTER_MS ? +process.env.AFTER_MS : 600); }
  await c.shot(`/tmp/krunker-ref/ours-${name}.png`); await c.dom(`/tmp/krunker-ref/ours-${name}.html`);
  console.log('ours-'+name, logs.filter(l=>/EXC|rror/.test(l)).slice(0,5).join(' | '));
  c.close(); process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
