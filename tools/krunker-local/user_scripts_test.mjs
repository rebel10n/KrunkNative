// User scripts of the official-client harness (user_scripts.mjs, doc/v10/userscripts.md). The page is a stub: a vm context
// with a window object. A user script runs there first, then a "page script" reads what it left. No network, no Chromium.
// Run: node --test tools/krunker-local/user_scripts_test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import vm from "node:vm";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loadUserScripts, userScriptPaths, wrapUserScript } from "./user_scripts.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));

function tmpFile(name, source) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kn-userscripts-"));
  const file = path.join(dir, name);
  fs.writeFileSync(file, source);
  return file;
}

// A stub page: the window the scripts see, and the console errors the wrapper logs
function stubPage() {
  const window = {};
  const errors = [];
  const context = vm.createContext({ window, console: { error: (m) => errors.push(String(m)), log: () => {} } });
  const run = (script) => vm.runInContext(wrapUserScript(script), context);
  const page = (source) => vm.runInContext(source, context);
  return { window, errors, run, page };
}

test("a user script sets a flag before the page scripts run, and the page sees it", () => {
  const [a] = loadUserScripts([tmpFile("a.js", "window.__userA = true;")]);
  const p = stubPage();
  p.run(a);
  p.page("window.pageSaw = window.__userA === true;");
  assert.equal(p.window.pageSaw, true);
  assert.deepEqual(p.errors, []);
});

test("two user scripts run in the order given", () => {
  const scripts = loadUserScripts([
    tmpFile("first.js", "window.order = []; window.order.push('first');"),
    tmpFile("second.js", "window.order.push('second');"),
  ]);
  const p = stubPage();
  for (const s of scripts) p.run(s);
  assert.deepEqual(Array.from(p.window.order), ["first", "second"]); // Array.from: the array was made in the vm realm
});

test("a script that throws is logged and does not stop the next one", () => {
  const scripts = loadUserScripts([
    tmpFile("boom.js", "throw new Error('boom');"),
    tmpFile("after.js", "window.afterRan = true;"),
  ]);
  const p = stubPage();
  for (const s of scripts) p.run(s);
  assert.equal(p.window.afterRan, true);
  assert.equal(p.errors.length, 1);
  assert.match(p.errors[0], /^\[user-script\] boom\.js threw: Error: boom/);
});

test("a missing file is refused by loadUserScripts, before anything starts", () => {
  const missing = path.join(os.tmpdir(), "kn-userscripts-no-such-file.js");
  assert.throws(() => loadUserScripts([missing]), /user script not found: .*no-such-file\.js/);
});

test("the CLI exits 1 with the error for a missing --user-script, before it launches Chromium", () => {
  const missing = path.join(os.tmpdir(), "kn-userscripts-no-such-file.js");
  const r = spawnSync(process.execPath, [path.join(HERE, "krunker-local.mjs"), "join", "--user-script", missing], {
    encoding: "utf8", env: { ...process.env, KRUNKER_LOCAL_USER_SCRIPTS: "" },
  });
  assert.equal(r.status, 1);
  assert.match(r.stderr, /krunker-local: user script not found: /);
});

test("the paths: KRUNKER_LOCAL_USER_SCRIPTS first (colon-separated), then --user-script in the order given", () => {
  const argv = ["node", "krunker-local.mjs", "join", "--user-script", "/c.js", "--user-script=/d.js"];
  assert.deepEqual(userScriptPaths(argv, { KRUNKER_LOCAL_USER_SCRIPTS: "/a.js:/b.js" }), ["/a.js", "/b.js", "/c.js", "/d.js"]);
  assert.deepEqual(userScriptPaths(["node", "x.mjs"], {}), []);
  assert.throws(() => userScriptPaths(["node", "x.mjs", "--user-script"], {}), /needs a FILE/);
});
