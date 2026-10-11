# Krunker v3.1.1 reference captures

Screenshots and DOM dumps of the REAL Krunker v3.1.1 client (the offline server
from the `Krunker_v3.1.1` tree, headless Chromium 1280x720 unless noted), taken
2026-10-10. They are the acceptance reference for KrunkNative's HTML/CSS UI:
our menu and HUD are diffed against these images.

- `menu/` — logged-out loaded menu (`real-1280.png`, `real-1920.png`), the
  settings/class/loadout/servers/custom-games windows (`real-1280-win*.png`),
  and the loaded DOM with inline styles (`real-1280.dom.html`).
- `windows/` — every menu window (`win-*.png` + `win-*.html`).
- `ingame/` — HUD states from a real two-client match: plain (720p and 1080p),
  scoped, reloading, scoreboard (Tab), kill feed, chat open, death card, end
  screen, ESC pause, one hit-marker frame; DOM dumps for plain, scoreboard,
  death and end screen.

How they were produced: `PORT=6768 LOBBY_COUNT=1 npm run server` in the
reference tree, `localStorage.krk_force_offline_sandbox=false`, a
`requestPointerLock` shim so the real `enterGame()` path runs, CDP-driven
clicks/keys. Nothing in the reference tree was modified.
