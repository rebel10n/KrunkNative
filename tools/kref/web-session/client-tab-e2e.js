'use strict';
// Web e2e: the settings rows the native Client tab holds that a browser can do live in the game's own
// tabs (ui_client_settings.c CS_FOLDERS): Display > Interface, Game > Gameplay / Gameplay Privacy. The web build shows no Client tab; the native-only rows
// (Window Mode, V-Sync, Audio Backend, Process Priority, Discord, Resource Swapper, Proxy) are not on web.
// A stored value is read (knClient hidePlayerNames), every moved control runs without an exception or
// console error, and the Sound tab of the advanced view still gains the Audio Output row.
//   KN_WEB_RUN=<stack run dir> node tools/kref/web-session/client-tab-e2e.js [screenshot.png]
const { openPage, sleep, stackEnv } = require('./session.js');

// Folder -> rows it holds, per settings tab, in that order (the folder may hold other rows around them: the
// basic Gameplay folder gets its Bhop Mode row at run time, first; the rows of the client join at its end).
const ADVANCED = {
  Display: { Interface: ['Exit Button', 'Custom CSS', 'Show Game Time'] },
  Game: {
    Gameplay: ['Auto-restart for updates', 'Start Offline', 'Auto Join', 'Account Mode', 'Account Service URL',
      'Server Finder URL', 'Auto Spectate', 'Always Play HS Sound on Kill', 'Client Folder', 'Clear Cache & Storage',
      'Reset Client Settings'],
    'Gameplay Privacy': ['Streamer Mode', 'Hide player names'],
  },
};
// The basic General tab keeps v10's curated list and Quickplay; the basic Game tab gets the same rows.
const BASIC = {
  Display: ADVANCED.Display,
  Game: {
    'Gameplay Privacy': ['Hide player names'],
    Gameplay: ['Auto-restart for updates', 'Auto Spectate', 'Always Play HS Sound on Kill', 'Clear Cache & Storage'],
  },
};
// The native-only rows: none of them is generated on the web build.
const NATIVE_ONLY = ['Window Mode', 'V-Sync', 'Audio Backend', 'Process Priority', 'Discord Rich Presence', 'Resource Swapper', 'Proxy', 'Proxy Scope'];
// The rows that moved out of the Client tab (each one's controls are run below).
const MOVED = new Set(['Exit Button', 'Custom CSS', 'Show Game Time', 'Auto-restart for updates', 'Start Offline', 'Auto Join',
  'Account Mode', 'Account Service URL', 'Server Finder URL', 'Client Folder', 'Clear Cache & Storage', 'Reset Client Settings',
  'Auto Spectate', 'Always Play HS Sound on Kill', 'Hide player names']);
// The rows appear in this order (others may sit between them).
const inOrder = (got, want) => {
  let at = 0;
  for (const row of got) if (row === want[at]) at++;
  return at === want.length;
};
// Rows this run does not toggle: the restart-required rows (needsRestart: changing one opens the restart
// confirmation, which would cover the next control), and Auto Join (on, it enters the match as soon as the game
// has loaded, which closes this window by design). The native settings test covers them.
const SKIP_ROWS = ['Start Offline', 'Account Mode', 'Auto Join'];
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
  // The settings row named `label` (its own text, not its controls') in the open tab, by name: a notice a
  // button opens can re-render the holder, so positions are not kept.
  const ROW_OF = `const rowOf = (label) => [...document.querySelectorAll('#settHolder .settName')].find(e =>
    [...e.childNodes].filter(n => n.nodeType === 3).map(n => n.textContent).join('').trim() === label);`;
  const checkedOf = (label) => page.cdp.eval(`(() => { ${ROW_OF}
    const box = rowOf(${JSON.stringify(label)})?.querySelector('input[type=checkbox]'); return box ? box.checked : null; })()`);
  // Clicks the k-th element matching sel inside the row named `label`.
  const clickNamed = async (label, sel, k = 0) => {
    const ok = await page.cdp.eval(`(() => { document.querySelectorAll('[data-cct]').forEach(e => e.removeAttribute('data-cct'));
      ${ROW_OF}
      const e = rowOf(${JSON.stringify(label)})?.querySelectorAll(${JSON.stringify(sel)})[${k}]; if (!e) return false; e.setAttribute('data-cct', '1'); return true; })()`);
    return ok && page.clickSel('[data-cct="1"]');
  };
  // The tab's element index by its name (the tab row of the open view).
  const tabNamed = (name) => page.cdp.eval(`(() => { const t = [...document.querySelectorAll('#settingsTabLayout .settingTab')];
    return t.findIndex(e => e.textContent.trim() === ${JSON.stringify(name)}); })()`);
  // The Advanced switch (typeBtn): on for the advanced view, off for the basic one.
  const setAdvanced = async (on) => {
    const cur = await page.cdp.eval(`(() => { const b = document.getElementById('typeBtn'); return b ? b.checked : null; })()`);
    if (cur !== null && cur !== on) {
      await page.cdp.eval(`(() => { document.querySelectorAll('[data-cct]').forEach(e => e.removeAttribute('data-cct'));
        const b = document.getElementById('typeBtn'); const t = b.closest('label') || b.parentElement; t.setAttribute('data-cct', '1'); return true; })()`);
      await page.clickSel('[data-cct="1"]');
      await sleep(800);
    }
    return page.cdp.eval(`(() => { const b = document.getElementById('typeBtn'); return !!(b && b.checked) === ${on}; })()`);
  };
  const openTab = async (name) => {
    const i = await tabNamed(name);
    if (i < 0) return false;
    await clickNth('#settingsTabLayout .settingTab', i);
    await sleep(900);
    return true;
  };

  try {
    // A stored value the page must read: Hide player names on (knClient), set before the settings open.
    await page.cdp.eval(`(() => { try { localStorage.setItem('knClient', JSON.stringify({ hidePlayerNames: true })); } catch (e) {} return true; })()`);
    await page.cdp.eval(`location.reload()`);
    let menu = false;
    for (let i = 0; i < 120 && !menu; i++) { await sleep(500); menu = await page.visible('#menuBtnQuickMatch'); }
    check(menu, 'the main menu renders');
    // Settings: the top bar's Settings item (v10_menu.c wires it as #knV10Settings: showWindow(1)).
    let link = null;
    for (let i = 0; i < 60 && !link; i++) { await sleep(500); link = await page.rect('#knV10Settings'); }
    check(!!link && await page.clickId('knV10Settings'), 'the Settings link opens the settings window');
    await sleep(1200);

    // The web build has no Client tab: its rows are in the game's tabs.
    check(await setAdvanced(false), 'the basic view is on');
    check(await tabNamed('Client') < 0, 'the basic view has no Client tab (web)');
    for (const [tab, want] of Object.entries(BASIC)) {
      check(await openTab(tab), `basic view: ${tab} tab`);
      const fold = await folders();
      for (const [name, rows] of Object.entries(want)) {
        const got = fold[name] || [];
        check(inOrder(got, rows), `basic ${tab} > ${name}: ${JSON.stringify(got)}`);
      }
    }
    check(await checkedOf('Hide player names') === true, 'the stored Hide player names is read (on)');

    // The advanced view: the same folders, plus the Sound tab's Audio Output row.
    check(await setAdvanced(true), 'the advanced view is on');
    check(await tabNamed('Client') < 0, 'the advanced view has no Client tab (web)');
    for (const [tab, want] of Object.entries(ADVANCED)) {
      check(await openTab(tab), `advanced view: ${tab} tab`);
      const fold = await folders();
      for (const [name, rows] of Object.entries(want)) {
        const got = fold[name] || [];
        check(inOrder(got, rows), `advanced ${tab} > ${name}: ${JSON.stringify(got)}`);
      }
      const names = Object.values(fold).flat();
      for (const bad of NATIVE_ONLY) check(!names.includes(bad), `advanced ${tab}: no native-only row "${bad}" on web`);
    }
    await openTab('Game');
    await page.shot(shot);

    // Every moved control runs: switches toggle twice (back to the start), selects move to their other
    // option and back, buttons open their confirmation or notice (dismissed or noticed).
    for (const tab of Object.keys(ADVANCED)) {
      await openTab(tab);
      const got = await labels();
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
        if (!MOVED.has(name) || SKIP_ROWS.includes(name)) continue;
        const snap = page.mark();
        if (kind === 'switch') {
          await clickNamed(name, 'label.switch');
          await sleep(300);
          await clickNamed(name, 'label.switch');
          await sleep(300);
        } else if (kind.startsWith('select:') && Number(kind.slice(7)) > 1) {
          // move to the second option (change event as the page's onchange) and back
          for (const idx of [1, 0]) {
            await page.cdp.eval(`(() => { ${ROW_OF}
              const s = rowOf(${JSON.stringify(name)})?.querySelector('select');
              if (!s) return false; s.selectedIndex = ${idx}; s.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
            await sleep(300);
          }
        } else if (kind === 'button') {
          // Open / Edit / Clear Now / Reset: each opens a notice or a declined dialog
          const count = await page.cdp.eval(`(() => { ${ROW_OF} return rowOf(${JSON.stringify(name)})?.querySelectorAll('.settingsBtn').length || 0; })()`);
          for (let b = 0; b < count; b++) { await clickNamed(name, '.settingsBtn', b); await sleep(400); }
        }
        const d = page.since(snap);
        const bad = [...d.exceptions, ...d.errors.filter((t) => !t.startsWith('[ui] '))];
        check(bad.length === 0, `control "${name}" (${kind}) runs without an exception or console error ${JSON.stringify(bad.slice(0, 2))}`);
      }
    }
    check(await checkedOf('Hide player names') === true, 'Hide player names is back on after its two toggles');

    // The game's Sound tab gains the Audio Output row.
    check(await openTab('Sound'), 'the advanced view has the Sound tab');
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
