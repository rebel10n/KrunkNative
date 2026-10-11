// Burst-screenshot right after shots at B; keep frames where B's health dropped
// (the canvas-drawn hit marker is only visible for a few hundred ms).
const { CDP, sleep } = require('./cdp.js'); const fs = require('fs');
const OUT='/tmp/krunker-ref'; const CX=640, CY=360; const SHIM=fs.readFileSync('/tmp/kref-tools/shim.js','utf8');
const log=(...a)=>console.log(new Date().toISOString().slice(11,19),...a);
const probe = `(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};return {igu:d('inGameUI'),instrT:(g('instructions')||{}).textContent.slice(0,30),kc:d('killCardHolder'),end:d('endUI'),hp:(g('healthValue')||{}).textContent,feed:(g('killFeed')||{childNodes:[]}).childNodes.length,timer:(g('timerVal')||{}).textContent}})()`;
async function conn(port,tag){ const c=await CDP.connect(port); c.tag=tag; await c.send('Page.enable'); await c.send('Runtime.enable'); await c.send('Page.addScriptToEvaluateOnNewDocument',{source:SHIM}); return c; }
async function ensure(c){ for(let i=0;i<12;i++){ const s=await c.eval(probe); if(s.igu==='block'&&s.kc!=='block') return s; if(/Kicked|seek/i.test(s.instrT)){ await c.send('Page.navigate',{url:'http://127.0.0.1:6768/'}); await sleep(6000); try{await c.eval('typeof checkTerms==="function"&&checkTerms(1)');}catch(e){} } await c.click(CX,CY); await sleep(2500);} return await c.eval(probe); }
(async () => {
  const A=await conn(9333,'A'), B=await conn(9334,'B');
  log('A',JSON.stringify(await ensure(A))); log('B',JSON.stringify(await ensure(B)));
  let lastMove=0, got=0, shots=0; const t0=Date.now();
  while (Date.now()-t0<240000 && got<3) {
    const now=Date.now();
    if (now-lastMove>12000){ for(const c of [A,B]){await c.keyDown('w','KeyW',87);} await sleep(400); for(const c of [A,B]){await c.keyUp('w','KeyW',87);} lastMove=now; }
    const a=await A.eval(probe), b=await B.eval(probe);
    if (a.igu!=='block'){ await ensure(A); continue; } if (b.igu!=='block'||b.kc==='block'){ await sleep(1500); await ensure(B); continue; }
    const t=await A.eval(`(()=>{const t=Game.players.list.find(p=>!p.isYou&&p.active);if(!t)return null;CONTROLS.camLookAt(t.x,t.y+t.height/1.5,t.z);return [Math.round(t.x),Math.round(t.y),Math.round(t.z),t.health]})()`);
    if(!t){ await sleep(400); continue; }
    const hp0=parseInt(((await B.eval(`document.getElementById('healthValue').textContent`))||'0'))||0;
    await A.mouse('mousePressed',CX,CY,'left'); await sleep(30); await A.mouse('mouseReleased',CX,CY,'left'); shots++;
    const frames=[]; const ts=Date.now();
    for(let k=0;k<7;k++){ const r=await A.send('Page.captureScreenshot',{format:'png'}); frames.push({dt:Date.now()-ts,data:r.data}); }
    const hp1=parseInt(((await B.eval(`document.getElementById('healthValue').textContent`))||'0'))||0;
    if (hp1!==null && hp1<hp0) { got++; frames.forEach((f,i)=>fs.writeFileSync(`${OUT}/ingame-hitmarker-burst${got}-${i}-${f.dt}ms.png`,Buffer.from(f.data,'base64'))); await A.dom(`${OUT}/ingame-hitmarker.html`); log('HIT burst',got,'hp',hp0,'->',hp1,'frames',frames.map(f=>f.dt).join(',')); fs.appendFileSync(`${OUT}/ingame-captures.log`, `hitmarker-burst${got}\tA\treal-match\thp ${hp0}->${hp1}\n`); }
    if ((await A.eval(`parseInt(document.getElementById('ammoVal').textContent)||0`))<=1){ await A.key('r','KeyR',82); await sleep(1800);} else await sleep(250);
  }
  log('done shots',shots,'hits',got);
  A.close(); B.close(); process.exit(0);
})().catch(e=>{console.error('FAILED',e);process.exit(1);});
