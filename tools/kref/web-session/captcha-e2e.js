'use strict';
// Web e2e: Box Office's captcha in the web build (doc/v10/captcha.md), against a stack whose brain enforces it:
//   KN_WEB_BRAIN_ARGS="--captcha.enforce=true --captcha.max_number=20000 --debug=2" tools/kref/web-session/stack.sh RUN &
//   KN_WEB_RUN=RUN node tools/kref/web-session/captcha-e2e.js
// The page (the same C as native, in wasm, through net-bridge's /mm proxy) seeks at load: the brain answers
// CaptchaRequired, the page solves the seek's ALTCHA and seeks again, joins the node, and answers the node's in-game
// "cpt". Then the login popup signs in: the page solves the login challenge first. Checked in the stack's brain and
// node logs (their DEBUG / INFO lines) and on the page.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { openPage, sleep, stackEnv } = require('./session.js');

const findPt = (sel, txt) => `(() => { const e = [...document.querySelectorAll('${sel}')].find((x) => (x.innerText || x.textContent || '').trim() === ${JSON.stringify(txt)} && x.getBoundingClientRect().width > 0); if (!e) return null; const b = e.getBoundingClientRect(); return [b.x + b.width / 2, b.y + b.height / 2]; })()`;

// the ALTCHA widget's search and value (libs/altcha.1.0.6.min.js): the test's own registration over HTTP
function solve(ch) {
  for (let n = 0; n <= ch.maxnumber; n++) {
    if (crypto.createHash('sha256').update(ch.salt + n).digest('hex') === ch.challenge) {
      return Buffer.from(JSON.stringify({ algorithm: ch.algorithm, challenge: ch.challenge, number: n, salt: ch.salt,
        signature: ch.signature, took: 1 })).toString('base64');
    }
  }
  return null;
}

(async () => {
  const env = stackEnv();
  const read = (f) => { try { return fs.readFileSync(path.join(env.RUN, f), 'utf8'); } catch (e) { return ''; } };
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  check(/captcha enforced/.test(read('brain.log')), 'the stack brain enforces the captcha');

  const username = 'webcaptcha' + (process.pid % 10000), password = 'Passw0rd!web-captcha';
  const ch = await (await fetch(env.BASE + '/mm/captcha?for=register')).json();
  check(ch && ch.algorithm === 'SHA-256', 'GET /mm/captcha passes through the bridge (its /mm proxy)');
  const reg = await fetch(env.BASE + '/auth/register', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, password, captcha: solve(ch) }) });
  check(reg.ok, `the test account is registered with a solved captcha (HTTP ${reg.status})`);

  const page = await openPage(env.BASE + '/');
  try {
    // the load's seek: CaptchaRequired, the seek's captcha, the join, the node's "cpt"
    let joined = false;
    for (let i = 0; i < 120 && !joined; i++) {
      await sleep(500);
      joined = /\[captcha\] client \d+: accepted/.test(read('node.log'));
    }
    check(/seek from \S+: captcha accepted/.test(read('brain.log')), 'the page solved the seek captcha after CaptchaRequired (brain)');
    check(joined, 'the page joined the node and answered its in-game cpt, accepted (node)');
    // the one refusal: the first seek, which carries no captchaToken (its CaptchaRequired starts the retry)
    const refusals = (read('node.log') + read('brain.log')).split('\n').filter((l) => /CAPTCHA FAILED|refused: captcha/.test(l));
    check(refusals.every((l) => /seek from \S+ refused: captcha missing/.test(l)), `nothing else was refused ${JSON.stringify(refusals.slice(0, 2))}`);

    // the sign-in: the page solves the login challenge before the social socket's "a"
    for (let i = 0; i < 60; i++) { await sleep(500); if (await page.visible('#menuBtnQuickMatch')) break; }
    await page.clickSel('[onclick*="login"]');
    for (let i = 0; i < 20 && !(await page.visible('#knV10LoginSubmit')); i++) await sleep(300);
    await page.clickId('knV10LoginMode'); await sleep(500); // the username field
    const fill = (sel, text) => page.cdp.eval(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.focus(); e.value = ${JSON.stringify(text)}; e.dispatchEvent(new Event('input', { bubbles: true })); return e.value === ${JSON.stringify(text)}; })()`);
    const field = (await page.visible('#accName')) ? '#accName' : '#accEmail';
    check(await fill(field, username), 'the username is typed');
    await sleep(500);
    check(await fill('#accPass', password), 'the password is typed');
    await sleep(500);
    const btn = await page.cdp.eval(findPt('button', 'Login'));
    if (btn) await page.cdp.click(btn[0], btn[1]);
    let signed = false;
    for (let i = 0; i < 40 && !signed; i++) { await sleep(500); signed = !(await page.visible('#knV10LoginSubmit')); }
    await sleep(1000);
    check(/login from \S+: captcha accepted/.test(read('brain.log')), 'the page solved the login captcha (brain)');
    const top = (await page.text('body')) || '';
    check(signed && top.includes(username), 'the page is signed in');
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`web-session captcha: ${failures.length} failed`); process.exit(1); }
  console.log('web-session captcha: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
