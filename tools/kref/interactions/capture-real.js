'use strict';
const { CDP, sleep } = require('/home/user/src/KrunkNative/tools/kref/cdp.js');
const fs = require('fs');
const OUT = process.env.OUT || '/tmp/kn-menus-ref/real';
const steps = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
(async () => {
  const c = await CDP.connect(+process.env.CDP_PORT || 7777);
  await c.setViewport(1280, 720); await sleep(500);
  for (const s of steps) {
    try {
      if (!s.keep) await c.eval(`(()=>{try{clearPops()}catch(e){};try{if(windowHolder.style.display=='block')showWindow(0)}catch(e){};try{closeWindow&&closeWindow()}catch(e){};return 1})()`);
      await sleep(200);
      if (s.js) await c.eval(`(()=>{${s.js};return 1})()`);
      for (const cl of (s.clicks || [])) { await c.click(cl[0], cl[1]); await sleep(300); }
      if (s.move) await c.mouse('mouseMoved', s.move[0], s.move[1], 'none');
      await sleep(s.wait || 700);
      await c.shot(`${OUT}/${s.name}.png`);
      const info = await c.eval(`(()=>({hdr:(document.getElementById('windowHeader')||{}).innerText, wh:windowHolder.style.display, mw:(document.getElementById('menuWindow')||{}).innerHTML, pop:popupHolder.style.display}))()`);
      fs.writeFileSync(`${OUT}/${s.name}.json`, JSON.stringify(info));
      console.log(s.name, info.hdr, info.wh, (info.mw || '').length);
    } catch (e) { console.log(s.name, 'ERR', e.message); }
  }
  c.close(); process.exit(0);
})();
