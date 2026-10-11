const { CDP, sleep } = require('./cdp.js'); const fs = require('fs');
const OUT='/tmp/krunker-ref'; const log=(...a)=>console.log(new Date().toISOString().slice(11,19),...a);
(async () => {
  const A = await CDP.connect(9333); const B = await CDP.connect(9334);
  await A.send('Page.enable'); await B.send('Page.enable');
  const q = `(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};return {end:d('endUI'),igu:d('inGameUI'),menu:d('menuHolder'),timer:(g('timerVal')||{}).textContent,endTable:!!g('endTable')}})()`;
  const t0=Date.now(); let got=false;
  while (Date.now()-t0 < 9*60*1000) {
    const a = await A.eval(q);
    if (a.end !== 'none' && a.end !== '-') {
      await sleep(1200);
      await A.shot(`${OUT}/ingame-endscreen.png`); await A.dom(`${OUT}/ingame-endscreen.html`);
      fs.appendFileSync(`${OUT}/ingame-captures.log`, `endscreen\tA\treal-match\t${JSON.stringify(await A.eval(q))}\n`);
      log('captured endscreen', JSON.stringify(a));
      try { const b = await B.eval(q); if (b.end !== 'none') { await B.shot(`${OUT}/ingame-endscreen-B.png`); } } catch(e){}
      await sleep(6000);
      await A.shot(`${OUT}/ingame-endscreen-late.png`); await A.dom(`${OUT}/ingame-endscreen-late.html`);
      log('captured endscreen-late', JSON.stringify(await A.eval(q)));
      got=true; break;
    }
    await sleep(700);
  }
  if (!got) log('no end screen within 9 min', JSON.stringify(await A.eval(q)));
  A.close(); B.close(); process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
