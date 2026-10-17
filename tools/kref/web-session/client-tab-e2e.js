'use strict';
// Web e2e: Settings > Advanced > Client shows the same rows as the native client's tab
// (client/src/ui/ui_client_settings.c, the Linux row set). The rows the browser cannot honestly
// show are listed in WEB_HIDDEN with the reason; everything else must be there with the same
// label, in the same folder order, and every control must run without an exception. The Sound
// tab of the same view must gain the Audio Output row.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/client-tab-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

// The native tab, in its folder order (ui_client_settings.c SETTINGS, native rows only).
const NATIVE_FOLDERS = {
  Display: ['Window Mode', 'Resolution', 'V-Sync'],
  System: ['Audio Backend', 'Process Priority'],
  Client: ['Exit Button', 'Discord Rich Presence', 'Resource Swapper', 'Auto-restart for updates', 'Custom CSS'],
  Startup: ['Start Offline', 'Auto Join', 'Account Mode', 'Account Service URL', 'Server Finder URL'],
  Network: ['Proxy', 'Proxy Scope'],
  Game: ['Auto Spectate', 'Always Play HS Sound on Kill'],
  Storage: ['Client Folder', 'Clear Cache & Storage', 'Reset Client Settings'],
};
// Rows (and the Network folder they fill) the web build leaves out, each for a reason the browser
// imposes: the swapper reads game files from a folder on the player's disk; a page cannot route
// its traffic through a proxy (the browser's own network settings apply).
const WEB_HIDDEN = ['Resource Swapper', 'Proxy', 'Proxy Scope'];
const WEB_FOLDERS = {};
for (const [folder, rows] of Object.entries(NATIVE_FOLDERS)) {
  const kept = rows.filter((r) => !WEB_HIDDEN.includes(r));
  if (kept.length) WEB_FOLDERS[folder] = kept;
}
const SOUND_ROW = 'Audio Output'; // the game's Sound tab gains it (ui_client_settings.c extend_sound_tab)

(async () => {
  const env = stackEnv();
  const shot = process.argv[2] || '/tmp/kn-web-client-tab.png';
  const page = await openPage(env.BASE + '/?offline=1');
  const failures = [];
  const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failures.push(what); };
  // Browser dialogs (window.confirm / window.prompt, the web's ui_confirm / ui_prompt) are declined.
  page.cdp.on((m) => {
    if (m.method === 'Page.javascriptDialogOpening') page.cdp.send('Page.handleJavaScriptDialog', { accept: false }).catch(() => {});
  });
  // Tags the n-th element matching a selector with data-cct and clicks it like a user.
  const clickNth = async (selector, n) => {
    const ok = await page.cdp.eval(`(() => { document.querySelectorAll('[data-cct]').forEach(e => e.removeAttribute('data-cct'));
      const e = document.querySelectorAll(${JSON.stringify(selector)})[${n}]; if (!e) return false; e.setAttribute('data-cct', '1'); return true; })()`);
    return ok && page.clickSel('[data-cct="1"]');
  };
  // Clicks the k-th (default first) element matching sel inside the settings row i.
  const clickRow = async (i, sel, k = 0) => {
    const ok = await page.cdp.eval(`(() => { document.querySelectorAll('[data-cct]').forEach(e => e.removeAttribute('data-cct'));
      const row = document.querySelectorAll('#settHolder .settName')[${i}]; if (!row) return false;
      const e = row.querySelectorAll(${JSON.stringify(sel)})[${k}]; if (!e) return false; e.setAttribute('data-cct', '1'); return true; })()`);
    return ok && page.clickSel('[data-cct="1"]');
  };
  const labels = () => page.cdp.eval(`(() => [...document.querySelectorAll('#settHolder .settName')].map(e =>
    [...e.childNodes].filter(n => n.nodeType === 3).map(n => n.textContent).join('').trim()))()`);
  const folders = () => page.cdp.eval(`(() => {
    const out = {}; let cur = null;
    for (const e of document.querySelectorAll('#settHolder > .setHed, #settHolder > .setBodH')) {
      if (e.classList.contains('setHed')) { cur = [...e.childNodes].filter(c => c.nodeType === 3).map(c => c.textContent).join('').trim(); out[cur] = []; }
      else if (cur) out[cur].push(...[...e.querySelectorAll('.settName')].map(n => [...n.childNodes].filter(c => c.nodeType === 3).map(c => c.textContent).join('').trim()));
    }
    return out;
  })()`);
  // The tab's element index of the Client tab (the last tab) and of the Sound tab.
  const tabNamed = (name) => page.cdp.eval(`(() => { const t = [...document.querySelectorAll('#settingsTabLayout .settingTab')];
    const i = t.findIndex(e => e.textContent.trim() === ${JSON.stringify(name)}); return i; })()`);

  try {
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders');
    // Settings: the top bar's Settings item (v10_menu.c wires it as #knV10Settings: showWindow(1)).
    let link = null;
    for (let i = 0; i < 60 && !link; i++) { await sleep(500); link = await page.rect('#knV10Settings'); }
    check(!!link && await page.clickId('knV10Settings'), 'the Settings link opens the settings window');
    await sleep(1200);
    // Advanced view (the typeBtn checkbox), then the Client tab.
    const adv = await page.cdp.eval(`(() => { const b = document.getElementById('typeBtn'); return b ? b.checked : null; })()`);
    if (adv === false) { // the Advanced switch: its visible slider (the input itself is hidden)
      await page.cdp.eval(`(() => { document.querySelectorAll('[data-cct]').forEach(e => e.removeAttribute('data-cct'));
        const b = document.getElementById('typeBtn'); const t = b.closest('label') || b.parentElement; t.setAttribute('data-cct', '1'); return true; })()`);
      await page.clickSel('[data-cct="1"]');
      await sleep(800);
    }
    check(await page.cdp.eval(`(() => { const b = document.getElementById('typeBtn'); return !!(b && b.checked); })()`), 'the advanced view is on');
    const clientTab = await tabNamed('Client');
    check(clientTab >= 0, 'the settings window has a Client tab');
    await clickNth('#settingsTabLayout .settingTab', clientTab);
    await sleep(1000);

    const got = await labels();
    const want = Object.values(WEB_FOLDERS).flat();
    check(JSON.stringify(got) === JSON.stringify(want),
      `Client tab rows match native minus web-hidden: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);
    const fold = await folders();
    check(JSON.stringify(Object.keys(fold)) === JSON.stringify(Object.keys(WEB_FOLDERS)),
      `Client tab folders ${JSON.stringify(Object.keys(fold))} want ${JSON.stringify(Object.keys(WEB_FOLDERS))}`);
    for (const [name, rows] of Object.entries(WEB_FOLDERS)) {
      check(JSON.stringify(fold[name] || []) === JSON.stringify(rows), `folder ${name}: ${JSON.stringify(fold[name])}`);
    }
    await page.shot(shot);

    // Every control runs: switches toggle twice (back to the start), selects move to their other
    // option and back, buttons open their confirmation or notice (dismissed or noticed).
    const kinds = await page.cdp.eval(`(() => [...document.querySelectorAll('#settHolder .settName')].map(r => {
      if (r.querySelector('label.switch')) return 'switch';
      if (r.querySelector('select')) { const s = r.querySelector('select'); return 'select:' + s.options.length; }
      if (r.querySelector('input[type=text]')) return 'text';
      if (r.querySelector('.settingsBtn')) return 'button';
      return 'other';
    }))()`);
    for (let i = 0; i < kinds.length; i++) {
      const kind = kinds[i];
      const name = got[i];
      const snap = page.mark();
      if (kind === 'switch') {
        await clickRow(i, 'label.switch');
        await sleep(300);
        await clickRow(i, 'label.switch');
        await sleep(300);
      } else if (kind.startsWith('select:') && Number(kind.slice(7)) > 1) {
        // move to the second option (change event as the page's onchange) and back
        for (const idx of [1, 0]) {
          await page.cdp.eval(`(() => { const s = document.querySelectorAll('#settHolder .settName')[${i}].querySelector('select');
            if (!s) return false; s.selectedIndex = ${idx}; s.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
          await sleep(300);
        }
      } else if (kind === 'button') {
        // Open / Edit / Clear Now / Reset: each opens a notice or a declined dialog
        const count = await page.cdp.eval(`document.querySelectorAll('#settHolder .settName')[${i}].querySelectorAll('.settingsBtn').length`);
        for (let b = 0; b < count; b++) { await clickRow(i, '.settingsBtn', b); await sleep(400); }
      }
      const d = page.since(snap);
      const bad = [...d.exceptions, ...d.errors.filter((t) => !t.startsWith('[ui] '))];
      check(bad.length === 0, `control "${name}" (${kind}) runs without an exception or console error ${JSON.stringify(bad.slice(0, 2))}`);
    }

    // The game's Sound tab gains the Audio Output row.
    const soundTab = await tabNamed('Sound');
    await clickNth('#settingsTabLayout .settingTab', soundTab);
    await sleep(800);
    const soundRows = await labels();
    check(soundRows.includes(SOUND_ROW), `the Sound tab has the ${SOUND_ROW} row (${soundRows.length} rows)`);
    await page.shot(shot.replace(/\.png$/, '-sound.png'));

    check(page.log.exceptions.length === 0, `no uncaught exception in the session ${JSON.stringify(page.log.exceptions.slice(0, 2))}`);
    check(page.log.gl.length === 0, `no WebGL error ${JSON.stringify(page.log.gl.slice(0, 2))}`);
  } finally {
    page.close();
  }
  if (failures.length) { console.log(`client-tab e2e: ${failures.length} failed`); process.exit(1); }
  console.log('client-tab e2e: PASS');
  process.exit(0);
})().catch((e) => { console.error('ERR', e); process.exit(1); });
