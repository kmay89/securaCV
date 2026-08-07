/* lab-settings — the Lab's Settings panel: which build is running, whether a
   newer one exists, and the log of every update the app has ever done.

   App-only by construction. The browser Lab has no updater and no app data
   dir, so `IN_APP` is false there, the shell never offers the entry, and
   nothing below runs. Everything it shows comes from the native side
   (desktop-lab/src-tauri): `app_info` for the build stamp, `check_update` /
   `install_update` for the release channel, `read_update_journal` /
   `update_journal_path` for the journal `self_update.rs` has always written.

   The journal is the record — "last checked" is derived from its newest line
   rather than tracked separately, so the panel can't claim a check that isn't
   written down. canary-local/tests/lab_settings.test.js is the parity gate
   against the Flasher's About panel. */

export const IN_APP = !!(window.__TAURI__ && window.__TAURI__.core);

const invoke = (cmd, args) => window.__TAURI__.core.invoke(cmd, args);
const listen = (evt, cb) =>
  window.__TAURI__.event ? window.__TAURI__.event.listen(evt, cb) : Promise.resolve(() => {});

/* ---- tiny hyperscript (same idiom as lab-shell.js / lab-nav.js) ---- */
function h(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  if (attrs) for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === "class") n.className = v;
    else if (k === "html") n.innerHTML = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2).toLowerCase(), v);
    else n.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) {
    if (c == null || c === false) continue;
    n.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return n;
}

/* ---- formatting ---- */
const pad = (n) => String(n).padStart(2, "0");
function fmtEpoch(secs) {
  if (!secs) return "—";
  const d = new Date(secs * 1000);
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}
// Journal lines lead with `YYYY-MM-DD HH:MM:SSZ` (self_update.rs:utc_stamp).
const JOURNAL_STAMP = /^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})Z\s+(.*)$/;
function splitLine(line) {
  const m = JOURNAL_STAMP.exec(line);
  return m ? { when: `${m[1]} ${m[2]}Z`, msg: m[3] } : { when: "", msg: line };
}

/* Release notes are markdown-ish (`- ` bullets, `**bold**`). Render them as
   real elements — never innerHTML, so release copy can't inject markup. */
function notesNode(notes) {
  const box = h("div", { class: "set-notes" }, h("b", {}, "What's changing"));
  const list = h("ul", {});
  let any = false;
  for (const raw of String(notes).split("\n")) {
    const line = raw.trim().replace(/\*\*/g, "");
    if (!line) continue;
    if (line.startsWith("- ")) { list.append(h("li", {}, line.slice(2))); any = true; }
    else box.append(h("p", {}, line));
  }
  if (any) box.append(list);
  return box;
}

/* ---- the panel ---- */
export function settingsView() {
  const root = h("div", { class: "settings" });

  root.append(
    h("div", { class: "crumbs" }, h("b", {}, "Settings")),
    h("div", { class: "c-h" }, h("h1", {}, "Updates & about")),
  );

  const specs = h("dl", { class: "set-specs" });
  const updateBox = h("div", { class: "set-card" }, h("p", { class: "set-status" }, "Checking the release channel…"));
  const logBox = h("div", { class: "set-log" }, h("div", { class: "set-log-row" }, h("span", { class: "muted" }, "Reading the update journal…")));
  const pathNote = h("p", { class: "set-path" });

  root.append(
    h("section", { class: "set-sec" }, h("h2", {}, "This build"), specs),
    h("section", { class: "set-sec" }, h("h2", {}, "Updates"), updateBox),
    h("section", { class: "set-sec" },
      h("h2", {}, "Update log"),
      h("p", { class: "set-lede" },
        "Every check and install this app has done, newest first — written to disk as it happens, so what the app did is visible and recoverable, never silent."),
      logBox, pathNote),
  );

  hydrate({ specs, updateBox, logBox, pathNote });
  return root;
}

async function hydrate(ui) {
  // ---- this build -------------------------------------------------------
  let info = {};
  try { info = (await invoke("app_info")) || {}; } catch { /* shown as — below */ }
  let train = null;
  try {
    const reg = await (await fetch("devices/registry.json", { cache: "no-cache" })).json();
    train = reg && reg.fw_train;
  } catch { /* optional */ }

  const rows = [
    ["Version", info.version ? "v" + info.version : "—"],
    ["Build", info.build_rev && info.build_rev !== "source" ? info.build_rev : "source"],
    ["Built", fmtEpoch(info.build_epoch)],
    ["Firmware train", train || "—"],
  ];
  ui.specs.replaceChildren(
    ...rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]),
  );

  // ---- the journal (also gives us "last checked") ------------------------
  await refreshLog(ui);
  try { ui.pathNote.textContent = "Kept at " + (await invoke("update_journal_path")); } catch { /* optional */ }

  // A live install appends to the journal through this event; mirror it so the
  // log grows while you watch instead of going quiet mid-install.
  listen("update:log", (ev) => {
    if (typeof ev.payload === "string") prependLog(ui, ev.payload);
  });

  // ---- the release channel ----------------------------------------------
  await runCheck(ui, info);
}

async function refreshLog(ui) {
  let lines = [];
  try { lines = (await invoke("read_update_journal")) || []; } catch { /* below */ }
  if (!lines.length) {
    ui.logBox.replaceChildren(h("div", { class: "set-log-row" },
      h("span", { class: "muted" }, "Nothing logged yet — this app hasn't checked for an update on this machine.")));
    return;
  }
  ui.logBox.replaceChildren(...lines.map((l) => {
    const { when, msg } = splitLine(l);
    return h("div", { class: "set-log-row" }, h("time", {}, when), h("span", { class: "l-msg" }, msg));
  }));
}

function prependLog(ui, line) {
  const { when, msg } = splitLine(line);
  const row = h("div", { class: "set-log-row" }, h("time", {}, when), h("span", { class: "l-msg" }, msg));
  const first = ui.logBox.firstElementChild;
  if (first && first.querySelector(".muted")) ui.logBox.replaceChildren(row);
  else ui.logBox.prepend(row);
}

async function runCheck(ui, info) {
  ui.updateBox.replaceChildren(h("p", { class: "set-status" }, "Checking the release channel…"));
  let update = null, failed = null;
  try { update = await invoke("check_update"); }
  catch (e) { failed = String(e); }
  await refreshLog(ui);   // the check itself was just journaled

  const checkBtn = (label) =>
    h("button", { class: "btn ghost", onclick: () => runCheck(ui, info) }, label);

  if (failed) {
    // Offline is the ordinary case here, not a fault — say so plainly.
    ui.updateBox.replaceChildren(
      h("p", { class: "set-status warn" }, "Couldn't reach the release channel."),
      h("p", { class: "set-lede" }, failed),
      h("p", { class: "set-lede" },
        "The Lab works entirely offline; this only affects whether it can see a newer version."),
      h("div", { class: "actions" }, checkBtn("Try again")),
    );
    return;
  }

  if (!update) {
    ui.updateBox.replaceChildren(
      h("p", { class: "set-status ok" },
        info.version ? `You're on the newest build (v${info.version}).` : "You're on the newest build."),
      h("p", { class: "set-lede" },
        "The Lab checks on its own — shortly after launch and every six hours it stays open. " +
        "Updates are signed and verified before they install, and nothing installs without your OK."),
      h("div", { class: "actions" }, checkBtn("Check now")),
    );
    return;
  }

  const install = h("button", { class: "btn primary" }, "Update & relaunch");
  install.addEventListener("click", async () => {
    install.disabled = true;
    install.textContent = "Installing…";
    try { await invoke("install_update"); }   // relaunches on success
    catch (e) {
      install.disabled = false;
      install.textContent = "Update & relaunch";
      ui.updateBox.append(h("p", { class: "set-status warn" }, String(e)));
      await refreshLog(ui);
    }
  });

  ui.updateBox.replaceChildren(
    h("p", { class: "set-status ok" },
      `Version ${update.version} is ready to install (you have ${update.current_version}).`),
    update.notes ? notesNode(update.notes) : null,
    h("p", { class: "set-lede" }, "Signed and verified before it installs."),
    h("div", { class: "actions" }, install, checkBtn("Check again")),
  );
}
