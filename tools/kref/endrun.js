// Rejoin both clients, stay active (server idle kick = 60 s without 1 unit of
// movement), fish for a hit marker, and capture the end-of-round screen.
const { CDP, sleep } = require('./cdp.js'); const fs = require('fs');
const OUT='/tmp/krunker-ref'; const CX=640, CY=360; const SHIM=fs.readFileSync('/tmp/kref-tools/shim.js','utf8');
const log=(...a)=>console.log(new Date().toISOString().slice(11,19),...a);
const probe = `(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};return {igu:d('inGameUI'),menu:d('menuHolder'),instr:d('instructionHolder'),instrT:(g('instructions')||{}).textContent.slice(0,40),kc:d('killCardHolder'),end:d('endUI'),hp:(g('healthValue')||{}).textContent,feed:(g('killFeed')||{childNodes:[]}).childNodes.length,timer:(g('timerVal')||{}).textContent}})()`;
async function open(port, tag){ const c=await CDP.connect(port); c.tag=tag; await c.send('Page.enable'); await c.send('Runtime.enable'); await c.send('Page.addScriptToEvaluateOnNewDocument',{source:SHIM}); await c.setViewport(1280,720); await c.send('Page.navigate',{url:'http://127.0.0.1:6768/'}); await c.waitFor(`(()=>{const e=document.getElementById('instructions');return !!e && /PLAY/i.test(e.textContent||'')})()`,90000); try{await c.eval('typeof checkTerms==="function"&&checkTerms(1)');}catch(e){} await sleep(800); return c; }
async function spawn(c){ for(let i=0;i<15;i++){ const s=await c.eval(probe); if(s.igu==='block') return s; if(/Kicked|seek/i.test(s.instrT)) { log(c.tag,'kicked; reload'); await c.send('Page.navigate',{url:'http://127.0.0.1:6768/'}); await sleep(6000); } await c.click(CX,CY); await sleep(3000);} return await c.eval(probe); }
async function cap(c,state,extra){ await c.shot(`${OUT}/ingame-${state}.png`); await c.dom(`${OUT}/ingame-${state}.html`); const st=await c.eval(probe); fs.appendFileSync(`${OUT}/ingame-captures.log`, `${state}\t${c.tag}\treal-match\t${JSON.stringify(st)}\n`); log(c.tag,'captured',state,JSON.stringify(st),extra||''); }
(async () => {
  const A=await open(9333,'A'); const B=await open(9334,'B');
  log('A',JSON.stringify(await spawn(A))); log('B',JSON.stringify(await spawn(B)));
  await A.eval(`(()=>{const hm=document.getElementById('hitmarker');window.__hits=[];new MutationObserver(()=>{window.__hits.push({t:performance.now(),d:hm.style.display,o:hm.style.opacity,w:hm.style.width})}).observe(hm,{attributes:true,attributeFilter:['style']});})()`);
  let gotHit=false, gotEnd=false, gotRestart=false, lastMove=0, lastShot=0, kills=0; const t0=Date.now();
  while (Date.now()-t0 < 11*60*1000 && !(gotEnd && gotRestart)) {
    const now=Date.now();
    const a=await A.eval(probe); const b=await B.eval(probe);
    if (a.end!=='none' && a.end!=='-' && !gotEnd) { await sleep(1500); await cap(A,'endscreen'); try{ if((await B.eval(probe)).end!=='none') await B.shot(`${OUT}/ingame-endscreen-B.png`);}catch(e){} await sleep(7000); await cap(A,'endscreen-late'); gotEnd=true; continue; }
    if (gotEnd && a.end==='none') { await sleep(2500); if ((await A.eval(probe)).igu!=='block') await A.click(CX,CY); await sleep(3000); await cap(A,'round-restart'); gotRestart=true; break; }
    if (!gotEnd) {
      // respawn whoever is on the menu/kill card (after the death delay)
      for (const [c,s] of [[A,a],[B,b]]) { if (s.igu!=='block' && s.end==='none') { if(/Kicked/i.test(s.instrT)){ log(c.tag,'kicked, rejoin'); await c.send('Page.navigate',{url:'http://127.0.0.1:6768/'}); await sleep(5000); await spawn(c);} else { await c.click(CX,CY); } } }
      if (now-lastMove>12000) { for (const c of [A,B]) { await c.keyDown('w','KeyW',87); } await sleep(500); for (const c of [A,B]) { await c.keyUp('w','KeyW',87); } lastMove=now; }
      if (!gotHit && now-lastShot>400 && a.igu==='block' && b.igu==='block') {
        const t=await A.eval(`(()=>{const t=Game.players.list.find(p=>!p.isYou&&p.active);if(!t)return null;CONTROLS.camLookAt(t.x,t.y+t.height/1.5,t.z);return [Math.round(t.x),Math.round(t.y),Math.round(t.z),t.health]})()`);
        if (t) { const before=await A.eval('window.__hits.length'); await A.mouse('mousePressed',CX,CY,'left'); await sleep(40); await A.mouse('mouseReleased',CX,CY,'left'); lastShot=Date.now();
          for(let k=0;k<10;k++){ await sleep(20); const n=await A.eval('window.__hits.length'); if(n>before){ await cap(A,'hitmarker','target='+JSON.stringify(t)); gotHit=true; log('hits',JSON.stringify(await A.eval('window.__hits.slice(0,4)'))); break; } }
          if (((await A.eval(`parseInt(document.getElementById('ammoVal').textContent)||0`))<=2)) { await A.key('r','KeyR',82); await sleep(1500); }
        }
      }
    }
    await sleep(300);
  }
  log('done', JSON.stringify({gotHit,gotEnd,gotRestart}), JSON.stringify(await A.eval(probe)));
  A.close(); B.close(); process.exit(0);
})().catch(e=>{console.error('FAILED',e);process.exit(1);});
