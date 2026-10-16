'use strict';
// Web account e2e: the v10 login modal of the web build, against the stack's kn_brain through the
// bridge's /social/ws. Registers a new account (the modal's Register card), checks the header shows
// it, reloads (the saved session signs the page in again), signs out, reloads signed out, and signs
// back in with the username. Fails on any uncaught exception (an inline onclick that names an element
// id, e.g. v10's id="knV10LoginSubmit" onclick="knV10LoginSubmit()", throws in the browser before C
// sees the click) and on a header that does not show the account.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/account-e2e.js [screenshot.png]
const { openPage, stackEnv, sleep } = require('./session.js');

const env = stackEnv();
const shot = process.argv[2] || '/tmp/kn-web-session-account.png';
const name = 'wpacct' + Date.now().toString(36).slice(-6);
const pass = 'Passw0rd-' + name;
const failures = [];
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };

async function boot(page) {
  for (let i = 0; i < 120; i++) { await sleep(500); if (await page.visible('#menuBtnQuickMatch')) break; }
  await sleep(4000); // the social socket opens; the account service answers over it
}
const hdrHas = (page, t) => page.cdp.eval(`[...document.querySelectorAll('*')].some(x => x.children.length === 0 && x.textContent.trim() === ${JSON.stringify(t)} && x.getBoundingClientRect().width > 0)`);
const setVal = (page, sel, v) => page.cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.value = ${JSON.stringify(v)}; e.dispatchEvent(new Event('input', { bubbles: true })); return true; })()`);
const clickSel = async (page, sel) => {
  const r = await page.cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; const b = e.getBoundingClientRect(); return b.width ? [b.x + b.width / 2, b.y + b.height / 2] : null; })()`);
  if (r) await page.cdp.click(r[0], r[1]);
  return !!r;
};
const clickText = async (page, t) => {
  const r = await page.cdp.eval(`(() => { const e = [...document.querySelectorAll('*')].filter(x => [...x.childNodes].some(c => c.nodeType === 3 && c.textContent.trim() === ${JSON.stringify(t)}) && x.getBoundingClientRect().width > 0).pop(); if (!e) return null; const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`);
  if (r) await page.cdp.click(r[0], r[1]);
  return !!r;
};
const bad = (page, snap) => page.since(snap).exceptions.filter((e) => /is not a function|is not defined/.test(e));

(async () => {
  // start as a guest: a session left over from an earlier run is signed out first
  let page = await openPage(env.BASE + '/');
  await boot(page);
  if (!(await hdrHas(page, 'Login or Register'))) {
    await page.cdp.eval(`knCall('logoutAcc', [])`);
    await sleep(2000);
    page.close();
    page = await openPage(env.BASE + '/');
    await boot(page);
  }
  try {
    let snap = page.mark();
    check(await clickText(page, 'Login or Register'), 'the header opens the login modal');
    await sleep(800);
    check(await page.visible('#knV10Login'), 'the modal is up');
    check(await clickSel(page, '#knV10AuthSwitch'), 'switch to the Create Account card');
    await sleep(500);
    await setVal(page, '#accName', name);
    await setVal(page, '#accEmail', name + '@example.test');
    await setVal(page, '#accPass', pass);
    check(await clickSel(page, '#knV10LoginSubmit'), 'submit the registration');
    await sleep(5000);
    check(bad(page, snap).length === 0, 'the submit button runs its handler (no "is not a function")');
    check(await hdrHas(page, name), 'the header shows the new account');
    await page.shot(shot);

    // reload: the saved session signs the page in again
    page.close();
    const p2 = await openPage(env.BASE + '/');
    await boot(p2);
    check(await hdrHas(p2, name), 'after a reload the session still signs the account in');
    snap = p2.mark();
    await p2.cdp.eval(`knCall('logoutAcc', [])`);
    // the header follows the sign-out a moment later: poll for up to 10 s (a fixed 3 s wait was flaky)
    for (let i = 0; i < 20 && !((await hdrHas(p2, 'Login or Register')) && !(await hdrHas(p2, name))); i++) await sleep(500);
    check(!(await hdrHas(p2, name)) && (await hdrHas(p2, 'Login or Register')), 'sign-out shows the guest header');
    p2.close();

    // signed out after a reload, then sign in with the username
    const p3 = await openPage(env.BASE + '/');
    await boot(p3);
    check(await hdrHas(p3, 'Login or Register'), 'after sign-out and a reload the page is a guest');
    snap = p3.mark();
    await clickText(p3, 'Login or Register');
    await sleep(800);
    if (await p3.visible('#accEmail')) { await clickSel(p3, '#knV10LoginMode'); await sleep(400); }
    await setVal(p3, '#accName', name);
    await setVal(p3, '#accPass', pass);
    await clickSel(p3, '#knV10LoginSubmit');
    await sleep(5000);
    check(bad(p3, snap).length === 0, 'the login submit runs its handler');
    check(await hdrHas(p3, name), 'signing in with the username shows the account');
    p3.close();
  } finally {
    try { page.close(); } catch (e) { /* closed above */ }
  }
  if (failures.length) { console.log(`account e2e: ${failures.length} failed`); process.exit(1); }
  console.log('account e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
