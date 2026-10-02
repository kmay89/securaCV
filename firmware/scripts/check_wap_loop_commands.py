#!/usr/bin/env python3
"""Hold the canary-wap's loop-task ownership: mesh, Chirp and Bluetooth
commands, mesh and Chirp status reads, MQTT re-inits, and the MQTT client's
network timeout.

Sweep F96: `canary_wap.ino`'s `handle_mesh_*` REST handlers called
`remove_peer`, `leave_opera`, `start_pairing_*`, `cancel_pairing`,
`confirm_pairing`, `set_enabled`, `set_opera_name` and `clear_alerts` on
esp_http_server's task, while `mesh_network::update()` read and wrote the
same peer table, pairing session, opera config (and its one `g_prefs` NVS
handle) and alert history on the loop task. Now those functions are
internal to `mesh_network.cpp`, a handler hands a `Command` to
`mesh_network::submit()`, and `update()` drains the command ring on the
loop task (`loop_command_ring.h`).

Sweep F106: `csi_mqtt::init()` tore the esp_mqtt client down and built a
new one. A config POST and `POST /api/mqtt/test` ran it on the httpd task,
and a QR hub provision on the scanner's, under a loop-task publish holding
the old handle. Now the client is the loop task's: other tasks call
`request_reinit()`, and `csi_mqtt::loop()` serves the re-init. And the loop
task never stops a client itself (the F106 review): `esp_mqtt_client_stop()`
can wait out a whole connect attempt (the esp_mqtt task holds the client's
lock across it, 10 s by default), past the loop task's 8 s panic watchdog.
The loop task detaches the client and a one-shot worker (`retire_task`)
stops and destroys it; a later pass opens the new one.

Sweep F110: `GET /api/mesh`, `/api/mesh/peers` and `/api/mesh/alerts` read
the peer table, the pairing session, the opera config and the alert history
on the httpd task while `update()` writes them on the loop task. Now
`update()` publishes a `StatusView` at the end of every pass and after each
command it drains (and `init()` the first), the alert history is a log the
loop task changes under its lock,
and the three routes read whole copies through `read_status()` and
`read_alerts()` (`loop_snapshot.h`).

`test_mesh_commands_wap.cpp`, `test_mqtt_reinit.cpp`,
`test_loop_command_ring.cpp` and `test_loop_snapshot.cpp` run the real code
on the host. No host test can
compile `canary_wap.ino`, and none can see which task a call will run on;
this check holds the sources to the shape those tests assume.

## The rules

Mesh (F96):

1. `mesh_network.h` declares none of the owner commands' functions
   (`MUTATORS` below) in `namespace mesh_network`; `mesh_network.cpp`
   defines each once, `static`. Nothing outside the file can call them.
2. In `mesh_network.cpp` they are called only from `run_command()` (the
   drain's runner), except `cancel_pairing()`, which `update()` (the pairing
   timeout) and the pairing frame handlers (dispatched from `update()`) call
   too. `run_command(` is never called directly: only `drain(` runs it.
3. `update()` drains the ring (`g_commands.drain(run_command);`) once, before
   its first `return` (a disabled mesh, which is what a fresh device is,
   still takes `enable` and `pair/start`); nothing else in the file drains.
4. `submit()` only posts and waits (`loop_command_ring::submit(`): it names
   no owner command, no `run_command(` and no `drain(`. The ESP-NOW
   callbacks (`espnow_recv_cb`, `espnow_send_cb`, on the Wi-Fi task) name
   none of them either.
5. Across the sketch (comments and strings blanked), no file but
   `mesh_network.cpp` names `mesh_network::<owner command>(`, and no file
   says `using namespace mesh_network`. Each of the nine changing
   `handle_mesh_*` handlers in `canary_wap.ino` calls
   `mesh_network::submit(` exactly once and answers a command that did not
   run with `mesh_network::not_run_status(`. `mesh_network::submit(` appears
   only in HTTP handlers (a function taking `httpd_req_t*`): from the loop
   task it would wait for itself. `mesh_network::update(` is called once,
   from the sketch's `loop()`. `http_send_error()` sets its status line with
   `http_status_line(status_code)` (`http_status_line.h`, host-tested), so a
   409 `mesh_busy` and a 503 `mesh_timeout` go out with their own lines.
   `mesh_network::save_replay_counters(` (the peer table and the one
   `g_prefs` handle) is called only from the sketch's `loop()`; the
   pre-reboot hook, which POST /api/reboot and the safe-mode retry run on
   the httpd task, calls `mesh_network::save_replay_counters_before_reboot(`,
   which saves in place only on the loop task (`xTaskGetCurrentTaskHandle()`
   against the one `init()` recorded) and otherwise hands
   `MESH_CMD_SAVE_REPLAY` to `submit(`.

Mesh status reads (F110):

9. No HTTP handler anywhere in the sketch names a live mesh reader
   (`mesh_network::get_status(`, `get_peer(`, `get_peer_count(`,
   `get_alerts(`, `get_opera_config(`, `get_pairing_session(`, `is_enabled(`,
   `has_opera(` and the rest of `LIVE_READERS`): those read what `update()`
   writes. `handle_mesh_status` and `handle_mesh_peers` each call
   `mesh_network::read_status(` once, and `handle_mesh_alerts` calls
   `mesh_network::read_alerts(` once. In `mesh_network.cpp`:
   `publish_view(` is called only from `update()`, `run_command()`,
   `init()` and `deinit()`; `update()` calls it right before its every
   `return` and as its last statement, and `run_command()` has one `return`,
   right after it (so the view shows a command before the drain posts its
   result and the handler answers: the dashboard reads the status right
   after a POST, while the loop task may still be at the rest of that pass);
   `g_status_view.publish(` only in `publish_view()`,
   `g_status_view.read(` only in `read_status()`, `g_alert_log.read(` only in
   `read_alerts()`, `.append(` only in `store_alert()`, `.clear(` only in
   `clear_alerts()`, `.attach(` only in `init()` and `clear_alerts()`, and
   `.storage(` only there and in `get_alerts()`; and `read_status()` and
   `read_alerts()` name none of the live state (`LIVE_STATE`). The rule sees
   a handler's own body, not what the functions it calls read.

MQTT (F106):

6. In `csi_mqtt.cpp`, who touches the client:
   - `esp_mqtt_client_stop(` is called only in `retire_task()` (the worker),
     and `esp_mqtt_client_destroy(` only there and once in `open_client()`,
     on its own never-started `client` after a failed
     `esp_mqtt_client_start(`. `esp_mqtt_client_init(` and
     `esp_mqtt_client_start(` are called only in `open_client()`, which
     publishes the client (`s_client.store(client`) before it starts it.
     `s_client` is written (`=`, `.store(`, `.exchange(`) only in
     `open_client()` and `detach_client()`.
   - `retire_task` is never called: `retire_finished()` hands it to
     `xTaskCreate(`, and nothing else creates it. `detach_client(` and
     `retire_finished(` are called only in `serve_reinit()`,
     `open_client(` only in `serve_reinit()` and `init()`, bare `init(`
     nowhere, and `serve_reinit(` once, in `loop()`, before
     `csi_event_egress::pump(`.
   - `serve_reinit()` returns while `retire_finished()` is false, both before
     it looks at the requests and after `detach_client()`; reads
     `s_reinit_wanted` into `wanted` after that, then runs `open_client()`,
     then `s_reinit_served.store(wanted`.
   - `request_reinit()` only bumps `s_reinit_wanted`.
   - The esp_mqtt event handler returns first for a client that is not
     `s_client` (`e->client != s_client.load(`), subscribes on `e->client`
     only, and names no lifecycle function.
7. `handle_config_post` and `handle_test` each call `request_reinit(`; no
   HTTP handler anywhere in the sketch names `init(` / `csi_mqtt::init(`,
   a lifecycle function of rule 6, an `esp_mqtt_client_*(` call, a write of
   `s_client`, `csi_mqtt::loop(`, `mesh_network::update(` or a `publish_`
   function (the owner commands of rule 1 are rule 5's).
   `set_update_auto_state()` only caches (no `publish_raw(`, no
   `build_topic(`), and `loop()` publishes the `update/auto` topic.
8. Across the sketch: `csi_mqtt::init(` is called once, from
   `register_api_routes()` (the boot init), which only `start_http_server()`
   calls (one of its two servers registers the routes per boot), which is
   called once, from `setup()`: the loop task, before `loop()` starts.
   `csi_mqtt::set_identity(` is called once, from `setup()`, and
   `csi_mqtt::loop(` once, from the sketch's `loop()`. The QR scanner
   (`qr_scan_task_fn`) calls `csi_mqtt::request_reinit(`. No file says
   `using namespace csi_mqtt`.

Chirp and Bluetooth (F111): `chirp_api.h`'s and `bluetooth_api.h`'s
handlers called the channels' mutators on esp_http_server's task while
`chirp_channel::update()` and `bluetooth_channel::update()` read and write
the same state on the loop task (and a Bluetooth PIN confirm and the pairing
timeout's cancel could both answer and delete one pending pairing). Now each
POST hands a `Command` to the channel's `submit()`, and `update()` drains
its ring (`test_chirp_commands_wap.cpp`, `test_bluetooth_commands_wap.cpp`).

C1. `mesh_network.h`'s `namespace chirp_channel` declares none of
    `CHIRP_MUTATORS`, and `bluetooth_channel.h` none of `BT_MUTATORS`; each
    channel's .cpp defines each of its own once, `static`.
C2. In each .cpp they are called only from `run_command()` and the paths
    that always called them (`CHIRP_INTERNAL`, `BT_INTERNAL`: the uncalled
    lifecycle helpers, the Bluetooth bring-up's `init()`, its NimBLE
    callbacks, `update()`'s timeouts and the mutators' own composition).
    `run_command(` is never called directly; `update()` drains the ring once,
    before its first `return` (a disabled channel still takes `enable`), and
    nothing else drains; `submit()` only posts and waits. In
    `bluetooth_channel.cpp` no function calls a bare `init(`: a command never
    brings the NimBLE stack up (it can block past the loop task's watchdog).
C3. Each changing handler (`CHIRP_HANDLERS`, `BT_HANDLERS`) calls the
    channel's `submit(` exactly once, as
    `const loop_command_ring::Wait w = <channel>::submit(...);`, and the
    statement right after it is
    `if (w != loop_command_ring::Wait::kDone) return send_not_run(req, w);`
    (a withdrawn command ran nothing and must not answer success);
    `send_not_run()` sets its status line with
    `http_status_line(<channel>::not_run_status(w))`. What the handler hands
    over (`CHIRP_HANDLER_COMMANDS`, `BT_HANDLER_COMMANDS`): the command type
    its route names (the ack: "confirmed" confirms, "resolved" dismisses), and
    each Command field that type consumes, filled from the request once,
    before the submit. The settings POSTs fill each field in its own
    `if (input["<key>"]...)` block and name it there, with the flag or mask
    bit that field's key maps to (`CHIRP_SETTINGS_FIELDS`,
    `BT_SETTINGS_FIELDS`), and nowhere else. After the command ran, a handler
    calls nothing on the channel but pure lookups (`AFTER_SUBMIT_CALLS`): it
    answers from the Result. The Bluetooth enable, advertise and pair
    handlers bring the stack up first (`if (... !bring_up()) { return`): no
    command runs `init()`, so without it a device whose boot bring-up failed
    could never turn Bluetooth on.
C4. Across the sketch (comments and strings blanked), no file but the
    channel's .cpp names `chirp_channel::<mutator>(` or
    `bluetooth_channel::<mutator>(`, and none says `using namespace` for
    either. `<channel>::submit(` appears only in HTTP handlers (from the loop
    task it would wait for itself), and `<channel>::update(` once, from the
    sketch's `loop()`, as a statement of its own at the top level of its body
    with no `return` before it: the drain runs every pass, a disabled
    channel's included (that is where its enable waits).
    `bluetooth_channel::init(` is called only by `bluetooth_api.h`'s
    `bring_up()` (an HTTP handler's, as `enable()` did there before) and the
    sketch's `ble_bringup_task()`.

Chirp status reads (F138, the Chirp half): `GET /api/chirp`, `/nearby` and
`/recent` (`chirp_api.h`) read the session, cooldowns, mute and the recent
and nearby tables on the httpd task while `chirp_channel::update()` and the
chirp frames `mesh_network::update()` hands it rewrite them on the loop
task. Now `chirp_channel.cpp` publishes a `StatusView` every pass, after
each command and from `init()`, and the two tables whenever a frame, the
prune, a command or `init()` changed them (`g_tables_changed`), and the
routes read whole copies through `read_status()`, `read_nearby()` and
`read_recent()` (`loop_snapshot.h`; `test_chirp_commands_wap.cpp`).

CV1. No HTTP handler anywhere in the sketch names a live Chirp reader
     (`chirp_channel::get_status(`, `get_recent_chirps(`,
     `get_nearby_devices(`, `can_send_chirp(`, `has_presence_requirement(`
     and the rest of `CHIRP_LIVE_READERS`).
CV2. `handle_chirp_status`, `handle_chirp_nearby` and `handle_chirp_recent`
     each call their reader (`read_status(`, `read_nearby(`,
     `read_recent(`) once, and nothing else on the channel but pure lookups
     of the copy (`CHIRP_VIEW_LOOKUPS`).
CV3. In `chirp_channel.cpp`, `publish_view(` is called only from
     `update()`, `run_command()` and `init()`: `update()` calls it right
     before its every `return` and as its last statement, `run_command()`
     returns once, right after `g_tables_changed = true; publish_view();`
     (a read right after a POST's answer shows the command), and `init()`
     ends `g_tables_changed = true; publish_view(); return true;` (the
     HTTP server can answer before a pass runs).
CV4. Each view's `.publish(` is in `publish_view()` alone and its `.read(`
     in its own reader alone; `g_tables_changed = false` only in
     `publish_view()`, `= true` only in the four places that change the
     tables, `g_view_scratch` only in `init()` and `publish_view()`; the
     readers and `cannot_send_reason()` copy their view and name none of the
     live state (`CHIRP_LIVE_STATE`).
CV5. Every change to the tables is marked for the view: `on_espnow_recv()`
     sets `g_tables_changed = true;` once, right before its dispatch switch;
     `update()` sets it right after its prune; the functions that name the
     tables are `CHIRP_TABLE_FUNCS` (a new one is a new path to them), and
     the frame and prune paths are called only from the frame dispatch and
     `update()` (`CHIRP_TABLE_CALLERS`). Commands are rule C2's.
CV6. Each Chirp GET handler answers exactly the keys it always did
     (`CHIRP_GET_KEYS`): the dashboard parses them.

MQTT network timeout (F112): every loop-task publish runs
`esp_mqtt_client_publish()`, which writes the socket on the calling task
and takes the client's API lock, held by the esp_mqtt task across its own
socket operations; esp_mqtt's default timeout for each is 10 s, past the
loop task's 8 s watchdog (`test_mqtt_reinit.cpp`'s F112 tests, which also
hold that a loop pass never waits behind the CONNECTED burst).

M1. `open_client()` sets `cfg.network.timeout_ms = (int)kNetworkTimeoutMs;`
    before `esp_mqtt_client_init(&cfg)`. `csi_mqtt.h` defines
    `kNetworkTimeoutMs` (> 0) and `kNetworkOpsBudget` (>= 3: an esp_mqtt
    operation a publish waits behind, its own write, and room for the rest
    of the pass), and their product sits under `canary_wap.ino`'s
    `WATCHDOG_TIMEOUT_SEC` in milliseconds; the sketch static_asserts the
    same.

Bluetooth settings, events and views (F144, F143, F138): rules of their own,
in their own block below (`check_bluetooth_views`, `BV_MUTATIONS`).

BV1. A settings POST that turns Bluetooth on does it the way
     POST /api/bluetooth/enable does (F144). `handle_bluetooth_settings_set`
     brings the stack up on its own task before it submits, when the POST
     names `enabled: true`
     (`if ((cmd.set_mask & bluetooth_channel::BT_SET_ENABLED) && settings.enabled
     && !bring_up()) { return ...`), and answers a command refused for a stack
     that is not up with the init error, right after the not-run guard
     (`if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_ENABLED) { return
     send_bt_error(req, ...`). In `bluetooth_channel.cpp`, `set_settings()`
     reads `const bool was_enabled = g_settings.enabled;` before it assigns
     `g_settings = settings;` and names no `is_enabled(`: the reader returns
     the value just assigned, which is how both of its branches were dead.
BV2. The NimBLE host task's callbacks touch none of the channel's state
     (F143). In `bluetooth_channel.cpp` the callbacks (`BT_CALLBACKS`:
     `onConnect`, `onDisconnect`, `onAuthenticationComplete`,
     `onPassKeyDisplay`, `onConfirmPassKey`, `onWrite`, `onRead`,
     `onResult`, `onScanEnd`) and the helpers they build their events with
     (`make_event`, `link_event`, `post_event`) name none of `BT_LIVE_STATE`
     and no other file global but the queue (`post_event`'s `g_events`) and
     the data hook (`onWrite`'s `g_data_callback`), name nothing qualified
     `bluetooth_channel::`, call no `log_health(` and no `ble_*::` module,
     and call no function of the file but those helpers and
     `detect_device_type` (which reads only the advertisement). `onWrite`,
     `onRead` and `onResult` post with `EVENT_LOSSY_LIMIT`; a link's own
     callbacks (`BT_LINK_CALLBACKS`) never do.
     `g_events.post(` is called only in `post_event()`, and `post_event(`
     only in the callbacks. `update()` applies the events with
     `g_events.consume(apply_event);` once, before its first `return` and
     before `g_commands.drain(run_command);` (a link that ends while
     Bluetooth is off still ends; a command acts on the radio's latest
     state), and nothing else consumes them. `apply_event(` is never called
     (only `consume` runs it), and each `apply_<event>()` only from
     `apply_event()`. What a full queue does (a link's events kept, the
     drops logged by kind, a passkey failed closed) and what each event
     carries are `test_bluetooth_commands_wap.cpp`'s.
BV3. The Bluetooth status routes read only what the loop task published
     (F138). `bluetooth_channel.h` declares none of the live readers
     (`BT_LIVE_READERS`: `get_status`, `get_settings`, `get_scanned_devices`,
     `get_paired_devices`, `is_scanning`, ...), and no HTTP handler in the
     sketch names one. `handle_bluetooth_status`, `handle_bluetooth_scan_results`,
     `handle_bluetooth_paired_list` and `handle_bluetooth_settings_get` each
     call their reader (`read_status`, `read_scan`, `read_paired`,
     `read_settings`) once and nothing else on the channel but the pure
     helpers (`BT_GET_PURE`: names, distances, the address format, and the
     bring-up's own `init_fail_reason`). In `bluetooth_channel.cpp`:
     `publish_views(` is called only from `update()`, right before its every
     `return` and as its last statement, and from `run_command()`, whose one
     `return` comes right after it (so a GET right after a POST shows the
     command: the handler answers as soon as the drain posts the result);
     each view's `.publish(` only in its `publish_<view>_view()`, which only
     `publish_views()` calls; each view's `.read(` only in its readers; and
     the readers name none of `BT_LIVE_STATE`. Each route's JSON keys are
     the ones it always sent (`BT_GET_KEYS`, the response shapes), and the
     dashboard's Bluetooth panel (`web_ui.h`: `refreshBtStatus`,
     `toggleBtAdvertising`, `loadBtSettings`, `btStartScan`,
     `renderBtScanList`, `deviceIcon`, `loadBtPairedDevices`) reads no key
     its route does not send.

## It proves it bites

Each run applies mutations to the sources in memory and requires the check
to fail on every one. A mutation whose anchor moved fails the run.

Run locally:  python3 firmware/scripts/check_wap_loop_commands.py   (repo root)
CI:           firmware.yml "Regression guard", via regression_check.sh
"""

from __future__ import annotations

import functools
import re
import sys
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_event_egress_order import (  # noqa: E402  (shared C++ scanning helpers)
    AnchorMissing,
    blank_comments_and_strings as _blank,
    matching_paren,
    mutate_in,
    squash,
    the_body,
)

# The sketch is about 5 MB of source and every mutation re-checks all of it:
# blank each distinct text once.
blank_comments_and_strings = functools.lru_cache(maxsize=512)(_blank)

REPO = Path(__file__).resolve().parents[2]
SKETCH = "firmware/projects/canary-wap/arduino/canary_wap"
INO = f"{SKETCH}/canary_wap.ino"
MESH_H = f"{SKETCH}/mesh_network.h"
MESH_CPP = f"{SKETCH}/mesh_network.cpp"
MQTT_CPP = f"{SKETCH}/csi_mqtt.cpp"
MQTT_H = f"{SKETCH}/csi_mqtt.h"
CHIRP_CPP = f"{SKETCH}/chirp_channel.cpp"
CHIRP_API = f"{SKETCH}/chirp_api.h"
BT_H = f"{SKETCH}/bluetooth_channel.h"
BT_CPP = f"{SKETCH}/bluetooth_channel.cpp"
BT_API = f"{SKETCH}/bluetooth_api.h"
SKETCH_GLOBS = ("*.cpp", "*.h", "*.ino")

MUTATORS = ("set_enabled", "remove_peer", "set_opera_name", "leave_opera",
            "start_pairing_initiator", "start_pairing_joiner", "cancel_pairing",
            "confirm_pairing", "clear_alerts")
# Where cancel_pairing() may be called besides run_command(): update() (the
# pairing timeout) and the pairing frame handlers, which update() dispatches.
CANCEL_CALLERS = ("run_command", "update", "handle_pair_discover", "handle_pair_offer",
                  "handle_pair_accept", "handle_pair_confirm", "handle_pair_complete")
CHANGING_HANDLERS = ("handle_mesh_alerts_clear", "handle_mesh_enable", "handle_mesh_pair_start",
                     "handle_mesh_pair_join", "handle_mesh_pair_confirm", "handle_mesh_pair_cancel",
                     "handle_mesh_leave", "handle_mesh_remove", "handle_mesh_name")

SIG_HANDLER = r"\besp_err_t\s+(\w+)\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)"
SIG_SEND_ERROR = r"\bstatic\s+esp_err_t\s+http_send_error\s*\([^)]*\)"
SIG_UPDATE = r"\bvoid\s+update\s*\(\s*\)"
SIG_SUBMIT = r"\bloop_command_ring::Wait\s+submit\s*\([^)]*\)"
SIG_REBOOT_SAVE = r"\bbool\s+save_replay_counters_before_reboot\s*\(\s*\)"
SIG_RECV_CB = r"\bstatic\s+void\s+espnow_recv_cb\s*\([^)]*\)"
SIG_SEND_CB = r"\bstatic\s+void\s+espnow_send_cb\s*\([^)]*\)"
SIG_MQTT_LOOP = r"\bvoid\s+loop\s*\(\s*\)"
SIG_REQUEST = r"\buint32_t\s+request_reinit\s*\(\s*\)"
SIG_SET_AUTO = r"\bvoid\s+set_update_auto_state\s*\([^)]*\)"
SIG_EVENT_HANDLER = r"\bvoid\s+mqtt_event_handler\s*\([^)]*\)"
SIG_CONFIG_POST = r"\besp_err_t\s+handle_config_post\s*\([^)]*\)"
SIG_TEST = r"\besp_err_t\s+handle_test\s*\([^)]*\)"
SIG_SERVE = r"\bvoid\s+serve_reinit\s*\(\s*\)"
SIG_OPEN = r"\bbool\s+open_client\s*\(\s*\)"
SIG_RETIRE_FINISHED = r"\bbool\s+retire_finished\s*\(\s*\)"
SIG_INO_LOOP = r"\bvoid\s+loop\s*\(\s*\)"
SIG_SETUP = r"\bvoid\s+setup\s*\(\s*\)"
SIG_START_HTTP = r"\bstatic\s+void\s+start_http_server\s*\(\s*\)"
SIG_REGISTER = r"\bstatic\s+void\s+register_api_routes\s*\(\s*httpd_handle_t\s+\w+\s*\)"
SIG_QR_TASK = r"\bstatic\s+void\s+qr_scan_task_fn\s*\([^)]*\)"

# A call of a bare `init(`: not a member, not qualified, not part of a name.
BARE_INIT = r"(?<![\w:.>])init\s*\("


def body_of(code: str, sig: str, what: str, errors: list[str]) -> str | None:
    span = the_body(code, sig, what, errors)
    return None if span is None else code[span[0]:span[1]]


KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined", "alignof",
            "decltype", "static_assert", "else", "do"}


def close_brace(code: str, open_at: int) -> int:
    depth = 0
    for j in range(open_at, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return -1


def named_bodies(code: str) -> list[tuple[str, int, int]]:
    """(name, start, end) of every function definition's body in `code`: a
    name, its parameter list, then `{`, after a return type (an identifier,
    `*`, `&`, `>` or a `::` qualifier ends what precedes the name)."""
    out = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        close = matching_paren(code, m.end() - 1)
        if close < 0:
            continue
        tail = re.match(r"\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{", code[close + 1:close + 64])
        if not tail:
            continue
        before = code[max(0, m.start() - 64):m.start()].rstrip()
        if not before or not re.search(r"[\w*&>:]$", before) or re.search(r"\b(?:return|else)$", before):
            continue
        open_at = close + 1 + tail.end() - 1
        end = close_brace(code, open_at)
        if end > 0:
            out.append((name, open_at + 1, end))
    return out


def enclosing_function(spans: list[tuple[str, int, int]], pos: int) -> str | None:
    """The innermost named body holding `pos`."""
    best = None
    for name, s, e in spans:
        if s <= pos < e and (best is None or s > best[1]):
            best = (name, s, e)
    return None if best is None else best[0]


@functools.lru_cache(maxsize=1024)
def handler_spans(code: str) -> tuple[tuple[str, int, int], ...]:
    """(name, start, end) of every HTTP handler body (`esp_err_t f(httpd_req_t* r)`)."""
    out = []
    for m in re.finditer(SIG_HANDLER + r"\s*\{", code):
        end = close_brace(code, m.end() - 1)
        if end > 0:
            out.append((m.group(1), m.end(), end))
    return tuple(out)


def namespace_block(code: str, ns: str) -> str:
    """The text of every `namespace ns { ... }` block in `code`."""
    parts = []
    for m in re.finditer(r"\bnamespace\s+" + ns + r"\s*\{", code):
        end = close_brace(code, m.end() - 1)
        if end > 0:
            parts.append(code[m.end():end])
    return "\n".join(parts)


# ── Mesh (F96) ───────────────────────────────────────────────────────────

def check_mesh_internal(mesh_h: str, mesh_cpp: str, errors: list[str]) -> None:
    hcode = namespace_block(blank_comments_and_strings(mesh_h), "mesh_network")
    for fn in MUTATORS:
        if re.search(r"\b" + fn + r"\s*\(", hcode):
            errors.append(f"{MESH_H}: declares {fn}() in namespace mesh_network — the owner commands "
                          "stay internal to mesh_network.cpp, so no other task can call them (F96)")
    code = blank_comments_and_strings(mesh_cpp)
    for fn in MUTATORS:
        defs = [m for m in re.finditer(r"\b(?:bool|void)\s+" + fn + r"\s*\([^;{}]*\)\s*\{", code)]
        if len(defs) != 1:
            errors.append(f"{MESH_CPP}: expected one definition of {fn}(), found {len(defs)}")
            continue
        line_start = code.rfind("\n", 0, defs[0].start()) + 1
        if not re.match(r"\s*static\b", code[line_start:defs[0].start() + 1]):
            errors.append(f"{MESH_CPP}: {fn}() must be defined static — the owner commands are "
                          "internal to mesh_network.cpp (F96)")


def check_mesh_callers(mesh_cpp: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mesh_cpp)
    spans = named_bodies(code)
    for fn in MUTATORS:
        allowed = CANCEL_CALLERS if fn == "cancel_pairing" else ("run_command",)
        for m in re.finditer(r"(?<![\w:.>])" + fn + r"\s*\(", code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue                      # a declaration or the definition's own header
            if where not in allowed:
                errors.append(f"{MESH_CPP}: {where}() calls {fn}() — an owner command runs from "
                              f"run_command() (update()'s drain){' or the loop task paths ' + ', '.join(CANCEL_CALLERS[1:]) if fn == 'cancel_pairing' else ''} "
                              "only (F96)")
    for m in re.finditer(r"(?<![\w:.>])run_command\s*\(", code):
        if enclosing_function(spans, m.start()) is not None:
            errors.append(f"{MESH_CPP}: run_command( is called directly in "
                          f"{enclosing_function(spans, m.start())}() — only update()'s drain runs it (F96)")
    update = body_of(code, SIG_UPDATE, f"{MESH_CPP}: update()", errors)
    if update is not None:
        drain = "g_commands.drain(run_command);"
        s = squash(update)
        ret = re.search(r"\breturn\b", update)
        at = update.find("g_commands.drain(")
        if s.count(drain) != 1 or at < 0 or (ret is not None and ret.start() < at):
            errors.append(f"{MESH_CPP}: update() must run `{drain}` once, before its first return — "
                          "a disabled mesh still takes enable and pair/start (F96)")
    drains = [enclosing_function(spans, m.start()) for m in re.finditer(r"\.drain\s*\(", code)]
    if drains != ["update"]:
        errors.append(f"{MESH_CPP}: the command ring is drained in {drains or 'nothing'} — only "
                      "update(), on the loop task, drains it (F96)")
    submit = body_of(code, SIG_SUBMIT, f"{MESH_CPP}: submit()", errors)
    if submit is not None:
        if "loop_command_ring::submit(" not in submit:
            errors.append(f"{MESH_CPP}: submit() must post and wait through loop_command_ring::submit(")
        for tok in MUTATORS + ("run_command", "drain"):
            if re.search(r"\b" + tok + r"\s*\(", submit):
                errors.append(f"{MESH_CPP}: submit() names {tok}( — it runs on the HTTP server's "
                              "task and only posts the command and waits (F96)")
    reboot_save = body_of(code, SIG_REBOOT_SAVE, f"{MESH_CPP}: save_replay_counters_before_reboot()", errors)
    if reboot_save is not None:
        s = squash(reboot_save)
        if "xTaskGetCurrentTaskHandle()" not in s or "submit(make_command(MESH_CMD_SAVE_REPLAY)" not in s:
            errors.append(f"{MESH_CPP}: save_replay_counters_before_reboot() must save in place only on "
                          "the loop task (xTaskGetCurrentTaskHandle() against g_loop_task) and hand "
                          "MESH_CMD_SAVE_REPLAY to submit() from any other (F96)")
    for sig, what in ((SIG_RECV_CB, "espnow_recv_cb()"), (SIG_SEND_CB, "espnow_send_cb()")):
        cb = body_of(code, sig, f"{MESH_CPP}: {what}", errors)
        if cb is None:
            continue
        for tok in MUTATORS + ("run_command", "drain", "submit"):
            if re.search(r"\b" + tok + r"\s*\(", cb):
                errors.append(f"{MESH_CPP}: {what} names {tok}( — it runs on the Wi-Fi task (F96)")


@functools.lru_cache(maxsize=1024)
def mesh_file_findings(name: str, c: str) -> tuple[str, ...]:
    """Rule 5's per-file part, for one blanked file (cached: most files are
    the same text in every mutation)."""
    out = []
    if re.search(r"\busing\s+namespace\s+mesh_network\b", c):
        out.append(f"{name}: `using namespace mesh_network` hides the mesh's callers from "
                   "this check — call it qualified")
    if name == MESH_CPP:
        return tuple(out)
    if "mesh_network::" not in c:
        return tuple(out)
    for fn in MUTATORS:
        if re.search(r"\bmesh_network::" + fn + r"\s*\(", c):
            out.append(f"{name}: calls mesh_network::{fn}() — hand a Command to "
                       "mesh_network::submit() instead; update() runs it on the loop task (F96)")
    handlers = handler_spans(c)
    for m in re.finditer(r"\bmesh_network::submit\s*\(", c):
        if enclosing_function(handlers, m.start()) is None:
            out.append(f"{name}: mesh_network::submit( outside an HTTP handler — from the "
                       "loop task it would wait for itself (F96)")
    return tuple(out)


def check_mesh_sketch(ino: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    code = {name: blank_comments_and_strings(src) for name, src in files.items()}
    for name, c in code.items():
        errors.extend(mesh_file_findings(name, c))
    ino_code = code[INO]
    for h in CHANGING_HANDLERS:
        body = body_of(ino_code, r"\bstatic\s+esp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                       f"{INO}: {h}()", errors)
        if body is None:
            continue
        if body.count("mesh_network::submit(") != 1 or "mesh_network::not_run_status(" not in body:
            errors.append(f"{INO}: {h}() must hand its command to mesh_network::submit( once and "
                          "answer one that did not run with mesh_network::not_run_status( (F96)")
    herr = body_of(ino_code, SIG_SEND_ERROR, f"{INO}: http_send_error()", errors)
    if herr is not None and "httpd_resp_set_status(req,http_status_line(status_code));" not in squash(herr):
        errors.append(f"{INO}: http_send_error() must set its status with "
                      "httpd_resp_set_status(req, http_status_line(status_code)) — the host-tested table "
                      "(http_status_line.h) that gives 409 mesh_busy and 503 mesh_timeout their lines (F96)")
    loop = body_of(ino_code, SIG_INO_LOOP, f"{INO}: loop()", errors)
    loop_span = the_body(ino_code, SIG_INO_LOOP, "", [])
    saves = [(f, p) for f, c in code.items() for p in call_sites(c, "mesh_network::save_replay_counters")]
    in_loop = [p for f, p in saves if f == INO and loop_span is not None and loop_span[0] <= p < loop_span[1]]
    if len(in_loop) != len(saves):
        errors.append(f"{SKETCH}: mesh_network::save_replay_counters( outside the sketch's loop() "
                      f"({len(saves) - len(in_loop)} call(s)) — it reads the peer table and writes the "
                      "mesh's one g_prefs handle; from any other task (the pre-reboot hook) call "
                      "mesh_network::save_replay_counters_before_reboot() (F96)")
    if "mesh_network::save_replay_counters_before_reboot(" not in ino_code:
        errors.append(f"{INO}: the pre-reboot hook must call "
                      "mesh_network::save_replay_counters_before_reboot() (F96)")
    total = sum(c.count("mesh_network::update(") for c in code.values())
    if loop is None or loop.count("mesh_network::update(") != 1 or total != 1:
        errors.append(f"{SKETCH}: mesh_network::update( must be called once, from the sketch's "
                      f"loop() (found {total}) — its drain is the loop task's (F96)")


# ── Mesh status reads (F110) ─────────────────────────────────────────────

STATUS_HANDLERS = (("handle_mesh_status", "read_status"), ("handle_mesh_peers", "read_status"),
                   ("handle_mesh_alerts", "read_alerts"))
# What reads the loop task's live mesh state (mesh_network.h): the loop task's.
LIVE_READERS = ("get_status", "get_peer", "get_peer_count", "get_peer_by_fingerprint",
                "get_online_peer_count", "get_alerts", "get_opera_config", "get_pairing_session",
                "is_pairing", "is_active", "is_enabled", "has_opera", "get_message_stats",
                "get_self_fingerprint")
LIVE_READER_RE = r"\bmesh_network::(" + "|".join(LIVE_READERS) + r")\s*\("
# What the view's readers may not name.
LIVE_STATE = ("g_peers", "g_peer_count", "g_pairing", "g_opera_config", "g_mesh_state",
              "fill_status", "get_status", "get_alerts", "publish_view", "storage")
# Who may touch the published copies and the alert log, in mesh_network.cpp.
VIEW_CALLS = (
    (r"\bpublish_view\s*\(", ("update", "run_command", "init", "deinit"), "publish_view()",
     "the loop task publishes: update()'s passes, each drained command, init() and deinit()"),
    (r"\bg_status_view\s*\.\s*publish\s*\(", ("publish_view",), "g_status_view.publish(",
     "publish_view() builds the one view"),
    (r"\bg_status_view\s*\.\s*read\s*\(", ("read_status",), "g_status_view.read(",
     "read_status() is the view's one reader"),
    (r"\bg_alert_log\s*\.\s*read\s*\(", ("read_alerts",), "g_alert_log.read(",
     "read_alerts() is the history's one cross-task reader"),
    (r"\bg_alert_log\s*\.\s*append\s*\(", ("store_alert",), "g_alert_log.append(",
     "store_alert() records an alert, under the log's lock"),
    (r"\bg_alert_log\s*\.\s*clear\s*\(", ("clear_alerts",), "g_alert_log.clear(",
     "clear_alerts() empties the history, under the log's lock"),
    (r"\bg_alert_log\s*\.\s*attach\s*\(", ("init", "clear_alerts"), "g_alert_log.attach(",
     "the history's storage is allocated by init() (or clear_alerts() after a failed one)"),
    (r"\bg_alert_log\s*\.\s*storage\s*\(", ("init", "clear_alerts", "get_alerts"),
     "g_alert_log.storage(", "a write through the storage would bypass the log's lock"),
)
SIG_PUBLISH_VIEW = r"\bstatic\s+void\s+publish_view\s*\(\s*\)"
SIG_RUN_COMMAND = r"\bstatic\s+bool\s+run_command\s*\([^)]*\)"
SIG_READ_STATUS = r"\bvoid\s+read_status\s*\([^)]*\)"
SIG_READ_ALERTS = r"\bsize_t\s+read_alerts\s*\([^)]*\)"


@functools.lru_cache(maxsize=1024)
def live_read_findings(name: str, code: str) -> tuple[str, ...]:
    out = []
    for hname, s, e in handler_spans(code):
        m = re.search(LIVE_READER_RE, code[s:e])
        if m:
            out.append(f"{name}: HTTP handler {hname}() reads mesh_network::{m.group(1)}( — the live "
                       "mesh state is update()'s, written on the loop task; read the view "
                       "(mesh_network::read_status / read_alerts) (F110)")
    return tuple(out)


def check_mesh_status_reads(ino: str, others: dict[str, str], mesh_cpp: str, errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    for name, src in files.items():
        errors.extend(live_read_findings(name, blank_comments_and_strings(src)))
    ino_code = blank_comments_and_strings(ino)
    for h, reader in STATUS_HANDLERS:
        body = body_of(ino_code, r"\bstatic\s+esp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                       f"{INO}: {h}()", errors)
        if body is not None and len(re.findall(r"\bmesh_network::" + reader + r"\s*\(", body)) != 1:
            errors.append(f"{INO}: {h}() must read the published view with mesh_network::{reader}( "
                          "once (F110)")
    code = blank_comments_and_strings(mesh_cpp)
    spans = named_bodies(code)
    for pattern, allowed, label, why in VIEW_CALLS:
        for m in re.finditer(pattern, code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue                      # a declaration or the definition's own header
            if where not in allowed:
                errors.append(f"{MESH_CPP}: {where}() names {label} — {why} (F110)")
    update = body_of(code, SIG_UPDATE, f"{MESH_CPP}: update()", errors)
    if update is not None:
        s = squash(update)
        rets = [m.start() for m in re.finditer(r"\breturn\b", s)]
        if not s.endswith("publish_view();") or any(not s[:r].endswith("publish_view();") for r in rets):
            errors.append(f"{MESH_CPP}: update() must call publish_view() right before every return "
                          "and as its last statement — the status routes show the pass it ends, "
                          "a disabled mesh's included (F110)")
    run = body_of(code, SIG_RUN_COMMAND, f"{MESH_CPP}: run_command()", errors)
    if run is not None:
        rets = [m.start() for m in re.finditer(r"\breturn\b", run)]
        if len(rets) != 1 or not squash(run[:rets[0]]).endswith("publish_view();"):
            errors.append(f"{MESH_CPP}: run_command() must return once, right after publish_view() — "
                          "the drain posts the result when it returns and the handler answers at once, "
                          "so a status read right after the POST must already show the command (F110)")
    for sig, what, reads in ((SIG_READ_STATUS, "read_status()", "g_status_view.read("),
                             (SIG_READ_ALERTS, "read_alerts()", "g_alert_log.read(")):
        body = body_of(code, sig, f"{MESH_CPP}: {what}", errors)
        if body is None:
            continue
        if reads not in squash(body):
            errors.append(f"{MESH_CPP}: {what} must copy the published view ({reads}) (F110)")
        hit = re.search(r"\b(" + "|".join(LIVE_STATE) + r")\b", body)
        if hit:
            errors.append(f"{MESH_CPP}: {what} names {hit.group(1)} — it runs on the httpd task and "
                          "reads only what the loop task published (F110)")


# ── MQTT (F106) ──────────────────────────────────────────────────────────

# Who may call what in csi_mqtt.cpp (rule 6): the call, the functions it may
# appear in, and why.
MQTT_CALLERS = (
    ("esp_mqtt_client_stop", ("retire_task",),
     "a stop can wait out a connect attempt, past the loop task's watchdog: only the worker stops"),
    ("esp_mqtt_client_destroy", ("retire_task", "open_client"),
     "only the worker destroys a client that ran; open_client() only its own unstarted one"),
    ("esp_mqtt_client_init", ("open_client",), "only open_client() makes a client"),
    ("esp_mqtt_client_start", ("open_client",), "only open_client() starts a client"),
    ("detach_client", ("serve_reinit",), "only the loop task's re-init detaches the client"),
    ("retire_finished", ("serve_reinit",), "only the loop task's re-init retires the client"),
    ("open_client", ("serve_reinit", "init"), "the client opens on the loop task: the boot, then a re-init"),
    ("serve_reinit", ("loop",), "the re-init is served by loop(), on the loop task"),
)
# A write of the client handle.
S_CLIENT_WRITE = r"\bs_client\s*(?:=(?!=)|\.store\s*\(|\.exchange\s*\(|\.compare_exchange_\w+\s*\()"


def check_mqtt_reinit(mqtt: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mqtt)
    spans = named_bodies(code)

    def where_of(m: re.Match) -> str | None:
        return enclosing_function(spans, m.start())

    for call, allowed, why in MQTT_CALLERS:
        for m in re.finditer(r"(?<![\w:.>])" + call + r"\s*\(", code):
            where = where_of(m)
            if where is None or where == call:
                continue                      # the definition's own header
            if where not in allowed:
                errors.append(f"{MQTT_CPP}: {where}() calls {call}() — {why} (F106)")
    for m in re.finditer(S_CLIENT_WRITE, code):
        where = where_of(m)
        if where not in ("open_client", "detach_client"):
            errors.append(f"{MQTT_CPP}: {where + '()' if where else 'file scope'} writes s_client — only open_client() "
                          "and detach_client(), on the loop task, do (F106)")
    for m in re.finditer(BARE_INIT, code):
        where = where_of(m)
        if where is not None:
            errors.append(f"{MQTT_CPP}: {where}() calls init() — init() is the boot's (the sketch's "
                          "start_http_server); a re-init is serve_reinit()'s, and other tasks call "
                          "request_reinit() (F106)")
    for m in re.finditer(r"(?<![\w:.>])retire_task\b", code):
        where = where_of(m)
        if where is None or where == "retire_task":
            continue
        tail = code[m.end():m.end() + 8]
        before = code[max(0, m.start() - 24):m.start()]
        if where != "retire_finished" or not re.search(r"\bxTaskCreate\s*\(\s*$", before) or \
                tail.lstrip().startswith("("):
            errors.append(f"{MQTT_CPP}: {where}() names retire_task other than as "
                          "retire_finished()'s xTaskCreate( argument — the stop runs on that worker, "
                          "never on the caller's task (F106)")
    opened = body_of(code, SIG_OPEN, f"{MQTT_CPP}: open_client()", errors)
    if opened is not None:
        s = squash(opened)
        at_store = s.find("s_client.store(client")
        at_start = s.find("esp_mqtt_client_start(client")
        destroys = re.findall(r"esp_mqtt_client_destroy\(([^)]*)\)", s)
        if at_store < 0 or at_start < 0 or at_store > at_start:
            errors.append(f"{MQTT_CPP}: open_client() must store the new client in s_client before "
                          "esp_mqtt_client_start( — its event handler ignores any other client (F106)")
        if destroys and (destroys != ["client"] or s.find("esp_mqtt_client_destroy(") < at_start):
            errors.append(f"{MQTT_CPP}: open_client() destroys {destroys} — only its own client, "
                          "after a failed esp_mqtt_client_start( (never started: nothing to wait for) (F106)")
    serve = body_of(code, SIG_SERVE, f"{MQTT_CPP}: serve_reinit()", errors)
    if serve is not None:
        s = squash(serve)
        first_guard = s.find("if(!retire_finished())return;")
        at_wanted_cmp = s.find("s_reinit_wanted.load(")
        at_detach = s.find("detach_client();")
        second_guard = s.find("if(!retire_finished())return;", at_detach) if at_detach >= 0 else -1
        at_capture = s.rfind("constuint32_twanted=s_reinit_wanted.load(")
        at_open = s.find("open_client();")
        at_served = s.find("s_reinit_served.store(wanted")
        ok = (0 <= first_guard < at_wanted_cmp and first_guard < at_detach < second_guard < at_capture
              < at_open < at_served and s.count("open_client(") == 1)
        if not ok:
            errors.append(f"{MQTT_CPP}: serve_reinit() must wait for a retiring client without "
                          "blocking (`if (!retire_finished()) return;` first, and again after "
                          "detach_client()), then read `const uint32_t wanted = s_reinit_wanted.load(` "
                          "(a request made before the open's NVS read is the open's), run "
                          "open_client() once, then `s_reinit_served.store(wanted` (F106)")
    loop = body_of(code, SIG_MQTT_LOOP, f"{MQTT_CPP}: csi_mqtt::loop()", errors)
    if loop is not None:
        s = squash(loop)
        at_serve = s.find("serve_reinit();")
        at_pump = s.find("csi_event_egress::pump();")
        if s.count("serve_reinit();") != 1 or at_pump < 0 or at_serve > at_pump:
            errors.append(f"{MQTT_CPP}: csi_mqtt::loop() must call serve_reinit() once, before "
                          "csi_event_egress::pump() — the pass that opens a new destination's client "
                          "is the pass the pump sees its epoch (F106)")
        # The topic is a string: found in the source, placed by the blanked
        # code's offsets (the same in both).
        span = the_body(code, SIG_MQTT_LOOP, "", [])
        raw_loop = mqtt[span[0]:span[1]] if span is not None else ""
        if not re.search(r'build_topic\s*\([^;]*"update/auto"\s*\)', raw_loop) or \
                "publish_raw(" not in loop:
            errors.append(f"{MQTT_CPP}: csi_mqtt::loop() must publish the update/auto state "
                          "set_update_auto_state() left (F106)")
    finished = body_of(code, SIG_RETIRE_FINISHED, f"{MQTT_CPP}: retire_finished()", errors)
    if finished is not None and "xTaskCreate(retire_task," not in squash(finished):
        errors.append(f"{MQTT_CPP}: retire_finished() must hand the detached client to "
                      "xTaskCreate(retire_task, ...) — the stop is the worker's (F106)")
    req = body_of(code, SIG_REQUEST, f"{MQTT_CPP}: request_reinit()", errors)
    if req is not None:
        if "s_reinit_wanted.fetch_add(" not in squash(req) or re.search(
                BARE_INIT + r"|\b(?:open_client|serve_reinit|detach_client|retire_finished)\s*\(|publish",
                req):
            errors.append(f"{MQTT_CPP}: request_reinit() only bumps s_reinit_wanted — any task "
                          "calls it (F106)")
    auto = body_of(code, SIG_SET_AUTO, f"{MQTT_CPP}: set_update_auto_state()", errors)
    if auto is not None and re.search(r"\b(?:publish_raw|build_topic)\s*\(", auto):
        errors.append(f"{MQTT_CPP}: set_update_auto_state() publishes — it runs on the httpd task "
                      "too, and only caches; loop() publishes (F106)")
    handler = body_of(code, SIG_EVENT_HANDLER, f"{MQTT_CPP}: mqtt_event_handler()", errors)
    if handler is not None:
        s = squash(handler)
        guard = re.search(r"if\(!e\|\|e->client!=s_client\.load\([^)]*\)\)return;", s)
        if guard is None or guard.start() > s.find("switch("):
            errors.append(f"{MQTT_CPP}: mqtt_event_handler() must return first for an event whose "
                          "client is not s_client (`if (!e || e->client != s_client.load(...)) return;`) "
                          "— a detached client runs on until its worker's stop returns (F106)")
        if re.search(r"esp_mqtt_client_subscribe\((?!e->client,)", s):
            errors.append(f"{MQTT_CPP}: mqtt_event_handler() subscribes on another client than "
                          "e->client — the loop task may have detached s_client meanwhile (F106)")
        if re.search(BARE_INIT + r"|\b(?:open_client|serve_reinit|detach_client|retire_finished|"
                     r"esp_mqtt_client_stop|esp_mqtt_client_destroy)\s*\(", handler):
            errors.append(f"{MQTT_CPP}: mqtt_event_handler() runs a client lifecycle step — it runs on "
                          "the esp_mqtt task (F106)")
    for sig, what in ((SIG_CONFIG_POST, "handle_config_post()"), (SIG_TEST, "handle_test()")):
        body = body_of(code, sig, f"{MQTT_CPP}: {what}", errors)
        if body is not None and body.count("request_reinit(") != 1:
            errors.append(f"{MQTT_CPP}: {what} must ask the loop task for its re-init with "
                          "request_reinit( (F106)")


# What no HTTP handler may name. The owner commands of rule 1 are refused
# qualified anywhere outside mesh_network.cpp (rule 5), and unqualified they
# do not compile outside it (internal linkage).
HTTPD_FORBIDDEN = (
    (BARE_INIT, "init("),
    (r"\bcsi_mqtt::init\s*\(", "csi_mqtt::init("),
    (r"\b(?:open_client|serve_reinit|detach_client|retire_finished)\s*\(|\bretire_task\b",
     "a client lifecycle step of csi_mqtt.cpp"),
    (r"\besp_mqtt_client_\w+\s*\(", "an esp_mqtt_client_ call"),
    (S_CLIENT_WRITE, "a write of s_client"),
    (r"\bcsi_mqtt::loop\s*\(", "csi_mqtt::loop("),
    (r"\bmesh_network::update\s*\(", "mesh_network::update("),
    (r"(?<![\w:.>])publish_\w+\s*\(", "a publish_ function"),
    (r"\bcsi_mqtt::publish_\w+\s*\(", "csi_mqtt::publish_"),
)


@functools.lru_cache(maxsize=1024)
def httpd_findings(name: str, code: str) -> tuple[str, ...]:
    out = []
    for hname, s, e in handler_spans(code):
        body = code[s:e]
        for pattern, label in HTTPD_FORBIDDEN:
            if re.search(pattern, body):
                out.append(f"{name}: HTTP handler {hname}() names {label} — the httpd task "
                           "hands that work to the loop task (F96, F106)")
    return tuple(out)


def check_httpd_paths(files: dict[str, str], errors: list[str]) -> None:
    for name, src in files.items():
        errors.extend(httpd_findings(name, blank_comments_and_strings(src)))


@functools.lru_cache(maxsize=4096)
def call_sites(c: str, name: str) -> tuple[int, ...]:
    """Where `name(` is called in blanked `c`: declarations and definitions
    (a type before the name) are not calls."""
    if name.split("::")[-1] not in c:
        return ()
    out = []
    for m in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*\(", c):
        if re.search(r"\b(?:void|bool|esp_err_t|uint32_t)\s*$", c[max(0, m.start() - 40):m.start()]):
            continue
        out.append(m.start())
    return tuple(out)


def check_mqtt_sketch(ino: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    code = {name: blank_comments_and_strings(src) for name, src in files.items()}
    for name, c in code.items():
        if re.search(r"\busing\s+namespace\s+csi_mqtt\b", c):
            errors.append(f"{name}: `using namespace csi_mqtt` hides the bridge's callers from this "
                          "check — call it qualified")
    ino_code = code[INO]

    def sites(call: str) -> list[tuple[str, int]]:
        return [(fname, p) for fname, c in code.items() for p in call_sites(c, call[:-1])]

    def only_in(call: str, sig: str, where: str, times: int = 1) -> None:
        found = sites(call)
        span = the_body(ino_code, sig, f"{INO}: {where}", errors)
        inside = [p for f, p in found if f == INO and span is not None and span[0] <= p < span[1]]
        if len(inside) != len(found) or not (1 <= len(found) <= times):
            errors.append(f"{SKETCH}: `{call}` must be called only from {where} "
                          f"({'once' if times == 1 else f'at most {times} times'}; found {len(found)}, "
                          f"{len(found) - len(inside)} elsewhere) — the bridge's client is the loop "
                          "task's (F106)")

    only_in("csi_mqtt::init(", SIG_REGISTER, "register_api_routes()")
    # One of start_http_server()'s two servers registers the routes per boot.
    only_in("register_api_routes(", SIG_START_HTTP, "start_http_server()", times=2)
    only_in("start_http_server(", SIG_SETUP, "setup()")
    only_in("csi_mqtt::set_identity(", SIG_SETUP, "setup()")
    only_in("csi_mqtt::loop(", SIG_INO_LOOP, "loop()")
    qr = body_of(ino_code, SIG_QR_TASK, f"{INO}: qr_scan_task_fn()", errors)
    if qr is not None and "csi_mqtt::request_reinit(" not in qr:
        errors.append(f"{INO}: qr_scan_task_fn() must ask for the bridge's re-init with "
                      "csi_mqtt::request_reinit( — it runs on the scanner's task (F106)")


# ── Chirp and Bluetooth (F111) ───────────────────────────────────────────

CHIRP_MUTATORS = ("enable", "disable", "send_chirp", "send_all_clear", "confirm_chirp",
                  "dismiss_chirp", "clear_chirps", "mute", "unmute", "set_relay_enabled",
                  "set_urgency_filter", "deinit")
# Who may call a Chirp mutator in chirp_channel.cpp besides run_command(): the
# uncalled deinit() and send_all_clear(), which compose them.
CHIRP_INTERNAL = {"disable": ("deinit",), "send_chirp": ("send_all_clear",)}
CHIRP_HANDLERS = ("handle_chirp_enable", "handle_chirp_disable", "handle_chirp_send",
                  "handle_chirp_ack", "handle_chirp_dismiss", "handle_chirp_mute",
                  "handle_chirp_unmute", "handle_chirp_settings", "handle_chirp_confirm")

BT_MUTATORS = ("enable", "disable", "start_advertising", "stop_advertising", "start_scan",
               "stop_scan", "clear_scan_results", "start_pairing", "cancel_pairing",
               "confirm_pairing", "reject_pairing", "disconnect", "remove_paired_device",
               "clear_all_paired_devices", "set_device_trusted", "set_device_blocked",
               "set_settings", "set_device_name", "set_tx_power", "deinit")
# Who may call a Bluetooth mutator in bluetooth_channel.cpp besides
# run_command(): the paths that always did. The bring-up's init() (on its
# worker, or a handler's bring_up()), a link's end as the loop task applies
# it (apply_disconnect(), since F143: the NimBLE host's onDisconnect() only
# reports it), the loop task's update() and its timeouts, and the mutators
# composing each other. A new caller is a new task onto this state.
BT_INTERNAL = {
    "enable": ("init", "set_settings"),
    "disable": ("set_settings",),
    "start_advertising": ("init", "apply_disconnect", "start_pairing"),   # F143: the loop task's
    "stop_advertising": ("deinit", "disable"),
    "stop_scan": ("deinit", "disable", "handle_scan_timeout"),
    "clear_scan_results": ("start_scan",),
    "cancel_pairing": ("update", "reject_pairing",
                       "disable"),   # F143 review: turning Bluetooth off ends a pairing first
    "disconnect": ("deinit", "disable", "handle_inactivity_timeout"),
}
BT_HANDLERS = ("handle_bluetooth_enable", "handle_bluetooth_disable",
               "handle_bluetooth_advertise_start", "handle_bluetooth_advertise_stop",
               "handle_bluetooth_scan_start", "handle_bluetooth_scan_stop",
               "handle_bluetooth_scan_clear", "handle_bluetooth_pair_start",
               "handle_bluetooth_pair_cancel", "handle_bluetooth_pair_confirm",
               "handle_bluetooth_pair_reject", "handle_bluetooth_paired_remove",
               "handle_bluetooth_paired_clear", "handle_bluetooth_paired_trust",
               "handle_bluetooth_paired_block", "handle_bluetooth_disconnect",
               "handle_bluetooth_settings_set", "handle_bluetooth_name_set",
               "handle_bluetooth_power_set")

# What each changing handler hands the loop task (rule C3): the command type
# it makes, and the fields of the Command it fills from the request, each
# assigned once, before its submit(. A field the handler drops or overrides
# is the owner's choice lost on the way (an urgent chirp sent as info, a
# PIN confirm with no PIN). The ack handler's type depends on the request
# ("confirmed" or "resolved"); it is held on the source with its strings.
CMD_ONLY = ()
CHIRP_HANDLER_COMMANDS = {
    "handle_chirp_enable": ("CHIRP_CMD_ENABLE", CMD_ONLY),
    "handle_chirp_disable": ("CHIRP_CMD_DISABLE", CMD_ONLY),
    "handle_chirp_send": ("CHIRP_CMD_SEND", ("cmd.template_id=template_id;", "cmd.urgency=urgency;",
                                             "cmd.detail=detail;", "cmd.ttl_minutes=ttl;")),
    "handle_chirp_ack": (None, ("memcpy(cmd.nonce,nonce,sizeof(cmd.nonce));",)),
    "handle_chirp_dismiss": ("CHIRP_CMD_DISMISS", ("&cmd.nonce[i]",)),
    "handle_chirp_mute": ("CHIRP_CMD_MUTE", ("cmd.duration_minutes=duration;",)),
    "handle_chirp_unmute": ("CHIRP_CMD_UNMUTE", CMD_ONLY),
    "handle_chirp_settings": ("CHIRP_CMD_SETTINGS", CMD_ONLY),     # its fields: CHIRP_SETTINGS_PAIRS
    "handle_chirp_confirm": ("CHIRP_CMD_CONFIRM", ("&cmd.nonce[i]",)),
}
CHIRP_ACK_TYPE = ('strcmp(ack_type_str,"confirmed")==0?chirp_channel::CHIRP_CMD_CONFIRM'
                  ':chirp_channel::CHIRP_CMD_DISMISS')
BT_ADDRESS = "bluetooth_channel::parse_address(input[\"\"].as<constchar*>(),cmd.address)"
BT_HANDLER_COMMANDS = {
    "handle_bluetooth_enable": ("BT_CMD_ENABLE", CMD_ONLY),
    "handle_bluetooth_disable": ("BT_CMD_DISABLE", CMD_ONLY),
    "handle_bluetooth_advertise_start": ("BT_CMD_ADVERTISE_START", CMD_ONLY),
    "handle_bluetooth_advertise_stop": ("BT_CMD_ADVERTISE_STOP", CMD_ONLY),
    "handle_bluetooth_scan_start": ("BT_CMD_SCAN_START", ("cmd.duration_ms=duration_ms;",)),
    "handle_bluetooth_scan_stop": ("BT_CMD_SCAN_STOP", CMD_ONLY),
    "handle_bluetooth_scan_clear": ("BT_CMD_SCAN_CLEAR", CMD_ONLY),
    "handle_bluetooth_pair_start": ("BT_CMD_PAIR_START", CMD_ONLY),
    "handle_bluetooth_pair_cancel": ("BT_CMD_PAIR_CANCEL", CMD_ONLY),
    "handle_bluetooth_pair_confirm": ("BT_CMD_PAIR_CONFIRM", ("cmd.pin=input[\"\"].as<uint32_t>();",)),
    "handle_bluetooth_pair_reject": ("BT_CMD_PAIR_REJECT", CMD_ONLY),
    "handle_bluetooth_paired_remove": ("BT_CMD_PAIRED_REMOVE", (BT_ADDRESS,)),
    "handle_bluetooth_paired_clear": ("BT_CMD_PAIRED_CLEAR", CMD_ONLY),
    "handle_bluetooth_paired_trust": ("BT_CMD_PAIRED_TRUST", (BT_ADDRESS, "cmd.flag=trusted;")),
    "handle_bluetooth_paired_block": ("BT_CMD_PAIRED_BLOCK", (BT_ADDRESS, "cmd.flag=blocked;")),
    "handle_bluetooth_disconnect": ("BT_CMD_DISCONNECT", CMD_ONLY),
    "handle_bluetooth_settings_set": ("BT_CMD_SETTINGS",               # its fields: BT_SETTINGS_FIELDS
                                      ("bluetooth_channel::BluetoothSettings&settings=cmd.settings;",)),
    "handle_bluetooth_name_set": ("BT_CMD_NAME", ("strncpy(cmd.name,",)),
    "handle_bluetooth_power_set": ("BT_CMD_POWER", ("cmd.power=power;",)),
}
# The handlers that bring the Bluetooth stack up on their own task before
# they submit (bring_up(): the init() enable() ran there before F111). A
# command never runs init(), so without this a device whose boot bring-up
# failed could never turn Bluetooth on again.
BT_BRING_UP_HANDLERS = {
    "handle_bluetooth_enable": "if(!bring_up()){return",
    "handle_bluetooth_advertise_start": "if(!bluetooth_channel::is_enabled()&&!bring_up()){return",
    "handle_bluetooth_pair_start": "if(!bluetooth_channel::is_enabled()&&!bring_up()){return",
}
# The settings POSTs: each `if (input["<key>"]...) { ... }` block fills one
# field and names that field (and only it) for the loop task.
CHIRP_SETTINGS_FIELDS = {          # key: (the field it fills, the flag that names it)
    "relay_enabled": ("cmd.relay_enabled", "cmd.set_relay=true;"),
    "urgency_filter": ("cmd.urgency_filter", "cmd.set_filter=true;"),
}
BT_SETTINGS_FIELDS = {             # key: (the field it fills, the mask bit that names it)
    "enabled": ("settings.enabled", "cmd.set_mask|=bluetooth_channel::BT_SET_ENABLED;"),
    "auto_advertise": ("settings.auto_advertise", "cmd.set_mask|=bluetooth_channel::BT_SET_AUTO_ADVERTISE;"),
    "allow_pairing": ("settings.allow_pairing", "cmd.set_mask|=bluetooth_channel::BT_SET_ALLOW_PAIRING;"),
    "require_pin": ("settings.require_pin", "cmd.set_mask|=bluetooth_channel::BT_SET_REQUIRE_PIN;"),
    "device_name": ("settings.device_name", "cmd.set_mask|=bluetooth_channel::BT_SET_DEVICE_NAME;"),
    "tx_power": ("settings.tx_power", "cmd.set_mask|=bluetooth_channel::BT_SET_TX_POWER;"),
    "inactivity_timeout_sec": ("settings.inactivity_timeout_ms",
                               "cmd.set_mask|=bluetooth_channel::BT_SET_INACTIVITY;"),
    "notify_on_connect": ("settings.notify_on_connect",
                          "cmd.set_mask|=bluetooth_channel::BT_SET_NOTIFY_ON_CONNECT;"),
    "long_range_mode": ("settings.long_range_mode", "cmd.set_mask|=bluetooth_channel::BT_SET_LONG_RANGE;"),
}
# What a handler may call on the channel after its command ran: pure lookups
# of its request's own values. State the command changed is answered from
# the Result the loop task read, never from a live reader on this task.
AFTER_SUBMIT_CALLS = {"chirp_channel": ("get_template_text", "urgency_name", "send_refusal_error",
                                       "send_refusal_message"),
                      "bluetooth_channel": ()}
# Right after `const loop_command_ring::Wait w = <ns>::submit(...);`: every
# answer but kDone is a command that did not run.
NOT_RUN_GUARD = "if(w!=loop_command_ring::Wait::kDone)returnsend_not_run(req,w);"

SIG_CHANNEL_SUBMIT = r"\bloop_command_ring::Wait\s+submit\s*\([^)]*\)"
SIG_SEND_NOT_RUN = r"\besp_err_t\s+send_not_run\s*\([^)]*\)"
SIG_BRING_UP = r"\binline\s+bool\s+bring_up\s*\(\s*\)"
SIG_BT_BRINGUP_TASK = r"\bstatic\s+void\s+ble_bringup_task\s*\([^)]*\)"
SIG_BT_ENABLE = r"\bstatic\s+bool\s+enable\s*\(\s*\)"
# Each channel: (tag, ns, header, header namespace, cpp, api, mutators, internal callers, handlers)
CHANNELS = (
    ("Chirp", "chirp_channel", MESH_H, CHIRP_CPP, CHIRP_API, CHIRP_MUTATORS, CHIRP_INTERNAL,
     CHIRP_HANDLERS),
    ("Bluetooth", "bluetooth_channel", BT_H, BT_CPP, BT_API, BT_MUTATORS, BT_INTERNAL, BT_HANDLERS),
)


def check_channel_internal(tag: str, ns: str, h_name: str, h_src: str, cpp_name: str, cpp_src: str,
                           mutators: tuple[str, ...], internal: dict[str, tuple[str, ...]],
                           errors: list[str]) -> None:
    """Rules C1, C2 for one channel."""
    hcode = namespace_block(blank_comments_and_strings(h_src), ns)
    for fn in mutators:
        if re.search(r"(?<![\w:.>])" + fn + r"\s*\(", hcode):
            errors.append(f"{h_name}: declares {fn}() in namespace {ns} — the owner commands stay "
                          f"internal to {cpp_name.rsplit('/', 1)[-1]}, so no other task can call "
                          "them (F111)")
    code = blank_comments_and_strings(cpp_src)
    for fn in mutators:
        defs = list(re.finditer(r"\b(?:bool|void)\s+" + fn + r"\s*\([^;{}]*\)\s*\{", code))
        if len(defs) != 1:
            errors.append(f"{cpp_name}: expected one definition of {fn}(), found {len(defs)}")
            continue
        line_start = code.rfind("\n", 0, defs[0].start()) + 1
        if not re.match(r"\s*(?:\[\[maybe_unused\]\]\s*)?static\b", code[line_start:defs[0].start() + 1]):
            errors.append(f"{cpp_name}: {fn}() must be defined static — the {tag} owner commands "
                          "are internal to it (F111)")
    spans = named_bodies(code)
    for fn in mutators:
        allowed = ("run_command",) + internal.get(fn, ())
        for m in re.finditer(r"(?<![\w:.>])" + fn + r"\s*\(", code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue                      # a declaration or the definition's own header
            if where not in allowed:
                errors.append(f"{cpp_name}: {where}() calls {fn}() — a {tag} owner command runs "
                              f"from run_command() (update()'s drain) or {', '.join(allowed[1:]) or 'nothing else'} "
                              "(F111)")
    for m in re.finditer(r"(?<![\w:.>])run_command\s*\(", code):
        where = enclosing_function(spans, m.start())
        if where is not None:
            errors.append(f"{cpp_name}: run_command( is called directly in {where}() — only "
                          "update()'s drain runs it (F111)")
    update = body_of(code, SIG_UPDATE, f"{cpp_name}: update()", errors)
    if update is not None:
        drain = "g_commands.drain(run_command);"
        ret = re.search(r"\breturn\b", update)
        at = update.find("g_commands.drain(")
        if squash(update).count(drain) != 1 or at < 0 or (ret is not None and ret.start() < at):
            errors.append(f"{cpp_name}: update() must run `{drain}` once, before its first return — "
                          f"a disabled {tag} channel still takes enable (F111)")
    drains = [enclosing_function(spans, m.start()) for m in re.finditer(r"\.drain\s*\(", code)]
    if drains != ["update"]:
        errors.append(f"{cpp_name}: the command ring is drained in {drains or 'nothing'} — only "
                      "update(), on the loop task, drains it (F111)")
    submit = body_of(code, SIG_CHANNEL_SUBMIT, f"{cpp_name}: submit()", errors)
    if submit is not None:
        if "loop_command_ring::submit(" not in submit:
            errors.append(f"{cpp_name}: submit() must post and wait through loop_command_ring::submit(")
        for tok in mutators + ("run_command", "drain"):
            if re.search(r"(?<![\w:.>])" + tok + r"\s*\(", submit):
                errors.append(f"{cpp_name}: submit() names {tok}( — it runs on the HTTP server's "
                              "task and only posts the command and waits (F111)")
    if ns == "bluetooth_channel":
        for m in re.finditer(BARE_INIT, code):
            where = enclosing_function(spans, m.start())
            if where is not None:
                errors.append(f"{cpp_name}: {where}() calls init() — bringing the NimBLE stack up "
                              "can block past the loop task's watchdog, so no command does it; the "
                              "bring-up worker and a handler's bring_up() do (F111)")


@functools.lru_cache(maxsize=512)
def blank_comments_only(src: str) -> str:
    """Comments become spaces; string literals stay (a request key, the ack
    type's "confirmed"). Offsets and newlines are kept, as in
    blank_comments_and_strings()."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src[i] in "\"'":
            quote = src[i]
            j = i + 1
            while j < n and src[j] != quote and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


SETTINGS_BLOCK = r"\bif\s*\(\s*input\s*\[\s*\"\s*\"\s*\]\s*\.\s*is\s*<\s*JsonVariant\s*>\s*\(\s*\)\s*\)\s*\{"
# A write of a Command or settings field in a squashed block: `cmd.x = `,
# `settings.x[...] = ` or `strncpy(settings.x, `.
FIELD_WRITE = r"(?<![\w.])((?:settings|cmd)\.\w+)(?:\[[^\]]*\])?=(?!=)|strncpy\(((?:settings|cmd)\.\w+),"


def check_settings_blocks(api: str, h: str, body: str, kept: str, fields: dict[str, tuple[str, str]],
                          errors: list[str]) -> None:
    """Rule C3's settings part: each `if (input["<key>"]...) { ... }` block of a
    settings POST fills the one field its key names and names it for the loop
    task (the flag or mask bit), and nothing fills or names one outside them."""
    seen: list[str] = []
    outside = body
    for m in re.finditer(SETTINGS_BLOCK, body):
        key_m = re.search(r'input\s*\[\s*"(\w+)"', kept[m.start():m.end()])
        end = close_brace(body, m.end() - 1)
        if key_m is None or end < 0:
            errors.append(f"{api}: {h}() has a settings block this check cannot read (F111)")
            continue
        key = key_m.group(1)
        seen.append(key)
        block = squash(body[m.end():end])
        outside = outside[:m.start()] + " " * (end + 1 - m.start()) + outside[end + 1:]
        if key not in fields:
            errors.append(f"{api}: {h}() reads \"{key}\", which this check does not know — add it to "
                          "the settings table with the field it fills (F111)")
            continue
        field, flag = fields[key]
        written = {a or b for a, b in re.findall(FIELD_WRITE, block)}
        flag_field = re.match(r"(cmd\.\w+)", flag).group(1)
        if written - {flag_field} != {field} or block.count(flag) != 1 or \
                len(re.findall(r"cmd\.set_mask\|=", block)) > (1 if "set_mask" in flag else 0):
            errors.append(f"{api}: {h}()'s \"{key}\" block must fill {field} and name it with "
                          f"`{flag}` (and nothing else) — the loop task applies only the fields a "
                          "POST names, so a field filled but not named is dropped, and one named "
                          "but not filled is reset (F111)")
    for key in fields:
        if seen.count(key) != 1:
            errors.append(f"{api}: {h}() must read \"{key}\" in one settings block (found "
                          f"{seen.count(key)}) (F111)")
    s_out = squash(outside)
    stray = {a or b for a, b in re.findall(FIELD_WRITE, s_out)} & \
        ({f for f, _ in fields.values()} | {re.match(r"(cmd\.\w+)", fl).group(1) for _, fl in fields.values()})
    if stray or "cmd.set_mask|=" in s_out:
        errors.append(f"{api}: {h}() fills or names {sorted(stray) or 'a mask bit'} outside its "
                      "settings blocks (F111)")


def check_channel_handlers(ns: str, api: str, api_src: str, errors: list[str]) -> None:
    """Rule C3's per-handler part: what each changing handler hands the loop
    task, how it answers a command that did not run, and what it answers from."""
    code = blank_comments_and_strings(api_src)
    kept = blank_comments_only(api_src)
    spec = CHIRP_HANDLER_COMMANDS if ns == "chirp_channel" else BT_HANDLER_COMMANDS
    call = ns + "::submit("
    decl = "constloop_command_ring::Waitw=" + call
    for h, (ctype, fields) in spec.items():
        span = the_body(code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                        f"{api}: {h}()", errors)
        if span is None:
            continue
        body = code[span[0]:span[1]]
        s = squash(body)
        at = s.find(call)
        if at < 0 or s.count(call) != 1:
            continue                                  # rule C3's count reports it
        close = matching_paren(s, at + len(call) - 1)
        args = s[at + len(call):close]
        before, after = s[:at], s[close + 1:]
        if not before.endswith(decl[:-len(call)]) or not after.startswith(";" + NOT_RUN_GUARD):
            errors.append(f"{api}: {h}() must take `const loop_command_ring::Wait w = {call}...);` "
                          f"and answer every Wait but kDone with `{NOT_RUN_GUARD}` right after it — "
                          "a withdrawn command ran nothing, and must not answer success (F111)")
        made = f"{ns}::make_command({ns}::{ctype})" if ctype else None
        if ctype is None:                             # the ack: confirmed or resolved
            if CHIRP_ACK_TYPE not in squash(kept[span[0]:span[1]]) or args != "cmd,&r":
                errors.append(f"{api}: {h}() must submit `cmd` made as `{CHIRP_ACK_TYPE}` — "
                              "\"confirmed\" confirms, \"resolved\" dismisses (F111)")
        elif args == made + ",&r":
            if fields:
                errors.append(f"{api}: {h}() submits a bare {ctype} — it must fill {', '.join(fields)} "
                              "first (F111)")
        elif args != "cmd,&r" or before.count(f"{ns}::Commandcmd={made};") != 1 or \
                s.count("make_command(") != 1:
            errors.append(f"{api}: {h}() must submit {made} (as `cmd`, made once, or inline) — the "
                          "command its route names (F111)")
        for f in fields:
            target = re.match(r"(cmd\.\w+)=", f)
            writes = len(re.findall(re.escape(target.group(1)) + r"(?:\[[^\]]*\])?=(?!=)", s)) if target else 1
            if before.count(f) != 1 or f in after or writes != 1:
                errors.append(f"{api}: {h}() must fill the command with `{f}` once, before it "
                              "submits — the owner's request, not a default (F111)")
        for m in re.finditer(r"\b" + ns + r"::(\w+)\s*\(", after):
            if m.group(1) not in AFTER_SUBMIT_CALLS[ns]:
                errors.append(f"{api}: {h}() calls {ns}::{m.group(1)}() after its command ran — "
                              "answer from the Result the loop task read (`r.`), not from state "
                              "the loop task may be changing (F111)")
        if h in BT_BRING_UP_HANDLERS and BT_BRING_UP_HANDLERS[h] not in before:
            errors.append(f"{api}: {h}() must bring the stack up on its own task before it submits "
                          f"(`{BT_BRING_UP_HANDLERS[h]} ...`): no command runs init(), so without it a "
                          "device whose boot bring-up failed never turns Bluetooth on (F111)")
    for h, fields in (("handle_chirp_settings", CHIRP_SETTINGS_FIELDS),
                      ("handle_bluetooth_settings_set", BT_SETTINGS_FIELDS)):
        if h not in spec:
            continue
        span = the_body(code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)", "", [])
        if span is not None:
            check_settings_blocks(api, h, code[span[0]:span[1]], kept[span[0]:span[1]], fields, errors)


def check_loop_calls_updates(ino_code: str, errors: list[str]) -> None:
    """Rule C4's loop() part: each channel's update() is its own statement at
    loop()'s top level, with no return before it: its drain runs every pass,
    a disabled channel's too (that is where an enable waits)."""
    span = the_body(ino_code, SIG_INO_LOOP, "", [])
    if span is None:
        return
    body = re.sub(r"(?m)^[ \t]*#.*$", lambda m: " " * len(m.group(0)), ino_code[span[0]:span[1]])
    for _tag, ns, *_rest in CHANNELS:
        for m in re.finditer(r"\b" + ns + r"::update\s*\(", body):
            pre = body[:m.start()]
            depth = pre.count("{") - pre.count("}")
            prev = pre.rstrip()[-1:]
            if depth != 0 or prev not in (";", "{", "}", "") or re.search(r"\breturn\b", pre):
                errors.append(f"{INO}: loop() must call {ns}::update() every pass, as a statement of "
                              "its own at its top level with no return before it — a disabled "
                              "channel's enable waits in its ring for that drain (F111)")


@functools.lru_cache(maxsize=1024)
def channel_file_findings(name: str, c: str) -> tuple[str, ...]:
    """Rule C4's per-file part for one blanked file."""
    out = []
    handlers = None
    for tag, ns, _h, cpp, _api, mutators, _internal, _handlers in CHANNELS:
        if re.search(r"\busing\s+namespace\s+" + ns + r"\b", c):
            out.append(f"{name}: `using namespace {ns}` hides the {tag} channel's callers from this "
                       "check — call it qualified")
        if name == cpp or (ns + "::") not in c:
            continue
        for fn in mutators:
            if re.search(r"\b" + ns + "::" + fn + r"\s*\(", c):
                out.append(f"{name}: calls {ns}::{fn}() — hand a Command to {ns}::submit() instead; "
                           "update() runs it on the loop task (F111)")
        if handlers is None:
            handlers = handler_spans(c)
        for m in re.finditer(r"\b" + ns + r"::submit\s*\(", c):
            if enclosing_function(handlers, m.start()) is None:
                out.append(f"{name}: {ns}::submit( outside an HTTP handler — from the loop task it "
                           "would wait for itself (F111)")
    return tuple(out)


def check_channels(ino: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    code = {name: blank_comments_and_strings(src) for name, src in files.items()}
    for name, c in code.items():
        errors.extend(channel_file_findings(name, c))
    loop = body_of(code[INO], SIG_INO_LOOP, f"{INO}: loop()", errors)
    for tag, ns, h, cpp, api, mutators, internal, handlers in CHANNELS:
        if h not in files or cpp not in files or api not in files:
            errors.append(f"{SKETCH}: the {tag} channel's sources ({h}, {cpp}, {api}) are missing")
            continue
        check_channel_internal(tag, ns, h, files[h], cpp, files[cpp], mutators, internal, errors)
        api_code = code[api]
        for hname in handlers:
            body = body_of(api_code, r"\besp_err_t\s+" + hname + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                           f"{api}: {hname}()", errors)
            if body is None:
                continue
            if body.count(ns + "::submit(") != 1 or "send_not_run(" not in body:
                errors.append(f"{api}: {hname}() must hand its command to {ns}::submit( once and "
                              "answer one that did not run with send_not_run( (F111)")
        nr = body_of(api_code, SIG_SEND_NOT_RUN, f"{api}: send_not_run()", errors)
        if nr is not None and f"httpd_resp_set_status(req,http_status_line({ns}::not_run_status(w)));" \
                not in squash(nr):
            errors.append(f"{api}: send_not_run() must set its status with "
                          f"httpd_resp_set_status(req, http_status_line({ns}::not_run_status(w))) — "
                          "409 busy and 503 timeout, not 200 (F111)")
        check_channel_handlers(ns, api, files[api], errors)
        total = sum(c.count(ns + "::update(") for c in code.values())
        if loop is None or loop.count(ns + "::update(") != 1 or total != 1:
            errors.append(f"{SKETCH}: {ns}::update( must be called once, from the sketch's loop() "
                          f"(found {total}) — its drain is the loop task's (F111)")
    check_loop_calls_updates(code[INO], errors)
    # Who brings the Bluetooth stack up: the boot worker and a handler's bring_up().
    allowed = []
    for name, sig in ((BT_API, SIG_BRING_UP), (INO, SIG_BT_BRINGUP_TASK)):
        if name in code:
            span = the_body(code[name], sig, f"{name}: {sig}", errors)
            if span is not None:
                allowed.append((name, span))
    for name, c in code.items():
        for p in call_sites(c, "bluetooth_channel::init"):
            if not any(name == n and s <= p < e for n, (s, e) in allowed):
                errors.append(f"{name}: calls bluetooth_channel::init( outside bluetooth_api.h's "
                              "bring_up() and the sketch's ble_bringup_task() — the NimBLE bring-up "
                              "never runs on the loop task (F111)")


# ── Chirp status reads (F138) ────────────────────────────────────────────

# Each Chirp GET handler in chirp_api.h and the reader of the published view
# it reads (mesh_network.h's namespace chirp_channel).
CHIRP_STATUS_HANDLERS = (("handle_chirp_status", "read_status"), ("handle_chirp_nearby", "read_nearby"),
                         ("handle_chirp_recent", "read_recent"))
CHIRP_VIEW_READERS = tuple(r for _h, r in CHIRP_STATUS_HANDLERS)
# What reads the loop task's live Chirp state: the loop task's (rule CV1).
CHIRP_LIVE_READERS = ("get_status", "get_recent_chirps", "get_pending_chirps", "get_nearby_devices",
                      "get_nearby_count", "get_cooldown_tier", "get_cooldown_remaining_ms",
                      "has_presence_requirement", "can_send_chirp", "is_active", "is_enabled",
                      "is_muted", "is_relay_enabled", "get_urgency_filter", "get_session_emoji",
                      "get_session_id")
CHIRP_LIVE_READER_RE = r"\bchirp_channel::(" + "|".join(CHIRP_LIVE_READERS) + r")\s*\("
# What a Chirp GET handler may call on the channel besides its one reader:
# pure lookups of the copy it read (rule CV2).
CHIRP_VIEW_LOOKUPS = ("state_name", "category_name", "urgency_name", "get_template_text",
                      "get_detail_text", "get_validation_status", "cannot_send_reason")
# What the readers may not name: the live state and its readers (rule CV4).
CHIRP_LIVE_STATE = ("g_state", "g_session", "g_cooldown", "g_recent_chirps", "g_recent_chirp_count",
                    "g_nearby_devices", "g_nearby_count", "g_muted", "g_mute_until_ms", "g_relay_enabled",
                    "g_urgency_filter", "g_session_start_ms", "g_last_chirp_sent_ms", "g_view_scratch",
                    "g_tables_changed", "publish_view") + CHIRP_LIVE_READERS
CHIRP_READER_FUNCS = CHIRP_VIEW_READERS + ("cannot_send_reason",)
# Who may touch the published copies, in chirp_channel.cpp (rules CV3, CV4).
CHIRP_VIEW_CALLS = (
    (r"\bpublish_view\s*\(", ("update", "run_command", "init"), "publish_view()",
     "the loop task publishes: update()'s passes, each command run_command() runs, and init()"),
    (r"\bg_status_view\s*\.\s*publish\s*\(", ("publish_view",), "g_status_view.publish(",
     "publish_view() builds the one view"),
    (r"\bg_nearby_view\s*\.\s*publish\s*\(", ("publish_view",), "g_nearby_view.publish(",
     "publish_view() builds the one view"),
    (r"\bg_recent_view\s*\.\s*publish\s*\(", ("publish_view",), "g_recent_view.publish(",
     "publish_view() builds the one view"),
    (r"\bg_status_view\s*\.\s*read\s*\(", ("read_status",), "g_status_view.read(",
     "read_status() is the status view's one reader"),
    (r"\bg_nearby_view\s*\.\s*read\s*\(", ("read_nearby",), "g_nearby_view.read(",
     "read_nearby() is the nearby view's one reader"),
    (r"\bg_recent_view\s*\.\s*read\s*\(", ("read_recent",), "g_recent_view.read(",
     "read_recent() is the recent view's one reader"),
    (r"\bg_tables_changed\s*=\s*false\b", ("publish_view",), "g_tables_changed = false",
     "only the publish that copied the tables clears it"),
    (r"\bg_tables_changed\s*=\s*true\b", ("on_espnow_recv", "update", "run_command", "init"),
     "g_tables_changed = true", "the tables change in a frame, the prune, a command and init()"),
    (r"\bg_view_scratch\b", ("publish_view", "init"), "g_view_scratch",
     "init() allocates the tables' scratch and publish_view() alone builds in it"),
)
# The recent and nearby tables (rule CV5), and every function that names
# them today. The view shows them only when g_tables_changed says a pass
# changed them, so each change must come through a path that sets it: a chirp
# frame (on_espnow_recv), update()'s prune, an owner command (run_command:
# confirm_chirp, dismiss_chirp, disable, clear_chirps, which rule C2 holds
# there) or init(). A new function that names a table is a new path: add it
# here once it is reached only from one of those.
CHIRP_TABLE_STATE = ("g_recent_chirps", "g_recent_chirp_count", "g_nearby_devices", "g_nearby_count")
CHIRP_TABLE_FUNCS = ("handle_presence", "handle_witness", "handle_ack", "handle_suppress_vote",
                     "relay_chirp", "priority_heap_insert", "nearby_has_pubkey_with_presence",
                     "prune_stale_nearby", "prune_old_chirps", "confirm_chirp", "dismiss_chirp",
                     "disable", "clear_chirps", "init", "publish_view", "get_status",
                     "get_recent_chirps", "get_pending_chirps", "get_nearby_count", "get_nearby_devices")
# Who may call the frame and prune paths: the frame dispatch and update().
CHIRP_TABLE_CALLERS = {
    "on_espnow_recv": ("dispatch_espnow_message",),
    "handle_presence": ("on_espnow_recv",),
    "handle_witness": ("on_espnow_recv",),
    "handle_ack": ("on_espnow_recv",),
    "handle_suppress_vote": ("on_espnow_recv",),
    "relay_chirp": ("handle_ack",),
    "priority_heap_insert": ("handle_witness",),
    "nearby_has_pubkey_with_presence": ("handle_ack",),
    "prune_stale_nearby": ("update",),
    "prune_old_chirps": ("update",),
}
# The keys each GET route has always answered (rule CV6): the dashboard reads
# them, and a view that dropped or renamed one would answer a different shape.
CHIRP_GET_KEYS = {
    "handle_chirp_status": ("state", "session_emoji", "nearby_count", "recent_chirps",
                            "last_chirp_sent_ms", "cooldown_remaining_sec", "cooldown_tier",
                            "presence_met", "night_mode", "relay_enabled", "muted",
                            "mute_remaining_sec", "can_send", "cannot_send_reason"),
    "handle_chirp_nearby": ("count", "devices", "emoji", "age_sec", "rssi", "listening"),
    "handle_chirp_recent": ("chirps", "emoji", "template_id", "template_text", "detail", "category",
                            "urgency", "hop_count", "age_sec", "confirm_count", "validated", "status",
                            "relayed", "suppressed", "nonce"),
}
SIG_CHIRP_INIT = r"\bbool\s+init\s*\(\s*\)"
SIG_CHIRP_RUN = r"\bstatic\s+Result\s+run_command\s*\([^)]*\)"
SIG_CHIRP_RECV = r"\bstatic\s+void\s+on_espnow_recv\s*\([^)]*\)"
SIG_CHIRP_PUBLISH = r"\bstatic\s+void\s+publish_view\s*\(\s*\)"
CHIRP_READER_SIGS = {
    "read_status": r"\bvoid\s+read_status\s*\([^)]*\)",
    "read_nearby": r"\bvoid\s+read_nearby\s*\([^)]*\)",
    "read_recent": r"\bvoid\s+read_recent\s*\([^)]*\)",
    "cannot_send_reason": r"\bconst\s+char\s*\*\s*cannot_send_reason\s*\([^)]*\)",
}
CHIRP_READER_VIEW = {"read_status": "g_status_view.read(", "read_nearby": "g_nearby_view.read(",
                     "read_recent": "g_recent_view.read("}


@functools.lru_cache(maxsize=1024)
def chirp_live_read_findings(name: str, code: str) -> tuple[str, ...]:
    """Rule CV1 for one blanked file."""
    if "chirp_channel::" not in code:
        return ()
    out = []
    for hname, s, e in handler_spans(code):
        m = re.search(CHIRP_LIVE_READER_RE, code[s:e])
        if m:
            out.append(f"{name}: HTTP handler {hname}() reads chirp_channel::{m.group(1)}( — the live "
                       "Chirp state is update()'s, written on the loop task; read the view "
                       "(chirp_channel::read_status / read_nearby / read_recent) (F138)")
    return tuple(out)


def check_chirp_status_reads(ino: str, others: dict[str, str], errors: list[str]) -> None:
    """Rules CV1-CV6: the Chirp GET routes read only what the loop task published."""
    files = dict(others)
    files[INO] = ino
    for name, src in files.items():
        errors.extend(chirp_live_read_findings(name, blank_comments_and_strings(src)))
    if CHIRP_API not in files or CHIRP_CPP not in files:
        errors.append(f"{SKETCH}: the Chirp sources ({CHIRP_API}, {CHIRP_CPP}) are missing")
        return
    # CV2, CV6: the three GET handlers.
    api_code = blank_comments_and_strings(files[CHIRP_API])
    api_kept = blank_comments_only(files[CHIRP_API])
    for h, reader in CHIRP_STATUS_HANDLERS:
        span = the_body(api_code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                        f"{CHIRP_API}: {h}()", errors)
        if span is None:
            continue
        body = api_code[span[0]:span[1]]
        calls = re.findall(r"\bchirp_channel::(\w+)\s*\(", body)
        if calls.count(reader) != 1:
            errors.append(f"{CHIRP_API}: {h}() must read the published view with "
                          f"chirp_channel::{reader}( once (found {calls.count(reader)}) (F138)")
        for fn in sorted(set(calls) - {reader} - set(CHIRP_VIEW_LOOKUPS)):
            errors.append(f"{CHIRP_API}: {h}() calls chirp_channel::{fn}( — a Chirp GET route answers "
                          f"from the copy {reader}() returned, through pure lookups only "
                          f"({', '.join(CHIRP_VIEW_LOOKUPS)}) (F138)")
        keys = re.findall(r'\[\s*"(\w+)"\s*\]', api_kept[span[0]:span[1]])
        want = set(CHIRP_GET_KEYS[h])
        if set(keys) != want:
            missing, extra = sorted(want - set(keys)), sorted(set(keys) - want)
            errors.append(f"{CHIRP_API}: {h}() answers a different shape (missing {missing}, new "
                          f"{extra}) — the routes answer what they always did; the dashboard parses "
                          "them (F138)")
    # CV3, CV4, CV5: chirp_channel.cpp.
    code = blank_comments_and_strings(files[CHIRP_CPP])
    spans = named_bodies(code)
    for pattern, allowed, label, why in CHIRP_VIEW_CALLS:
        for m in re.finditer(pattern, code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue                      # a declaration or the definition's own header
            if where not in allowed:
                errors.append(f"{CHIRP_CPP}: {where}() names {label} — {why} (F138)")
    update = body_of(code, SIG_UPDATE, f"{CHIRP_CPP}: update()", errors)
    if update is not None:
        s = squash(update)
        rets = [m.start() for m in re.finditer(r"\breturn\b", s)]
        if not s.endswith("publish_view();") or any(not s[:r].endswith("publish_view();") for r in rets):
            errors.append(f"{CHIRP_CPP}: update() must call publish_view() right before every return "
                          "and as its last statement — the status route shows the pass it ends, a "
                          "disabled channel's included (F138)")
        if "prune_old_chirps();g_tables_changed=true;" not in s:
            errors.append(f"{CHIRP_CPP}: update() must set `g_tables_changed = true;` right after its "
                          "prune — the pass that drops a stale neighbor or an old chirp publishes the "
                          "shorter tables (F138)")
    run = body_of(code, SIG_CHIRP_RUN, f"{CHIRP_CPP}: run_command()", errors)
    if run is not None:
        rets = [m.start() for m in re.finditer(r"\breturn\b", run)]
        if len(rets) != 1 or not squash(run[:rets[0]]).endswith("g_tables_changed=true;publish_view();"):
            errors.append(f"{CHIRP_CPP}: run_command() must return once, right after `g_tables_changed = "
                          "true; publish_view();` — the drain posts the result when it returns and the "
                          "handler answers at once, so a read right after the POST (the dashboard's "
                          "status, its recent list after a dismiss) must already show it (F138)")
    init = body_of(code, SIG_CHIRP_INIT, f"{CHIRP_CPP}: init()", errors)
    if init is not None and not squash(init).endswith("g_tables_changed=true;publish_view();returntrue;"):
        errors.append(f"{CHIRP_CPP}: init() must end `g_tables_changed = true; publish_view(); return "
                      "true;` — the HTTP server can answer before loop() runs a pass, and the first "
                      "view carries the settings init() loaded (F138)")
    recv = body_of(code, SIG_CHIRP_RECV, f"{CHIRP_CPP}: on_espnow_recv()", errors)
    if recv is not None:
        s = squash(recv)
        at = s.find("g_tables_changed=true;switch(")
        if s.count("g_tables_changed=true;") != 1 or at < 0 or "return" in s[at:s.find("{", at)]:
            errors.append(f"{CHIRP_CPP}: on_espnow_recv() must set `g_tables_changed = true;` once, right "
                          "before its dispatch switch — a chirp frame changes the tables, and the pass "
                          "that handled it publishes them (F138)")
    for m in re.finditer(r"\b(" + "|".join(CHIRP_TABLE_STATE) + r")\b", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where not in CHIRP_TABLE_FUNCS:
            errors.append(f"{CHIRP_CPP}: {where}() names {m.group(1)} — the view republishes the tables "
                          "only on a frame, the prune, a command or init(); a new path to them must be "
                          "reached only from one of those (CHIRP_TABLE_FUNCS) (F138)")
    for fn, allowed in CHIRP_TABLE_CALLERS.items():
        for m in re.finditer(r"(?<![\w:.>])" + fn + r"\s*\(", code):
            where = enclosing_function(spans, m.start())
            if where is not None and where not in allowed:
                errors.append(f"{CHIRP_CPP}: {where}() calls {fn}() — it changes the tables, and only "
                              f"{', '.join(allowed)} marks them for the view (F138)")
    for fn, sig in CHIRP_READER_SIGS.items():
        body = body_of(code, sig, f"{CHIRP_CPP}: {fn}()", errors)
        if body is None:
            continue
        if fn in CHIRP_READER_VIEW and CHIRP_READER_VIEW[fn] not in squash(body):
            errors.append(f"{CHIRP_CPP}: {fn}() must copy the published view ({CHIRP_READER_VIEW[fn]}) "
                          "(F138)")
        hit = re.search(r"\b(" + "|".join(CHIRP_LIVE_STATE) + r")\b", body)
        if hit:
            errors.append(f"{CHIRP_CPP}: {fn}() names {hit.group(1)} — it runs on the httpd task and "
                          "reads only what the loop task published (F138)")


# ── MQTT network timeout (F112) ──────────────────────────────────────────

def int_constant(code: str, name: str) -> int | None:
    m = re.search(r"\b" + name + r"\s*=\s*(\d+)\s*[uUlL]*\s*;", code)
    return None if m is None else int(m.group(1))


def check_mqtt_timeout(ino: str, mqtt_h: str | None, mqtt: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mqtt)
    opened = body_of(code, SIG_OPEN, f"{MQTT_CPP}: open_client()", errors)
    if opened is not None:
        s = squash(opened)
        at_set = s.find("cfg.network.timeout_ms=(int)kNetworkTimeoutMs;")
        at_init = s.find("esp_mqtt_client_init(&cfg)")
        if at_set < 0 or at_init < 0 or at_set > at_init:
            errors.append(f"{MQTT_CPP}: open_client() must set `cfg.network.timeout_ms = "
                          "(int)kNetworkTimeoutMs;` before esp_mqtt_client_init(&cfg) — esp_mqtt's "
                          "10 s default holds a loop-task publish past the 8 s watchdog (F112)")
    if mqtt_h is None:
        errors.append(f"{MQTT_H}: missing")
        return
    hcode = blank_comments_and_strings(mqtt_h)
    icode = blank_comments_and_strings(ino)
    timeout = int_constant(hcode, "kNetworkTimeoutMs")
    budget = int_constant(hcode, "kNetworkOpsBudget")
    watchdog_s = int_constant(icode, "WATCHDOG_TIMEOUT_SEC")
    if timeout is None or budget is None or watchdog_s is None:
        errors.append(f"{MQTT_H}: kNetworkTimeoutMs and kNetworkOpsBudget (and {INO}'s "
                      "WATCHDOG_TIMEOUT_SEC) must be integer constants this check can read (F112)")
    elif timeout <= 0 or budget < 3 or budget * timeout >= watchdog_s * 1000:
        errors.append(f"{MQTT_H}: kNetworkOpsBudget ({budget}, at least 3: an esp_mqtt operation "
                      f"a publish waits behind, its own write, the rest of the pass) x "
                      f"kNetworkTimeoutMs ({timeout} ms) must sit under the loop task's "
                      f"{watchdog_s} s watchdog (F112)")
    assert_ok = re.search(r"static_assert\(csi_mqtt::kNetworkTimeoutMs>0&&csi_mqtt::kNetworkOpsBudget"
                          r"\*csi_mqtt::kNetworkTimeoutMs<WATCHDOG_TIMEOUT_SEC\*1000u,", squash(icode))
    if assert_ok is None:
        errors.append(f"{INO}: must static_assert(csi_mqtt::kNetworkTimeoutMs > 0 && "
                      "csi_mqtt::kNetworkOpsBudget * csi_mqtt::kNetworkTimeoutMs < "
                      "WATCHDOG_TIMEOUT_SEC * 1000u, ...) — the device build's own pin (F112)")


# ── Bluetooth settings, events and views (F144, F143, F138) ──────────────

SIG_BT_SETTINGS_SET = r"\besp_err_t\s+handle_bluetooth_settings_set\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)"
SIG_BT_SET_SETTINGS = r"\bstatic\s+bool\s+set_settings\s*\([^)]*\)"
# BV1: what the settings POST does before and after its submit when it turns
# Bluetooth on (squashed text).
BV1_BRING_UP = "if((cmd.set_mask&bluetooth_channel::BT_SET_ENABLED)&&settings.enabled&&!bring_up()){return"
BV1_REFUSED = "if(r.refusal==bluetooth_channel::BT_REFUSED_NOT_ENABLED){returnsend_bt_error(req,"


def check_bluetooth_settings_enable(api_src: str, cpp_src: str, errors: list[str]) -> None:
    """Rule BV1 (F144)."""
    code = blank_comments_and_strings(api_src)
    body = body_of(code, SIG_BT_SETTINGS_SET, f"{BT_API}: handle_bluetooth_settings_set()", errors)
    if body is not None:
        s = squash(body)
        at = s.find("bluetooth_channel::submit(")
        guard = "if(w!=loop_command_ring::Wait::kDone)returnsend_not_run(req,w);"
        after_guard = s.find(guard, at) + len(guard) if at >= 0 and s.find(guard, at) >= 0 else -1
        if at < 0 or s.count(BV1_BRING_UP) != 1 or s.find(BV1_BRING_UP) > at:
            errors.append(f"{BT_API}: handle_bluetooth_settings_set() must bring the stack up before it "
                          f"submits when the POST turns Bluetooth on (`{BV1_BRING_UP} ...`): a command "
                          "never runs init(), so the enable would be refused on a device whose stack "
                          "is down (F144)")
        if after_guard < 0 or not s[after_guard:].startswith(BV1_REFUSED):
            errors.append(f"{BT_API}: handle_bluetooth_settings_set() must answer a refused enable with "
                          f"`{BV1_REFUSED} ...` right after its not-run guard — nothing was applied, "
                          "so \"Settings updated\" would be false (F144)")
    cpp = blank_comments_and_strings(cpp_src)
    setter = body_of(cpp, SIG_BT_SET_SETTINGS, f"{BT_CPP}: set_settings()", errors)
    if setter is not None:
        s = squash(setter)
        read_at = s.find("constboolwas_enabled=g_settings.enabled;")
        assign_at = s.find("g_settings=settings;")
        if read_at < 0 or assign_at < 0 or read_at > assign_at or \
                re.search(r"(?<![\w:.>])is_enabled\s*\(", setter):
            errors.append(f"{BT_CPP}: set_settings() must read `const bool was_enabled = "
                          "g_settings.enabled;` before `g_settings = settings;` and decide the on/off "
                          "change from it, never from is_enabled() (which reads the value just "
                          "assigned: both branches were dead) (F144)")


# BV2 (F143): the NimBLE host task's callbacks, the helpers they build and
# post their events with, and what the loop task's side of the queue is.
BT_CALLBACKS = ("onConnect", "onDisconnect", "onAuthenticationComplete", "onPassKeyDisplay",
                "onConfirmPassKey", "onWrite", "onRead", "onResult", "onScanEnd")
BT_EVENT_HELPERS = ("make_event", "link_event", "post_event")
BT_CALLBACK_CALLS = BT_EVENT_HELPERS + ("detect_device_type",)
BT_EVENT_APPLIERS = ("apply_connect", "apply_disconnect", "apply_auth_complete",
                     "apply_passkey_display", "apply_confirm_passkey", "apply_scan_result",
                     "apply_scan_end", "apply_activity")
# The channel's state: the loop task's (update(), its commands and the
# events it applies). A callback on the NimBLE host task names none of it.
BT_LIVE_STATE = ("g_state", "g_settings", "g_initialized", "g_connection", "g_pairing",
                 "g_pending_pair_info", "g_pending_pair_active", "g_connection_handle",
                 "g_connection_mtu", "g_paired_devices", "g_paired_count", "g_scanned_devices",
                 "g_scanned_count", "g_scanning", "g_scan_start_ms", "g_scan_duration_ms",
                 "g_total_connections", "g_total_bytes_sent", "g_total_bytes_received",
                 "g_advertising_start_ms", "g_advertising_total_ms", "g_connected_total_ms",
                 "g_advertising", "g_scanner", "g_server", "g_status_char", "g_commands",
                 "g_conn_callback", "g_pair_callback", "g_scan_callback", "g_link_drops",
                 "g_lossy_drops")
BT_LIVE_STATE_RE = r"\b(" + "|".join(BT_LIVE_STATE) + r")\b"
CALL_RE = r"(?<![\w:.>])([A-Za-z_]\w*)\s*\("
# The only file globals a callback or its event helpers may name: the queue
# (post_event's) and the data hook (onWrite's, which runs on the NimBLE host
# task with the written bytes). Any other g_ name is the loop task's or the
# bring-up's (g_init_fail_reason, g_local_address, the views, ...).
BT_CALLBACK_GLOBALS = {"g_events": ("post_event",), "g_data_callback": ("onWrite",)}
# What a callback hands the loop task, by kind: GATT activity and scan
# results are posted under EVENT_LOSSY_LIMIT (a burst of them never takes
# the room a link's events are kept), a link's own at the full limit.
BT_LOSSY_CALLBACKS = ("onWrite", "onRead", "onResult")
BT_LINK_CALLBACKS = ("onConnect", "onDisconnect", "onAuthenticationComplete", "onPassKeyDisplay",
                     "onConfirmPassKey", "onScanEnd")


def check_bluetooth_callbacks(cpp_src: str, errors: list[str]) -> None:
    """Rule BV2 (F143)."""
    code = blank_comments_and_strings(cpp_src)
    spans = named_bodies(code)
    defined = {name for name, _s, _e in spans}
    for name, s, e in spans:
        if name not in BT_CALLBACKS + BT_EVENT_HELPERS:
            continue
        body = code[s:e]
        hit = re.search(BT_LIVE_STATE_RE, body)
        if hit:
            errors.append(f"{BT_CPP}: {name}() names {hit.group(1)} — it runs on the NimBLE host task "
                          "(or builds a callback's event there); the channel's state is the loop "
                          "task's: post an event for update() to apply (F143)")
        for m in re.finditer(CALL_RE, body):
            callee = m.group(1)
            if callee in defined and callee not in BT_CALLBACK_CALLS and callee != name:
                errors.append(f"{BT_CPP}: {name}() calls {callee}() — a NimBLE callback only "
                              f"builds and posts its event ({', '.join(BT_CALLBACK_CALLS)}); "
                              "update() applies it on the loop task (F143)")
        for m in re.finditer(r"\b(g_\w+)\b", body):
            if name not in BT_CALLBACK_GLOBALS.get(m.group(1), ()) and not hit:
                errors.append(f"{BT_CPP}: {name}() names {m.group(1)} — on the NimBLE host task a "
                              "callback names only the event queue (post_event) and the data hook "
                              "(onWrite); the rest is the loop task's or the bring-up's (F143)")
                break
        other = re.search(r"\bbluetooth_channel::|(?<![\w:.>])log_health\s*\(|\bble_\w+::", body)
        if other:
            errors.append(f"{BT_CPP}: {name}() names {other.group(0).rstrip('(').strip()} — the "
                          "channel's functions, the health log and the BLE modules are the loop "
                          "task's; a NimBLE callback only posts its event (F143)")
        if name in BT_LOSSY_CALLBACKS + BT_LINK_CALLBACKS:
            for post in re.finditer(r"(?<![\w:.>])post_event\s*\(", body):
                close = matching_paren(body, post.end() - 1)
                args = squash(body[post.end():close]) if close > 0 else ""
                lossy = args.endswith(",EVENT_LOSSY_LIMIT")
                if name in BT_LOSSY_CALLBACKS and not lossy:
                    errors.append(f"{BT_CPP}: {name}() posts without EVENT_LOSSY_LIMIT — a burst of "
                                  "GATT activity or scan results would take the room kept for a "
                                  "link's own events (F143)")
                if name in BT_LINK_CALLBACKS and "EVENT_LOSSY_LIMIT" in args:
                    errors.append(f"{BT_CPP}: {name}() posts under EVENT_LOSSY_LIMIT — a link's own "
                                  "events are posted at the full limit, into the room kept for them "
                                  "(F143)")
    for pattern, allowed, label in (
            (r"\bg_events\s*\.\s*post\s*\(", ("post_event",), "g_events.post("),
            (r"(?<![\w:.>])post_event\s*\(", BT_CALLBACKS, "post_event("),
            (r"\bg_events\s*\.\s*consume\s*\(", ("update",), "g_events.consume("),
            (r"(?<![\w:.>])apply_event\s*\(", (), "apply_event(")) + tuple(
            (r"(?<![\w:.>])" + a + r"\s*\(", ("apply_event",), a + "(") for a in BT_EVENT_APPLIERS):
        for m in re.finditer(pattern, code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue                      # a declaration or the definition's own header
            if where not in allowed:
                errors.append(f"{BT_CPP}: {where}() names {label} — only "
                              f"{', '.join(allowed) or 'g_events.consume(apply_event)'} may: the "
                              "NimBLE host task posts events, the loop task's update() applies "
                              "them (F143)")
    update = body_of(code, SIG_UPDATE, f"{BT_CPP}: update()", errors)
    if update is not None:
        sq = squash(update)
        consume = "g_events.consume(apply_event);"
        drain = "g_commands.drain(run_command);"
        ret = re.search(r"\breturn\b", update)
        at = update.find("g_events.consume(")
        if sq.count(consume) != 1 or at < 0 or (ret is not None and ret.start() < at) or \
                sq.find(consume) > sq.find(drain):
            errors.append(f"{BT_CPP}: update() must apply the NimBLE host task's events with "
                          f"`{consume}` once, before its first return and before `{drain}` — a "
                          "link that ends while Bluetooth is off still ends, and a command acts on "
                          "the radio's latest state (F143)")


# BV3 (F138): the status routes' reads.
BT_LIVE_READERS = ("get_status", "get_state", "get_settings", "get_scanned_devices",
                   "get_paired_devices", "is_scanning", "is_connected", "get_connection_info",
                   "get_pairing_state", "get_pairing_pin")
BT_GET_ROUTES = (("handle_bluetooth_status", "read_status"),
                 ("handle_bluetooth_scan_results", "read_scan"),
                 ("handle_bluetooth_paired_list", "read_paired"),
                 ("handle_bluetooth_settings_get", "read_settings"))
BT_GET_PURE = ("state_name", "format_address", "estimate_distance_m", "distance_label",
               "device_type_name", "security_level_name", "pairing_state_name", "init_fail_reason")
# Who may touch each published view, in bluetooth_channel.cpp.
BT_VIEW_CALLS = (
    (r"(?<![\w:.>])publish_views\s*\(", ("update", "run_command"), "publish_views(",
     "the loop task publishes: update()'s passes and each owner command"),
    (r"(?<![\w:.>])publish_status_view\s*\(", ("publish_views",), "publish_status_view(", "publish_views() publishes"),
    (r"(?<![\w:.>])publish_scan_view\s*\(", ("publish_views",), "publish_scan_view(", "publish_views() publishes"),
    (r"(?<![\w:.>])publish_paired_view\s*\(", ("publish_views",), "publish_paired_view(", "publish_views() publishes"),
    (r"\bg_status_view\s*\.\s*publish\s*\(", ("publish_status_view",), "g_status_view.publish(", "one builder"),
    (r"\bg_scan_view\s*\.\s*publish\s*\(", ("publish_scan_view",), "g_scan_view.publish(", "one builder"),
    (r"\bg_paired_view\s*\.\s*publish\s*\(", ("publish_paired_view",), "g_paired_view.publish(", "one builder"),
    (r"\bg_status_view\s*\.\s*read\s*\(", ("read_status", "read_settings"), "g_status_view.read(",
     "the readers copy the view"),
    (r"\bg_scan_view\s*\.\s*read\s*\(", ("read_scan",), "g_scan_view.read(", "the reader copies the view"),
    (r"\bg_paired_view\s*\.\s*read\s*\(", ("read_paired",), "g_paired_view.read(", "the reader copies the view"),
)
SIG_BT_RUN_COMMAND = r"\bstatic\s+Result\s+run_command\s*\([^)]*\)"
BT_READERS = ("read_status", "read_settings", "read_scan", "read_paired")
# The JSON each GET route sends: `<object>.<key>` for every literal key it
# sets (doc is the top level; conn, pair, stats and dev are its objects).
BT_GET_KEYS = {
    "handle_bluetooth_status": {
        "doc": ("state", "enabled", "advertising", "init_fail_reason", "scanning", "connected",
                "device_name", "local_address", "tx_power", "mtu", "battery_pct", "paired_count",
                "scanned_count", "connection", "pairing", "stats"),
        "conn": ("address", "name", "rssi", "distance_m", "distance_label", "security",
                 "connected_sec", "bytes_sent", "bytes_received"),
        "pair": ("state", "pin", "peer_address", "peer_name"),
        "stats": ("total_connections", "total_bytes_sent", "total_bytes_received",
                  "advertising_time_sec", "connected_time_sec"),
    },
    "handle_bluetooth_scan_results": {
        "doc": ("scanning", "count", "devices"),
        "dev": ("address", "name", "rssi", "distance_m", "distance_label", "type", "connectable",
                "is_securacv", "age_sec"),
    },
    "handle_bluetooth_paired_list": {
        "doc": ("count", "devices"),
        "dev": ("address", "name", "paired_timestamp", "last_connected_sec", "connection_count",
                "security", "trusted", "blocked"),
    },
    "handle_bluetooth_settings_get": {
        "doc": ("enabled", "auto_advertise", "allow_pairing", "require_pin", "device_name",
                "tx_power", "inactivity_timeout_sec", "notify_on_connect", "long_range_mode"),
    },
}
WEB_UI = f"{SKETCH}/web_ui.h"
# What the dashboard's Bluetooth panel reads from each route: (its function,
# the variable holding the answer, the route's object the keys belong to).
BT_JS_READS = (
    ("refreshBtStatus", r"data", ("handle_bluetooth_status", "doc")),
    ("refreshBtStatus", r"data\.connection", ("handle_bluetooth_status", "conn")),
    ("refreshBtStatus", r"data\.pairing", ("handle_bluetooth_status", "pair")),
    ("toggleBtAdvertising", r"st", ("handle_bluetooth_status", "doc")),
    ("loadBtSettings", r"data", ("handle_bluetooth_settings_get", "doc")),
    ("btStartScan", r"r", ("handle_bluetooth_scan_results", "doc")),
    ("renderBtScanList", r"d", ("handle_bluetooth_scan_results", "dev")),
    ("deviceIcon", r"d", ("handle_bluetooth_scan_results", "dev")),
    ("loadBtPairedDevices", r"data", ("handle_bluetooth_paired_list", "doc")),
    ("loadBtPairedDevices", r"d", ("handle_bluetooth_paired_list", "dev")),
)


def js_function_body(src: str, name: str) -> str | None:
    m = re.search(r"\bfunction\s+" + name + r"\s*\([^)]*\)\s*\{", src)
    if m is None:
        return None
    end = close_brace(src, m.end() - 1)
    return None if end < 0 else src[m.end():end]


def check_bluetooth_reads(files: dict[str, str], errors: list[str]) -> None:
    """Rule BV3 (F138)."""
    hcode = namespace_block(blank_comments_and_strings(files[BT_H]), "bluetooth_channel") \
        if BT_H in files else ""
    for fn in BT_LIVE_READERS:
        if re.search(r"(?<![\w:.>])" + fn + r"\s*\(", hcode):
            errors.append(f"{BT_H}: declares {fn}() — the channel's state is the loop task's; another "
                          "task reads the published view (read_status, read_settings, read_scan, "
                          "read_paired) (F138)")
    live_call = r"\bbluetooth_channel::(" + "|".join(BT_LIVE_READERS) + r")\s*\("
    for name, src in files.items():
        c = blank_comments_and_strings(src)
        if "bluetooth_channel::" not in c:
            continue
        for hname, s, e in handler_spans(c):
            m = re.search(live_call, c[s:e])
            if m:
                errors.append(f"{name}: HTTP handler {hname}() reads bluetooth_channel::{m.group(1)}( — "
                              "read the view the loop task published (F138)")
    api = blank_comments_and_strings(files[BT_API])
    api_kept = blank_comments_only(files[BT_API])
    for h, reader in BT_GET_ROUTES:
        span = the_body(api, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                        f"{BT_API}: {h}()", errors)
        if span is None:
            continue
        body = api[span[0]:span[1]]
        calls = re.findall(r"\bbluetooth_channel::(\w+)\s*\(", body)
        if calls.count(reader) != 1 or any(c not in BT_GET_PURE + (reader,) for c in calls):
            errors.append(f"{BT_API}: {h}() must read bluetooth_channel::{reader}( once and call "
                          f"nothing else on the channel but {', '.join(BT_GET_PURE)} (found "
                          f"{', '.join(calls) or 'none'}) — the route answers from the view the "
                          "loop task published, whole (F138)")
        kept = api_kept[span[0]:span[1]]
        sent = {(obj, key) for obj, key in re.findall(r"\b(\w+)\s*\[\s*\"(\w+)\"\s*\]", kept)
                if obj != "input"}
        want = {(obj, key) for obj, keys in BT_GET_KEYS[h].items() for key in keys}
        if sent != want:
            errors.append(f"{BT_API}: {h}()'s JSON keys changed (missing {sorted(want - sent)}, new "
                          f"{sorted(sent - want)}) — the dashboard and the apps read this shape; F138 "
                          "moved where the values come from, not what is sent")
    code = blank_comments_and_strings(files[BT_CPP])
    spans = named_bodies(code)
    for pattern, allowed, label, why in BT_VIEW_CALLS:
        for m in re.finditer(pattern, code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue
            if where not in allowed:
                errors.append(f"{BT_CPP}: {where}() names {label} — {why}; only {', '.join(allowed)} "
                              "may (F138)")
    update = body_of(code, SIG_UPDATE, f"{BT_CPP}: update()", errors)
    if update is not None:
        sq = squash(update)
        rets = [m.start() for m in re.finditer(r"\breturn\b", sq)]
        if not sq.endswith("publish_views();") or any(not sq[:r].endswith("publish_views();") for r in rets):
            errors.append(f"{BT_CPP}: update() must call publish_views() right before every return and "
                          "as its last statement — the status routes show the pass it ends, a disabled "
                          "channel's included (F138)")
    run = body_of(code, SIG_BT_RUN_COMMAND, f"{BT_CPP}: run_command()", errors)
    if run is not None:
        rets = [m.start() for m in re.finditer(r"\breturn\b", run)]
        if len(rets) != 1 or not squash(run[:rets[0]]).endswith("publish_views();"):
            errors.append(f"{BT_CPP}: run_command() must return once, right after publish_views() — the "
                          "drain posts the result when it returns and the handler answers at once, so "
                          "a GET right after the POST must already show the command (F138)")
    for name, s0, e0 in spans:
        if name in BT_READERS:
            hit = re.search(BT_LIVE_STATE_RE, code[s0:e0])
            if hit:
                errors.append(f"{BT_CPP}: {name}() names {hit.group(1)} — it runs on the httpd task and "
                              "reads only what the loop task published (F138)")
    for r in BT_READERS:
        if not any(n == r for n, _s, _e in spans):
            errors.append(f"{BT_CPP}: {r}() is not defined (F138)")
    ui = files.get(WEB_UI)
    if ui is None:
        errors.append(f"{WEB_UI}: missing")
        return
    for fn, var, (route, obj) in BT_JS_READS:
        body = js_function_body(ui, fn)
        if body is None:
            errors.append(f"{WEB_UI}: the dashboard's {fn}() is not where this check reads it (F138)")
            continue
        keys = set(BT_GET_KEYS[route][obj])
        for m in re.finditer(r"(?<![\w.])" + var + r"\.(\w+)", body):
            key = m.group(1)
            nested = [k for k in ("connection", "pairing") if var == "data" and key == k]
            if key not in keys and not nested and not (var in ("data", "st", "r") and key in ("length",)):
                errors.append(f"{WEB_UI}: {fn}() reads {var}.{key}, which {route}() does not send in "
                              f"its {obj} object (F138)")


def check_bluetooth_views(files: dict[str, str], errors: list[str]) -> None:
    """Rules BV1..BV3: the Bluetooth channel's settings enable (F144), the
    NimBLE host task's events (F143) and the status routes' reads (F138)."""
    if BT_API not in files or BT_CPP not in files:
        errors.append(f"{SKETCH}: the Bluetooth channel's sources ({BT_API}, {BT_CPP}) are missing")
        return
    check_bluetooth_settings_enable(files[BT_API], files[BT_CPP], errors)
    check_bluetooth_callbacks(files[BT_CPP], errors)
    check_bluetooth_reads(files, errors)


def check(ino: str, mesh_h: str, mesh_cpp: str, mqtt: str, others: dict[str, str]) -> list[str]:
    errors: list[str] = []
    files = dict(others)
    files[INO] = ino
    files[MESH_H] = mesh_h
    files[MESH_CPP] = mesh_cpp
    files[MQTT_CPP] = mqtt
    rest = {k: v for k, v in files.items() if k != INO}
    check_mesh_internal(mesh_h, mesh_cpp, errors)
    check_mesh_callers(mesh_cpp, errors)
    check_mesh_sketch(ino, rest, errors)
    check_mesh_status_reads(ino, rest, mesh_cpp, errors)
    check_mqtt_reinit(mqtt, errors)
    check_httpd_paths(files, errors)
    check_mqtt_sketch(ino, rest, errors)
    check_channels(ino, rest, errors)
    check_chirp_status_reads(ino, rest, errors)
    check_mqtt_timeout(ino, files.get(MQTT_H), mqtt, errors)
    check_bluetooth_views(files, errors)
    return errors


# ── Self-test: mutations the check must refuse ───────────────────────────

Srcs = dict  # {"ino", "mesh_h", "mesh_cpp", "mqtt"}
Mutation = Callable[[dict], dict]


def on(key: str, sig: str, pattern: str, repl: str, need: str | None = None) -> Mutation:
    def mutate(s: dict) -> dict:
        out = dict(s)
        out[key] = mutate_in(s[key], sig, pattern, repl, need=need)
        return out
    return mutate


def raw(key: str, old: str, new: str) -> Mutation:
    def mutate(s: dict) -> dict:
        if s[key].count(old) != 1:
            raise AnchorMissing(old)
        out = dict(s)
        out[key] = s[key].replace(old, new, 1)
        return out
    return mutate


def on_other(path: str, sig: str, pattern: str, repl: str, need: str | None = None) -> Mutation:
    """A mutation of one of the sketch's other files (`others`, by path)."""
    def mutate(s: dict) -> dict:
        others = dict(s["others"])
        others[path] = mutate_in(others[path], sig, pattern, repl, need=need)
        out = dict(s)
        out["others"] = others
        return out
    return mutate


def raw_other(path: str, old: str, new: str) -> Mutation:
    def mutate(s: dict) -> dict:
        others = dict(s["others"])
        if others[path].count(old) != 1:
            raise AnchorMissing(old)
        others[path] = others[path].replace(old, new, 1)
        out = dict(s)
        out["others"] = others
        return out
    return mutate


def api_handler(h: str) -> str:
    return r"\binline\s+esp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)"


def ino_handler(h: str) -> str:
    return r"\bstatic\s+esp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)"


SUBMIT_IF = (r"const\s+loop_command_ring::Wait\s+w\s*=\s*mesh_network::submit\(cmd,\s*&ok\);\s*"
             r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)\s*\{[^}]*\}")

MUTATIONS: list[tuple[str, Mutation]] = [
    # Rule 5: a handler changes the mesh itself.
    ("the remove handler calls remove_peer itself",
     on("ino", ino_handler("handle_mesh_remove"), SUBMIT_IF,
        "ok = mesh_network::remove_peer(cmd.fingerprint);")),
    ("the alerts DELETE clears the history on the httpd task too",
     on("ino", ino_handler("handle_mesh_alerts_clear"), r"(bool\s+ok\s*=\s*false;)",
        r"\1 mesh_network::clear_alerts();")),
    ("the leave handler answers 200 to a command that did not run",
     on("ino", ino_handler("handle_mesh_leave"),
        r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)\s*\{[^}]*\}", "(void)w;")),
    ("the loop task submits a command (it would wait for itself)",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)",
        r"\1 (void)mesh_network::submit(mesh_network::make_command(mesh_network::MESH_CMD_CLEAR_ALERTS), nullptr);")),
    ("the sketch hides the mesh's callers behind a using-directive",
     raw("ino", '#include "mesh_network.h"', '#include "mesh_network.h"\nusing namespace mesh_network;')),
    ("http_send_error() goes back to the pre-F96 status chain (409 and 503 sent as 400)",
     on("ino", SIG_SEND_ERROR, r"http_status_line\(status_code\)",
        'status_code == 400 ? "400 Bad Request" : status_code == 404 ? "404 Not Found" : '
        'status_code == 500 ? "500 Internal Server Error" : "400 Bad Request"')),
    ("the pre-reboot hook saves the replay counters in place (on the httpd task)",
     raw("ino", "(void)mesh_network::save_replay_counters_before_reboot();",
         "(void)mesh_network::save_replay_counters();")),
    ("save_replay_counters_before_reboot() saves in place on any task",
     on("mesh_cpp", SIG_REBOOT_SAVE, r"bool\s+ok\s*=\s*false;\s*\(void\)submit\([^;]*;\s*return\s+ok;",
        "return save_replay_counters();")),
    ("the sketch calls mesh_network::update twice",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)", r"\1 mesh_network::update();")),
    # Rule 1: the owner commands are internal.
    ("mesh_network.h declares remove_peer again",
     raw("mesh_h", "MESH_CMD_SET_ENABLED.)\nbool is_enabled();",
         "MESH_CMD_SET_ENABLED.)\nbool is_enabled();\nbool remove_peer(const uint8_t* fingerprint);")),
    ("remove_peer is defined without static",
     raw("mesh_cpp", "static bool remove_peer(const uint8_t* fingerprint) {\n",
         "bool remove_peer(const uint8_t* fingerprint) {\n")),
    # Rule 2: only run_command (and update's loop-task paths, for cancel) call them.
    ("update() clears the alert history itself",
     on("mesh_cpp", SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"\1 clear_alerts();")),
    ("send_heartbeat cancels the pairing",
     on("mesh_cpp", r"\bvoid\s+send_heartbeat\s*\(\s*\)", r"(if\s*\(!g_opera_config\.configured\)\s*return;)",
        r"\1 cancel_pairing();")),
    ("update() runs a command directly",
     on("mesh_cpp", SIG_UPDATE, r"(g_commands\.drain\(run_command\);)",
        r"\1 (void)run_command(make_command(MESH_CMD_LEAVE));")),
    # Rule 3: update drains first.
    ("update() drains after its disabled return",
     lambda s: on("mesh_cpp", SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")(
         on("mesh_cpp", SIG_UPDATE, r"(mesh_channel_policy::poll_radio\(\);)",
            r"g_commands.drain(run_command); \1")(s))),
    ("update() never drains",
     on("mesh_cpp", SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")),
    ("send_heartbeat drains the ring too",
     on("mesh_cpp", r"\bvoid\s+send_heartbeat\s*\(\s*\)", r"(if\s*\(!g_opera_config\.configured\)\s*return;)",
        r"\1 g_commands.drain(run_command);")),
    # Rule 4: submit only posts; the Wi-Fi task's callbacks touch none of it.
    ("submit() runs the command in place",
     on("mesh_cpp", SIG_SUBMIT, r"(bool\s+result\s*=\s*false;)",
        r"\1 if (ok != nullptr) *ok = run_command(cmd); return loop_command_ring::Wait::kDone;")),
    ("submit() drains the ring itself",
     on("mesh_cpp", SIG_SUBMIT, r"(bool\s+result\s*=\s*false;)", r"\1 g_commands.drain(run_command);")),
    ("the ESP-NOW receive callback drains the ring",
     on("mesh_cpp", SIG_RECV_CB, r"(g_rx_pending\s*=\s*true;)", r"\1 g_commands.drain(run_command);")),
    # Rule 9: the status routes read the published view (F110).
    ("the status handler reads mesh_network::get_status() again",
     on("ino", ino_handler("handle_mesh_status"), r"(mesh_network::read_status\(&v\);)",
        r"\1 v.status = mesh_network::get_status();")),
    ("the peers handler walks the live peer table",
     on("ino", ino_handler("handle_mesh_peers"), r"(mesh_network::read_status\(&v\);)",
        r"\1 (void)mesh_network::get_peer(0);")),
    ("the peers handler takes its count from mesh_network::get_peer_count()",
     on("ino", ino_handler("handle_mesh_peers"),
        r"mesh_network::StatusView\s+v;\s*mesh_network::read_status\(&v\);",
        "mesh_network::StatusView v = {}; v.peer_count = mesh_network::get_peer_count();")),
    ("the alerts handler reads the live history",
     on("ino", ino_handler("handle_mesh_alerts"),
        r"mesh_network::read_alerts\(alerts,\s*mesh_network::MAX_ALERT_HISTORY\)",
        "0; size_t live_n = 0; (void)mesh_network::get_alerts(&live_n)")),
    ("the status handler takes the code from the live pairing session",
     on("ino", ino_handler("handle_mesh_status"), r"doc\[\"pairing_code\"\]\s*=\s*v\.pairing_code;",
        'doc["pairing_code"] = mesh_network::get_pairing_session()->confirmation_code;')),
    ("another handler reads mesh_network::is_enabled() live",
     on("ino", ino_handler("handle_mesh_leave"), r"(bool\s+ok\s*=\s*false;)",
        r"\1 if (!mesh_network::is_enabled()) {}")),
    ("update() returns early without publishing",
     on("mesh_cpp", SIG_UPDATE, r"\n[ \t]*publish_view\(\);[^\n]*\n([ \t]*return;)", r"\n\1")),
    ("update() does not publish at the end of its pass",
     on("mesh_cpp", SIG_UPDATE, r"\n[ \t]*publish_view\(\);\s*$", "\n")),
    ("send_heartbeat publishes the view",
     on("mesh_cpp", r"\bvoid\s+send_heartbeat\s*\(\s*\)", r"(if\s*\(!g_opera_config\.configured\)\s*return;)",
        r"\1 publish_view();")),
    ("read_status() builds the view from the live peer table",
     on("mesh_cpp", SIG_READ_STATUS, r"(out->status\.uptime_ms\s*=)",
        r"out->peer_count = g_peer_count; \1")),
    ("read_status() publishes before it reads",
     on("mesh_cpp", SIG_READ_STATUS, r"(if\s*\(!g_status_view\.read\(out\)\))", r"publish_view(); \1")),
    ("read_alerts() copies the live history",
     on("mesh_cpp", SIG_READ_ALERTS, r"return\s+g_alert_log\.read\(out,\s*cap\);",
        "size_t n = 0; const MeshAlert* a = get_alerts(&n); if (n > cap) n = cap; "
        "memcpy(out, a, n * sizeof(MeshAlert)); return n;")),
    ("store_alert() writes the history past the log's lock",
     on("mesh_cpp", r"\bstatic\s+void\s+store_alert\s*\([^)]*\)",
        r"\(void\)g_alert_log\.append\(\*alert\);",
        "if (g_alert_log.storage()) g_alert_log.storage()[0] = *alert;")),
    ("run_command() answers before it publishes (a read right after the POST shows the old pass)",
     on("mesh_cpp", SIG_RUN_COMMAND, r"\n[ \t]*publish_view\(\);[^\n]*\n", "\n")),
    ("run_command() returns from a case before it publishes",
     on("mesh_cpp", SIG_RUN_COMMAND, r"ok\s*=\s*remove_peer\(cmd\.fingerprint\);\s*break;",
        "return remove_peer(cmd.fingerprint);")),
    ("run_command() publishes before it runs the command",
     lambda s: on("mesh_cpp", SIG_RUN_COMMAND, r"\n[ \t]*publish_view\(\);[^\n]*\n", "\n")(
         on("mesh_cpp", SIG_RUN_COMMAND, r"(bool\s+ok\s*=\s*false;)", r"\1 publish_view();")(s))),
    ("a second place publishes the view",
     on("mesh_cpp", SIG_UPDATE, r"(g_commands\.drain\(run_command\);)",
        r"\1 { StatusView v = {}; (void)g_status_view.publish(v); }")),
    # Rule 6: the loop task serves a re-init without ever stopping a client;
    # only the worker stops one.
    ("the config POST runs init() itself",
     on("mqtt", SIG_CONFIG_POST, r"\(void\)wait_reinit\(request_reinit\(\),\s*kReinitWaitMs\);",
        "init(s_device_id, s_firmware_version, s_public_key_hex);")),
    ("the test handler runs init() itself",
     on("mqtt", SIG_TEST, r"wait_reinit\(request_reinit\(\),\s*kTestBudgetMs\)",
        "init(s_device_id, s_firmware_version, s_public_key_hex)")),
    ("the test handler stops and destroys the client by hand (the review's S1)",
     on("mqtt", SIG_TEST, r"(const\s+uint32_t\s+start\s*=\s*millis\(\);)",
        r"{ esp_mqtt_client_handle_t c = s_client.load(); if (c) { esp_mqtt_client_stop(c); "
        r"esp_mqtt_client_destroy(c); s_client = nullptr; } } \1")),
    ("the config POST publishes on the client directly (the review's S2)",
     on("mqtt", SIG_CONFIG_POST, r"(\(void\)wait_reinit\(request_reinit\(\),\s*kReinitWaitMs\);)",
        r'\1 esp_mqtt_client_publish(s_client.load(), "t", "p", 1, 0, 0);')),
    ("loop() no longer serves re-inits",
     on("mqtt", SIG_MQTT_LOOP, r"\n[ \t]*serve_reinit\(\);", "")),
    ("loop() serves the re-init after the pump (the review's MQ3)",
     lambda s: on("mqtt", SIG_MQTT_LOOP, r"(csi_event_egress::pump\(\);)", r"\1 serve_reinit();")(
         on("mqtt", SIG_MQTT_LOOP, r"\n[ \t]*serve_reinit\(\);", "")(s))),
    ("serve_reinit() stops the old client on the loop task (the reviewed shape)",
     on("mqtt", SIG_SERVE, r"detach_client\(\);\s*if\s*\(!retire_finished\(\)\)\s*return;[^\n]*",
        "esp_mqtt_client_handle_t old = s_client.exchange(nullptr); "
        "esp_mqtt_client_stop(old); esp_mqtt_client_destroy(old);")),
    ("serve_reinit() opens the new client before the worker is done",
     on("mqtt", SIG_SERVE, r"(detach_client\(\);\s*)if\s*\(!retire_finished\(\)\)\s*return;",
        r"\1(void)retire_finished();")),
    ("serve_reinit() opens while a client is still retiring",
     on("mqtt", SIG_SERVE, r"if\s*\(!retire_finished\(\)\)\s*return;", "")),
    ("serve_reinit() marks every request so far served (the review's MQ2)",
     on("mqtt", SIG_SERVE, r"s_reinit_served\.store\(wanted,", "s_reinit_served.store(s_reinit_wanted.load(),")),
    ("serve_reinit() reads the requests before it detaches",
     lambda s: on("mqtt", SIG_SERVE, r"(\n[ \t]*if\s*\(s_client\.load\()",
                  r"\n  const uint32_t wanted = s_reinit_wanted.load(std::memory_order_acquire);\1")(
         on("mqtt", SIG_SERVE, r"\n[ \t]*const\s+uint32_t\s+wanted\s*=\s*s_reinit_wanted\.load\([^;]*;", "")(s))),
    ("init() stops an open client in place",
     on("mqtt", r"\bbool\s+init\s*\([^)]*\)", r"\(void\)request_reinit\(\);",
        "{ esp_mqtt_client_handle_t old = s_client.exchange(nullptr); esp_mqtt_client_stop(old); "
        "esp_mqtt_client_destroy(old); }")),
    ("retire_finished() stops the client itself when the worker cannot start",
     on("mqtt", SIG_RETIRE_FINISHED, r"(if\s*\(!s_retire_create_failed_logged\)\s*\{)",
        r"esp_mqtt_client_stop(s_retiring); esp_mqtt_client_destroy(s_retiring); \1")),
    ("retire_finished() runs the worker's body in place",
     on("mqtt", SIG_RETIRE_FINISHED,
        r"if\s*\(xTaskCreate\(retire_task,[^;]*?\)\s*!=\s*pdPASS\)\s*\{",
        "retire_task(s_retiring); if (false) {")),
    ("loop() stops the client itself",
     on("mqtt", SIG_MQTT_LOOP, r"(csi_event_egress::pump\(\);)", r"esp_mqtt_client_stop(s_client.load()); \1")),
    ("open_client() destroys the open client",
     on("mqtt", SIG_OPEN, r"(ca_load\(\);)", r"esp_mqtt_client_destroy(s_client.load()); \1")),
    ("open_client() starts the client before it is s_client",
     lambda s: on("mqtt", SIG_OPEN, r'(\n[ \t]*Serial\.printf\("\[MQTT\] bridge started)',
                  r"\n  s_client.store(client, std::memory_order_release);\1")(
         on("mqtt", SIG_OPEN, r"\n[ \t]*s_client\.store\(client,[^;]*;", "")(s))),
    ("publish_raw() clears the client",
     on("mqtt", r"\bbool\s+publish_raw\s*\([^)]*\)", r"(if\s*\(msg_id\s*<\s*0\))",
        r"if (msg_id < -1) s_client = nullptr; \1")),
    ("request_reinit() re-inits in place",
     on("mqtt", SIG_REQUEST, r"(return\s+s_reinit_wanted)", r"serve_reinit(); \1")),
    ("the esp_mqtt handler re-inits",
     on("mqtt", SIG_EVENT_HANDLER, r"(s_connected\.store\(false,[^;]*;)", r"\1 (void)init(nullptr, nullptr, nullptr);")),
    ("the esp_mqtt handler stops its own client",
     on("mqtt", SIG_EVENT_HANDLER, r"(s_connected\.store\(false,[^;]*;)", r"\1 esp_mqtt_client_stop(e->client);")),
    ("the esp_mqtt handler handles a detached client's events",
     on("mqtt", SIG_EVENT_HANDLER, r"if\s*\(!e\s*\|\|\s*e->client\s*!=\s*s_client\.load\([^)]*\)\)\s*return;", "")),
    ("the esp_mqtt handler subscribes on s_client",
     on("mqtt", SIG_EVENT_HANDLER, r"esp_mqtt_client_subscribe\(e->client,", "esp_mqtt_client_subscribe(s_client.load(),")),
    ("set_update_auto_state publishes in place",
     on("mqtt", SIG_SET_AUTO, r"(s_update_auto_dirty\.store\([^;]*;)",
        r'\1 { char t[192]; build_topic(t, sizeof(t), "update/auto"); publish_raw(t, "ON", 2, true); }')),
    ("loop() never publishes the auto-update state",
     on("mqtt", SIG_MQTT_LOOP, r'build_topic\(topic,\s*sizeof\(topic\),\s*"update/auto"\);',
        'build_topic(topic, sizeof(topic), "update/x");')),
    # Rules 7, 8: the sketch's other tasks.
    ("the OTA settings handler publishes the switch on the httpd task",
     on("ino", ino_handler("handle_ota_config"), r"csi_mqtt::set_update_auto_state\(enabled\);",
        "csi_mqtt::publish_update_state(\"{}\");")),
    ("the QR scanner runs init() itself",
     on("ino", SIG_QR_TASK, r"\(void\)csi_mqtt::request_reinit\(\);",
        'csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, "");')),
    ("start_http_server() inits the bridge a second time",
     on("ino", SIG_START_HTTP, r"(register_api_routes\(g_http_server\);)",
        r'\1 csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, "");')),
    ("loop() starts the HTTP server (and with it the boot init) again",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)", r"\1 start_http_server();")),
    ("setup() inits the bridge a second time",
     on("ino", SIG_SETUP, r"(csi_mqtt::set_identity\([^;]*;)",
        r'\1 csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, "");')),
    ("setup() never gives the bridge its identity",
     on("ino", SIG_SETUP, r"csi_mqtt::set_identity\([^;]*;", "")),
    ("an HTTP handler pumps the bridge",
     on("ino", ino_handler("handle_ota_config"), r"(csi_mqtt::set_update_auto_state\(enabled\);)",
        r"\1 csi_mqtt::loop();")),
    ("the sketch hides the bridge's callers behind a using-directive",
     raw("ino", '#include "csi_mqtt.h"', '#include "csi_mqtt.h"\nusing namespace csi_mqtt;')),
    # Rules C1-C4: the Chirp channel's commands (F111).
    ("the Chirp send handler calls send_chirp itself",
     on_other(CHIRP_API, api_handler("handle_chirp_send"),
              r"const\s+loop_command_ring::Wait\s+w\s*=\s*chirp_channel::submit\(cmd,\s*&r\);",
              "r.ok = chirp_channel::send_chirp(template_id, urgency, detail, ttl); "
              "const loop_command_ring::Wait w = loop_command_ring::Wait::kDone;")),
    ("the Chirp mute handler answers 200 to a command that did not run",
     on_other(CHIRP_API, api_handler("handle_chirp_mute"),
              r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)\s*return\s+send_not_run\(req,\s*w\);",
              "(void)w;")),
    ("the Chirp settings handler stores the relay setting on the httpd task too",
     on_other(CHIRP_API, api_handler("handle_chirp_settings"), r"(cmd\.set_relay\s*=\s*true;)",
              r"\1 chirp_channel::set_relay_enabled(input[\"relay_enabled\"].as<bool>());")),
    ("the Chirp not-run answer goes out as 200",
     on_other(CHIRP_API, SIG_SEND_NOT_RUN,
              r"httpd_resp_set_status\(req,\s*http_status_line\(chirp_channel::not_run_status\(w\)\)\);", "")),
    ("mesh_network.h declares chirp_channel::mute again",
     raw("mesh_h", "bool is_muted();", "bool mute(uint8_t duration_minutes);\nbool is_muted();")),
    ("chirp_channel.cpp defines unmute without static",
     raw_other(CHIRP_CPP, "static void unmute() {\n", "void unmute() {\n")),
    ("chirp_channel's update() drains after its disabled return",
     lambda s: on_other(CHIRP_CPP, SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")(
         on_other(CHIRP_CPP, SIG_UPDATE, r"(uint32_t\s+now\s*=\s*millis\(\);)",
                  r"g_commands.drain(run_command); \1")(s))),
    ("chirp_channel's update() never drains",
     on_other(CHIRP_CPP, SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")),
    ("chirp_channel's submit() runs the command in place",
     on_other(CHIRP_CPP, SIG_CHANNEL_SUBMIT, r"(Result\s+r;)",
              r"\1 r = run_command(cmd); if (result != nullptr) *result = r; "
              r"return loop_command_ring::Wait::kDone;")),
    ("chirp_channel's send_presence() mutes the channel",
     on_other(CHIRP_CPP, r"\bstatic\s+void\s+send_presence\s*\(\s*\)", r"(ChirpHeader\*\s+hdr\s*=)",
              r"(void)mute(15); \1")),
    ("the sketch's loop() submits a Chirp command (it would wait for itself)",
     on("ino", SIG_INO_LOOP, r"(chirp_channel::update\(\);)",
        r"\1 (void)chirp_channel::submit(chirp_channel::make_command(chirp_channel::CHIRP_CMD_UNMUTE), nullptr);")),
    ("the sketch calls chirp_channel::update twice",
     on("ino", SIG_INO_LOOP, r"(chirp_channel::update\(\);)", r"\1 chirp_channel::update();")),
    # Rules C1-C4: the Bluetooth channel's commands (F111).
    ("the Bluetooth confirm handler calls confirm_pairing itself",
     on_other(BT_API, api_handler("handle_bluetooth_pair_confirm"),
              r"const\s+loop_command_ring::Wait\s+w\s*=\s*bluetooth_channel::submit\(cmd,\s*&r\);",
              "r.ok = bluetooth_channel::confirm_pairing(cmd.pin); "
              "const loop_command_ring::Wait w = loop_command_ring::Wait::kDone;")),
    ("the Bluetooth cancel handler cancels on the httpd task too",
     on_other(BT_API, api_handler("handle_bluetooth_pair_cancel"),
              r"(return\s+send_success\(req,\s*\"Pairing canceled\"\);)",
              r"bluetooth_channel::cancel_pairing(); \1")),
    ("the Bluetooth enable handler enables in place",
     on_other(BT_API, api_handler("handle_bluetooth_enable"),
              r"const\s+loop_command_ring::Wait\s+w\s*=\s*bluetooth_channel::submit\([^;]*;",
              "r.ok = bluetooth_channel::enable(); "
              "const loop_command_ring::Wait w = loop_command_ring::Wait::kDone;")),
    ("the Bluetooth scan-stop handler answers 200 to a command that did not run",
     on_other(BT_API, api_handler("handle_bluetooth_scan_stop"),
              r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)\s*return\s+send_not_run\(req,\s*w\);",
              "(void)w;")),
    ("the Bluetooth not-run answer goes out as 200",
     on_other(BT_API, SIG_SEND_NOT_RUN,
              r"httpd_resp_set_status\(req,\s*http_status_line\(bluetooth_channel::not_run_status\(w\)\)\);",
              "")),
    ("bluetooth_channel.h declares disable again",
     raw_other(BT_H, "bool is_enabled();\nbool is_advertising();",
               "void disable();\nbool is_enabled();\nbool is_advertising();")),
    ("bluetooth_channel.cpp defines confirm_pairing without static",
     raw_other(BT_CPP, "static bool confirm_pairing(uint32_t pin) {\n", "bool confirm_pairing(uint32_t pin) {\n")),
    ("bluetooth_channel's update() drains after its early return",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")(
         on_other(BT_CPP, SIG_UPDATE, r"(static\s+uint32_t\s+last_status_update\s*=\s*0;)",
                  r"g_commands.drain(run_command); \1")(s))),
    ("bluetooth_channel's update() never drains",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")),
    ("bluetooth_channel's submit() runs the command in place",
     on_other(BT_CPP, SIG_CHANNEL_SUBMIT, r"(Result\s+r;)",
              r"\1 r = run_command(cmd); if (result != nullptr) *result = r; "
              r"return loop_command_ring::Wait::kDone;")),
    ("bluetooth_channel's enable() brings the stack up again (on the loop task)",
     on_other(BT_CPP, SIG_BT_ENABLE, r"if\s*\(!g_initialized\)\s*return\s+false;",
              "if (!g_initialized) { if (!init()) return false; }")),
    ("bluetooth_channel's update_status_characteristic() disconnects",
     on_other(BT_CPP, r"\bstatic\s+void\s+update_status_characteristic\s*\(\s*\)",
              r"(if\s*\(!g_status_char\)\s*return;)", r"\1 (void)disconnect();")),
    ("the sketch's loop() brings the Bluetooth stack up",
     on("ino", SIG_INO_LOOP, r"(bluetooth_channel::update\(\);)", r"(void)bluetooth_channel::init(); \1")),
    ("the sketch hides the Bluetooth channel's callers behind a using-directive",
     raw("ino", '#include "bluetooth_api.h"', '#include "bluetooth_api.h"\nusing namespace bluetooth_channel;')),
    # Rule C4, loop(): the drains run every pass (the F111 review's X8, X9).
    ("the sketch's loop() runs chirp_channel::update() only while Chirp is on",
     on("ino", SIG_INO_LOOP, r"(chirp_channel::update\(\);)", r"if (chirp_channel::is_enabled()) \1")),
    ("the sketch's loop() runs bluetooth_channel::update() only while Bluetooth is on",
     on("ino", SIG_INO_LOOP, r"(bluetooth_channel::update\(\);)", r"if (bluetooth_channel::is_enabled()) \1")),
    ("the sketch's loop() can return before the channels' drains",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)", r"if (millis() < 1000) return; \1")),
    # Rule C3, the not-run answer (the review's X11, X12).
    ("the Chirp disable handler answers success to a withdrawn command",
     on_other(CHIRP_API, api_handler("handle_chirp_disable"),
              r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)",
              "if (w == loop_command_ring::Wait::kBusy)")),
    ("the Bluetooth disable handler answers success to a withdrawn command",
     on_other(BT_API, api_handler("handle_bluetooth_disable"),
              r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)",
              "if (w == loop_command_ring::Wait::kBusy)")),
    # Rule C3, bring_up() (the review's B-j, B-k).
    ("the Bluetooth enable handler no longer brings the stack up",
     on_other(BT_API, api_handler("handle_bluetooth_enable"),
              r"if\s*\(!bring_up\(\)\)\s*\{[^}]*\}", "")),
    ("the Bluetooth advertise handler no longer brings the stack up",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"),
              r"if\s*\(!bluetooth_channel::is_enabled\(\)\s*&&\s*!bring_up\(\)\)\s*\{[^}]*\}", "")),
    ("the Bluetooth pair handler no longer brings the stack up",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"),
              r"if\s*\(!bluetooth_channel::is_enabled\(\)\s*&&\s*!bring_up\(\)\)\s*\{[^}]*\}", "")),
    # Rule C3, what the command carries (the review's C-k, C-l, C-m, B-n, B-g).
    ("the Chirp ack handler dismisses a confirmation",
     on_other(CHIRP_API, api_handler("handle_chirp_ack"),
              r"\?\s*chirp_channel::CHIRP_CMD_CONFIRM(\s*):\s*chirp_channel::CHIRP_CMD_DISMISS",
              r"? chirp_channel::CHIRP_CMD_DISMISS\1: chirp_channel::CHIRP_CMD_CONFIRM")),
    ("the Chirp ack handler drops the nonce",
     on_other(CHIRP_API, api_handler("handle_chirp_ack"),
              r"\n[ \t]*memcpy\(cmd\.nonce,\s*nonce,\s*sizeof\(cmd\.nonce\)\);", "")),
    ("the Chirp send handler drops the TTL",
     on_other(CHIRP_API, api_handler("handle_chirp_send"), r"\n[ \t]*cmd\.ttl_minutes\s*=\s*ttl;", "")),
    ("the Chirp send handler sends every chirp as info",
     on_other(CHIRP_API, api_handler("handle_chirp_send"), r"(cmd\.ttl_minutes\s*=\s*ttl;)",
              r"\1 cmd.urgency = chirp_channel::CHIRP_URG_INFO;")),
    ("the Chirp mute handler drops the duration",
     on_other(CHIRP_API, api_handler("handle_chirp_mute"), r"\n[ \t]*cmd\.duration_minutes\s*=\s*duration;", "")),
    ("the Chirp unmute handler submits a mute",
     on_other(CHIRP_API, api_handler("handle_chirp_unmute"), r"chirp_channel::CHIRP_CMD_UNMUTE",
              "chirp_channel::CHIRP_CMD_MUTE")),
    ("the Chirp settings handler fills the relay setting without naming it",
     on_other(CHIRP_API, api_handler("handle_chirp_settings"), r"\n[ \t]*cmd\.set_relay\s*=\s*true;", "")),
    ("the Chirp settings handler answers the relay setting from the live state",
     on_other(CHIRP_API, api_handler("handle_chirp_settings"), r"=\s*r\.relay_enabled;",
              "= chirp_channel::is_relay_enabled();")),
    ("the Bluetooth PIN confirm handler drops the PIN",
     on_other(BT_API, api_handler("handle_bluetooth_pair_confirm"),
              r"\n[ \t]*cmd\.pin\s*=\s*input\[\"pin\"\]\.as<uint32_t>\(\);", "")),
    ("the Bluetooth trust handler always trusts",
     on_other(BT_API, api_handler("handle_bluetooth_paired_trust"), r"cmd\.flag\s*=\s*trusted;", "cmd.flag = true;")),
    ("the Bluetooth pair handler answers allow_pairing from the live settings",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"), r"if\s*\(!r\.allow_pairing\)",
              "if (!bluetooth_channel::get_settings().allow_pairing)")),
    ("the Bluetooth settings handler fills require_pin without naming it",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"\n[ \t]*cmd\.set_mask\s*\|=\s*bluetooth_channel::BT_SET_REQUIRE_PIN;", "")),
    ("the Bluetooth settings handler names the wrong field for long_range_mode",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"bluetooth_channel::BT_SET_LONG_RANGE;", "bluetooth_channel::BT_SET_REQUIRE_PIN;")),
    ("the Bluetooth settings handler fills a copy of the command's settings",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"bluetooth_channel::BluetoothSettings&\s*settings\s*=\s*cmd\.settings;",
              "bluetooth_channel::BluetoothSettings settings = cmd.settings;")),
    # Rule M1: the MQTT client's network timeout (F112).
    ("open_client() leaves esp_mqtt's 10 s network timeout",
     on("mqtt", SIG_OPEN, r"\n[ \t]*cfg\.network\.timeout_ms\s*=\s*\(int\)kNetworkTimeoutMs;", "")),
    ("open_client() sets the timeout after the client is made",
     lambda s: on("mqtt", SIG_OPEN, r"(esp_mqtt_client_register_event\()",
                  r"cfg.network.timeout_ms = (int)kNetworkTimeoutMs; \1")(
         on("mqtt", SIG_OPEN, r"\n[ \t]*cfg\.network\.timeout_ms\s*=\s*\(int\)kNetworkTimeoutMs;", "")(s))),
    ("kNetworkTimeoutMs grows past the watchdog's budget",
     raw_other(MQTT_H, "constexpr uint32_t kNetworkTimeoutMs = 2000;", "constexpr uint32_t kNetworkTimeoutMs = 3000;")),
    ("kNetworkOpsBudget shrinks below its three operations",
     raw_other(MQTT_H, "constexpr uint32_t kNetworkOpsBudget = 3;", "constexpr uint32_t kNetworkOpsBudget = 1;")),
    ("the sketch drops its static_assert of the MQTT timeout budget",
     raw("ino", "static_assert(csi_mqtt::kNetworkTimeoutMs > 0 &&", "static_assert(true ||")),
    # Rule CV1: no HTTP handler reads the live Chirp state (F138).
    ("the Chirp status handler reads chirp_channel::get_status() again",
     on_other(CHIRP_API, api_handler("handle_chirp_status"), r"(chirp_channel::read_status\(&v\);)",
              r"\1 v.state = chirp_channel::get_status().state;")),
    ("the Chirp nearby handler reads the live table",
     on_other(CHIRP_API, api_handler("handle_chirp_nearby"), r"(chirp_channel::read_nearby\(t\);)",
              r"\1 size_t live_n = 0; (void)chirp_channel::get_nearby_devices(&live_n);")),
    ("the Chirp recent handler reads the live table",
     on_other(CHIRP_API, api_handler("handle_chirp_recent"), r"(chirp_channel::read_recent\(t\);)",
              r"\1 size_t live_n = 0; (void)chirp_channel::get_recent_chirps(&live_n);")),
    ("the mesh status handler reads chirp_channel::can_send_chirp()",
     on("ino", ino_handler("handle_mesh_status"), r"(doc\[\"uptime_ms\"\]\s*=\s*status\.uptime_ms;)",
        r'\1 doc["chirp_ready"] = chirp_channel::can_send_chirp();')),
    # Rule CV2: each GET handler reads its view once and answers through lookups.
    ("the Chirp status handler reads the view twice",
     on_other(CHIRP_API, api_handler("handle_chirp_status"), r"(chirp_channel::read_status\(&v\);)",
              r"\1 chirp_channel::read_status(&v);")),
    ("the Chirp status handler asks the channel for night mode itself",
     on_other(CHIRP_API, api_handler("handle_chirp_status"), r"=\s*v\.night_mode;",
              "= chirp_channel::is_night_mode();")),
    ("the Chirp nearby handler never reads the view",
     on_other(CHIRP_API, api_handler("handle_chirp_nearby"), r"chirp_channel::read_nearby\(t\);",
              "memset(t, 0, sizeof(*t));")),
    # Rule CV3: when the loop task publishes.
    ("chirp update() no longer publishes at the end of its pass",
     on_other(CHIRP_CPP, SIG_UPDATE, r"\n[ \t]*publish_view\(\);\n\}?$", "\n")),
    ("chirp update()'s disabled return publishes nothing",
     on_other(CHIRP_CPP, SIG_UPDATE, r"\{\s*publish_view\(\);\s*return;\s*\}", "{ return; }")),
    ("chirp run_command() answers before it publishes",
     on_other(CHIRP_CPP, SIG_CHIRP_RUN, r"publish_view\(\);\s*return\s+r;", "return r;")),
    ("chirp run_command() publishes the status but not the tables",
     on_other(CHIRP_CPP, SIG_CHIRP_RUN, r"g_tables_changed\s*=\s*true;\s*(publish_view\(\);\s*return\s+r;)",
              r"\1")),
    ("chirp init() publishes nothing",
     on_other(CHIRP_CPP, SIG_CHIRP_INIT, r"publish_view\(\);\s*(return\s+true;\s*)$", r"\1")),
    ("chirp send_presence() publishes the view",
     on_other(CHIRP_CPP, r"\bstatic\s+void\s+send_presence\s*\(\s*\)", r"(ChirpHeader\*\s+hdr\s*=)",
              r"publish_view(); \1")),
    # Rule CV4: the published copies and their readers.
    ("chirp read_status() reads the live state",
     on_other(CHIRP_CPP, CHIRP_READER_SIGS["read_status"], r"(const\s+uint32_t\s+now\s*=\s*millis\(\);)",
              r"\1 out->state = g_state;")),
    ("chirp read_recent() counts the live table",
     on_other(CHIRP_CPP, CHIRP_READER_SIGS["read_recent"], r"(memset\(out,\s*0,\s*sizeof\(\*out\)\);)",
              r"\1 out->count = (uint8_t)g_recent_chirp_count;")),
    ("chirp read_nearby() reads nothing published",
     on_other(CHIRP_CPP, CHIRP_READER_SIGS["read_nearby"], r"if\s*\(!g_nearby_view\.read\(out\)\)\s*", "")),
    ("chirp unmute() publishes a status of its own",
     on_other(CHIRP_CPP, r"\bstatic\s+void\s+unmute\s*\(\s*\)", r"(g_muted\s*=\s*false;)",
              r"\1 { StatusView z; memset(&z, 0, sizeof z); (void)g_status_view.publish(z); }")),
    ("chirp update() clears the tables' flag before it publishes them",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"\1 g_tables_changed = false;")),
    # Rule CV5: every change to the tables is marked for the view.
    ("a chirp frame no longer marks the tables",
     on_other(CHIRP_CPP, SIG_CHIRP_RECV, r"g_tables_changed\s*=\s*true;\s*(switch)", r"\1")),
    ("the prune no longer marks the tables",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(prune_old_chirps\(\);)\s*g_tables_changed\s*=\s*true;", r"\1")),
    ("chirp send_presence() empties the recent table",
     on_other(CHIRP_CPP, r"\bstatic\s+void\s+send_presence\s*\(\s*\)", r"(ChirpHeader\*\s+hdr\s*=)",
              r"g_recent_chirp_count = 0; \1")),
    ("chirp update() handles a presence frame itself",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(reset_cooldown_if_stale\(\);)",
              r"\1 handle_presence(nullptr, 0, 0);")),
    # Rule CV6: the routes answer the keys they always did.
    ("the Chirp status route renames recent_chirps",
     raw_other(CHIRP_API, 'doc["recent_chirps"] = v.recent_chirp_count;',
               'doc["recent_count"] = v.recent_chirp_count;')),
    ("the Chirp nearby route drops listening",
     on_other(CHIRP_API, api_handler("handle_chirp_nearby"),
              r"\n[ \t]*dev\[\"listening\"\]\s*=\s*devices\[i\]\.listening;", "")),
    ("the Chirp recent route answers a new key",
     on_other(CHIRP_API, api_handler("handle_chirp_recent"), r"(c\[\"nonce\"\]\s*=\s*nonce_hex;)",
              r'\1 c["hop_limit"] = 3;')),
]

# Rules BV1..: the Bluetooth channel's settings enable (F144).
BV_MUTATIONS: list[tuple[str, Mutation]] = [
    ("the Bluetooth settings handler no longer brings the stack up for enabled:true",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"if\s*\(\(cmd\.set_mask\s*&\s*bluetooth_channel::BT_SET_ENABLED\)\s*&&\s*settings\.enabled"
              r"\s*&&\s*!bring_up\(\)\)\s*\{[^}]*\}", "")),
    ("the Bluetooth settings handler brings the stack up after the command ran",
     lambda s: on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
                        r"(if\s*\(r\.ok\))", r"if (!bring_up()) { return send_bt_error(req, \"x\"); } \1")(
         on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
                  r"if\s*\(\(cmd\.set_mask\s*&\s*bluetooth_channel::BT_SET_ENABLED\)\s*&&\s*settings\.enabled"
                  r"\s*&&\s*!bring_up\(\)\)\s*\{[^}]*\}", "")(s))),
    ("the Bluetooth settings handler answers a refused enable as updated",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"if\s*\(r\.refusal\s*==\s*bluetooth_channel::BT_REFUSED_NOT_ENABLED\)\s*\{[^}]*\}", "")),
    ("set_settings() decides from the setting it just assigned (F144's dead branches)",
     on_other(BT_CPP, SIG_BT_SET_SETTINGS, r"g_settings\.enabled\s*&&\s*!was_enabled",
              "g_settings.enabled && !is_enabled()")),
    ("set_settings() reads the old setting after the assignment",
     lambda s: on_other(BT_CPP, SIG_BT_SET_SETTINGS, r"(g_settings\s*=\s*settings;)",
                        r"\1 const bool was_enabled = g_settings.enabled;")(
         on_other(BT_CPP, SIG_BT_SET_SETTINGS, r"const\s+bool\s+was_enabled\s*=\s*g_settings\.enabled;",
                  "")(s))),
]
# Rule BV2: the NimBLE host task's callbacks (F143).
SIG_BT_CB = r"\bvoid\s+{}\s*\([^)]*\)\s*override"
BV_MUTATIONS += [
    ("onConnect writes the connection on the NimBLE host task",
     on_other(BT_CPP, SIG_BT_CB.format("onConnect"), r"(\(void\)post_event\()",
              r"g_connection.connected = true; \1")),
    ("onAuthenticationComplete saves the paired list on the NimBLE host task",
     on_other(BT_CPP, SIG_BT_CB.format("onAuthenticationComplete"), r"(\(void\)post_event\()",
              r"save_paired_devices(); \1")),
    ("onConfirmPassKey takes the pending pairing itself (the pre-F143 race)",
     on_other(BT_CPP, SIG_BT_CB.format("onConfirmPassKey"), r"(e\.u\.passkey\.conn\s*=\s*new)",
              r"delete g_pending_pair_info; \1")),
    ("onResult counts the scan table on the NimBLE host task",
     on_other(BT_CPP, SIG_BT_CB.format("onResult"), r"(\(void\)post_event\()",
              r"if (g_scanned_count < MAX_SCANNED_DEVICES) { } \1")),
    ("onScanEnd clears the scan flag in place",
     on_other(BT_CPP, SIG_BT_CB.format("onScanEnd"), r"(\(void\)post_event\()", r"g_scanning = false; \1")),
    ("onWrite applies its activity in place",
     on_other(BT_CPP, SIG_BT_CB.format("onWrite"), r"\(void\)post_event\(e,\s*EVENT_LOSSY_LIMIT\);",
              "apply_activity(e);")),
    ("link_event reads the live connection",
     on_other(BT_CPP, r"\bstatic\s+Event\s+link_event\s*\([^)]*\)", r"(return\s+e;)",
              r"e.u.link.reason = g_connection.connected; \1")),
    ("post_event applies the event in place (the callbacks' old shape)",
     on_other(BT_CPP, r"\bstatic\s+bool\s+post_event\s*\([^)]*\)", r"return\s+g_events\.post\(e,\s*limit\);",
              "(void)limit; apply_event(e); return true;")),
    ("the bring-up posts an event",
     on_other(BT_CPP, r"\bbool\s+init\s*\(\s*\)", r"(g_initialized\s*=\s*true;)",
              r"\1 (void)post_event(make_event(BT_EV_SCAN_END));")),
    ("update() never applies the events",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*g_events\.consume\(apply_event\);", "")),
    ("update() applies the events after its early return",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*g_events\.consume\(apply_event\);", "")(
         on_other(BT_CPP, SIG_UPDATE, r"(static\s+uint32_t\s+last_status_update\s*=\s*0;)",
                  r"g_events.consume(apply_event); \1")(s))),
    ("update() runs the commands before the events",
     on_other(BT_CPP, SIG_UPDATE,
              r"g_events\.consume\(apply_event\);(\s*)g_commands\.drain\(run_command\);",
              r"g_commands.drain(run_command);\1g_events.consume(apply_event);")),
    ("handle_scan_timeout consumes the events too",
     on_other(BT_CPP, r"\bstatic\s+void\s+handle_scan_timeout\s*\(\s*\)", r"(if\s*\(!g_scanning\)\s*return;)",
              r"\1 g_events.consume(apply_event);")),
    ("update() applies a link's end directly",
     on_other(BT_CPP, SIG_UPDATE, r"(handle_scan_timeout\(\);)", r"\1 if (false) apply_disconnect(make_event(BT_EV_DISCONNECT));")),
    ("onRead posts its activity at the full limit",
     on_other(BT_CPP, SIG_BT_CB.format("onRead"), r"post_event\(make_event\(BT_EV_ACTIVITY\),\s*EVENT_LOSSY_LIMIT\)",
              "post_event(make_event(BT_EV_ACTIVITY))")),
    ("onWrite posts its activity at the full limit",
     on_other(BT_CPP, SIG_BT_CB.format("onWrite"), r"post_event\(e,\s*EVENT_LOSSY_LIMIT\)", "post_event(e)")),
    ("onResult posts a scan result at the full limit",
     on_other(BT_CPP, SIG_BT_CB.format("onResult"), r"post_event\(e,\s*EVENT_LOSSY_LIMIT\)", "post_event(e)")),
    ("onDisconnect posts a link's end under the lossy limit",
     on_other(BT_CPP, SIG_BT_CB.format("onDisconnect"), r"\(void\)post_event\(e\);",
              "(void)post_event(e, EVENT_LOSSY_LIMIT);")),
    ("onScanEnd clears the bring-up's failure reason",
     on_other(BT_CPP, SIG_BT_CB.format("onScanEnd"), r"(\(void\)post_event\()", r"g_init_fail_reason[0] = 0; \1")),
    ("onScanEnd reads the published scan view",
     on_other(BT_CPP, SIG_BT_CB.format("onScanEnd"), r"(\(void\)post_event\()",
              r"ScanView v; (void)g_scan_view.read(&v); \1")),
    ("onConnect writes the health log on the NimBLE host task",
     on_other(BT_CPP, SIG_BT_CB.format("onConnect"), r"(\(void\)post_event\()",
              r"log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, \"BLE device connected\", nullptr); \1")),
    ("onConnect tells the presence sensor on the NimBLE host task",
     on_other(BT_CPP, SIG_BT_CB.format("onConnect"), r"(\(void\)post_event\()",
              r"ble_presence::notify_console_connected(true); \1")),
    ("onScanEnd stops the scan by its qualified name",
     on_other(BT_CPP, SIG_BT_CB.format("onScanEnd"), r"(\(void\)post_event\()",
              r"bluetooth_channel::stop_scan(); \1")),
]
# Rule BV3: the status routes' reads (F138).
SIG_BT_PUBLISH_SCAN = r"\bstatic\s+void\s+publish_scan_view\s*\(\s*\)"
BV_MUTATIONS += [
    ("bluetooth_channel.h declares get_status again",
     raw_other(BT_H, "// Status\nconst char* state_name(BluetoothState state);",
               "// Status\nBluetoothStatus get_status();\nconst char* state_name(BluetoothState state);")),
    ("the status route reads get_status() again",
     on_other(BT_API, api_handler("handle_bluetooth_status"),
              r"bluetooth_channel::BluetoothStatus\s+status;\s*bluetooth_channel::read_status\(&status\);",
              "bluetooth_channel::BluetoothStatus status = bluetooth_channel::get_status();")),
    ("the scan route takes the scan flag live",
     on_other(BT_API, api_handler("handle_bluetooth_scan_results"), r"=\s*view\.scanning;",
              "= bluetooth_channel::is_scanning();")),
    ("the settings route reads the enabled flag live",
     on_other(BT_API, api_handler("handle_bluetooth_settings_get"), r"=\s*settings\.enabled;",
              "= bluetooth_channel::is_enabled();")),
    ("the paired route reads the view twice (two passes in one answer)",
     on_other(BT_API, api_handler("handle_bluetooth_paired_list"), r"(bluetooth_channel::read_paired\(&view\);)",
              r"\1 bluetooth_channel::read_paired(&view);")),
    ("the status route drops a key",
     on_other(BT_API, api_handler("handle_bluetooth_status"), r"\n[ \t]*doc\[\"mtu\"\]\s*=\s*status\.mtu;", "")),
    ("the scan route renames a key",
     on_other(BT_API, api_handler("handle_bluetooth_scan_results"), r"dev\[\"age_sec\"\]", "dev[\"age_s\"]")),
    ("the dashboard reads a status key the route does not send",
     raw_other(WEB_UI, "if (data.connected && data.mtu) {", "if (data.connected && data.att_mtu) {")),
    ("the dashboard reads a scan key the route does not send",
     raw_other(WEB_UI, "if (d.is_securacv) return", "if (d.securacv) return")),
    ("update() returns early without publishing the views",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*publish_views\(\);[^\n]*\n(\s*return;)", r"\n\1")),
    ("update() does not publish the views at the end of its pass",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*publish_views\(\);\s*$", "\n")),
    ("run_command() answers before it publishes the views",
     on_other(BT_CPP, SIG_BT_RUN_COMMAND, r"\n[ \t]*publish_views\(\);[^\n]*\n", "\n")),
    ("run_command() returns from a case before it publishes",
     on_other(BT_CPP, SIG_BT_RUN_COMMAND, r"(r\.ok\s*=\s*set_tx_power\(cmd\.power\);)", r"\1 return r;")),
    ("init() publishes the views (the bring-up task, a second writer)",
     on_other(BT_CPP, r"\bbool\s+init\s*\(\s*\)", r"(g_initialized\s*=\s*true;)", r"\1 publish_views();")),
    ("a scan result publishes the scan view itself",
     on_other(BT_CPP, r"\bstatic\s+void\s+apply_scan_result\s*\([^)]*\)", r"(\*entry\s*=\s*seen;)",
              r"\1 publish_scan_view();")),
    ("update() publishes the scan view past publish_views()",
     on_other(BT_CPP, SIG_UPDATE, r"(handle_scan_timeout\(\);)", r"\1 (void)g_scan_view.publish(ScanView());")),
    ("read_status() takes the state live",
     on_other(BT_CPP, r"\bvoid\s+read_status\s*\([^)]*\)", r"(\*out\s*=\s*v\.status;)", r"\1 out->state = g_state;")),
    ("read_scan() copies the live table",
     on_other(BT_CPP, r"\bvoid\s+read_scan\s*\([^)]*\)", r"if\s*\(!g_scan_view\.read\(out\)\)\s*memset\(out,\s*0,\s*sizeof\(\*out\)\);",
              "memset(out, 0, sizeof(*out)); out->count = (uint8_t)g_scanned_count;")),
    ("read_settings() returns the live settings",
     on_other(BT_CPP, r"\bBluetoothSettings\s+read_settings\s*\(\s*\)", r"return\s+v\.settings;", "return g_settings;")),
    ("publish_scan_view() reads the paired view",
     on_other(BT_CPP, SIG_BT_PUBLISH_SCAN, r"(\(void\)g_scan_view\.publish\(v\);)",
              r"PairedView pv; (void)g_paired_view.read(&pv); \1")),
]
MUTATIONS += BV_MUTATIONS


def self_test(srcs: dict, others: dict[str, str]) -> list[str]:
    problems = []
    srcs = dict(srcs)
    srcs["others"] = others
    for name, mutate in MUTATIONS:
        try:
            m = mutate(srcs)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) — "
                            "the source changed shape; update this guard's mutations with it")
            continue
        if m == srcs:
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check(m["ino"], m["mesh_h"], m["mesh_cpp"], m["mqtt"], m.get("others", others)):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def sketch_others() -> dict[str, str]:
    """Every other source file of the sketch, by repo-relative path."""
    skip = {INO, MESH_H, MESH_CPP, MQTT_CPP}
    out = {}
    for pattern in SKETCH_GLOBS:
        for path in sorted((REPO / SKETCH).glob(pattern)):
            rel = path.relative_to(REPO).as_posix()
            if rel not in skip:
                out[rel] = path.read_text(encoding="utf-8", errors="replace")
    return out


def main() -> int:
    srcs = {
        "ino": (REPO / INO).read_text(encoding="utf-8"),
        "mesh_h": (REPO / MESH_H).read_text(encoding="utf-8"),
        "mesh_cpp": (REPO / MESH_CPP).read_text(encoding="utf-8"),
        "mqtt": (REPO / MQTT_CPP).read_text(encoding="utf-8"),
    }
    others = sketch_others()
    errors = check(srcs["ino"], srcs["mesh_h"], srcs["mesh_cpp"], srcs["mqtt"], others)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(srcs, others)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print(f"canary-wap loop-task ownership holds: the mesh's, Chirp's and Bluetooth's owner "
          f"commands are internal to their channels and run from update()'s drain, the REST "
          f"handlers only submit, the mesh and Chirp status routes read only what the loop task "
          f"published, "
          f"the MQTT client is replaced only by loop()'s re-init, which never stops a client "
          f"(the retire_task worker does), and every client's network timeout keeps a "
          f"loop-task publish under the watchdog "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
