// User scripts for the official-client harness. A user script is a file whose contents run in every new document of the
// official page, through Page.addScriptToEvaluateOnNewDocument: after the harness's own profile seed, before the page's
// scripts (doc/v10/userscripts.md). The files are read only; nothing is written to the official tree or to the repo.
import fs from "node:fs";
import path from "node:path";

// The list of paths, in the order they run: the colon-separated KRUNKER_LOCAL_USER_SCRIPTS first, then each
// --user-script FILE (or --user-script=FILE) on the command line.
export function userScriptPaths(argv = process.argv, env = process.env) {
  const out = (env.KRUNKER_LOCAL_USER_SCRIPTS || "").split(":").filter((p) => p !== "");
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--user-script") {
      if (i + 1 >= argv.length) throw new Error("--user-script needs a FILE");
      out.push(argv[++i]);
    } else if (a.startsWith("--user-script=")) {
      out.push(a.slice("--user-script=".length));
    }
  }
  return out;
}

// Each path must be an existing, readable file. The error names the path; nothing is started before this runs.
export function loadUserScripts(paths) {
  return paths.map((p) => {
    const file = path.resolve(p);
    let st;
    try { st = fs.statSync(file); } catch (e) { throw new Error(`user script not found: ${p}`); }
    if (!st.isFile()) throw new Error(`user script is not a file: ${p}`);
    let source;
    try { source = fs.readFileSync(file, "utf8"); } catch (e) { throw new Error(`user script not readable: ${p} (${e.code || e.message})`); }
    return { name: path.basename(file), path: file, bytes: Buffer.byteLength(source, "utf8"), source };
  });
}

// One line for the harness log: the scripts in the order they run, with their byte counts.
export function describeUserScripts(scripts) {
  return `user scripts (${scripts.length}): ` + scripts.map((s) => `${s.name} (${s.bytes} bytes)`).join(", ");
}

// The source the page receives. The file runs inside a try/catch: a throw is logged on the page console (the harness report
// keeps the page's console errors) and does not stop the scripts after it. A file that does not parse fails alone, too,
// since each file is its own addScriptToEvaluateOnNewDocument call.
export function wrapUserScript(script) {
  return `(function () { try {\n${script.source}\n} catch (e) { console.error("[user-script] " + ${JSON.stringify(script.name)} + " threw: " + (e && e.stack ? e.stack : e)); } })();`;
}
