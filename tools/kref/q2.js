const { CDP, sleep } = require('./cdp.js');
(async () => {
  const c = await CDP.connect(9333);
  await c.send('Runtime.enable');
  const logs=[]; c.on(m=>{ if(m.method==='Runtime.consoleAPICalled') logs.push(m.params.args.map(a=>a.value!==undefined?String(a.value):a.description).join(' ').slice(0,200)); if(m.method==='Runtime.exceptionThrown') logs.push('EXC '+JSON.stringify(m.params.exceptionDetails).slice(0,500)); });
  const probe = `(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};return JSON.stringify({url:location.href,instr:(g('instructions')||{}).textContent,instrSub:(g('instructionsSub')||{}).textContent,instrH:d('instructionHolder'),menu:d('menuHolder'),igu:d('inGameUI'),windowH:d('windowHolder'),lock:!!document.pointerLockElement,subtitle:(g('playStatusSubtitle')||{}).textContent,pop:d('popupHolder'),eui:d('endUI'),players:typeof Game!=='undefined'&&Game.players.list.map(p=>p.alias+':'+p.active+':'+p.isYou),elAt:(()=>{const e=document.elementFromPoint(640,360);return e?e.tagName+'#'+e.id+'.'+e.className:null})()})})()`;
  console.log('before', await c.eval(probe));
  await c.click(640,360); await sleep(2500);
  console.log('after click', await c.eval(probe));
  console.log('try enterGame()'); try { await c.eval('window.enterGame()'); } catch(e){ console.log('err', e.message); }
  await sleep(3000); console.log('after enterGame', await c.eval(probe));
  console.log(logs.slice(-20).join('\n'));
  c.close(); process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
