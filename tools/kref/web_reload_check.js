// Web reload check (headless Chromium over CDP): F5 on the web build is the page reload, and the
// offline address bar (?offline=1&map=<catalogue key> / ?sandbox, client/src/ui/servers_ui.c
// page_write_offline) brings it back onto the same map.
//   node tools/kref/web_reload_check.js <dist/web> <httpPort> <cdpPort>
// Ports: free ones (server/tests/kn_proto.py free_ports). Profile in ~/.cache. Exit 0 = all passed.
'use strict';
const http = require('http'), fs = require('fs'), path = require('path'), { spawn } = require('child_process');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const root = path.resolve(process.argv[2]);
const HTTP = +process.argv[3], CDPP = +process.argv[4];
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css', '.json': 'application/json', '.png': 'image/png' };
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -- ' + detail : ''}`); };

(async () => {
  const server = http.createServer((req, res) => {
    let rel = decodeURIComponent(req.url.split('?')[0]);
    if (rel.endsWith('/')) rel += 'index.html';
    const file = path.join(root, path.normalize(rel));
    fs.stat(file, (err, st) => {
      if (err || !st.isFile()) { res.writeHead(404); return res.end(); }
      res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Content-Length': st.size, 'Cache-Control': 'no-store' });
      fs.createReadStream(file).pipe(res);
    });
  });
  await new Promise(r => server.listen(HTTP, '127.0.0.1', r));
  const prof = fs.mkdtempSync(path.join(process.env.HOME, '.cache', 'kn-f5-chrome-'));
  const ch = spawn(process.env.KN_CHROME || 'chromium-browser', ['--headless=new', '--no-sandbox', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
    '--mute-audio', '--window-size=1280,720', `--remote-debugging-port=${CDPP}`, `--user-data-dir=${prof}`, 'about:blank'], { stdio: 'ignore', detached: true });
  const done = (code) => { try { process.kill(-ch.pid, 'SIGKILL'); } catch (e) {} server.close(); fs.rmSync(prof, { recursive: true, force: true }); process.exit(code); };
  try {
    const c = await CDP.connect(CDPP);
    const logs = [];
    c.on(m => { if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map(a => a.value).join(' ')); });
    await c.send('Runtime.enable'); await c.send('Page.enable');
    await c.setViewport(1280, 720);
    const base = `http://127.0.0.1:${HTTP}/`;
    const waitLog = async (re, t) => { const t0 = Date.now(); while (Date.now() - t0 < t) { if (logs.some(l => re.test(l))) return true; await sleep(250); } return false; };
    const search = () => c.eval('location.search');
    const waitSearch = async (want, t = 30000) => { const t0 = Date.now(); let s; while (Date.now() - t0 < t) { s = await search().catch(() => ''); if (want.test(s)) return s; await sleep(250); } return s; };
    const f5 = async () => { logs.length = 0; await c.key('F5', 'F5', 116); };

    // 1. ?offline=1&map=kanji: the address bar names the catalogue map; F5 reloads onto it
    await c.send('Page.navigate', { url: base + '?offline=1&map=kanji' });
    check('?offline=1&map=kanji plays Kanji', await waitLog(/\[sandbox\] Loaded map Kanji/, 120000));
    let s = await waitSearch(/map=kanji/);
    check('address bar keeps ?offline=1&map=kanji', s === '?offline=1&map=kanji', s);
    await sleep(1500);
    await f5();
    check('F5 reloads the page onto Kanji', await waitLog(/\[sandbox\] Loaded map Kanji/, 120000), logs.filter(l => /sandbox\]|startup map/.test(l)).join(' | '));
    s = await search();
    check('after the reload: still ?offline=1&map=kanji', s === '?offline=1&map=kanji', s);

    // 2. a path-style ?map= is rewritten to the catalogue key, readable
    logs.length = 0;
    await c.send('Page.navigate', { url: base + '?offline=1&map=custom/ESP_Parkour' });
    check('?map=custom/ESP_Parkour loads', await waitLog(/\[sandbox\] Loaded map/, 120000));
    s = await waitSearch(/map=ESP_Parkour$/);
    check('address bar: ?offline=1&map=ESP_Parkour', s === '?offline=1&map=ESP_Parkour', s);

    // 3. ?offline=1 with no map: a rotation map, then named in the address bar so F5 keeps it
    logs.length = 0;
    await c.send('Page.navigate', { url: base + '?offline=1' });
    check('?offline=1 starts a map', await waitLog(/\[sandbox\] Loaded map/, 120000));
    s = await waitSearch(/map=/);
    const first = logs.find(l => /\[sandbox\] Loaded map/.test(l)) || '';
    check('?offline=1 gets &map=<key> in the address bar', /^\?offline=1&map=[^&%]+$/.test(s), `${s} (${first})`);
    await sleep(1000);
    await f5();
    const again = await waitLog(/\[sandbox\] Loaded map/, 120000) && logs.find(l => /\[sandbox\] Loaded map/.test(l));
    check('F5 lands on the same map', again && again.replace(/\(\d+\).*/, '') === first.replace(/\(\d+\).*/, ''), `${first} -> ${again}`);

    // 4. ?sandbox: the sandbox map, the address bar stays ?sandbox, F5 keeps it
    logs.length = 0;
    await c.send('Page.navigate', { url: base + '?sandbox' });
    check('?sandbox plays the Test map', await waitLog(/\[sandbox\] Loaded map Test/, 120000));
    await sleep(1500);
    s = await search();
    check('address bar stays ?sandbox', s === '?sandbox', s);
    await f5();
    check('F5 reloads onto the sandbox map', await waitLog(/\[sandbox\] Loaded map Test/, 120000));
    done(results.every(Boolean) ? 0 : 1);
  } catch (e) { console.error(e); done(2); }
})();
