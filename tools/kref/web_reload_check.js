// Web reload check (headless Chromium over CDP): F5 on the web build is the page reload, and the
// offline address bar (?offline=1&map=<catalogue key> / ?sandbox, client/src/ui/servers_ui.c
// page_write_offline) brings it back onto the same map.
//   node tools/kref/web_reload_check.js <dist/web> <httpPort> <cdpPort> [<native build dir>]
// With a native build dir (kn_brain + kn_fakenode under services/brain/) it also checks game links:
// a kn_brain --no-db on an ephemeral port with a fake CHITOWN2 node in region CH; ?game=CH2:<lobby>
// (the short link form, shared/include/server_names.h) joins CHITOWN2:<lobby>, stays short in the
// address bar and survives F5; the old long ?game=CHITOWN2:<lobby> is rewritten to the short form.
// Ports: free ones (server/tests/kn_proto.py free_ports). Profile in ~/.cache. Exit 0 = all passed.
'use strict';
const http = require('http'), fs = require('fs'), path = require('path'), { spawn } = require('child_process');
const { CDP, sleep } = require(path.join(__dirname, 'cdp.js'));
const root = path.resolve(process.argv[2]);
const HTTP = +process.argv[3], CDPP = +process.argv[4];
const BUILD = process.argv[5] ? path.resolve(process.argv[5]) : null;
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
  const procs = []; // kn_brain / kn_fakenode of section 5 (only ours are killed)
  const done = (code) => { for (const p of [ch, ...procs]) { try { process.kill(-p.pid, 'SIGKILL'); } catch (e) {} } server.close(); fs.rmSync(prof, { recursive: true, force: true }); process.exit(code); };
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

    // 5. game links: ?game=<short id> joins the lobby, stays short, survives F5; long ids still work
    if (!BUILD) {
      console.log('SKIP  game links (no native build dir argument)');
    } else {
      const secret = 'reloadcheck' + process.pid;
      const brain = spawn(path.join(BUILD, 'services/brain/kn_brain'), ['--no-db', '--brain.port=0', '--brain.id=reloadcheck' + process.pid,
        '--brain.internal_secret=' + secret], { cwd: path.join(__dirname, '..', '..'), env: { ...process.env, KN_LOG_LEVEL: 'info' }, stdio: ['ignore', 'pipe', 'pipe'], detached: true });
      procs.push(brain);
      const brainPort = await new Promise((res, rej) => {
        let out = '';
        const t = setTimeout(() => rej(new Error('kn_brain did not start: ' + out)), 20000);
        const on = (d) => { out += d; const m = /listening on http:\/\/[0-9.]+:(\d+)/.exec(out); if (m) { clearTimeout(t); res(+m[1]); } };
        brain.stdout.on('data', on); brain.stderr.on('data', on);
      });
      const node = spawn(path.join(BUILD, 'services/brain/kn_fakenode'), ['--brain', `ws://127.0.0.1:${brainPort}/mm/internal/ws`, '--secret', secret,
        '--node', 'CHITOWN2', '--region', 'CH', '--host', '127.0.0.1', '--port', String(HTTP), '--quiet'], { stdio: 'ignore', detached: true });
      procs.push(node);
      const getJSON = (u) => new Promise((res) => http.get(u, (r) => { let d = ''; r.on('data', (c) => d += c); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { res(null); } }); }).on('error', () => res(null)));
      let lobby = null;
      for (let i = 0; i < 80 && !lobby; i++) {
        const list = await getJSON(`http://127.0.0.1:${brainPort}/mm/game-list`);
        const t = list && (list.games || []).find((g) => /^CHITOWN2:/.test(g[0]));
        if (t) lobby = t[0].split(':')[1]; else await sleep(250);
      }
      check('fake node CHITOWN2 (region CH) is listed', lobby, lobby || 'no lobby');
      const brainQ = 'brain=' + encodeURIComponent(`http://127.0.0.1:${brainPort}`);
      const info = await getJSON(`http://127.0.0.1:${brainPort}/mm/game-info?game=CH2:${lobby}`);
      check('brain resolves /mm/game-info?game=CH2:<lobby>', info && info[0] === `CHITOWN2:${lobby}`, JSON.stringify(info && info[0]));
      const joining = new RegExp(`\\[mm\\] joining CHITOWN2:${lobby} `);
      const want = `?game=CH2:${lobby}&${brainQ}`;

      logs.length = 0;
      await c.send('Page.navigate', { url: base + want });
      check('?game=CH2:<lobby> joins CHITOWN2:<lobby>', await waitLog(joining, 120000), logs.filter((l) => /\[mm\]/.test(l)).join(' | '));
      s = await waitSearch(/^\?game=CH2:/, 5000);
      check('address bar keeps the short ?game=CH2:<lobby>', s === want, s);
      await sleep(1500);
      await f5();
      check('F5 joins CHITOWN2:<lobby> again', await waitLog(joining, 120000), logs.filter((l) => /\[mm\]/.test(l)).join(' | '));
      s = await search();
      check('after the reload: still ?game=CH2:<lobby>', s === want, s);

      logs.length = 0;
      await c.send('Page.navigate', { url: base + `?game=CHITOWN2:${lobby}&${brainQ}` });
      check('old long ?game=CHITOWN2:<lobby> still joins', await waitLog(joining, 120000), logs.filter((l) => /\[mm\]/.test(l)).join(' | '));
      s = await waitSearch(/^\?game=CH2:/, 10000);
      check('address bar rewrites it to ?game=CH2:<lobby>', s === want, s);
    }
    done(results.every(Boolean) ? 0 : 1);
  } catch (e) { console.error(e); done(2); }
})();
