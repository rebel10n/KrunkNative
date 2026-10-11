#!/usr/bin/env node
// Drives the REAL Krunker v3.1.1 social page (public/social.html + js/social.js,
// read-only reference tree) in headless Chromium against a running kn_brain:
// the page's own code connects to the brain's social socket (<origin>/api/ws, as
// the v3.1.1 offline hook.js sets socialURL/apiURL) and HTTP API (/api/maps ...).
//
//   node v311_social_client.js <sitePort> <cdpPort> <user> <pass> <outDir>
//
// Needs: v311_site_proxy.js on 127.0.0.1:<sitePort> (public/ + brain proxy, the
// nginx of a deployment) and `chromium-browser --headless=new
// --remote-debugging-port=<cdpPort>` already running (never on the user's display).
// recaptcha/hcaptcha/cdn scripts are stubbed (offline); the page code is untouched.
'use strict';
const { CDP, sleep } = require('/home/user/src/KrunkNative/tools/kref/cdp.js');
const fs = require('fs');

const [staticPort, cdpPort, user, pass, outDir] = process.argv.slice(2);
const results = [];
function check(ok, what) { results.push([ok, what]); console.log((ok ? 'ok   ' : 'FAIL ') + what); }

const RECAPTCHA_STUB = `window.grecaptcha = { ready: f => f(), execute: () => Promise.resolve('captcha-token'), render: () => 0 };
window.Chart = function () { return { update() {}, data: { labels: [], datasets: [] } }; };
window.Chart.defaults = { global: {} };
// recaptcha's onload=captchaCallback fires after the page loaded (not while social.js evaluates)
(function boot() { if (document.readyState === 'complete' && typeof window.captchaCallback === 'function') setTimeout(window.captchaCallback, 100); else setTimeout(boot, 50); })();`;

async function main() {
  const cdp = await CDP.connect(+cdpPort);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  const logs = [];
  cdp.on((m) => {
    if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.type + ': ' + m.params.args.map(a => a.value || a.description || '').join(' '));
    if (m.method === 'Runtime.exceptionThrown') logs.push('exception: ' + JSON.stringify(m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description || m.params.exceptionDetails.text));
  });
  await cdp.send('Fetch.enable', { patterns: [
    { urlPattern: '*recaptcha_api.js*', requestStage: 'Request' },
    { urlPattern: '*hcaptcha.com*', requestStage: 'Request' },
    { urlPattern: '*cdnjs.cloudflare.com*', requestStage: 'Request' },
    { urlPattern: '*krunker.io*', requestStage: 'Request' },
    { urlPattern: '*googletagmanager*', requestStage: 'Request' },
    { urlPattern: '*twitch.tv*', requestStage: 'Request' },
  ] });
  cdp.on(async (m) => {
    if (m.method !== 'Fetch.requestPaused') return;
    const p = m.params;
    const url = p.request.url;
    try {
      if (url.includes('recaptcha_api.js')) {
        await cdp.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: 200,
          responseHeaders: [{ name: 'Content-Type', value: 'application/javascript' }], body: Buffer.from(RECAPTCHA_STUB).toString('base64') });
      } else {
        await cdp.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: 404, body: '' });
      }
    } catch (e) { logs.push('fetch hook: ' + e.message); }
  });

  const page = (q) => `http://127.0.0.1:${staticPort}/social.html${q}`;
  const evalq = (e) => cdp.eval(e).catch(err => 'ERR ' + err.message);
  async function waitFor(expr, ms = 10000) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { if (await evalq(expr) === true) return true; await sleep(100); }
    return false;
  }

  // 1) guest: profile page of the user, rendered by the page's io_0
  await cdp.send('Page.navigate', { url: page('?p=profile&q=' + user) });
  check(await waitFor(`!!document.querySelector('#profileHolder') && getComputedStyle(profileHolder).display === 'block' && rightProfile.innerHTML.includes(${JSON.stringify(user)})`),
    'guest sees the profile page rendered from the brain');
  await cdp.shot(outDir + '/v311-profile-guest.png');

  // 2) login through the page's own login popup (social.js loginPopup/loginAcc)
  await evalq(`loginPopup(); accName.value = ${JSON.stringify(user)}; accPass.value = ${JSON.stringify(pass)}; loginAcc(); true`);
  check(await waitFor(`localStorage.getItem('krunker_token') && localStorage.getItem('krunker_token').length === 32`),
    'login via the page: token saved');
  check(await waitFor(`profileName.innerHTML.includes(${JSON.stringify(user)}) && profileName.innerHTML.includes('KR')`),
    'header shows the account name and KR');
  await cdp.shot(outDir + '/v311-logged-in.png');

  // 3) reload: the page resumes with ["a", 1, [name, null, token]]
  await cdp.send('Page.navigate', { url: page('?p=feed') });
  check(await waitFor(`profileName.innerHTML.includes(${JSON.stringify(user)})`), 'token resume after reload');

  // 4) post on the feed and see it rendered by Je()
  const text = 'hello from the v3.1.1 page ' + Date.now();
  await waitFor(`getComputedStyle(postEntryC).display !== 'none'`, 5000);
  await evalq(`postInput.value = ${JSON.stringify(text)}; makePost(); true`);
  check(await waitFor(`postHolder.innerHTML.includes(${JSON.stringify(text)})`), 'makePost(): psr ack renders the post');
  await cdp.send('Page.navigate', { url: page('?p=feed') });
  check(await waitFor(`postHolder.innerHTML.includes(${JSON.stringify(text)})`), 'feed renders the new post');
  await cdp.shot(outDir + '/v311-feed.png');

  // 5) leaderboard page
  await cdp.send('Page.navigate', { url: page('?p=leaders') });
  check(await waitFor(`leaderList.innerHTML.includes(${JSON.stringify(user)})`), 'leaderboard lists the account');
  await cdp.shot(outDir + '/v311-leaders.png');

  // 6) maps tab from /api/maps
  await cdp.send('Page.navigate', { url: page('?p=maps') });
  check(await waitFor(`mapList.innerHTML.includes('Soul Sanctum') && mapList.innerHTML.includes('Zombie_High')`, 12000), 'maps tab lists the v3.1.1 catalogue');
  await cdp.shot(outDir + '/v311-maps.png');

  // 7) wrong password shows the brain's error string
  await evalq(`localStorage.removeItem('krunker_token'); true`);
  await cdp.send('Page.navigate', { url: page('?p=feed') });
  await sleep(1500);
  await evalq(`loginPopup(); accName.value = ${JSON.stringify(user)}; accPass.value = 'wrong-password'; loginAcc(); true`);
  check(await waitFor(`popupContent.innerHTML.includes('Invalid login.')`), 'wrong password popup "Invalid login."');

  fs.writeFileSync(outDir + '/v311-console.txt', logs.join('\n'));
  const failed = results.filter(r => !r[0]).length;
  console.log(failed + ' failure(s); console log: ' + outDir + '/v311-console.txt');
  process.exit(failed ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(2); });
