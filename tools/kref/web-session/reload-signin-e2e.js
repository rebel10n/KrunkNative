'use strict';
// Web reload e2e: a listed krunk.io account stays signed in across page reloads, also when the load's
// brain probe fails (HTTP 503, injected with CDP Fetch on every second reload) and when the brain refuses
// the saved session once (its sessions are moved away from the stack's database for 1.5 s). The probe is
// retried in the background and a refused session is asked once more before it is forgotten.
//   KN_WEB_RUN=<stack run dir> RELOADS=10 node tools/kref/web-session/reload-signin-e2e.js [screenshot.png]
// Run through tools/kref/web-session/web-session-e2e.sh. Exit 0 = every reload signed in and the refused
// session was retried; a second refusal leaves the page a guest, and the next reload is a guest too.
const fs = require('fs');
const { spawn } = require('child_process');
const { openPage, stackEnv, sleep } = require('./session.js');

const env = stackEnv();
const RELOADS = +(process.env.RELOADS || 10);
const shot = process.argv[2] || '/tmp/kn-web-session-reload-signin.png';
const name = 'rl' + Date.now().toString(36).slice(-7);
const pass = 'Passw0rd-' + name;
const DB = `${env.RUN}/brain.db`;
const failures = [];
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

const ev = async (page, expr) => { try { return await page.cdp.eval(expr); } catch (e) { return null; } };
const hdrHas = (page, t) => ev(page, `[...document.querySelectorAll('*')].some(x => x.children.length === 0 && x.textContent.trim() === ${JSON.stringify(t)} && x.getBoundingClientRect().width > 0)`).then((v) => !!v);
const origin = (page) => ev(page, 'performance.timeOrigin');
const waitFor = async (fn, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await fn()) return Date.now() - t0; await sleep(250); } return -1; };
const setVal = (page, sel, v) => ev(page, `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.value = ${JSON.stringify(v)}; e.dispatchEvent(new Event('input', { bubbles: true })); return true; })()`);
const clickLastInside = async (page, sel) => {
  const r = await ev(page, `(() => { const all = [...document.querySelectorAll(${JSON.stringify(sel)})].filter(e => e.getBoundingClientRect().width > 0); const e = all.pop(); if (!e) return null; const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`);
  if (r) await page.cdp.click(r[0], r[1]);
  return !!r;
};

// The brain's sessions of this account, moved out of the database; put back after `back` seconds (or never).
const PY = `import sqlite3, sys, time
db, user, back = sys.argv[1], sys.argv[2], float(sys.argv[3])
c = sqlite3.connect(db, timeout=10)
aid = c.execute("SELECT id FROM accounts WHERE username = ?", (user,)).fetchone()[0]
cols = [d[1] for d in c.execute("PRAGMA table_info(sessions)")]
rows = c.execute("SELECT * FROM sessions WHERE account_id = ?", (aid,)).fetchall()
c.execute("DELETE FROM sessions WHERE account_id = ?", (aid,)); c.commit()
print("removed", len(rows), flush=True)
if back >= 0:
    time.sleep(back)
    c.executemany("INSERT INTO sessions (%s) VALUES (%s)" % (",".join(cols), ",".join("?" * len(cols))), rows); c.commit()
`;
const moveSessions = (back) => new Promise((resolve) => {
  const p = spawn('python3', ['-I', '-c', PY, DB, name, String(back)]);
  let out = '';
  p.stdout.on('data', (d) => { out += d; if (/removed/.test(out)) resolve(out.trim()); });
  p.on('close', () => resolve(out.trim()));
});

// Injected probe failures: the first GET /mm/status of a load answers HTTP 503 (on the injected loads only).
const probe = { failNext: false, failed: 0 };
const injectProbe = async (page) => {
  await page.cdp.send('Fetch.enable', { patterns: [{ urlPattern: '*/mm/status*', requestStage: 'Request' }] });
  page.cdp.on((m) => {
    if (m.method !== 'Fetch.requestPaused') return;
    const q = m.params;
    if (probe.failNext) {
      probe.failNext = false;
      probe.failed++;
      page.cdp.send('Fetch.fulfillRequest', { requestId: q.requestId, responseCode: 503, responseHeaders: [{ name: 'content-type', value: 'text/plain' }], body: '' }).catch(() => {});
      return;
    }
    page.cdp.send('Fetch.continueRequest', { requestId: q.requestId }).catch(() => {});
  });
};

// A reload (the browser's, Page.reload): the new document is checked, not the old one still on screen.
const reload = async (page, failProbe) => {
  const before = await origin(page);
  probe.failNext = failProbe;
  await page.cdp.send('Page.reload', { ignoreCache: false });
  await waitFor(async () => { const o = await origin(page); return o !== null && o !== before; }, 30000);
  return waitFor(() => hdrHas(page, name), 30000);
};

(async () => {
  const reg = await fetch(`http://127.0.0.1:${env.BRAIN}/auth/register`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username: name, password: pass, email: name + '@example.test' }),
  });
  check(reg.status === 200, 'the brain registers the test account');

  const page = await openPage(env.BASE + '/');
  await injectProbe(page);
  await waitFor(async () => (await ev(page, "!!document.querySelector('#knV10AccountsBtn')")) === true, 150000);
  await page.clickSel('#knV10AccountsBtn'); // the guest header's Account Manager, as a player opens it
  await waitFor(() => page.visible('#knAltUser'), 30000);
  await setVal(page, '#knAltUser', name);
  await setVal(page, '#knAltPass', pass);
  await clickLastInside(page, '.knAltAdd .joinBtn');
  const added = await waitFor(() => hdrHas(page, name), 90000);
  check(added >= 0, 'the account is listed and signed in (Account Manager Add)');
  if (added < 0) { page.close(); process.exit(1); }
  await sleep(2000);

  // 1. reloads, every second one with its first probe failing
  let signedIn = 0;
  for (let i = 1; i <= RELOADS; i++) {
    const failProbe = i % 2 === 0;
    const ms = await reload(page, failProbe);
    if (ms >= 0) signedIn++;
    check(ms >= 0, `reload ${i}${failProbe ? ' (its probe answers 503)' : ''}: signed in${ms >= 0 ? ' after ' + ms + ' ms' : ''}`);
  }
  check(probe.failed === Math.floor(RELOADS / 2), `the injected probe failures were answered (${probe.failed})`);
  check(signedIn === RELOADS, `all ${RELOADS} reloads signed in (${signedIn})`);

  // 2. a refused saved session: refused once (the sessions are back 1.5 s later), then accepted on the retry
  await moveSessions(1.5);
  const retried = await reload(page, false);
  check(retried >= 0, 'a refused saved session is asked once more and signs the account in');
  await sleep(1500);

  // 3. refused for good: the saved session is forgotten, the page is a guest, and so is the next reload
  await moveSessions(-1);
  const gone = await reload(page, false);
  await sleep(2000);
  check(gone < 0 && (await hdrHas(page, 'Login or Register')), 'a session refused twice leaves the page a guest');
  const again = await reload(page, false);
  check(again < 0, 'the next reload is a guest too (the session was forgotten)');

  await page.shot(shot);
  page.close();
  if (failures.length) { console.log(`reload signin e2e: ${failures.length} failed`); process.exit(1); }
  console.log('reload signin e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
