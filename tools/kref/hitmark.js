const { CDP, sleep } = require('./cdp.js'); const fs = require('fs');
const OUT='/tmp/krunker-ref'; const CX=640, CY=360; const log=(...a)=>console.log(new Date().toISOString().slice(11,19),...a);
const probe = `(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};return {igu:d('inGameUI'),menu:d('menuHolder'),kc:d('killCardHolder'),hp:(g('healthValue')||{}).textContent,hm:d('hitmarker'),hmo:(g('hitmarker')||{style:{}}).style.opacity,feed:(g('killFeed')||{childNodes:[]}).childNodes.length}})()`;
async function ensureIn(c, tag){ for(let i=0;i<10;i++){ const s=await c.eval(probe); if(s.igu==='block'&&s.kc!=='block') return s; await c.click(CX,CY); await sleep(2500);} return await c.eval(probe); }
(async () => {
  const A = await CDP.connect(9333); const B = await CDP.connect(9334);
  for (const c of [A,B]) { await c.send('Page.enable'); }
  log('A', JSON.stringify(await ensureIn(A,'A'))); log('B', JSON.stringify(await ensureIn(B,'B')));
  await sleep(1500);
  // Watch hitmarker style changes inside the page so the 350 ms flash is not missed.
  await A.eval(`(()=>{const hm=document.getElementById('hitmarker');window.__hits=[];new MutationObserver(()=>{window.__hits.push({t:performance.now(),d:hm.style.display,o:hm.style.opacity,w:hm.style.width,h:hm.style.height,tr:hm.style.transition})}).observe(hm,{attributes:true,attributeFilter:['style']});})()`);
  let shots=0, done=false;
  for (let i=0;i<60 && !done;i++) {
    const t = await A.eval(`(()=>{const t=Game.players.list.find(p=>!p.isYou&&p.active);if(!t)return null;CONTROLS.camLookAt(t.x,t.y+t.height/1.5,t.z);return [Math.round(t.x),Math.round(t.y),Math.round(t.z),t.health]})()`);
    if (!t) { await sleep(500); continue; }
    const before = await A.eval('window.__hits.length');
    await A.mouse('mousePressed',CX,CY,'left'); await sleep(50); await A.mouse('mouseReleased',CX,CY,'left');
    shots++;
    // poll quickly for the flash
    for (let k=0;k<8;k++){ await sleep(25); const n=await A.eval('window.__hits.length'); if(n>before){ await A.shot(`${OUT}/ingame-hitmarker.png`); await A.dom(`${OUT}/ingame-hitmarker.html`); const st=await A.eval(probe); log('captured hitmarker', JSON.stringify(st), 'target', JSON.stringify(t)); fs.appendFileSync(`${OUT}/ingame-captures.log`, `hitmarker\tA\treal-match\t${JSON.stringify(st)}\n`); done=true; break; } }
    if (shots%10===0){ await A.key('r','KeyR',82); await sleep(1800);} else await sleep(150);
  }
  log('hits log', JSON.stringify(await A.eval('window.__hits.slice(0,6)')));
  if (!done) log('no hit registered in', shots, 'shots');
  // keep shooting until kill so B's death card + A's kill shows again with hitmarker
  A.close(); B.close(); process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
