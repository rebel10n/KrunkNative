const { CDP } = require('./cdp.js');
(async () => {
  for (const port of [9333, 9334]) {
    const c = await CDP.connect(port);
    const r = await c.eval(`(()=>{const g=id=>document.getElementById(id);const d=id=>{const e=g(id);return e?e.style.display||getComputedStyle(e).display:'-'};return JSON.stringify({instr:(g('instructions')||{}).textContent.slice(0,120),instrH:d('instructionHolder'),menu:d('menuHolder'),igu:d('inGameUI'),end:d('endUI'),endTxt:(g('endUI')||{}).textContent&&g('endUI').textContent.replace(/\\s+/g,' ').slice(0,160),kc:d('killCardHolder'),win:d('windowHolder'),pop:d('popupHolder'),visible:[...document.body.children].filter(e=>e.id&&getComputedStyle(e).display!=='none').map(e=>e.id).join(','),timer:(g('timerVal')||{}).textContent,gt:Game.gameTimer,bodyTop:document.body.firstElementChild.id})})()`);
    console.log(port, r); c.close();
  }
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
