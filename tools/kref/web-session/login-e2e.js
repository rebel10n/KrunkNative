'use strict';
// Web e2e: the login popup (v10_login.c) signs in on the web. Its Login button and the Email <->
// Username switch are inline handlers (knV10LoginSubmit(), knV10LoginMode()) on <button>s with the
// same ids; the page's own calls must reach C. Before the fix the click threw
// "knV10LoginSubmit is not a function" (the element named by the id shadowed the forwarder) and the
// popup stayed open.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/login-e2e.js [screenshot.png]
const path = require('path');
const { openPage, sleep, stackEnv } = require('./session.js');

const findPt = (sel, txt) => `(() => { const e = [...document.querySelectorAll('${sel}')].find((x) => (x.innerText || x.textContent || '').trim() === ${JSON.stringify(txt)} && x.getBoundingClientRect().width > 0); if (!e) return null; const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`;

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-session-login.png';
  // one account per stack run (the brain's registration limit is per minute): taken = made earlier
  const username = 'weblogine2e', password = 'Passw0rd!web-e2e';
  const register = () => fetch(env.BASE + '/auth/register', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, email: `${username}@example.com`, password }) });
  let reg = await register();
  if (reg.status === 429) { await sleep(65000); reg = await register(); }
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  const haveAccount = reg.ok || reg.status === 409;
  check(haveAccount, `the test account exists on the stack brain (register: ${reg.status})`);
  if (!haveAccount) { console.log('web-session login: FAIL (registration)'); process.exit(1); }

  const page = await openPage(env.BASE + '/');
  try {
    for (let i = 0; i < 120; i++) { await sleep(500); if (await page.visible('#menuBtnQuickMatch')) break; }
    await sleep(2000);
    const snap = page.mark();
    await page.clickSel('[onclick*="login"]');
    for (let i = 0; i < 20 && !(await page.visible('#knV10LoginSubmit')); i++) await sleep(300);
    check(await page.visible('#knV10LoginSubmit'), 'the login popup opens');

    // Email <-> Username switch: the field's placeholder follows the mode
    const before = await page.cdp.eval(`(document.querySelector('#accEmail') || {}).placeholder || ''`);
    await page.clickId('knV10LoginMode'); await sleep(500);
    const after = await page.cdp.eval(`(document.querySelector('#accEmail') || {}).placeholder || ''`);
    check(before !== after, `the Email/Username switch changes the field (${JSON.stringify(before)} -> ${JSON.stringify(after)})`);
    const modeErr = page.since(snap).exceptions.filter((e) => /is not a function/.test(e));
    check(modeErr.length === 0, `the switch reaches C (no "is not a function") ${JSON.stringify(modeErr.slice(0, 1))}`);
    await page.clickId('knV10LoginMode'); await sleep(400); // back to the mode the form starts in

    // the fields are filled through the DOM (focus, value, input event): typing re-renders the popup
    // (oninput -> knV10LoginInput), which moves the focus between the keystrokes
    const fill = (sel, text) => page.cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.focus(); e.value = ${JSON.stringify(text)}; e.dispatchEvent(new Event('input', { bubbles: true })); return e.value === ${JSON.stringify(text)}; })()`);
    check(await fill('#accEmail', username), 'the username is in the Email field');
    await sleep(600);
    check(await fill('#accPass', password), 'the password is in the Password field');
    await sleep(600);
    const btn = await page.cdp.eval(findPt('button', 'Login'));
    check(!!btn, 'the Login button is on screen');
    if (btn) await page.cdp.click(btn[0], btn[1]);
    let signed = false;
    for (let i = 0; i < 40 && !signed; i++) {
      await sleep(500);
      signed = !(await page.visible('#knV10LoginSubmit'));
    }
    await sleep(1500);
    await page.shot(shot);
    const d = page.since(snap);
    check(d.exceptions.filter((e) => /is not a function/.test(e)).length === 0,
      `the Login button reaches C (no "is not a function") ${JSON.stringify(d.exceptions.slice(0, 1)).slice(0, 200)}`);
    check(signed, 'the popup closes after the sign-in');
    const top = (await page.text('body')) || '';
    check(top.includes(username) && !/Login or Register/.test(top), 'the header shows the signed-in account (the guest "Login or Register" is gone)');
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`web-session login: ${failures.length} failed`); process.exit(1); }
  console.log('web-session login: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
