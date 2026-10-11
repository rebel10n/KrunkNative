const { CDP, sleep } = require('./cdp.js'); const fs = require('fs');
(async () => {
  const c = await CDP.connect(9333);
  await c.send('Page.enable'); await c.send('Runtime.enable');
  const logs = [];
  c.on(m => { if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value !== undefined ? a.value : a.description).join(' ').slice(0, 200)); if (m.method==='Runtime.exceptionThrown') logs.push('EXC '+JSON.stringify(m.params.exceptionDetails).slice(0,300)); });
  await c.send('Page.addScriptToEvaluateOnNewDocument', { source: fs.readFileSync('/tmp/kref-tools/shim.js', 'utf8') });
  await c.setViewport(1280,720); await c.send('Page.navigate', { url: process.env.URL || 'http://127.0.0.1:6768/' });
  for (let i = 0; i < 40; i++) {
    await sleep(1000);
    const st = await c.eval(`(()=>{const g=id=>document.getElementById(id);const vis=id=>{const e=g(id);return e?getComputedStyle(e).display:'-'};return JSON.stringify({instr:(g('instructions')||{}).textContent,instrH:vis('instructionHolder'),menu:vis('menuHolder'),loading:vis('loadingBG'),igu:vis('inGameUI'),windowH:vis('windowHolder'),title:document.title,gl:!!document.querySelector('canvas'),lock:!!document.pointerLockElement})})()`);
    console.log(i, st);
    if (JSON.parse(st).instr && /PLAY/i.test(JSON.parse(st).instr)) break;
  }
  await c.shot('/tmp/krunker-ref/probe-menu.png');
  fs.writeFileSync('/tmp/krunker-ref/probe-console.log', logs.join('\n'));
  console.log('console lines', logs.length); console.log(logs.slice(-15).join('\n'));
  c.close(); process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
