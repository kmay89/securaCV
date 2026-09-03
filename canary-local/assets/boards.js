/* boards.js — lifted out of boards.html so the page can carry a strict Content-Security-Policy
   (script-src 'self', no inline, no hashes to re-pin on every edit; the policy table is
   canary-local/tools/gen_csp.py). Same code, same load order — only the file moved. */
// A module specifier resolves against this file, not the page — it was
// "./assets/board-room.js" while the script was inline in boards.html.
import { buildBoardRoom } from "./board-room.js";
const mount = document.getElementById("boardroom");
const grab = (p) => fetch(p).then((r) => (r.ok ? r.json() : null)).catch(() => null);
Promise.all([grab("devices/boards.json"), grab("devices/wiring.json")])
  .then(([boards, wiring]) => buildBoardRoom(mount, boards, wiring));
