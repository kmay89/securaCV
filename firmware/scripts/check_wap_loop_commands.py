#!/usr/bin/env python3
"""Hold the canary-wap's loop-task ownership: mesh, Chirp and Bluetooth
commands, mesh, Chirp and Bluetooth status reads, the NimBLE host task's
callbacks, MQTT re-inits, and the MQTT client's network timeout.

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
   pre-reboot hook, which the safe-mode retry runs on the httpd task (and
   POST /api/reboot on the loop task, since sweep F92), calls
   `mesh_network::save_replay_counters_before_reboot(`,
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
     `s_reinit_wanted` into `wanted` after that, then runs
     `open_client(&busy)` once, returns if it reported a save in progress
     (`if (busy) return;` — sweep F289's review: nothing opens from a row
     another task is still writing, and the request waits for the next
     pass), then `s_reinit_served.store(wanted`.
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
    handlers (`BT_BRING_UP_HANDLERS`) bring the stack up first, whenever it
    is down, whatever the saved setting says (F235): the enable handler's
    first statement is `if (!bring_up()) { return send_bring_up_refused(req,
    ...`, and none of them reads the published setting or status
    (`read_enabled(`, `read_settings(`, `read_status(`) in its own body. No
    command runs `init()`, so without it a tap could never bring up a stack
    whose boot bring-up failed or the heap guard refused. That was the
    advertise and pair handlers' case on a device saved on (the default)
    until F235: they brought the stack up only when the published setting
    said off, so every tap answered a failed start. The enable, advertise and
    pair handlers (`BT_BOOT_WAIT_HANDLERS`) first leave the stack to the
    sketch's boot bring-up until it is over (the F235 review for advertise
    and pair; F259 for enable, and whatever the saved setting: the sketch
    defers the bring-up out of the provisioning join window and skips it in
    safe mode, and the firmware's other NimBLE init callers run inside it,
    the worker's ble_manager::init() on FULL and the Scout's at finalize, so
    a tap there undid the deferral and could run NimBLE's init beside
    theirs, the worker's own init() then losing its latch): their first
    statement is `if (waits_for_boot_bring_up()) { return
    send_boot_bring_up_pending(req); }`, then the bring-up guard, and the
    settings POST's "enabled": true branch says the same right before its
    own bring-up guard (BV1). `waits_for_boot_bring_up()` is exactly `return
    !bluetooth_channel::is_initialized() &&
    !bluetooth_channel::boot_bring_up_tried();` (F259 dropped `&&
    bluetooth_channel::read_enabled()`: the worker runs init() whatever the
    setting, so a device saved off waits like one saved on), called once by
    each of those four and by nothing else; `send_boot_bring_up_pending()`
    is exactly a 503 with `boot_bring_up_skipped_error()` when the sketch
    skipped its boot bring-up (safe mode, F259: the tap was told Bluetooth
    would start within five minutes), `bring_up_starting_error()` once
    `boot_bring_up_started()` says the sketch has started its boot bring-up
    or `bring_up_starting()` says a bring-up runs, else
    `boot_bring_up_pending_error()` (wave 16's verify: after the worker's
    init() was refused, and before the finalize stage, a tap was told
    Bluetooth had not started yet, with Wi-Fi setup done and the refusal
    recorded). In `bluetooth_channel.cpp`,
    `boot_bring_up_started()` is exactly the acquire load of
    `g_boot_bring_up_started`, whose one release store is
    `bringup_worker_started()`'s (BV7 holds that body), and nothing else
    names it;
    `boot_bring_up_tried()` is exactly the acquire load of
    `g_boot_bring_up_tried` and `boot_bring_up_finished()` exactly
    `g_boot_stack_up = is_initialized();` (F260, BV10) and then its release
    store, and nothing else names the flag (BV7 holds the sketch's
    two calls of the setter: last in its finalize stage, after the Scout's
    init, and where the worker could not be created). `bring_up()` is
    exactly `return bluetooth_channel::is_initialized() ||
    bluetooth_channel::init();`. It is called only as `!bring_up()` in a
    guard of `BT_BRING_UP_GUARDED` (the three handlers and the settings
    POST), unqualified, and no other file of the sketch names `bring_up` or
    `bluetooth_api::bring_up`, every call and every other reference of every
    file looked at (from loop() it is NimBLE's init on the loop task; since
    wave 16's verify a reference too, so `&bluetooth_api::bring_up` taken
    into a function pointer and called through it is refused, in
    `bluetooth_api.h` as anywhere else), and no file of the sketch,
    `bluetooth_api.h` among them, names the namespace another way
    (`namespace x = bluetooth_api;` or `using namespace bluetooth_api;`:
    since the second verify, as through either `x::bring_up` is read by
    neither pattern; a name built by token pasting is a lexical limit, not
    held); every such guard (the
    settings POST's too, BV1) answers through
    `send_bring_up_refused(req, `, which is exactly a 503 with
    `bring_up_starting_error()` while `bluetooth_channel::bring_up_starting()`
    says another task's bring-up holds the latch or has just brought the
    stack up, else `send_bt_error(req, fallback)` (the refusal by name). In
    `bluetooth_channel.cpp`, `bring_up_starting()` is exactly `return
    init_running() || is_initialized();`, the latch read first (init()
    publishes the stack before it clears the latch, so read the other way a
    bring-up that came up between the two reads would answer its
    predecessor's refusal).
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
`read_recent()` (`loop_snapshot.h`; `test_chirp_commands_wap.cpp`). The
status's copy is static; the tables' copies live in one PSRAM block
(`g_view_tables`, `loop_snapshot::AttachedValue`), so they take nothing
back from the internal heap the PSRAM diet freed for the BLE stack.

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
     ends by attaching the two tables' copies to their PSRAM block, then
     `g_tables_changed = true; publish_view(); return true;` (the HTTP
     server can answer before a pass runs).
CV4. Each view's `.publish(` is in `publish_view()` alone and its `.read(`
     in its own reader alone. The two tables' views are
     `loop_snapshot::AttachedValue`s (their copies in the PSRAM block, not
     in internal SRAM: the PSRAM diet's budget for the BLE heap), their
     `.attach(` only in `init()`, and `g_view_tables` (that block: the
     copies and the scratch, from `csi_large_calloc()`) only in `init()`
     and `publish_view()`. `g_tables_changed = false` only in
     `publish_view()`, once, right after its `if (!g_tables_changed || ...)
     return;` (a pass nothing marked builds no table); `= true` only in the
     four places that change the tables. The readers and
     `cannot_send_reason()` copy their view and name none of the live
     state (`CHIRP_LIVE_STATE`).
CV5. Every change to the tables is marked for the view: `on_espnow_recv()`
     sets `g_tables_changed = true;` once, right before its dispatch switch;
     `update()` sets it right after its prune; the functions that name the
     tables are `CHIRP_TABLE_FUNCS` (a new one is a new path to them), and
     the frame and prune paths are called only from the frame dispatch and
     `update()` (`CHIRP_TABLE_CALLERS`). Commands are rule C2's.
CV6. Each Chirp GET handler answers exactly the keys it always did
     (`CHIRP_GET_KEYS`): the dashboard parses them.
CV7. What the routes put under each key (`chirp_api.h` is not host-compiled:
     ArduinoJson is not on the host, so this is what holds the glue). Each
     GET key is set once, from its field of the copy (`CHIRP_STATUS_FIELDS`,
     `CHIRP_NEARBY_FIELDS`, `CHIRP_RECENT_FIELDS`); `cannot_send_reason`
     only from one `chirp_channel::cannot_send_reason(v)` (F146:
     `clock_unsynced`); the rows from the copy's own count and array, the
     recent route skipping a dismissed row first (`CHIRP_ROUTE_SHAPE`);
     `free(t)` right after `serializeJson()` (ArduinoJson keeps a `const`
     char array, the copy's emoji, by pointer until it serializes) and
     before every `return` that follows the read. `POST /api/chirp/send`
     answers a refusal exactly as `CHIRP_SEND_ANSWER` says: `error` and
     `message` from `send_refusal_error()` and `send_refusal_message()`
     (host-tested), the cooldown's two fields only for a cooldown, its time
     left rounded up by `seconds_left()` (F178: a cooldown's last second read
     0 s), as the status route's is.
CV8. The send cooldown is a timer, not a state (F178): in
     `chirp_channel.cpp`, `CHIRP_COOLDOWN` is named only by `shown_state()`
     (what an active channel reads as while the timer runs) and
     `state_name()`. Nothing stores it, so a mute cannot overwrite it, and
     nothing gates on it: `send_gate()` (what `can_send_chirp()` answers
     and the send refuses on), `read_status()` and `cannot_send_reason()`
     read the timer (`test_chirp_commands_wap.cpp`'s
     `a_mute_does_not_end_the_cooldown`, `a_send_while_muted_stays_muted`,
     `a_send_just_after_the_cooldown_goes_out`,
     `a_send_at_an_edge_names_why`).
CV9. A refused confirm says why, and a dismiss whether its suppress vote
     went out (F174). `send_confirm_answer()` and `send_dismiss_answer()`
     are exactly `CHIRP_CONFIRM_ANSWER` and `CHIRP_DISMISS_ANSWER`: the
     status, `error` and `message` from `confirm_refusal_status()`,
     `confirm_refusal_error()` and `confirm_refusal_message()`, a hidden
     chirp's `vote_sent` and, when it stayed home, `vote_error` and
     `vote_unsent_message()` (host-tested), all from the `Result`. The
     confirm and dismiss routes answer through them right after the
     not-run guard, and `/api/chirp/ack` through the one of the command it
     ran.
CV10. Every `chirp_api.h` function that answers a `message` serializes into
     a buffer of at least `CHIRP_MESSAGE_BUFFER` bytes. `serializeJson()`
     into a char array leaves it unterminated when the answer fills it, and
     `httpd_resp_sendstr()` then sends the stack after it: the confirm
     route's 85-byte refusal went out of a 64-byte buffer that way, and so
     did the mute route's 101-byte one (F174; an ArduinoJson 7.4.1 scratch
     harness showed both). The confirm and dismiss answers' 256 bytes are
     CV9's (the longest, 161 bytes, is host-tested). Since F196
     `check_wap_json_answers.py` measures every REST answer buffer of the
     sketch against the longest answer it computes; this floor stays.
CV11. The dashboard shows what the routes now say (`web_ui.h`): the Chirp
     card takes Send's state from `WebUiLogic.chirpSendGate()` (Send off for
     any `can_send` that is not true, F178), and a confirm or a dismiss puts
     `WebUiLogic.chirpActionNote()` of its answer under the list (F174);
     `web_ui_logic.test.js` tests both functions. A mute or an unmute puts it
     there too (F192; `web_ui_logic.test.js` runs both buttons' glue).
CV12. A refused mute or unmute says why (F192). `send_mute_answer()` is
     exactly `CHIRP_MUTE_ANSWER`: the status (409 for the channel off; a
     duration the channel lacks keeps its 200, F195's decision, and sets no
     status line, which `http_status_line()` has none for), `error` and
     `message` from `mute_refusal_status()`, `mute_refusal_error()` and
     `mute_refusal_message()` (host-tested), all from the `Result`. The mute
     and unmute routes answer through it right after the not-run guard. A
     mute on a channel that was off turned it on with no session, and the
     unmute answered success for nothing (`test_chirp_commands_wap.cpp`'s
     `a_mute_needs_a_channel_that_is_on`).

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

Bluetooth settings, events, views and bring-up (F144, F143, F138, F171,
F167, F189, F196, F210, F191, F209, F259, F260): rules of their own, in
their own block below (`check_bluetooth_views`, `BV_MUTATIONS`).

BV1. A settings POST that turns Bluetooth on does it the way
     POST /api/bluetooth/enable does (F144). `handle_bluetooth_settings_set`
     brings the stack up on its own task before it submits, when the POST
     names `enabled: true`
     (`if ((cmd.set_mask & bluetooth_channel::BT_SET_ENABLED) && settings.enabled
     && !bring_up()) { return send_bring_up_refused(req, ...`, C3's answer to
     a bring-up that left the stack down), and answers a command refused for a stack
     that is not up with the init error, right after the not-run guard
     (`if (r.refusal == bluetooth_channel::BT_REFUSED_NOT_ENABLED) { return
     send_bt_error(req, ...`). Right before that bring-up it leaves the stack
     to the sketch's boot bring-up, as Enable does (F259: `if
     ((cmd.set_mask & bluetooth_channel::BT_SET_ENABLED) && settings.enabled
     && waits_for_boot_bring_up()) { return send_boot_bring_up_pending(req);
     }`, once). In `bluetooth_channel.cpp`, `set_settings()`
     reads `const bool was_enabled = g_settings.enabled;` before it assigns
     `g_settings = settings;` and names no `is_enabled(`: the reader returns
     the value just assigned, which is how both of its branches were dead.
BV2. The NimBLE host task's callbacks touch none of the channel's state
     (F143). In `bluetooth_channel.cpp` the callbacks (`BT_CALLBACKS`:
     `onConnect`, `onDisconnect`, `onAuthenticationComplete`,
     `onPassKeyDisplay`, `onConfirmPassKey`, `onWrite`, `onRead`,
     `onResult`, `onScanEnd`) and the helpers they build their events with
     (`make_event`, `link_event`, `post_event`) name none of `BT_LIVE_STATE`
     and no other file global but the queue (`post_event`'s `g_events`),
     the data hook (`onWrite`'s `g_data_callback`) and, since F191,
     `onPassKeyDisplay`'s passkey gate (`g_owner_pairing_open`, BV9), name nothing qualified
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
     `apply_event()`, but `apply_connect()` and `apply_disconnect()`, which
     `reconcile_link()` calls too (F169: after a link's event was dropped,
     the loop task ends or records the link from the stack's own record),
     and `apply_auth_complete()`, which `refuse_unauthenticated_links()`
     calls too (F191's review, BV9).
     `reconcile_link();` is `update()`'s, once, between the consume and the
     drain, and nobody else calls it. What a full queue does (a link's
     events kept, the drops logged by kind, a passkey failed closed, the
     reconciliation) and what each event carries are
     `test_bluetooth_commands_wap.cpp`'s.
BV3. The Bluetooth status routes read only what the loop task published
     (F138). `bluetooth_channel.h` declares none of the live readers
     (`BT_LIVE_READERS`: `get_status`, `get_settings`, `get_scanned_devices`,
     `get_paired_devices`, `is_scanning`, ..., and since F210 `is_enabled`),
     and no HTTP handler in the sketch names one. `handle_bluetooth_status`, `handle_bluetooth_scan_results`,
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

BD1. One set of NimBLE server callbacks, two owners (F171). NimBLE keeps one
     callbacks pointer per server, and on the FULL profile Opera's
     `setCallbacks()` replaced the pairing channel's, so the library's
     default answered every Numeric Comparison yes. In the sketch, every
     `setCallbacks(` but the dispatcher's own hands over an object of a
     class derived (in any number of steps) from
     `NimBLECharacteristicCallbacks`, by address, by name or new: never a
     server callbacks object (of any class derived from
     `NimBLEServerCallbacks`, however many steps down), a pointer variable,
     or `nullptr` / `NULL` / `0`, which put NimBLE's default server
     callbacks back (the F171 review). Each owner hands its server
     callbacks to the dispatcher: `bluetooth_channel.cpp`'s
     `adopt_init_result()` as `kPairing` (`&g_server_callbacks`: the loop
     task, once it takes `init()`'s result, F167) with
     `ble_server_dispatch::set_owner(`, and `ble_opera.h`'s `init()` as
     `kLink` (`&g_serverCallbacks`) with `ble_server_dispatch::install(`,
     each once, and no other file names a role. `ble_server_dispatch.h`'s
     `attach()` is the one `setCallbacks(`, of the dispatcher, with
     NimBLE's deleteCallbacks false (the dispatcher is not on the heap);
     `install()` records the owner and calls it, nothing else;
     `set_owner()` only records the owner (`g_dispatcher.set(role,
     owner);`), and only the channel calls it and never `install()` (the
     F167 review: a second `setCallbacks()` from the loop task raced
     Opera's on the bring-up worker); and the channel's `init()` attaches
     the dispatcher to the server it creates, once (F167: until the loop
     task names the channel's callbacks, the server would otherwise
     answer with NimBLE's defaults). What the dispatcher hands each owner is
     `test_bluetooth_commands_wap.cpp`'s, which compiles both inits over the
     stand-in.

BV4. `init()` hands its result to the loop task (F167). It runs on the BLE
     bring-up worker or on an HTTP handler's task (`bring_up()`), and it
     wrote `g_settings`, the paired list, the state, the NimBLE object
     pointers, `g_initialized` (a plain bool) and the refusal text, and
     with auto-advertise on called `enable()` and `start_advertising()`,
     while the loop task's `update()` read and wrote the same. In
     `bluetooth_channel.cpp`: `init()` names no file global but its latch,
     its hand-over (`g_bringup`, `g_bringup_ready`), `g_stack_up`, the
     device metadata (`g_meta_*`) and the GATT and scan callbacks objects
     (`BT_INIT_GLOBALS`), and calls nothing of the file but
     `load_settings()` (the settings it gives the stack, into the
     hand-over) and `set_init_fail_reason()` (`BT_INIT_CALLS`); the
     loaders name no file global (they fill what they are given). `init()`
     starts `if (__atomic_load_n(&g_stack_up, __ATOMIC_ACQUIRE)) return true;`
     and then takes its latch, `bool expected = false; if
     (!__atomic_compare_exchange_n(&g_init_in_progress, &expected, true,
     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { return false; }`, with the
     guard that clears it, `struct InitLatchClear { ~InitLatchClear() {
     __atomic_store_n(&g_init_in_progress, false, __ATOMIC_RELEASE); } }
     latch_clear;`, then looks at the stack again, `if
     (__atomic_load_n(&g_stack_up, __ATOMIC_ACQUIRE)) return true;` (wave
     16's verify: without it a caller that read the stack down just before a
     winner finished took the freed latch and ran NimBLE's init and
     createServer() a second time), naming `g_init_in_progress`,
     `InitLatchClear` and `latch_clear` nowhere else (an explicit
     `latch_clear.~InitLatchClear();` or an inner guard is the early clear by
     another spelling), and
     `init_running()` is exactly its acquire load (the F235 review: the latch
     clears as `init()` returns, after the stack's publish, which C3's
     `bring_up_starting()` reads the latch first on; cleared earlier, a tap
     that lost the latch answered a refusal for a stack coming up). `init()` publishes the hand-over
     once, `__atomic_store_n(&g_bringup_ready, true, __ATOMIC_RELEASE);`,
     writes none of it after, and only then sets
     `__atomic_store_n(&g_stack_up, true, __ATOMIC_RELEASE);`, touching
     `g_stack_up` no other way. `adopt_init_result()` starts with
     `if (g_initialized || !__atomic_load_n(&g_bringup_ready,
     __ATOMIC_ACQUIRE)) return;` and is called only by `update()`, as its
     first statement, once; `g_initialized` is assigned only there and in
     the uncalled `deinit()`, and the paired list's rebuild
     (`migrate_paired_devices(`) runs only there. `is_initialized()` is
     exactly the acquire load of `g_stack_up`, and `init_fail_reason()`
     exactly the acquire load of `g_init_fail_reason`, which
     `set_init_fail_reason()` publishes with a release store after writing
     the text whole into the next of its slots. The hand-over's globals are
     named only by those functions (`BT_HANDOFF_NAMES`). And the F167
     review: the hand-over carried the saved settings, and the adoption
     overwrote what the owner's commands had done while `init()` ran (a
     Disable came back on). `update()` starts `load_saved();
     adopt_init_result();`, each once; `load_saved()` starts `if
     (g_saved_loaded) return; g_saved_loaded = true;`, loads the settings
     and the paired list, is called only by `update()` and alone names
     `g_saved_loaded`; `load_paired_devices()` is called only by
     `load_saved()`, `load_settings()` only by it and `init()`;
     `adopt_init_result()` writes none of `g_settings`, the paired list or
     its count and flag (`BT_ADOPT_KEEPS`); and `remove_paired_device()`
     and `clear_all_paired_devices()` start `if (!g_initialized) {
     *refusal = BT_REFUSED_NOT_UP; return false; }` (the list is loaded
     before the stack is up; its bonds are NimBLE's). That nothing of the
     loop task's moves on the bring-up's task, the commands kept across
     it, and the threads under TSAN, are `test_bluetooth_commands_wap.cpp`'s.

BV5. The bond store never evicts behind the owner, and the paired list takes
     only a bond the store holds (F189). NimBLE keeps 3 bonds by default
     where the list keeps 8, and its default store-status answer unpaired
     the oldest bond through `ble_gap_unpair()`'s busy guard when a new one
     found the store full. `bluetooth_channel.cpp`'s `init()` installs
     `NimBLEDevice::setDeviceCallbacks(&g_store_callbacks);` once, and no
     other file of the sketch sets the device callbacks;
     `StoreCallbacks::onStoreStatus()` calls nothing that deletes a bond
     (`BT_STORE_EVICTS`: `ble_gap_unpair*`, `deleteBond`, NimBLE's default
     answer, the store's deletes) and answers `BLE_STORE_EVENT_FULL` and
     `BLE_STORE_EVENT_OVERFLOW` with `return BLE_HS_ESTORE_CAP;`, 0 only for
     a peer the store already holds; it posts under `EVENT_LOSSY_LIMIT`
     (BV2). `apply_auth_complete()` adds a new entry only behind
     `if (!found && !NimBLEDevice::isBonded(identity)) { ... } else if
     (!found && g_paired_count < MAX_PAIRED_DEVICES) {`. What a full store
     does, over NimBLE's store as the stand-in models it, is
     `test_bluetooth_commands_wap.cpp`'s.

BV6. Every Bluetooth REST answer goes out at its own length (F196). The
     routes serialized into fixed char buffers (128, 320, 512 bytes) sized
     by eye; a document that filled one is left unterminated by
     `serializeJson()`, and `httpd_resp_sendstr()` sends what follows it.
     `bluetooth_api.h` serializes in one function, `send_doc()`, whose body
     is exactly `String out; if (!out.reserve(measureJson(doc) + 1)) {
     return send_json_response(req, "..."); } serializeJson(doc, out);
     return send_json_response(req, out.c_str());` (a failed reservation
     answers the fixed allocation error), and no other function of the file
     calls `serializeJson(`, `serializeJsonPretty(` or `serializeMsgPack(`.

BV7. No advertising start while the sketch's bring-up worker registers GATT
     services (the F167 review). After the channel's `init()` returns, the
     worker (`canary_wap.ino`'s `ble_bringup_task`) goes on to
     `ble_status::init()` and, on FULL, `ble_opera::init()`, both adding
     services to the one server, and `NimBLEAdvertising::start()` starts
     that server (`NimBLEServer::start()` walks the service list the
     worker appends to). In `bluetooth_channel.cpp`: `start_advertising()`
     holds the start (`if (g_bringup_worker_running) {
     g_advertise_after_bringup = true; return true; }`) before it starts
     anything; `restore_radio()` puts a stopped advertiser back only
     outside the bring-up (`if (g_bringup_worker_running) {
     g_advertise_after_bringup = true; } else { g_advertising->start(); }`);
     no other function starts `g_advertising`; `stop_advertising()` starts
     `g_advertise_after_bringup = false;`; `bringup_worker_started()` and
     `bringup_worker_finished()` are exactly `g_bringup_worker_running =
     true; __atomic_store_n(&g_boot_bring_up_started, true,
     __ATOMIC_RELEASE);` (the second since wave 16's verify, C3's
     `boot_bring_up_started()`) and `g_bringup_worker_running = false; if
     (g_advertise_after_bringup) { g_advertise_after_bringup = false;
     start_advertising(); }`, and nothing else sets the flag. In
     `canary_wap.ino`: `ble_discovery_start_if_due()` calls
     `bluetooth_channel::bringup_worker_started();` once, before
     `xTaskCreate(ble_bringup_task, ...` (the worker may run before it
     returns), and `bringup_worker_finished();` when the create fails;
     `ble_bringup_finalize_if_done()` calls `bringup_worker_finished();`
     once, after its acquire of `g_ble_bringup_done`; no other function of
     the sketch calls either (both are loop-task only). The boot bring-up's
     end (the F235 review, C3's `boot_bring_up_tried()`):
     `bluetooth_channel::boot_bring_up_finished();` is called once in
     `ble_discovery_start_if_due()`, after the failed create's
     `bringup_worker_finished();`, and once in
     `ble_bringup_finalize_if_done()` as its last call into the channel, the
     Scout or ble_manager (after `ble_scout::ble_scout_init();`, whose
     `nimble_scan_init()` runs `NimBLEDevice::init()` on the loop task when
     the stack is down), and by nothing else. And each runs every time it is
     reached (the verifier's escapes, wave 16; `unconditional_in()`,
     `pp_guards()`): the gate's `bringup_worker_finished();` and
     `boot_bring_up_finished();` are statements of the one `if
     (xTaskCreate(ble_bringup_task, ...) != pdPASS) {` block itself, no
     condition, branch, inner block or `return`/`goto` ahead of them there,
     compiled under the gate's `#if FEATURE_BLE || FEATURE_BLUETOOTH ||
     FEATURE_BLE_SCAN` with `#if FEATURE_BLUETOOTH` alone inside it (run
     after a create that worked, the first lets an advertising start walk
     the worker's services and the second lets a tap's `init()` run beside
     the worker's inits and the Scout's: B1); the finalize stage has no
     `return` or `goto` after its head, and its mark is a statement of its
     body's own under `#if FEATURE_BLUETOOTH` alone (skipped on one result, a
     device saved on answered every Start Advertising and Pair tap 503 "has
     not started yet" until a reboot: F235's own symptom). Since the second
     verify (wave 16) the same holds the other three: the gate's
     `bringup_worker_started();` (which also opens C3's still-starting
     answer) is a statement of the gate's body itself, under the gate's
     guard with `#if FEATURE_BLUETOOTH` alone inside it, with nothing but
     its `#endif` between it and the create (no `return`, `goto`, label or
     statement fits there, so it runs whenever the create does, and the
     create whenever it has run); the finalize stage's
     `bringup_worker_finished();` is a statement of its body's own under
     `#if FEATURE_BLUETOOTH` alone with no `return` or `goto` between the
     head and it; and `ble_bringup_task()` ends with
     `__atomic_store_n(&g_ble_bringup_done, true, __ATOMIC_RELEASE);` then
     its one `vTaskDelete(NULL);`, the store a statement of its body's own
     outside every `#if`, no `return` or `goto` in the worker, and nothing
     in the sketch names `g_ble_bringup_done` but its definition, acquire
     reads and that store. (A start mark skipped before a create held
     nothing while the worker registered services and told a tap after a
     refused worker init that Bluetooth had not started; one whose create
     was skipped, or a worker that ended without its store, held every tap
     at 503 "still starting" until a reboot; a finalize that skipped its
     `bringup_worker_finished();` answered a tap 200 "Advertising started"
     with nothing on air until a reboot, on every DEV device under
     `#if FEATURE_BLE`, which DEV sets to 0.) Not held, read only: that
     `loop()` reaches `ble_discovery_start_if_due()` and that it runs the
     finalize stage first; a worker or finalize stage that never gets to
     its end (a wait that never ends, a restart); a NimBLE init reached
     through a helper the finalize stage calls after its mark (the "last
     call" scan is lexical). That a start waits for the worker, over a
     stand-in whose advertising start walks the services, is
     `test_bluetooth_commands_wap.cpp`'s; the sketch itself is not compiled
     on the host, so this rule is what holds its marks.

BV8. Whether Bluetooth is on, as another task reads it (F210). The Start
     Advertising and Pair handlers decided whether to bring the stack up
     before they submit from it, and they read `is_enabled()`, the loop
     task's `g_settings.enabled`, in place on the httpd task while the loop
     task's commands and its first pass's `load_saved()` wrote it: a data
     race, which TSAN reports in the threaded test when the read is put
     back. They read `bluetooth_channel::read_enabled()` until F235, when C3
     stopped them deciding a bring-up from the setting (the stack being down
     decides it); since the F235 review they read it, through
     `waits_for_boot_bring_up()`, only to leave the stack to the sketch's
     boot bring-up on a device saved on, and `read_enabled()` stays any
     task's race-free read of the setting. In `bluetooth_channel.cpp`, `read_enabled()`
     is exactly `return read_settings().enabled;` (the settings the last pass
     published; BV3 adds it to the readers, which name none of the live
     state); `is_enabled()` is defined once, `static`, and called only from
     `run_command()` (the commands that auto-enable, on the loop task); no
     other file names `bluetooth_channel::is_enabled(`, and BV3 keeps it out
     of `bluetooth_channel.h`. What `read_enabled()` answers before the first
     pass, mid-command and after each command's answer, and the threads
     under TSAN, are `test_bluetooth_commands_wap.cpp`'s
     (`the_handlers_read_enabled_from_the_view`,
     `threads_bring_up_loop_and_reads`).

BV9. No pairing completes, and no bond stays, without the owner (F191,
     F209). A keyboard-only phone pairs by Passkey Entry (it types the six
     digits this device shows), and the digits reached the dashboard's PIN
     box and the health log in or out of pairing mode; a DisplayOnly or
     NoInputNoOutput phone pairs by Just Works, and NimBLE kept its
     unauthenticated bond, unlisted, in the 3-bond store, where three of them
     refused the owner. In `bluetooth_channel.cpp`: `PASSKEY_REFUSED` is
     defined once, at least 1000000 (past what NimBLE's
     `ble_sm_inject_io()` takes); `onPassKeyDisplay()` adds it to its six
     random digits, before it posts them, exactly when `if
     (!__atomic_load_n(&g_owner_pairing_open, __ATOMIC_ACQUIRE))`, and
     returns that passkey; `owner_pairing_open()` is exactly `return
     g_settings.enabled && g_settings.allow_pairing && (g_pairing.state ==
     PAIR_INITIATED || g_pairing.state == PAIR_PIN_DISPLAYED);`,
     `publish_views()` stores it into the gate once (release), and nothing
     else names the gate; `apply_passkey_display()` starts `if
     (e.u.passkey.pin >= PASSKEY_REFUSED || !owner_pairing_open()) {
     g_passkey_refused_count++; return; }`; no `log_health(` of the file
     names a pairing's digits (`pin`, `pin_*`, `*_pin`, `passkey*`,
     `numcmp`: the log is durable). `apply_auth_complete()` answers a bond
     that is not authenticated in one `} else if (link.bonded) {` branch,
     after the authenticated block, with `forget_bond(NimBLEAddress(
     link.id_address))` (the radio quiet, as Remove) and
     `g_server->disconnect(link.handle);`, calls no bare `deleteBond(` or
     `ble_gap_unpair*(`, and forgets nothing in the authenticated block. The
     bond store's count beside the list (F209): `refresh_bond_store()` is
     called only by `publish_status_view()`, once, and the counts
     (`g_bonds_held`, `g_bonds_unlisted`) are named only by those two. That
     a refused passkey is shown nowhere and an unauthenticated bond goes,
     over a stand-in that models the bond store and the busy guard, is
     `test_bluetooth_commands_wap.cpp`'s
     (`a_passkey_outside_pairing_mode_is_shown_nowhere`,
     `an_unauthenticated_bond_is_deleted_and_its_link_dropped`,
     `unlisted_bonds_no_longer_lock_the_owner_out`,
     `the_bond_store_shows_beside_the_list`); that NimBLE refuses the
     out-of-range passkey and times the pairing out is read from its source
     (2.3.8 and 2.5.0), not run.
     And the roads F191's and F209's review found (in
     `bluetooth_channel.cpp` unless named): `owner_comparison_open()` is
     exactly `return g_settings.enabled && g_settings.allow_pairing &&
     (g_pairing.state == PAIR_INITIATED || g_pairing.state ==
     PAIR_PIN_DISPLAYED || g_pairing.state == PAIR_CONFIRMING);`, and
     `apply_confirm_passkey()` answers no with `if
     (!owner_comparison_open()) { NimBLEDevice::injectConfirmPasskey(
     *e.u.passkey.conn, false); delete e.u.passkey.conn;
     g_passkey_refused_count++; return; }` before it takes the pending copy
     (a Numeric Comparison asked the owner with no pairing mode); the
     unauthenticated branch drops the link as `if (g_server) {
     g_server->disconnect(link.handle); }`, on no other condition and by
     no other handle, names no `g_pairing` (that bond was never the owner's
     pairing), says `drop_entries_without_bond();` and
     `keep_for_retry(link.id_address);`, and the authenticated block says
     `drop_kept(link.id_address);`; `refuse_unauthenticated_links()` skips
     exactly a link that is not the stack's, not bonded, authenticated, or
     whose bond is gone, and applies the rest as their report
     (`apply_auth_complete(link_event(BT_EV_AUTH_COMPLETE, live));`), and
     is called once by `reconcile_link()` and once by `adopt_init_result()`
     after its `set_owner(`; `retry_kept_bonds(now);` is `update()`'s,
     once, and `reconcile_link()` ends every retry (`g_kept_bond_count =
     0;`), the kept list named only by `keep_for_retry()`, `drop_kept()`,
     `retry_kept_bonds()` and `reconcile_link()`; `apply_store_full()` and
     `reconcile_link()` each mark the store dirty (`g_bond_store_dirty =
     true;`); `ble_server_dispatch.h`'s `onPassKeyDisplay()` with no
     pairing owner returns `1000000u + esp_random() % 1000000;` (past what
     NimBLE takes); and no file of the sketch calls `setSecurityPasskey(`:
     NimBLE-Arduino calls `onPassKeyDisplay()` only while the static
     passkey is its default 123456 (NimBLEServer.cpp, 2.3.8 and 2.5.0), so
     one would hand every Passkey Entry that key, in or out of pairing
     mode, and the stand-in does not model it. Held over the stand-in by
     `a_comparison_outside_pairing_mode_asks_nobody`,
     `a_just_works_bond_beside_the_recorded_link_drops_its_own`,
     `a_bond_whose_report_never_came_goes_from_the_stacks_record`,
     `a_bond_the_stack_kept_is_asked_again_each_minute`,
     `the_bond_count_follows_a_dropped_report_and_a_refused_pairing` and
     `a_listed_phone_that_pairs_again_by_just_works_leaves_the_list`.

BV10. Safe mode says so, and a late bring-up says what it left out (F259,
     F260). `canary_wap.ino`'s `setup()` says
     `bluetooth_channel::skip_boot_bring_up_for_safe_mode();` once, a
     statement of the `else` of its Bluetooth gate (`if (!in_safe_mode) {
     g_ble_discovery_ready = true; ...`) under `#if FEATURE_BLUETOOTH` alone
     (a safe-mode boot never marks its bring-up over, and a tap was told
     Bluetooth would start within five minutes); its `ble_bringup_task()`
     says `bluetooth_channel::boot_service_registered(
     bluetooth_channel::BT_BOOT_SVC_STATUS);` once, a statement of the block
     `if (ble_status::init(...)) {` runs, and `...BT_BOOT_SVC_DISCOVERY);`
     once, a statement of the block `if (ble_manager::init(...)) {` runs,
     under `#if FEATURE_BLUETOOTH` alone; no other file calls either. In
     `bluetooth_channel.cpp`: `skip_boot_bring_up_for_safe_mode()` and
     `boot_bring_up_skipped()` are exactly the release store and the acquire
     load of `g_boot_bring_up_skipped`, `boot_service_registered()` exactly
     `__atomic_fetch_or(&g_boot_services_registered, service,
     __ATOMIC_RELEASE);` and `late_bring_up()` exactly `return g_initialized
     && boot_bring_up_tried() && !g_boot_stack_up;`, and the three flags are
     named only by their own functions (`g_boot_services_registered` also by
     `boot_services_missing()`, `g_boot_stack_up` by C3's
     `boot_bring_up_finished()`); `BOOT_SERVICES` is built from the macros
     the worker's registrations compile under (`FEATURE_BLE_STATUS`,
     `FEATURE_BLE`); `note_late_bring_up()` is called once, by
     `adopt_init_result()` (the loop task adopts a stack once per boot), and
     registers or starts nothing (the worker's services on a server that
     already started are F167's case). What a late bring-up reports, the
     boot's own bring-up in either order of the finalize stage and the
     adoption, and a tap that waits in every window, are
     `test_bluetooth_commands_wap.cpp`'s
     (`a_late_bring_up_says_what_it_left_out`,
     `every_tap_waits_for_the_boot_bring_up`,
     `the_worker_never_meets_a_taps_bring_up`); the sketch is not compiled on
     the host, so this rule holds its marks.

## It proves it bites

Each run applies mutations to the sources in memory and requires the check
to fail on every one. A mutation whose anchor moved fails the run.

The mutations are independent, and each re-runs the whole check (about a
second apiece, over 300 of them): run one after another they took about five
minutes, which took the Regression Guards job from 0.2 to as much as 6.4
minutes against its 10-minute limit (audit, 2026-10-09). They now run in a
pool of forked worker processes, one per CPU (LOOP_CMD_JOBS=<n> sets the
count; LOOP_CMD_JOBS=1, or a platform without fork, runs them in this
process), and the problems come back in the mutations' own order, so the
output is the same either way.

Run locally:  python3 firmware/scripts/check_wap_loop_commands.py   (repo root)
CI:           firmware.yml "Regression guard", via regression_check.sh
"""

from __future__ import annotations

import functools
import multiprocessing
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor
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
SIG_OPEN = r"\bbool\s+open_client\s*\(\s*bool\s*\*\s*busy\s*\)"
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
    return list(_named_bodies(code))


@functools.lru_cache(maxsize=1024)
def _named_bodies(code: str) -> tuple[tuple[str, int, int], ...]:
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
    return tuple(out)


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
        at_open = s.find("open_client(&busy);")
        at_busy = s.find("if(busy)return;")
        at_served = s.find("s_reinit_served.store(wanted")
        ok = (0 <= first_guard < at_wanted_cmp and first_guard < at_detach < second_guard < at_capture
              < at_open < at_busy < at_served and s.count("open_client(") == 1)
        if not ok:
            errors.append(f"{MQTT_CPP}: serve_reinit() must wait for a retiring client without "
                          "blocking (`if (!retire_finished()) return;` first, and again after "
                          "detach_client()), then read `const uint32_t wanted = s_reinit_wanted.load(` "
                          "(a request made before the open's NVS read is the open's), run "
                          "open_client(&busy) once, return on `if (busy) return;` (a save was writing "
                          "the row: nothing opened, the request waits — F289's review), then "
                          "`s_reinit_served.store(wanted` (F106)")
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


@functools.lru_cache(maxsize=4096)
def names_csi_mqtt_namespace(c: str) -> bool:
    return re.search(r"\busing\s+namespace\s+csi_mqtt\b", c) is not None


def check_mqtt_sketch(ino: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    code = {name: blank_comments_and_strings(src) for name, src in files.items()}
    for name, c in code.items():
        if names_csi_mqtt_namespace(c):
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
    # F167: the bring-up's auto-advertise is the loop task's, when it takes
    # init()'s result (adopt_init_result()); init() calls neither.
    "enable": ("adopt_init_result", "set_settings"),
    "disable": ("set_settings",),
    # F143: the loop task's. The F167 review: a start held while the sketch's
    # bring-up worker registered services is made by bringup_worker_finished(),
    # which canary_wap.ino calls on the loop task (rule BV7).
    "start_advertising": ("adopt_init_result", "apply_disconnect", "start_pairing",
                          "bringup_worker_finished"),
    "stop_advertising": ("deinit", "disable"),
    "stop_scan": ("deinit", "disable", "handle_scan_timeout",
                  "quiet_radio"),   # F172 review: a bond's delete ends the owner's scan (loop task)
    "clear_scan_results": ("start_scan",),
    "cancel_pairing": ("update", "reject_pairing",
                       "disable"),   # F143 review: turning Bluetooth off ends a pairing first
    # (The inactivity timeout drops only the recorded link since the F171
    # review: every link was its reach on FULL, Opera's clients included.)
    "disconnect": ("deinit", "disable"),
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
# they submit (bring_up(): the init() enable() ran there before F111),
# whenever it is down, whatever the saved setting says (F235): each one's
# first statement is the guard. A command never runs init(), so without it a
# tap could never bring up a stack whose boot bring-up failed or was refused.
BT_BRING_UP_HANDLERS = ("handle_bluetooth_enable", "handle_bluetooth_advertise_start",
                        "handle_bluetooth_pair_start")
BT_BRING_UP_GUARD = "if(!bring_up()){returnsend_bring_up_refused(req,"
# The F235 review: Start Advertising and Pair leave the stack to the sketch's
# boot bring-up until it is over, on a device saved on (the sketch defers it
# out of the provisioning join window and skips it in safe mode, and the
# firmware's other NimBLE init callers run inside it: the worker's
# ble_manager::init() on FULL, the Scout's at finalize). Their first
# statement is this guard, then the bring-up guard.
# Sweep F259: Enable too, and whatever the saved setting (a device saved off
# brought the stack up at once, and Enable never waited); the settings POST
# waits in its "enabled": true branch (BT_SETTINGS_BOOT_WAIT, BV1).
BT_BOOT_WAIT_HANDLERS = ("handle_bluetooth_enable", "handle_bluetooth_advertise_start",
                         "handle_bluetooth_pair_start")
BT_BOOT_WAIT_GUARD = "if(waits_for_boot_bring_up()){returnsend_boot_bring_up_pending(req);}"
BT_BOOT_WAIT_CALLERS = BT_BOOT_WAIT_HANDLERS + ("handle_bluetooth_settings_set",)
SIG_WAITS_FOR_BOOT = r"\binline\s+bool\s+waits_for_boot_bring_up\s*\(\s*\)"
BT_WAITS_FOR_BOOT_BODY = "return!bluetooth_channel::is_initialized()&&!bluetooth_channel::boot_bring_up_tried();"
SIG_SEND_BOOT_PENDING = r"\binline\s+esp_err_t\s+send_boot_bring_up_pending\s*\([^)]*\)"
BT_BOOT_PENDING_BODY = ("httpd_resp_set_status(req,http_status_line(503));"
                        "returnsend_error(req,bluetooth_channel::boot_bring_up_skipped()"
                        "?bluetooth_channel::boot_bring_up_skipped_error()"
                        ":bluetooth_channel::boot_bring_up_started()"
                        "||bluetooth_channel::bring_up_starting()"
                        "?bluetooth_channel::bring_up_starting_error()"
                        ":bluetooth_channel::boot_bring_up_pending_error());")
SIG_BT_BOOT_TRIED = r"\bbool\s+boot_bring_up_tried\s*\(\s*\)"
BT_BOOT_TRIED_BODY = "return__atomic_load_n(&g_boot_bring_up_tried,__ATOMIC_ACQUIRE);"
BT_BOOT_TRIED_STORE = "__atomic_store_n(&g_boot_bring_up_tried,true,__ATOMIC_RELEASE);"
# Sweep F260: the end also records whether the boot bring-up left the stack
# up, before the release (a later bring-up is then a tap's).
BT_BOOT_FINISHED_BODY = "g_boot_stack_up=is_initialized();" + BT_BOOT_TRIED_STORE
SIG_BT_BOOT_FINISHED = r"\bvoid\s+boot_bring_up_finished\s*\(\s*\)"
# Wave 16's verify: the boot bring-up has started (bringup_worker_started(),
# the gate open), any task's read; from there until it is over a tap is told
# Bluetooth is still starting, whatever the worker's init() answered.
SIG_BT_BOOT_STARTED = r"\bbool\s+boot_bring_up_started\s*\(\s*\)"
BT_BOOT_STARTED_BODY = "return__atomic_load_n(&g_boot_bring_up_started,__ATOMIC_ACQUIRE);"
BT_BOOT_STARTED_STORE = "__atomic_store_n(&g_boot_bring_up_started,true,__ATOMIC_RELEASE);"
# Where a bring_up() guard may stand: the bring-up handlers and the settings
# POST turning Bluetooth on (BV1).
BT_BRING_UP_GUARDED = BT_BRING_UP_HANDLERS + ("handle_bluetooth_settings_set",)
# What a bring-up handler never reads (F235): the published setting or status.
# Whether Bluetooth is saved on says nothing about whether the stack is up.
BT_SETTING_READERS = ("read_enabled", "read_settings", "read_status")
SIG_SEND_BRING_UP_REFUSED = r"\binline\s+esp_err_t\s+send_bring_up_refused\s*\([^)]*\)"
SIG_BT_BRING_UP_STARTING = r"\bbool\s+bring_up_starting\s*\(\s*\)"
BT_BRING_UP_BODY = "returnbluetooth_channel::is_initialized()||bluetooth_channel::init();"
BT_BRING_UP_REFUSED_BODY = ("if(bluetooth_channel::bring_up_starting()){"
                            "httpd_resp_set_status(req,http_status_line(503));"
                            "returnsend_error(req,bluetooth_channel::bring_up_starting_error());}"
                            "returnsend_bt_error(req,fallback);")
BT_BRING_UP_STARTING_BODY = "returninit_running()||is_initialized();"
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
                                       "send_refusal_message", "seconds_left", "confirm_refusal_status",
                                       "confirm_refusal_error", "confirm_refusal_message",
                                       "vote_unsent_message"),
                      "bluetooth_channel": ()}
# Right after `const loop_command_ring::Wait w = <ns>::submit(...);`: every
# answer but kDone is a command that did not run.
NOT_RUN_GUARD = "if(w!=loop_command_ring::Wait::kDone)returnsend_not_run(req,w);"

SIG_CHANNEL_SUBMIT = r"\bloop_command_ring::Wait\s+submit\s*\([^)]*\)"
SIG_SEND_NOT_RUN = r"\besp_err_t\s+send_not_run\s*\([^)]*\)"
SIG_MUTE_ANSWER = r"\binline\s+esp_err_t\s+send_mute_answer\s*\([^)]*\)"
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
    errors.extend(channel_internal_findings(tag, ns, h_name, h_src, cpp_name, cpp_src, mutators,
                                            tuple(sorted(internal.items()))))


# Pure in its arguments, and most self-test mutations leave both of a
# channel's files as they were: answer those from the first run.
@functools.lru_cache(maxsize=1024)
def channel_internal_findings(tag: str, ns: str, h_name: str, h_src: str, cpp_name: str,
                              cpp_src: str, mutators: tuple[str, ...],
                              internal_items: tuple[tuple[str, tuple[str, ...]], ...]) -> tuple[str, ...]:
    internal = dict(internal_items)
    errors: list[str] = []
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
    return tuple(errors)


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
        if h in BT_BRING_UP_HANDLERS:
            wait = BT_BOOT_WAIT_GUARD if h in BT_BOOT_WAIT_HANDLERS else ""
            if not s.startswith(wait + BT_BRING_UP_GUARD):
                errors.append(f"{api}: {h}() must bring the stack up on its own task first, before "
                              "anything else, whenever it is down (" +
                              (f"`{wait}` first, leaving it to the sketch's boot bring-up until that "
                               "is over on a device saved on (F235's review), then " if wait else "") +
                              f"`{BT_BRING_UP_GUARD} ...`): no command runs init(), so without it a tap "
                              "never brings up a stack whose boot bring-up failed or was refused (F111, F235)")
            for reader in BT_SETTING_READERS:
                if re.search(r"(?<![\w:.>])" + ns + r"::" + reader + r"\s*\(", body):
                    errors.append(f"{api}: {h}() reads {ns}::{reader}() — whether Bluetooth is saved "
                                  "on says nothing about whether the stack is up: the stack being "
                                  "down decides its bring-up, whatever the setting (F235)")
    for h, fields in (("handle_chirp_settings", CHIRP_SETTINGS_FIELDS),
                      ("handle_bluetooth_settings_set", BT_SETTINGS_FIELDS)):
        if h not in spec:
            continue
        span = the_body(code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)", "", [])
        if span is not None:
            check_settings_blocks(api, h, code[span[0]:span[1]], kept[span[0]:span[1]], fields, errors)


# Another name for bluetooth_api (the second verify, wave 16).
BT_API_OTHER_NAME = (r"\bnamespace\s+\w+\s*=\s*(?:::\s*)?bluetooth_api\b"
                     r"|\busing\s+namespace\s+(?:::\s*)?bluetooth_api\b")


def check_bluetooth_bring_up(api_src: str, cpp_src: str, errors: list[str],
                             files: dict[str, str] | None = None) -> None:
    """Rule C3's bring-up part (F235 and its review): what a tap's bring-up
    runs, when Start Advertising and Pair leave it to the sketch's boot
    bring-up, how a bring-up that left the stack down is answered, and who
    may call bring_up()."""
    code = blank_comments_and_strings(api_src)
    body = body_of(code, SIG_BRING_UP, f"{BT_API}: bring_up()", errors)
    if body is not None and squash(body) != BT_BRING_UP_BODY:
        errors.append(f"{BT_API}: bring_up() must be exactly `return bluetooth_channel::is_initialized() || "
                      "bluetooth_channel::init();` — the stack being down decides it, and init() "
                      "re-checks the heap guard on every tap (F235)")
    body = body_of(code, SIG_SEND_BRING_UP_REFUSED, f"{BT_API}: send_bring_up_refused()", errors)
    if body is not None and squash(body) != BT_BRING_UP_REFUSED_BODY:
        errors.append(f"{BT_API}: send_bring_up_refused() must answer 503 with "
                      "bluetooth_channel::bring_up_starting_error() while "
                      "bluetooth_channel::bring_up_starting() says another task's bring-up runs, and "
                      "send_bt_error(req, fallback) (the refusal by name) otherwise, exactly (F235)")
    spans = named_bodies(code)
    # Every reference, not only a call (wave 16's verify): `&bring_up` taken
    # into a function pointer and called through it was not a `bring_up(`.
    for m in re.finditer(r"\bbluetooth_api\s*::\s*bring_up\b|(?<![\w:.>])bring_up\b", code):
        before = code[:m.start()].rstrip()
        if re.search(r"\bbool$", before) and not code[m.start():m.end()].startswith("bluetooth_api"):
            continue                                  # the definition
        where = enclosing_function(spans, m.start())
        if where not in BT_BRING_UP_GUARDED or not before.endswith("!") or \
                not squash(code[m.start():m.start() + 160]).startswith(
                    "bring_up()){returnsend_bring_up_refused(req,"):
            errors.append(f"{BT_API}: {where or 'file scope'}() names bring_up other than as a guard "
                          "answering `if (... !bring_up()) { return send_bring_up_refused(req, ...` in "
                          f"{', '.join(BT_BRING_UP_GUARDED)} — the NimBLE bring-up runs only where a tap "
                          "needs the stack, and a bring-up that left it down is answered by name, or as "
                          "still starting (F235)")
            break
    # Nothing else in the sketch calls it (the F235 review), qualified or not:
    # from loop() it is NimBLE's init on the loop task, past its watchdog.
    # Every call in every other file is looked at (a definition of a function
    # of that name is skipped, its calls are not).
    for path, src in (files or {}).items():
        if path == BT_API:
            continue
        other = blank_comments_and_strings(src)
        for hit in re.finditer(r"\bbluetooth_api\s*::\s*bring_up\b|(?<![\w:.>~])bring_up\b", other):
            if not other[hit.start():hit.end()].startswith("bluetooth_api") and \
                    re.search(r"\b(?:bool|void|int|static|inline)\s*$", other[:hit.start()]):
                continue                              # a definition, not a call
            where = enclosing_function(named_bodies(other), hit.start())
            errors.append(f"{path}: {where or 'file scope'}() names bring_up (a call, or a reference such "
                          "as a function pointer) — bluetooth_api.h's, called only in its bring-up handlers' "
                          "guards: anywhere else it is NimBLE's init on a task that never needed it (from "
                          "loop(), past the loop task's watchdog) (F235; references since wave 16's verify)")
            break
    # And nothing names the namespace another way (the second verify, wave
    # 16): through `namespace x = bluetooth_api;`, or a namespace saying
    # `using namespace bluetooth_api;`, bring_up is spelled `x::bring_up`,
    # which neither pattern above reads.
    named = [(BT_API, code)] + [(path, blank_comments_and_strings(src))
                                for path, src in (files or {}).items() if path != BT_API]
    for path, blanked in named:
        hit = re.search(BT_API_OTHER_NAME, blanked)
        if hit:
            where = enclosing_function(named_bodies(blanked), hit.start())
            errors.append(f"{path}: {where or 'file scope'}() says `{' '.join(hit.group(0).split())}` — a "
                          "namespace alias or using-directive for bluetooth_api spells its names `x::name`, "
                          "where this rule cannot see a bring_up; name them `bluetooth_api::` (F235; the "
                          "second verify, wave 16)")
    # Start Advertising and Pair leave the stack to the boot bring-up (the
    # F235 review).
    body = body_of(code, SIG_WAITS_FOR_BOOT, f"{BT_API}: waits_for_boot_bring_up()", errors)
    if body is not None and squash(body) != BT_WAITS_FOR_BOOT_BODY:
        errors.append(f"{BT_API}: waits_for_boot_bring_up() must be exactly `return "
                      "!bluetooth_channel::is_initialized() && !bluetooth_channel::boot_bring_up_tried();` "
                      "— the stack down and the sketch's boot bring-up not over (it defers it out of the "
                      "join window and skips it in safe mode), whatever the saved setting: the worker "
                      "runs init() either way (F235's review; F259 dropped read_enabled(), which let a "
                      "device saved off bring NimBLE up inside the window)")
    body = body_of(code, SIG_SEND_BOOT_PENDING, f"{BT_API}: send_boot_bring_up_pending()", errors)
    if body is not None and squash(body) != BT_BOOT_PENDING_BODY:
        errors.append(f"{BT_API}: send_boot_bring_up_pending() must answer 503 with "
                      "bluetooth_channel::boot_bring_up_skipped_error() when the sketch skipped its boot "
                      "bring-up (safe mode, F259), bluetooth_channel::bring_up_starting_error() once "
                      "boot_bring_up_started() says the sketch has started it or bring_up_starting() says a "
                      "bring-up runs, and boot_bring_up_pending_error() otherwise, exactly (F235's review; "
                      "wave 16's verify: after the worker's init() was refused a tap was told Bluetooth had "
                      "not started yet)")
    calls = [m for m in re.finditer(r"(?<![\w:.>])waits_for_boot_bring_up\s*\(", code)
             if not re.search(r"\bbool$", code[:m.start()].rstrip())]
    where = sorted(enclosing_function(spans, m.start()) or "file scope" for m in calls)
    if where != sorted(BT_BOOT_WAIT_CALLERS):
        errors.append(f"{BT_API}: waits_for_boot_bring_up() is called by {', '.join(where) or 'nothing'} — "
                      f"exactly once each by {', '.join(BT_BOOT_WAIT_HANDLERS)}, first, and by "
                      "handle_bluetooth_settings_set's \"enabled\": true branch, before its bring-up "
                      "(F235's review; F259)")
    cpp = blank_comments_and_strings(cpp_src)
    body = body_of(cpp, SIG_BT_BOOT_TRIED, f"{BT_CPP}: boot_bring_up_tried()", errors)
    if body is not None and squash(body) != BT_BOOT_TRIED_BODY:
        errors.append(f"{BT_CPP}: boot_bring_up_tried() must be exactly `return "
                      "__atomic_load_n(&g_boot_bring_up_tried, __ATOMIC_ACQUIRE);` — any task asks it "
                      "(F235's review)")
    body = body_of(cpp, SIG_BT_BOOT_FINISHED, f"{BT_CPP}: boot_bring_up_finished()", errors)
    if body is not None and squash(body) != BT_BOOT_FINISHED_BODY:
        errors.append(f"{BT_CPP}: boot_bring_up_finished() must be exactly `g_boot_stack_up = "
                      "is_initialized(); __atomic_store_n(&g_boot_bring_up_tried, true, __ATOMIC_RELEASE);` "
                      "— whether the boot left the stack up (a later bring-up is a tap's, F260), then the "
                      "release a tap's acquire in boot_bring_up_tried() pairs with (F235's review)")
    body = body_of(cpp, SIG_BT_BOOT_STARTED, f"{BT_CPP}: boot_bring_up_started()", errors)
    if body is not None and squash(body) != BT_BOOT_STARTED_BODY:
        errors.append(f"{BT_CPP}: boot_bring_up_started() must be exactly `return "
                      "__atomic_load_n(&g_boot_bring_up_started, __ATOMIC_ACQUIRE);` — any task asks it "
                      "(wave 16's verify)")
    cspans = named_bodies(cpp)
    for m in re.finditer(r"\bg_boot_bring_up_started\b", cpp):
        where = enclosing_function(cspans, m.start())
        if where not in (None, "bringup_worker_started", "boot_bring_up_started"):
            errors.append(f"{BT_CPP}: {where}() names g_boot_bring_up_started — set only by "
                          "bringup_worker_started() (its release store, BV7), as the sketch opens the gate, "
                          "read only by boot_bring_up_started(); never cleared (wave 16's verify)")
            break
    for m in re.finditer(r"\bg_boot_bring_up_tried\b", cpp):
        where = enclosing_function(cspans, m.start())
        if where not in (None, "boot_bring_up_finished", "boot_bring_up_tried"):
            errors.append(f"{BT_CPP}: {where}() names g_boot_bring_up_tried — set only by "
                          "boot_bring_up_finished(), when the sketch says its boot bring-up is over (BV7), "
                          "read only by boot_bring_up_tried() (F235's review)")
            break
    body = body_of(cpp, SIG_BT_BRING_UP_STARTING, f"{BT_CPP}: bring_up_starting()", errors)
    if body is not None and squash(body) != BT_BRING_UP_STARTING_BODY:
        errors.append(f"{BT_CPP}: bring_up_starting() must be exactly `return init_running() || "
                      "is_initialized();` — the latch first: init() publishes the stack before it "
                      "clears the latch, so read the other way a bring-up that came up between the "
                      "two reads answers its predecessor's refusal (F235)")


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
        if ns == "bluetooth_channel":
            check_bluetooth_bring_up(files[api], files[cpp], errors, files)
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
                      "get_detail_text", "get_validation_status", "cannot_send_reason", "seconds_left")
# What the readers may not name: the live state and its readers (rule CV4).
CHIRP_LIVE_STATE = ("g_state", "g_session", "g_cooldown", "g_recent_chirps", "g_recent_chirp_count",
                    "g_nearby_devices", "g_nearby_count", "g_muted", "g_mute_until_ms", "g_relay_enabled",
                    "g_urgency_filter", "g_session_start_ms", "g_last_chirp_sent_ms", "g_view_tables",
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
    (r"\bg_view_tables\b", ("publish_view", "init"), "g_view_tables",
     "init() allocates the tables' PSRAM block and attaches their copies to it; publish_view() "
     "alone builds in its scratch"),
    (r"\bg_nearby_view\s*\.\s*attach\s*\(", ("init",), "g_nearby_view.attach(",
     "init() attaches the published copy to the PSRAM block, once it is allocated"),
    (r"\bg_recent_view\s*\.\s*attach\s*\(", ("init",), "g_recent_view.attach(",
     "init() attaches the published copy to the PSRAM block, once it is allocated"),
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
                     "recent_index_of", "drop_recent_at",
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
    # One nonce, one row (F270), and a suppressed chirp out at its vote (F269).
    "recent_index_of": ("handle_witness", "handle_suppress_vote"),
    "drop_recent_at": ("handle_suppress_vote",),
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
# How init() ends (rules CV3, CV4): the tables' copies attached to their
# PSRAM block, then the first view published; and the block's allocation.
CHIRP_INIT_TAIL = ("g_nearby_view.attach(&g_view_tables->nearby);g_recent_view.attach(&g_view_tables->recent);"
                   "g_tables_changed=true;publish_view();returntrue;")
CHIRP_BLOCK_ALLOC = "g_view_tables=(ViewTables*)csi_large_calloc(sizeof(ViewTables));"
CHIRP_READER_SIGS = {
    "read_status": r"\bvoid\s+read_status\s*\([^)]*\)",
    "read_nearby": r"\bvoid\s+read_nearby\s*\([^)]*\)",
    "read_recent": r"\bvoid\s+read_recent\s*\([^)]*\)",
    "cannot_send_reason": r"\bconst\s+char\s*\*\s*cannot_send_reason\s*\([^)]*\)",
}
CHIRP_READER_VIEW = {"read_status": "g_status_view.read(", "read_nearby": "g_nearby_view.read(",
                     "read_recent": "g_recent_view.read("}
# Rule CV8 (F178): the only functions in chirp_channel.cpp that may name
# CHIRP_COOLDOWN. The send cooldown is a timer; this is the state it reads as.
CHIRP_COOLDOWN_NAMERS = ("shown_state", "state_name")
# Rule CV9 (F174): how a confirm and a dismiss answer once their command ran.
CHIRP_REFUSAL_ANSWER = (
    "httpd_resp_set_status(req,http_status_line(chirp_channel::confirm_refusal_status(r.confirm_refusal)));"
    'doc["error"]=chirp_channel::confirm_refusal_error(r.confirm_refusal);'
    'doc["message"]=chirp_channel::confirm_refusal_message(r.confirm_refusal);'
)
CHIRP_ANSWER_TAIL = ('charbuffer[256];serializeJson(doc,buffer);httpd_resp_set_type(req,"application/json");'
                     'httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");returnhttpd_resp_sendstr(req,buffer);')
CHIRP_CONFIRM_ANSWER = ('JsonDocumentdoc;doc["success"]=r.ok;if(!r.ok){' + CHIRP_REFUSAL_ANSWER + '}'
                        + CHIRP_ANSWER_TAIL)
CHIRP_DISMISS_ANSWER = ('JsonDocumentdoc;doc["success"]=r.ok;if(!r.ok){' + CHIRP_REFUSAL_ANSWER + '}'
                        'else{doc["vote_sent"]=r.vote_sent;if(!r.vote_sent){'
                        'doc["vote_error"]=chirp_channel::confirm_refusal_error(r.confirm_refusal);'
                        'doc["message"]=chirp_channel::vote_unsent_message(r.confirm_refusal);}}'
                        + CHIRP_ANSWER_TAIL)
CHIRP_ANSWERS = {"send_confirm_answer": CHIRP_CONFIRM_ANSWER, "send_dismiss_answer": CHIRP_DISMISS_ANSWER}
# Where each route answers through them: right after its not-run guard.
CHIRP_ANSWERED_BY = {
    "handle_chirp_confirm": "returnsend_confirm_answer(req,r);",
    "handle_chirp_dismiss": "returnsend_dismiss_answer(req,r);",
    "handle_chirp_ack": "if(cmd.type==chirp_channel::CHIRP_CMD_CONFIRM)returnsend_confirm_answer(req,r);"
                        "returnsend_dismiss_answer(req,r);}",
}
# Rule CV12 (F192): how a mute and an unmute answer once their command ran,
# and where the two routes answer through it.
CHIRP_MUTE_ANSWER = (
    'JsonDocumentdoc;doc["success"]=r.ok;if(!r.ok){'
    "constintstatus=chirp_channel::mute_refusal_status(r.mute_refusal);"
    "if(status!=200)httpd_resp_set_status(req,http_status_line(status));"
    'doc["error"]=chirp_channel::mute_refusal_error(r.mute_refusal);'
    'doc["message"]=chirp_channel::mute_refusal_message(r.mute_refusal);}'
    'charbuffer[160];serializeJson(doc,buffer);httpd_resp_set_type(req,"application/json");'
    'httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");returnhttpd_resp_sendstr(req,buffer);'
)
CHIRP_MUTE_ANSWERED_BY = {
    "handle_chirp_mute": "returnsend_mute_answer(req,r);",
    "handle_chirp_unmute": "returnsend_mute_answer(req,r);",
}
# Rule CV10 (F174): the least a buffer that holds an answer with a message
# may be. The mute route's refusal is 101 bytes, the send not-run answer's
# 97; the confirm and dismiss answers (a dismiss whose vote waits for the
# clock is 161 bytes, test_chirp_commands_wap.cpp) are held at 256 by CV9.
CHIRP_MESSAGE_BUFFER = 160
# Rule CV11 (F174, F178): the dashboard glue, squashed, in each function.
CHIRP_DASHBOARD_GLUE = {
    "refreshChirpStatus": ("constgate=WebUiLogic.chirpSendGate(data);",
                           "document.getElementById('chirpSendBtn').disabled=gate.sendDisabled;"),
    "confirmChirp": ("constdata=awaitapi('/api/chirp/confirm','POST',{nonce});",
                     "document.getElementById('chirpActionNote').textContent=WebUiLogic.chirpActionNote(data);"),
    "dismissChirp": ("constdata=awaitapi('/api/chirp/dismiss','POST',{nonce});",
                     "document.getElementById('chirpActionNote').textContent=WebUiLogic.chirpActionNote(data);"),
    "muteChirps": ("constdata=awaitapi('/api/chirp/mute','POST',{duration_minutes:mins});",
                   "document.getElementById('chirpActionNote').textContent=WebUiLogic.chirpActionNote(data);"),
    "unmuteChirps": ("constdata=awaitapi('/api/chirp/unmute','POST');",
                     "document.getElementById('chirpActionNote').textContent=WebUiLogic.chirpActionNote(data);"),
}
# Rule CV7: what each GET key is set from (squashed right-hand sides), once.
CHIRP_STATUS_FIELDS = {
    "state": "chirp_channel::state_name(v.state)",
    "session_emoji": "v.session_emoji",
    "nearby_count": "v.nearby_count",
    "recent_chirps": "v.recent_chirp_count",
    "last_chirp_sent_ms": "v.last_chirp_sent_ms",
    # Rounded up (F178): a cooldown that still runs never reads 0 s.
    "cooldown_remaining_sec": "chirp_channel::seconds_left(v.cooldown_remaining_ms)",
    "cooldown_tier": "v.cooldown_tier",
    "presence_met": "v.presence_met",
    "night_mode": "v.night_mode",
    "relay_enabled": "v.relay_enabled",
    "muted": "v.muted",
    "mute_remaining_sec": "v.mute_remaining_ms/1000",
    "can_send": "v.can_send",
    "cannot_send_reason": "why",
}
CHIRP_NEARBY_FIELDS = {
    "count": "count",
    "emoji": "devices[i].emoji",
    "age_sec": "(millis()-devices[i].last_seen_ms)/1000",
    "rssi": "devices[i].rssi",
    "listening": "devices[i].listening",
}
CHIRP_RECENT_FIELDS = {
    "emoji": "chirps[i].sender_emoji",
    "template_id": "(uint8_t)chirps[i].template_id",
    "template_text": "chirp_channel::get_template_text(chirps[i].template_id)",
    "detail": "chirp_channel::get_detail_text(chirps[i].detail)",
    "category": "chirp_channel::category_name(cat)",
    "urgency": "chirp_channel::urgency_name(chirps[i].urgency)",
    "hop_count": "chirps[i].hop_count",
    "age_sec": "(millis()-chirps[i].received_ms)/1000",
    "confirm_count": "chirps[i].confirm_count",
    "validated": "chirps[i].validated",
    "status": "chirp_channel::get_validation_status(&chirps[i])",
    "relayed": "chirps[i].relayed",
    "suppressed": "chirps[i].suppressed",
    "nonce": "nonce_hex",
}
CHIRP_ROUTE_FIELDS = {"handle_chirp_status": CHIRP_STATUS_FIELDS, "handle_chirp_nearby": CHIRP_NEARBY_FIELDS,
                      "handle_chirp_recent": CHIRP_RECENT_FIELDS}
# Statements each GET handler must hold once (squashed, strings kept): where
# the rows and the reason come from.
CHIRP_ROUTE_SHAPE = {
    "handle_chirp_status": (
        'constchar*why=chirp_channel::cannot_send_reason(v);if(why!=nullptr){doc["cannot_send_reason"]=why;}',
    ),
    "handle_chirp_nearby": (
        "chirp_channel::read_nearby(t);constsize_tcount=t->count;"
        "constchirp_channel::NearbyView*devices=t->devices;",
        "for(size_ti=0;i<count&&i<chirp_channel::MAX_NEARBY_CACHE;i++){JsonObjectdev=arr.add<JsonObject>();",
    ),
    "handle_chirp_recent": (
        "chirp_channel::read_recent(t);constsize_tcount=t->count;"
        "constchirp_channel::RecentView*chirps=t->chirps;",
        "for(size_ti=0;i<count;i++){if(chirps[i].dismissed)continue;JsonObjectc=arr.add<JsonObject>();",
        "chirp_channel::ChirpCategorycat=(chirp_channel::ChirpCategory)((uint8_t)chirps[i].template_id>>4);",
        'charnonce_hex[17];for(intj=0;j<8;j++){snprintf(nonce_hex+j*2,3,"%02x",chirps[i].nonce[j]);}',
    ),
}
# Where the nearby and recent handlers free their copy: right after the
# serialize, the doc holding pointers into it until then.
CHIRP_ROUTE_FREE = {"handle_chirp_nearby": "serializeJson(doc,buffer,needed);free(t);",
                    "handle_chirp_recent": "serializeJson(doc,buffer,needed);free(t);"}
CHIRP_FREE_EARLY = ('free(t);httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Memoryallocationfailed");'
                    "returnESP_FAIL;}")
# How POST /api/chirp/send answers once its command ran (F146), from
# `bool success = r.ok;` to its buffer.
CHIRP_SEND_ANSWER = (
    'boolsuccess=r.ok;JsonDocumentdoc;doc["success"]=success;'
    'if(success){doc["template_text"]=chirp_channel::get_template_text(template_id);'
    'doc["cooldown_tier"]=r.cooldown_tier;}'
    'elseif(r.refusal!=chirp_channel::SEND_REFUSED_NONE){'
    'doc["error"]=chirp_channel::send_refusal_error(r.refusal);'
    'doc["message"]=chirp_channel::send_refusal_message(r.refusal);'
    'if(r.refusal==chirp_channel::SEND_REFUSED_COOLDOWN){'
    'doc["cooldown_remaining_sec"]=chirp_channel::seconds_left(r.cooldown_remaining_ms);'
    'doc["cooldown_tier"]=r.cooldown_tier;}}'
    "charbuffer[384];"
)


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
    """Rules CV1-CV11: the Chirp GET routes read only what the loop task published, the
    send cooldown is a timer, a refused confirm says why, no answer outgrows its buffer, and
    the dashboard shows what the routes say."""
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
    # CV7: what the routes answer under each key.
    check_chirp_route_answers(files[CHIRP_API], errors)
    # CV9, CV10: the confirm and dismiss answers, and the answer buffers.
    check_chirp_confirm_answers(files[CHIRP_API], errors)
    # CV11: the dashboard shows them.
    ui = files.get(f"{SKETCH}/web_ui.h")
    if ui is None:
        errors.append(f"{SKETCH}/web_ui.h: missing (rule CV11)")
    else:
        for fn, needs in CHIRP_DASHBOARD_GLUE.items():
            body = js_function_body(ui, fn)
            if body is None:
                errors.append(f"{SKETCH}/web_ui.h: the dashboard's {fn}() is not where rule CV11 reads it")
                continue
            for need in needs:
                if squash(body).count(need) != 1:
                    errors.append(f"{SKETCH}/web_ui.h: {fn}() must run `{need}` once — the Chirp card's Send "
                                  "follows chirpSendGate() (off for any can_send that is not true, F178), and "
                                  "a confirm, a dismiss (F174), a mute or an unmute (F192) shows "
                                  "chirpActionNote() of its answer")
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
    if init is not None and not squash(init).endswith(CHIRP_INIT_TAIL):
        errors.append(f"{CHIRP_CPP}: init() must end `g_nearby_view.attach(&g_view_tables->nearby); "
                      "g_recent_view.attach(&g_view_tables->recent); g_tables_changed = true; "
                      "publish_view(); return true;` — the HTTP server can answer before loop() runs "
                      "a pass, and the first view carries the settings init() loaded (F138)")
    if init is not None and squash(init).count(CHIRP_BLOCK_ALLOC) != 1:
        errors.append(f"{CHIRP_CPP}: init() must allocate the tables' block once with "
                      "`g_view_tables = (ViewTables*)csi_large_calloc(sizeof(ViewTables));` — their "
                      "published copies belong in PSRAM, not the internal heap the BLE stack needs "
                      "(F138)")
    decls = re.findall(r"\bstatic\s+loop_snapshot::(\w+)\s*<\s*(NearbyTable|RecentTable)\b", code)
    if sorted(t for _k, t in decls) != ["NearbyTable", "RecentTable"] or \
            any(k != "AttachedValue" for k, _t in decls):
        errors.append(f"{CHIRP_CPP}: the nearby and recent tables' views must each be one static "
                      "`loop_snapshot::AttachedValue<...>` — a `Value<>` keeps its 1-KB copy in the "
                      "object, internal SRAM, which the PSRAM diet reclaimed for the BLE stack (F138)")
    pub = body_of(code, SIG_CHIRP_PUBLISH, f"{CHIRP_CPP}: publish_view()", errors)
    if pub is not None:
        s = squash(pub)
        guard = re.search(r"if\(!g_tables_changed(?:\|\|[^)]*)?\)return;", s)
        if guard is None or not s[guard.end():].startswith("g_tables_changed=false;") or \
                s.count("g_tables_changed=false;") != 1:
            errors.append(f"{CHIRP_CPP}: publish_view() must clear the tables' flag right after its "
                          "`if (!g_tables_changed || ...) return;`, once — a pass nothing marked "
                          "builds no table and reads no PSRAM; with the flag left set every pass "
                          "rebuilds and compares both tables (F138)")
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
    # CV8: the send cooldown is a timer (F178).
    for m in re.finditer(r"\bCHIRP_COOLDOWN\b", code):
        where = enclosing_function(spans, m.start())
        if where not in CHIRP_COOLDOWN_NAMERS:
            errors.append(f"{CHIRP_CPP}: {where or 'file scope'} names CHIRP_COOLDOWN — the send cooldown "
                          "is a timer (get_cooldown_remaining_ms(), cooldown_left_ms()), not a state: a "
                          "stored CHIRP_COOLDOWN is one a mute overwrites, and a gate on it refuses a "
                          f"send after the timer ran out; only {', '.join(CHIRP_COOLDOWN_NAMERS)} name it "
                          "(F178)")
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


def check_chirp_confirm_answers(api_src: str, errors: list[str]) -> None:
    """Rules CV9, CV10, CV12: a refused confirm says why, a dismiss whether its
    vote went out, no answer with a message outgrows its buffer (F174), and a
    refused mute or unmute says why (F192)."""
    code = blank_comments_and_strings(api_src)
    kept = blank_comments_only(api_src)
    span = the_body(code, r"\binline\s+esp_err_t\s+send_mute_answer"
                    r"\s*\(\s*httpd_req_t\s*\*\s*req\s*,\s*const\s+chirp_channel::Result\s*&\s*r\s*\)",
                    f"{CHIRP_API}: send_mute_answer(httpd_req_t* req, const chirp_channel::Result& r)", errors)
    if span is not None and squash(kept[span[0]:span[1]]) != CHIRP_MUTE_ANSWER:
        errors.append(f"{CHIRP_API}: send_mute_answer() must answer exactly as rule CV12 says — a refusal's "
                      "status (409 the channel off), error and message from the host-tested lookups of "
                      "r.mute_refusal (F192: a mute on a channel that was off turned it on with no session)")
    for h, tail in CHIRP_MUTE_ANSWERED_BY.items():
        span = the_body(code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                        f"{CHIRP_API}: {h}()", errors)
        if span is None:
            continue
        body = squash(code[span[0]:span[1]])
        at = body.find(NOT_RUN_GUARD)
        if at < 0 or body[at + len(NOT_RUN_GUARD):] != tail or body.count("send_mute_answer(") != 1:
            errors.append(f"{CHIRP_API}: {h}() must answer right after its not-run guard with `{tail}` — "
                          "the refusal by name and status from the Result (F192: the mute answered every "
                          "refusal invalid_duration, the unmute success whatever happened)")
    for fn, want in CHIRP_ANSWERS.items():
        span = the_body(code, r"\binline\s+esp_err_t\s+" + fn +
                        r"\s*\(\s*httpd_req_t\s*\*\s*req\s*,\s*const\s+chirp_channel::Result\s*&\s*r\s*\)",
                        f"{CHIRP_API}: {fn}(httpd_req_t* req, const chirp_channel::Result& r)", errors)
        if span is None:
            continue
        if squash(kept[span[0]:span[1]]) != want:
            errors.append(f"{CHIRP_API}: {fn}() must answer exactly as rule CV9 says — the status, error "
                          "and message from the host-tested lookups of r.confirm_refusal (404 not found, "
                          "409 the rest; never 403, which the dashboard reads as a bad token), a dismissed "
                          "chirp's vote_sent and why its vote stayed home (F174)")
    for h, tail in CHIRP_ANSWERED_BY.items():
        span = the_body(code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                        f"{CHIRP_API}: {h}()", errors)
        if span is None:
            continue
        body = squash(code[span[0]:span[1]])
        at = body.find(NOT_RUN_GUARD)
        after = body[at + len(NOT_RUN_GUARD):]
        if at < 0 or not (after == tail if h != "handle_chirp_ack" else after.startswith(tail)) or \
                body.count("send_confirm_answer(") + body.count("send_dismiss_answer(") != tail.count("_answer("):
            errors.append(f"{CHIRP_API}: {h}() must answer right after its not-run guard with `{tail.rstrip('}')}` — "
                          "the refusal by name and status from the Result (F174: every refused confirm "
                          "answered not_found, the ack a bare success:false, a dismiss nothing of its vote)")
    for name, start, end in named_bodies(code):
        body = kept[start:end]
        if '["message"]' not in body.replace(" ", ""):
            continue
        for m in re.finditer(r"\bchar\s+buffer\s*\[\s*(\d+)\s*\]", body):
            if int(m.group(1)) < CHIRP_MESSAGE_BUFFER:
                errors.append(f"{CHIRP_API}: {name}() answers a message from a {m.group(1)}-byte buffer — "
                              f"at least {CHIRP_MESSAGE_BUFFER}: serializeJson() leaves a full char array "
                              "unterminated and httpd_resp_sendstr() sends the stack after it (F174)")


def check_chirp_route_answers(api_src: str, errors: list[str]) -> None:
    """Rule CV7: what the Chirp routes answer under each key, and when the GET
    routes free the copy they read (chirp_api.h is not host-compiled)."""
    code = blank_comments_and_strings(api_src)
    kept = blank_comments_only(api_src)
    for h, fields in CHIRP_ROUTE_FIELDS.items():
        span = the_body(code, r"\besp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                        f"{CHIRP_API}: {h}()", errors)
        if span is None:
            continue
        body = squash(kept[span[0]:span[1]])
        sets: dict[str, list[str]] = {}
        for _obj, key, rhs in re.findall(r'\b(\w+)\["(\w+)"\]=(?!=)([^;]*);', body):
            sets.setdefault(key, []).append(rhs)
        for key, rhs in fields.items():
            if sets.get(key) != [rhs]:
                errors.append(f"{CHIRP_API}: {h}() must set \"{key}\" once, from `{rhs}` (found "
                              f"{sets.get(key, [])}) — the route answers the copy's own field "
                              "(F138, F146)")
        for key in sorted(set(sets) - set(fields)):
            errors.append(f"{CHIRP_API}: {h}() sets \"{key}\", which this check does not know (F138)")
        for shape in CHIRP_ROUTE_SHAPE[h]:
            if body.count(shape) != 1:
                errors.append(f"{CHIRP_API}: {h}() must hold `{shape}` once — where its rows and its "
                              "reason come from (F138, F146)")
        if h not in CHIRP_ROUTE_FREE:
            continue
        read = body.find("chirp_channel::read_")
        after = body[read:] if read >= 0 else body
        if after.count(CHIRP_ROUTE_FREE[h]) != 1:
            errors.append(f"{CHIRP_API}: {h}() must free its copy right after it serializes "
                          f"(`{CHIRP_ROUTE_FREE[h]}`) — ArduinoJson keeps the copy's const char "
                          "arrays (an emoji) by pointer until then (F138)")
        ser = after.find("serializeJson(")
        for m in re.finditer(r"free\(t\);", after):
            if m.start() < ser and not after.startswith(CHIRP_FREE_EARLY, m.start()):
                errors.append(f"{CHIRP_API}: {h}() frees its copy before it serializes, outside an "
                              "allocation failure's early return — the doc still points into it (F138)")
        for m in re.finditer(r"\breturn\b", after):
            if "free(t);" not in after[:m.start()]:
                errors.append(f"{CHIRP_API}: {h}() returns without freeing its copy (F138)")
                break
    span = the_body(code, r"\besp_err_t\s+handle_chirp_send\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                    f"{CHIRP_API}: handle_chirp_send()", errors)
    if span is not None:
        body = squash(kept[span[0]:span[1]])
        at = body.find("boolsuccess=r.ok;")
        tail = body[at:] if at >= 0 else ""
        if not tail.startswith(CHIRP_SEND_ANSWER):
            errors.append(f"{CHIRP_API}: handle_chirp_send() must answer its command as "
                          "CHIRP_SEND_ANSWER says — a refusal's error and message from "
                          "send_refusal_error()/send_refusal_message() (a clock not set is "
                          "clock_unsynced, not a cooldown with 0 seconds left), the cooldown's fields "
                          "only for a cooldown (F146)")


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
BV1_BRING_UP = ("if((cmd.set_mask&bluetooth_channel::BT_SET_ENABLED)&&settings.enabled&&!bring_up())"
                "{returnsend_bring_up_refused(req,")
# Sweep F259: and right before it, the wait for the boot bring-up, as Enable's.
BT_SETTINGS_BOOT_WAIT = ("if((cmd.set_mask&bluetooth_channel::BT_SET_ENABLED)&&settings.enabled"
                         "&&waits_for_boot_bring_up()){returnsend_boot_bring_up_pending(req);}")
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
        if s.count(BT_SETTINGS_BOOT_WAIT) != 1 or s.find(BT_SETTINGS_BOOT_WAIT + BV1_BRING_UP) < 0:
            errors.append(f"{BT_API}: handle_bluetooth_settings_set() must leave the stack to the sketch's "
                          f"boot bring-up first when the POST turns Bluetooth on (`{BT_SETTINGS_BOOT_WAIT}` "
                          "right before its bring-up): it brought NimBLE up inside the join window, in safe "
                          "mode and beside the boot worker's init() (F259)")
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
                "onConfirmPassKey", "onWrite", "onRead", "onResult", "onScanEnd",
                "onStoreStatus")   # F189: the bond store's status, on the NimBLE host task
BT_EVENT_HELPERS = ("make_event", "link_event", "post_event")
BT_CALLBACK_CALLS = BT_EVENT_HELPERS + ("detect_device_type",)
BT_EVENT_APPLIERS = ("apply_connect", "apply_disconnect", "apply_auth_complete",
                     "apply_passkey_display", "apply_confirm_passkey", "apply_scan_result",
                     "apply_scan_end", "apply_activity", "apply_store_full", "apply_store_overflow")
# Who applies each event: apply_event(), the consume's runner; and, for a
# link's start and end, reconcile_link() (F169: after a link's event was
# dropped, the loop task asks the stack and applies what the lost event
# would have).
BT_EVENT_APPLIER_CALLERS = {"apply_connect": ("apply_event", "reconcile_link"),
                            "apply_disconnect": ("apply_event", "reconcile_link"),
                            # F191's review: a Just Works bond whose report never
                            # came, refused from the stack's record (BV9)
                            "apply_auth_complete": ("apply_event", "refuse_unauthenticated_links")}
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
BT_CALLBACK_GLOBALS = {"g_events": ("post_event",), "g_data_callback": ("onWrite",),
                       # F191: the passkey gate the loop task publishes (BV9)
                       "g_owner_pairing_open": ("onPassKeyDisplay",)}
# What a callback hands the loop task, by kind: GATT activity and scan
# results are posted under EVENT_LOSSY_LIMIT (a burst of them never takes
# the room a link's events are kept), a link's own at the full limit.
# The bond store's refusals (F189) too: a stranger's repeated pairing
# attempts must not take the room kept for a link's own events.
BT_LOSSY_CALLBACKS = ("onWrite", "onRead", "onResult", "onStoreStatus")
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
            (r"(?<![\w:.>])apply_event\s*\(", (), "apply_event("),
            (r"(?<![\w:.>])reconcile_link\s*\(", ("update",), "reconcile_link(")) + tuple(
            (r"(?<![\w:.>])" + a + r"\s*\(", BT_EVENT_APPLIER_CALLERS.get(a, ("apply_event",)), a + "(")
            for a in BT_EVENT_APPLIERS):
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
        reconcile = "reconcile_link();"
        if sq.count(reconcile) != 1 or not (sq.find(consume) < sq.find(reconcile) < sq.find(drain)):
            errors.append(f"{BT_CPP}: update() must call `{reconcile}` once, right after `{consume}` "
                          f"and before `{drain}` — a link event the full queue dropped is "
                          "reconciled with the stack before a command reads the connection (F169)")


# BV3 (F138): the status routes' reads.
BT_LIVE_READERS = ("get_status", "get_state", "get_settings", "get_scanned_devices",
                   "get_paired_devices", "is_scanning", "is_connected", "get_connection_info",
                   "get_pairing_state", "get_pairing_pin", "is_enabled")
BT_GET_ROUTES = (("handle_bluetooth_status", "read_status"),
                 ("handle_bluetooth_scan_results", "read_scan"),
                 ("handle_bluetooth_paired_list", "read_paired"),
                 ("handle_bluetooth_settings_get", "read_settings"))
BT_GET_PURE = ("state_name", "format_address", "estimate_distance_m", "distance_label",
               "device_type_name", "security_level_name", "pairing_state_name", "init_fail_reason",
               "boot_service_name")   # F260: a BootService bit's name
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
BT_READERS = ("read_status", "read_settings", "read_scan", "read_paired", "read_enabled")
# The JSON each GET route sends: `<object>.<key>` for every literal key it
# sets (doc is the top level; conn, pair, stats, store and dev are its objects).
BT_GET_KEYS = {
    "handle_bluetooth_status": {
        "doc": ("state", "enabled", "advertising", "init_fail_reason", "scanning", "connected",
                "device_name", "local_address", "tx_power", "mtu", "battery_pct", "paired_count",
                "scanned_count", "bond_store", "late_bring_up", "services_missing", "connection",
                "pairing", "stats"),
        "store": ("held", "capacity", "unlisted"),   # F209: NimBLE's bond store beside the list
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


@functools.lru_cache(maxsize=1024)
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
                          "read_paired, read_enabled) (F138, F210)")
    live_call = r"\bbluetooth_channel::(" + "|".join(BT_LIVE_READERS) + r")\s*\("
    for name, src in files.items():
        c = blank_comments_and_strings(src)
        if "bluetooth_channel::" not in c:
            continue
        for hname, s, e in handler_spans(c):
            m = re.search(live_call, c[s:e])
            if m:
                errors.append(f"{name}: HTTP handler {hname}() reads bluetooth_channel::{m.group(1)}( — "
                              "read the view the loop task published (F138, F210)")
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


# BD1 (F171): the server's callbacks go through the one dispatcher.
BT_DISPATCH_H = f"{SKETCH}/ble_server_dispatch.h"
BLE_OPERA_H = f"{SKETCH}/ble_opera.h"
SIG_BT_INIT = r"\bbool\s+init\s*\(\s*\)"
SIG_OPERA_INIT = r"\bstatic\s+bool\s+init\s*\(\s*const\s+char\s*\*\s*deviceIdHash[^)]*\)"
SIG_DISPATCH_INSTALL = r"\binline\s+bool\s+install\s*\([^)]*\)"
SIG_DISPATCH_ATTACH = r"\binline\s+bool\s+attach\s*\([^)]*\)"
SIG_BT_ADOPT = r"\bstatic\s+void\s+adopt_init_result\s*\(\s*\)"
SIG_DISPATCH_SET_OWNER = r"\binline\s+void\s+set_owner\s*\([^)]*\)"
# Each owner's install: (file, the signature of the function that installs,
# the call, squashed). The pairing channel's is the loop task's, when it
# takes init()'s result (F167), and only names the owner (set_owner(): its
# init() attached the dispatcher, and install() would call setCallbacks()
# again from the loop task while Opera's init calls it on the bring-up
# worker, the F167 review).
BT_DISPATCH_OWNERS = (
    (BT_CPP, SIG_BT_ADOPT,
     "ble_server_dispatch::set_owner(ble_server_dispatch::kPairing,&g_server_callbacks);"),
    (BLE_OPERA_H, SIG_OPERA_INIT,
     "ble_server_dispatch::install(g_pServer,ble_server_dispatch::kLink,&g_serverCallbacks);"),
)
# A class or struct and its bases (`class X final : public A, private B {`).
CLASS_BASES_RE = r"\b(?:class|struct)\s+(\w+)\s*(?:final\s*)?:\s*([^{;()]+)\{"


# BD1 reads every file of the sketch on every self-test mutation, and a
# mutation changes one file: scan each distinct (blanked) text once.
@functools.lru_cache(maxsize=4096)
def class_bases_in(code: str) -> tuple[tuple[str, frozenset[str]], ...]:
    """Each class or struct `code` declares with bases, and the bases' names
    (each by its last `::` part), in declaration order."""
    out = []
    for m in re.finditer(CLASS_BASES_RE, code):
        names = set()
        for part in m.group(2).split(","):
            words = [w for w in re.findall(r"[\w:]+", part)
                     if w not in ("public", "protected", "private", "virtual")]
            if words:
                names.add(words[-1].split("::")[-1])
        out.append((m.group(1), frozenset(names)))
    return tuple(out)


@functools.lru_cache(maxsize=None)
def instances_in(code: str, cls: str) -> frozenset[str]:
    """The objects of class `cls` that `code` declares (not pointers)."""
    return frozenset(re.findall(r"\b" + cls + r"\s+(\w+)\s*(?:;|\{|=|\()", code))


def derived_classes(blanked: dict[str, str], root: str) -> set[str]:
    """`root` and every class of the sketch derived from it, through any
    number of steps (a class derived from OperaServerCallbacks is a server
    callbacks class too). A base is named by its last `::` part."""
    bases: dict[str, set[str]] = {}
    for code in blanked.values():
        for cls, names in class_bases_in(code):
            bases.setdefault(cls, set()).update(names)
    out = {root}
    grew = True
    while grew:
        grew = False
        for cls, bs in bases.items():
            if cls not in out and bs & out:
                out.add(cls)
                grew = True
    return out


def instances_of(blanked: dict[str, str], classes: set[str]) -> set[str]:
    """The objects the sketch declares of one of `classes` (not pointers)."""
    out = set()
    for code in blanked.values():
        for cls in classes:
            out |= instances_in(code, cls)
    return out


@functools.lru_cache(maxsize=4096)
def set_callbacks_firsts(code: str) -> tuple[str, ...]:
    """The first argument of each `setCallbacks(` call in `code`, squashed."""
    out = []
    for m in re.finditer(r"\bsetCallbacks\s*\(", code):
        close = matching_paren(code, m.end() - 1)
        arg = squash(code[m.end():close]) if close > 0 else ""
        out.append(arg.split(",")[0])
    return tuple(out)


@functools.lru_cache(maxsize=4096)
def role_mentions(code: str, role: str) -> int:
    return len(re.findall(r"\bble_server_dispatch::" + role + r"\b", code))


def check_bluetooth_dispatch(files: dict[str, str], errors: list[str]) -> None:
    """Rule BD1 (F171)."""
    blanked = {path: blank_comments_and_strings(src) for path, src in files.items()}
    server_classes = derived_classes(blanked, "NimBLEServerCallbacks")
    char_classes = derived_classes(blanked, "NimBLECharacteristicCallbacks")
    char_instances = instances_of(blanked, char_classes)
    # Every other setCallbacks( in the sketch is a characteristic's: its one
    # argument a characteristic callbacks object the sketch declares (by
    # address or name) or makes (new). Anything else is refused, whatever
    # the receiver: a server callbacks object (an owner's own, a derived
    # class's, a bare NimBLEServerCallbacks), a pointer variable the check
    # cannot follow, and nullptr / NULL / 0, which put NimBLEServer back on
    # its defaultCallbacks, whose onConfirmPassKey answers yes (the F171
    # defect by another spelling; the F171 review).
    for path, code in blanked.items():
        for first in set_callbacks_firsts(code):
            if path == BT_DISPATCH_H and first == "&g_dispatcher":
                continue
            name = re.fullmatch(r"&?(\w+)", first)
            new = re.fullmatch(r"new(\w+)(?:\(.*\)|\{.*\})?", first)
            if name is not None and name.group(1) in char_instances:
                continue
            if new is not None and new.group(1) in char_classes:
                continue
            if first in ("nullptr", "NULL", "0"):
                why = ("puts NimBLE's default server callbacks back, whose onConfirmPassKey answers "
                       "every passkey yes")
            elif (name is not None and name.group(1) in instances_of(blanked, server_classes)) or \
                    (new is not None and new.group(1) in server_classes):
                why = ("installs server callbacks of its own — NimBLE keeps one pointer per server, so "
                       "it replaces the other owner's (on FULL the library's default then answers "
                       "every passkey yes)")
            else:
                why = ("hands over something that is not one of the sketch's characteristic callbacks "
                       "objects (a pointer variable, or an object the check cannot type), which could "
                       "be the server's")
            errors.append(f"{path}: setCallbacks({first}) {why}: server callbacks go to "
                          "ble_server_dispatch::install(), and a characteristic's take an object of a "
                          "NimBLECharacteristicCallbacks class (F171)")
    if BT_DISPATCH_H not in files:
        errors.append(f"{BT_DISPATCH_H}: missing — the server callbacks' dispatcher (F171)")
        return
    dcode = blanked[BT_DISPATCH_H]
    attach = body_of(dcode, SIG_DISPATCH_ATTACH, f"{BT_DISPATCH_H}: attach()", errors)
    if attach is not None:
        if squash(attach).count("server->setCallbacks(&g_dispatcher,false);") != 1:
            errors.append(f"{BT_DISPATCH_H}: attach() must put the dispatcher on the server once, "
                          "`server->setCallbacks(&g_dispatcher, false);` — NimBLE deletes a callbacks "
                          "object with the server when the flag is true, and the dispatcher is not on "
                          "the heap (F171)")
    install = body_of(dcode, SIG_DISPATCH_INSTALL, f"{BT_DISPATCH_H}: install()", errors)
    if install is not None and squash(install) != "g_dispatcher.set(role,owner);returnattach(server);":
        errors.append(f"{BT_DISPATCH_H}: install() records the owner and attaches the dispatcher, "
                      "`g_dispatcher.set(role, owner); return attach(server);`, nothing else (F171)")
    if len(re.findall(r"\bsetCallbacks\s*\(", dcode)) != 1:
        errors.append(f"{BT_DISPATCH_H}: the dispatcher is put on the server only by attach() (F171)")
    set_owner = body_of(dcode, SIG_DISPATCH_SET_OWNER, f"{BT_DISPATCH_H}: set_owner()", errors)
    if set_owner is not None and squash(set_owner) != "g_dispatcher.set(role,owner);":
        errors.append(f"{BT_DISPATCH_H}: set_owner() only records the owner, "
                      "`g_dispatcher.set(role, owner);` — no setCallbacks(): it is for an owner whose "
                      "server already carries the dispatcher (the F167 review)")
    # set_owner() skips the attach, so only the owner whose own init() attached
    # the dispatcher to the server may use it: the pairing channel, at adoption.
    for path, code in blanked.items():
        if path == BT_DISPATCH_H:
            continue
        n_set = len(call_starts(code, "ble_server_dispatch::set_owner"))
        n_install = len(call_starts(code, "ble_server_dispatch::install"))
        if path == BT_CPP and n_install:
            errors.append(f"{BT_CPP}: calls ble_server_dispatch::install() — the channel's init() "
                          "attached the dispatcher; the loop task only names its owner with set_owner(), "
                          "or it calls NimBLEServer::setCallbacks() again from the loop task while Opera's "
                          "init calls it on the bring-up worker (the F167 review)")
        if path != BT_CPP and n_set:
            errors.append(f"{path}: calls ble_server_dispatch::set_owner() — only the pairing channel, "
                          "whose init() attached the dispatcher, may skip the attach; another owner "
                          "installs with install() (the F167 review)")
    for path, sig, call in BT_DISPATCH_OWNERS:
        if path not in files:
            errors.append(f"{path}: missing — a server callbacks owner (F171)")
            continue
        who = "adopt_init_result()" if path == BT_CPP else "init()"
        body = body_of(blanked[path], sig, f"{path}: {who}", errors)
        if body is not None and squash(body).count(call) != 1:
            errors.append(f"{path}: {who} must install its server callbacks once with `{call}` "
                          "(F171: its role decides which callbacks reach it; F167: the channel's, "
                          "on the loop task, once it holds the state they apply to)")
    if BT_CPP in files:
        bt_init = body_of(blanked[BT_CPP], SIG_BT_INIT, f"{BT_CPP}: init()", errors)
        if bt_init is not None and squash(bt_init).count("ble_server_dispatch::attach(server);") != 1:
            errors.append(f"{BT_CPP}: init() must put the dispatcher on the server it creates, once, "
                          "`ble_server_dispatch::attach(server);` — until the loop task installs the "
                          "channel's callbacks the server would answer with NimBLE's defaults, which say "
                          "yes to every Numeric Comparison (F167, F171)")
    for path, code in blanked.items():
        if path == BT_DISPATCH_H:
            continue
        for role in ("kPairing", "kLink"):
            n = role_mentions(code, role)
            owns = any(p == path and role in c for p, _sig, c in BT_DISPATCH_OWNERS)
            if n != (1 if owns else 0):
                errors.append(f"{path}: names ble_server_dispatch::{role} {n} time(s) — each role "
                              "has one owner (the pairing channel every callback, Opera a link's "
                              "start and end), installed once (F171)")


# BV4 (F167): init() hands its result to the loop task.
SIG_BT_IS_INIT = r"\bbool\s+is_initialized\s*\(\s*\)"
SIG_BT_FAIL_REASON = r"\bconst\s+char\s*\*\s*init_fail_reason\s*\(\s*\)"
SIG_BT_SET_FAIL = r"\bstatic\s+void\s+set_init_fail_reason\s*\(\s*const\s+char\s*\*\s*fmt\s*,\s*\.\.\.\s*\)"
# Since sweep F261 it answers whether it refused the saved name (load_saved() logs it).
SIG_BT_LOAD_SETTINGS = r"\bstatic\s+(?:void|bool)\s+load_settings\s*\(\s*BluetoothSettings\s*\*\s*out\s*\)"
SIG_BT_LOAD_PAIRED = r"\bstatic\s+void\s+load_paired_devices\s*\([^)]*\)"
SIG_BT_DEINIT = r"\bstatic\s+void\s+deinit\s*\(\s*\)"
# The file globals init() may name: its latch, its handoff and the flag it
# publishes, the device metadata pushed to it before it runs (on its own
# task), and the GATT and scan callbacks objects it hands the stack.
BT_INIT_GLOBALS = ("g_init_in_progress", "g_stack_up", "g_bringup", "g_bringup_ready",
                   "g_char_callbacks", "g_scan_callbacks", "g_store_callbacks")
BT_INIT_GLOBAL_PREFIXES = ("g_meta_",)
# What init() may call of the file: the settings loader (into the hand-over:
# the settings it gives the stack) and the refusal's publisher. Not
# set_state(), not enable() / start_advertising(), not the list's loader, its
# rebuild or a save: those are the loop task's (the list's loader since the
# F167 review, load_saved()).
BT_INIT_CALLS = ("load_settings", "set_init_fail_reason")
# Who may name the hand-over's own globals.
BT_HANDOFF_NAMES = {
    "g_bringup": ("init", "adopt_init_result"),
    "g_bringup_ready": ("init", "adopt_init_result", "deinit"),
    "g_stack_up": ("init", "is_initialized", "deinit"),
    "g_init_in_progress": ("init", "init_running"),
    "g_init_fail_reason": ("set_init_fail_reason", "init_fail_reason"),
    "g_init_fail_text": ("set_init_fail_reason",),
    "g_init_fail_slot": ("set_init_fail_reason",),
}
BT_PUBLISH_HANDOFF = "__atomic_store_n(&g_bringup_ready,true,__ATOMIC_RELEASE);"
# init()'s latch (the F235 review): taken first, by the one compare-exchange,
# and cleared only by its RAII guard's release store, which runs as init()
# returns, after the body's last write (g_stack_up's publish among them). A
# tap that lost the latch then reads it clear (acquire) only once the stack
# is up or refused: bring_up_starting() reads the latch first on that.
# And (wave 16's verify) the re-check of the stack right after the guard: a
# caller that read the stack down just before a winner finished takes the
# freed latch, and without it runs NimBLEDevice::init() and createServer() a
# second time. The guard is named nowhere else in init(): an explicit
# `latch_clear.~InitLatchClear();` or an inner `{ InitLatchClear c; }`
# before the publish is the early clear by another spelling.
BT_INIT_HEAD = ("if(__atomic_load_n(&g_stack_up,__ATOMIC_ACQUIRE))returntrue;"
                "boolexpected=false;if(!__atomic_compare_exchange_n(&g_init_in_progress,&expected,true,false,"
                "__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){returnfalse;}"
                "structInitLatchClear{~InitLatchClear(){__atomic_store_n(&g_init_in_progress,false,"
                "__ATOMIC_RELEASE);}}latch_clear;"
                "if(__atomic_load_n(&g_stack_up,__ATOMIC_ACQUIRE))returntrue;")
SIG_BT_INIT_RUNNING = r"\bstatic\s+bool\s+init_running\s*\(\s*\)"
BT_INIT_RUNNING_BODY = "return__atomic_load_n(&g_init_in_progress,__ATOMIC_ACQUIRE);"
BT_PUBLISH_STACK_UP = "__atomic_store_n(&g_stack_up,true,__ATOMIC_RELEASE);"
BT_TAKE_HANDOFF = "if(g_initialized||!__atomic_load_n(&g_bringup_ready,__ATOMIC_ACQUIRE))return;"
# The F167 review: the saved settings and the paired list are the loop
# task's from its first pass (load_saved(), first in update()), and the
# adoption keeps them; Remove and Clear wait for the stack.
SIG_BT_LOAD_SAVED = r"\bstatic\s+void\s+load_saved\s*\(\s*\)"
SIG_BT_REMOVE = r"\bstatic\s+bool\s+remove_paired_device\s*\([^)]*\)"
SIG_BT_CLEAR = r"\bstatic\s+bool\s+clear_all_paired_devices\s*\([^)]*\)"
BT_LOAD_SAVED_LATCH = "if(g_saved_loaded)return;g_saved_loaded=true;"
BT_UPDATE_HEAD = "load_saved();adopt_init_result();"
BT_NOT_UP_GUARD = "if(!g_initialized){*refusal=BT_REFUSED_NOT_UP;returnfalse;}"
# What the adoption must not write: the loop task's settings and list.
BT_ADOPT_KEEPS = (r"\bg_settings\s*(?:\.\w+\s*)?=(?!=)", r"\bg_paired_count\s*=(?!=)",
                  r"\bg_paired_by_identity\s*=(?!=)", r"\bmemcpy\s*\(\s*(?:&\s*)?g_(?:settings|paired_devices)\b",
                  r"\bg_paired_devices\s*\[[^\]]*\]\s*=(?!=)", r"\bload_(?:settings|paired_devices)\s*\(")


def check_bluetooth_bringup(cpp_src: str, errors: list[str]) -> None:
    """Rule BV4 (F167)."""
    code = blank_comments_and_strings(cpp_src)
    spans = named_bodies(code)
    defined = {name for name, _s, _e in spans}
    init = body_of(code, SIG_BT_INIT, f"{BT_CPP}: init()", errors)
    if init is not None:
        for m in re.finditer(r"\b(g_\w+)\b", init):
            g = m.group(1)
            if g not in BT_INIT_GLOBALS and not g.startswith(BT_INIT_GLOBAL_PREFIXES):
                errors.append(f"{BT_CPP}: init() names {g} — it runs on the BLE bring-up worker or an "
                              "HTTP handler's task; the loop task's state is handed over through "
                              "g_bringup and taken by adopt_init_result() (F167)")
                break
        for m in re.finditer(CALL_RE, init):
            callee = m.group(1)
            if callee in defined and callee not in BT_INIT_CALLS and callee != "init":
                errors.append(f"{BT_CPP}: init() calls {callee}() — off the loop task it calls only "
                              f"{', '.join(BT_INIT_CALLS)}; the state, the list's rebuild, the "
                              "auto-advertise and the saves are the loop task's (F167)")
        sq = squash(init)
        if not sq.startswith(BT_INIT_HEAD) or sq.count("g_init_in_progress") != 2 or \
                sq.count("InitLatchClear") != 2 or sq.count("latch_clear") != 1:
            errors.append(f"{BT_CPP}: init() must start `{BT_INIT_HEAD}` and name g_init_in_progress, "
                          "InitLatchClear and latch_clear nowhere else — the latch is taken first, by the one "
                          "compare-exchange, cleared only by its guard's release store as init() returns, after "
                          "the stack's publish (cleared earlier, by an explicit destructor call or an inner "
                          "guard, a tap that lost the latch reads it clear with the stack not up yet and "
                          "answers a refusal for a bring-up that is coming up: the F235 review), and the stack "
                          "is looked at again once the latch is held (without it, a caller that read the "
                          "stack down just before a winner finished runs NimBLE's init a second time: wave "
                          "16's verify)")
        pub = sq.find(BT_PUBLISH_HANDOFF)
        if sq.count(BT_PUBLISH_HANDOFF) != 1:
            errors.append(f"{BT_CPP}: init() must publish its result once, `{BT_PUBLISH_HANDOFF}` "
                          "(a release: the loop task's acquire then sees every write before it) (F167)")
        else:
            after = sq[pub + len(BT_PUBLISH_HANDOFF):]
            if re.search(r"\bb\.\w+(?:\[[^\]]*\])?=|g_bringup\b(?!_ready)", after):
                errors.append(f"{BT_CPP}: init() writes its hand-over after publishing it — the loop "
                              "task may already be reading it (F167)")
            if after.count(BT_PUBLISH_STACK_UP) != 1 or sq.count("g_stack_up") != \
                    after.count("g_stack_up") + sq[:pub].count("g_stack_up"):
                errors.append(f"{BT_CPP}: init() sets the flag other tasks read once, after the "
                              f"hand-over, `{BT_PUBLISH_STACK_UP}` (F167)")
        for m in re.finditer(r"\bg_stack_up\b", sq):
            ctx = sq[max(0, m.start() - 30):m.end() + 30]
            if not (ctx.find("__atomic_load_n(&g_stack_up,__ATOMIC_ACQUIRE)") >= 0 or
                    ctx.find("__atomic_store_n(&g_stack_up,true,__ATOMIC_RELEASE)") >= 0):
                errors.append(f"{BT_CPP}: init() reads or writes g_stack_up other than by an acquire "
                              "load or a release store (F167)")
                break
    for sig, what in ((SIG_BT_LOAD_SETTINGS, "load_settings()"), (SIG_BT_LOAD_PAIRED, "load_paired_devices()")):
        body = body_of(code, sig, f"{BT_CPP}: {what}", errors)
        if body is not None and re.search(r"\bg_\w+", body):
            g = re.search(r"\bg_\w+", body).group(0)
            errors.append(f"{BT_CPP}: {what} names {g} — init()'s loader fills its out-parameters (the "
                          "hand-over), never the loop task's state (F167)")
    for g, allowed in BT_HANDOFF_NAMES.items():
        for m in re.finditer(r"\b" + g + r"\b", code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue
            if where not in allowed:
                errors.append(f"{BT_CPP}: {where}() names {g} — only {', '.join(allowed)} may (F167)")
                break
    adopt = body_of(code, SIG_BT_ADOPT, f"{BT_CPP}: adopt_init_result()", errors)
    if adopt is not None and not squash(adopt).startswith(BT_TAKE_HANDOFF):
        errors.append(f"{BT_CPP}: adopt_init_result() must start `{BT_TAKE_HANDOFF}` — the acquire "
                      "that makes init()'s writes visible comes before any read of them, and the result "
                      "is taken once (F167)")
    for m in re.finditer(r"(?<![\w:.>])adopt_init_result\s*\(", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where != "update":
            errors.append(f"{BT_CPP}: {where}() calls adopt_init_result() — only update(), first, takes "
                          "the bring-up's result (F167)")
    for m in re.finditer(r"\bg_initialized\s*=(?!=)", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where not in ("adopt_init_result", "deinit"):
            errors.append(f"{BT_CPP}: {where}() assigns g_initialized — the loop task's flag, set when "
                          "it takes the bring-up's result (F167)")
    for m in re.finditer(r"(?<![\w:.>])migrate_paired_devices\s*\(", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where != "adopt_init_result":
            errors.append(f"{BT_CPP}: {where}() rebuilds the paired list — the loop task's, when it "
                          "takes the bring-up's result (F167)")
    running = body_of(code, SIG_BT_INIT_RUNNING, f"{BT_CPP}: init_running()", errors)
    if running is not None and squash(running) != BT_INIT_RUNNING_BODY:
        errors.append(f"{BT_CPP}: init_running() must be exactly `return __atomic_load_n(&g_init_in_progress, "
                      "__ATOMIC_ACQUIRE);` — the acquire that pairs with the latch's release, so a latch "
                      "read clear sees the stack's publish (the F235 review)")
    is_init = body_of(code, SIG_BT_IS_INIT, f"{BT_CPP}: is_initialized()", errors)
    if is_init is not None and squash(is_init) != "return__atomic_load_n(&g_stack_up,__ATOMIC_ACQUIRE);":
        errors.append(f"{BT_CPP}: is_initialized() must be `return __atomic_load_n(&g_stack_up, "
                      "__ATOMIC_ACQUIRE);` — any task asks it, and g_initialized is the loop task's (F167)")
    reason = body_of(code, SIG_BT_FAIL_REASON, f"{BT_CPP}: init_fail_reason()", errors)
    if reason is not None and squash(reason) != \
            "return__atomic_load_n(&g_init_fail_reason,__ATOMIC_ACQUIRE);":
        errors.append(f"{BT_CPP}: init_fail_reason() must be `return __atomic_load_n(&g_init_fail_reason, "
                      "__ATOMIC_ACQUIRE);` (F167)")
    setter = body_of(code, SIG_BT_SET_FAIL, f"{BT_CPP}: set_init_fail_reason()", errors)
    if setter is not None:
        sq = squash(setter)
        write = sq.find("vsnprintf(text,")
        store = sq.find("__atomic_store_n(&g_init_fail_reason,(constchar*)text,__ATOMIC_RELEASE);")
        if write < 0 or store < write or "g_init_fail_slot=(g_init_fail_slot+1)%INIT_FAIL_SLOTS;" not in sq \
                or re.search(r"\bg_init_fail_reason\s*=", setter):
            errors.append(f"{BT_CPP}: set_init_fail_reason() writes a refusal whole into the next slot "
                          "(`g_init_fail_slot = (g_init_fail_slot + 1) % INIT_FAIL_SLOTS;`, then "
                          "`vsnprintf(text, ...)`) and only then publishes its address with a release "
                          "store — a reader holding the last one is never written under (F167)")
    update = body_of(code, SIG_UPDATE, f"{BT_CPP}: update()", errors)
    if update is not None:
        sq = squash(update)
        if not sq.startswith(BT_UPDATE_HEAD) or sq.count("adopt_init_result();") != 1 or \
                sq.count("load_saved();") != 1:
            errors.append(f"{BT_CPP}: update() must start `load_saved(); adopt_init_result();`, each once: "
                          "the saved settings and list are the loop task's before any command or event "
                          "of its first pass, and the bring-up's result is taken before either runs "
                          "(F167 and its review)")
    # The F167 review: the loop task loads, the adoption keeps.
    saved = body_of(code, SIG_BT_LOAD_SAVED, f"{BT_CPP}: load_saved()", errors)
    if saved is not None:
        sq = squash(saved)
        if not sq.startswith(BT_LOAD_SAVED_LATCH) or "load_settings(&" not in sq or \
                "load_paired_devices(g_paired_devices,&g_paired_count,&g_paired_by_identity);" not in sq:
            errors.append(f"{BT_CPP}: load_saved() loads the saved settings and the paired list once, "
                          f"starting `{BT_LOAD_SAVED_LATCH}` — a load on a later pass would overwrite what "
                          "the owner's commands changed (the F167 review)")
    for name, allowed in (("load_saved", ("update",)), ("load_paired_devices", ("load_saved",)),
                          ("load_settings", ("load_saved", "init"))):
        for m in re.finditer(r"(?<![\w:.>])" + name + r"\s*\(", code):
            where = enclosing_function(spans, m.start())
            if where is not None and where != name and where not in allowed:
                errors.append(f"{BT_CPP}: {where}() calls {name}() — only {', '.join(allowed)} may: the "
                              "saved settings and list are loaded once, by the loop task, before any "
                              "command (init() reads the settings only for the stack) (the F167 review)")
    for m in re.finditer(r"\bg_saved_loaded\b", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where != "load_saved":
            errors.append(f"{BT_CPP}: {where}() names g_saved_loaded — load_saved()'s own latch "
                          "(the F167 review)")
            break
    if adopt is not None:
        for pattern in BT_ADOPT_KEEPS:
            m = re.search(pattern, adopt)
            if m:
                errors.append(f"{BT_CPP}: adopt_init_result() writes the loop task's settings or paired "
                              f"list (`{m.group(0).strip()}`) — they are the loop task's from its first pass, "
                              "and the owner's commands may have changed them while init() ran: a "
                              "Disable came back on (the F167 review)")
                break
    for sig, what in ((SIG_BT_REMOVE, "remove_paired_device()"), (SIG_BT_CLEAR, "clear_all_paired_devices()")):
        body = body_of(code, sig, f"{BT_CPP}: {what}", errors)
        if body is not None and not squash(body).startswith(BT_NOT_UP_GUARD):
            errors.append(f"{BT_CPP}: {what} must start `{BT_NOT_UP_GUARD}` — the list is loaded before "
                          "the stack is up, and its bonds are NimBLE's, reachable once it is: an entry "
                          "dropped or a list cleared then leaves the bond (the F167 review)")


# BV5 (F189): the bond store never evicts behind the owner, and the paired
# list takes only a bond the store holds.
SIG_BT_STORE_STATUS = r"\bint\s+onStoreStatus\s*\([^)]*\)\s*override"
SIG_BT_AUTH_COMPLETE = r"\bstatic\s+void\s+apply_auth_complete\s*\([^)]*\)"
BT_STORE_INSTALL = "NimBLEDevice::setDeviceCallbacks(&g_store_callbacks);"
BT_STORE_EVICTS = (r"\bble_gap_unpair\w*\s*\(", r"\bdeleteBond\s*\(", r"\bdeleteAllBonds\s*\(",
                   r"\bble_store_util_status_rr\s*\(", r"\bNimBLEDeviceCallbacks\s*::\s*onStoreStatus\s*\(",
                   r"\bble_store_util_delete\w*\s*\(")


# BV5 and BV7 read every file of the sketch on every self-test mutation, and
# a mutation changes one file: find each call in each distinct text once.
@functools.lru_cache(maxsize=None)
def call_starts(code: str, name: str) -> tuple[int, ...]:
    """Where each `name(` starts in blanked `code` (a word boundary first)."""
    return tuple(m.start() for m in re.finditer(r"\b" + name + r"\s*\(", code))


def check_bluetooth_bond_store(files: dict[str, str], errors: list[str]) -> None:
    """Rule BV5 (F189)."""
    blanked = {path: blank_comments_and_strings(src) for path, src in files.items()}
    for path, code in blanked.items():
        n = len(call_starts(code, "setDeviceCallbacks"))
        if path == BT_CPP:
            init = body_of(code, SIG_BT_INIT, f"{BT_CPP}: init()", errors)
            if n != 1 or init is None or squash(init).count(BT_STORE_INSTALL) != 1:
                errors.append(f"{BT_CPP}: init() must answer the bond store's status itself, once, "
                              f"`{BT_STORE_INSTALL}`, and nothing else in the file sets the device "
                              "callbacks — NimBLE's default evicts the oldest bond behind the owner when "
                              "the store is full (F189)")
        elif n:
            errors.append(f"{path}: sets NimBLE's device callbacks — one object per stack, the pairing "
                          "channel's (StoreCallbacks); another would put the store's eviction back (F189)")
    code = blanked.get(BT_CPP, "")
    status = body_of(code, SIG_BT_STORE_STATUS, f"{BT_CPP}: StoreCallbacks::onStoreStatus()", errors)
    if status is not None:
        for pattern in BT_STORE_EVICTS:
            m = re.search(pattern, status)
            if m:
                errors.append(f"{BT_CPP}: onStoreStatus() calls {m.group(0).rstrip('(').strip()} — the "
                              "store-full path evicts no bond (it would go through ble_gap_unpair()'s busy "
                              "guard, behind the owner): a full store refuses (F189)")
                break
        sq = squash(status)
        if sq.count("returnBLE_HS_ESTORE_CAP;") != 2 or "return0;" in sq.replace(
                "if(peer.getConnHandle()==event->full.conn_handle&&NimBLEDevice::isBonded(peer.getIdAddress())){"
                "return0;", ""):
            errors.append(f"{BT_CPP}: onStoreStatus() answers BLE_STORE_EVENT_FULL and "
                          "BLE_STORE_EVENT_OVERFLOW with `return BLE_HS_ESTORE_CAP;` (refused, not kept), "
                          "and 0 only for a peer the store already holds (F189)")
    auth = body_of(code, SIG_BT_AUTH_COMPLETE, f"{BT_CPP}: apply_auth_complete()", errors)
    if auth is not None:
        sq = squash(auth)
        gate = sq.find("if(!found&&!NimBLEDevice::isBonded(identity)){")
        add = sq.find("g_paired_devices[g_paired_count++]")
        if gate < 0 or add < 0 or add < gate or "}elseif(!found&&g_paired_count<MAX_PAIRED_DEVICES){" not in sq:
            errors.append(f"{BT_CPP}: apply_auth_complete() lists a new phone only when the bond store "
                          "holds its bond (`if (!found && !NimBLEDevice::isBonded(identity)) { ... } else "
                          "if (!found && g_paired_count < MAX_PAIRED_DEVICES) {`) — NimBLE reports the link "
                          "bonded whether or not the bond was kept (F189)")


# BV7 (the F167 review): no advertising start while the sketch's bring-up
# worker registers GATT services.
SIG_BT_START_ADV = r"\bstatic\s+bool\s+start_advertising\s*\(\s*\)"
SIG_BT_STOP_ADV = r"\bstatic\s+void\s+stop_advertising\s*\(\s*\)"
SIG_BT_RESTORE_RADIO = r"\bstatic\s+void\s+restore_radio\s*\([^)]*\)"
SIG_BT_WORKER_STARTED = r"\bvoid\s+bringup_worker_started\s*\(\s*\)"
SIG_BT_WORKER_FINISHED = r"\bvoid\s+bringup_worker_finished\s*\(\s*\)"
SIG_INO_DISCOVERY_START = r"\bstatic\s+void\s+ble_discovery_start_if_due\s*\(\s*\)"
SIG_INO_FINALIZE = r"\bstatic\s+void\s+ble_bringup_finalize_if_done\s*\(\s*\)"
SIG_INO_BRINGUP_TASK = r"\bstatic\s+void\s+ble_bringup_task\s*\(\s*void\s*\*\s*\w*\s*\)"
BT_ADV_HOLD = "if(g_bringup_worker_running){g_advertise_after_bringup=true;returntrue;}"
BT_RESTORE_HOLD = "if(g_bringup_worker_running){g_advertise_after_bringup=true;}else{g_advertising->start();}"
BT_WORKER_STARTED_BODY = "g_bringup_worker_running=true;" + BT_BOOT_STARTED_STORE
BT_WORKER_FINISHED_BODY = ("g_bringup_worker_running=false;if(g_advertise_after_bringup){"
                           "g_advertise_after_bringup=false;start_advertising();}")
INO_WORKER_STARTED = "bluetooth_channel::bringup_worker_started();"
INO_WORKER_FINISHED = "bluetooth_channel::bringup_worker_finished();"
# The F235 review: the boot bring-up is over (C3's boot_bring_up_tried()).
INO_BOOT_FINISHED = "bluetooth_channel::boot_bring_up_finished();"
# A call into what can run NimBLEDevice::init() or start the stack's radio in
# the finalize stage; the boot bring-up's end is the last of them.
INO_FINALIZE_STACK_CALL = r"\b(?:bluetooth_channel|ble_scout|ble_manager)\s*::\s*\w+\s*\("
INO_WORKER_CREATE = "xTaskCreate(ble_bringup_task,"
INO_FINALIZE_HEAD = ("if(g_ble_bringup_finalized||!__atomic_load_n(&g_ble_bringup_done,__ATOMIC_ACQUIRE)){return;}"
                     "g_ble_bringup_finalized=true;")
# The failed create's block (the verifier's escapes, wave 16): its calls run
# only when no worker was created.
INO_FAILED_CREATE = r"\bif\s*\(\s*xTaskCreate\s*\(\s*ble_bringup_task\s*,[^;{}]*?\)\s*!=\s*pdPASS\s*\)\s*\{"
INO_FINALIZE_HEAD_END = r"\bg_ble_bringup_finalized\s*=\s*true\s*;"
# The worker's handoff to the finalize stage (the second verify's escapes,
# wave 16): its last statement before it deletes itself, the one write of
# the flag anywhere.
INO_DONE_STORE = "__atomic_store_n(&g_ble_bringup_done,true,__ATOMIC_RELEASE);"
INO_WORKER_END = INO_DONE_STORE + "vTaskDelete(NULL);"
# The preprocessor conditionals each call sits under, as pp_guards() spells
# them: the gate's whole body is `#if FEATURE_BLE || FEATURE_BLUETOOTH ||
# FEATURE_BLE_SCAN` (the finalize stage is defined under the same, at file
# scope), and each channel call is under `#if FEATURE_BLUETOOTH` and nothing
# else: on a build with Bluetooth, it is compiled in.
INO_BLE_GUARD = "if FEATURE_BLE || FEATURE_BLUETOOTH || FEATURE_BLE_SCAN"
INO_BT_GUARD = "if FEATURE_BLUETOOTH"
PP_CONDITIONAL = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)", re.M)


def pp_guards(code: str, start: int, pos: int) -> tuple[str, ...] | None:
    """The preprocessor conditionals open at `pos` in blanked `code`, counted
    from `start`: each `#if X` as `if X` (`ifdef X`, `ifndef X`), whitespace
    squeezed, and a branch an `#elif` or `#else` took over as `... | elif Y`
    or `... | else`. None when one closes or turns a conditional opened
    before `start`."""
    stack: list[str] = []
    for m in PP_CONDITIONAL.finditer(code, start, pos):
        kind, rest = m.group(1), " ".join(m.group(2).split())
        if kind in ("if", "ifdef", "ifndef"):
            stack.append(f"{kind} {rest}")
        elif not stack:
            return None
        elif kind == "endif":
            stack.pop()
        else:
            stack[-1] = f"{stack[-1]} | {kind} {rest}".rstrip()
    return tuple(stack)


def unconditional_in(code: str, start: int, pos: int, jumps: bool = True) -> str | None:
    """Why the statement at `pos` of blanked `code` may not run each time
    control passes `start`, the start of the block (or body) holding it, or
    None: it must sit at the block's own brace depth, start a statement (after
    `;`, `}` or the block's start: not after `if (...)`, `else`, `do` or a
    label) and have no `return` or `goto` ahead of it in the block. With
    `jumps=False` a `return` or `goto` ahead of it is the caller's to judge
    (a statement that must run whenever the next one does, not whenever the
    block starts). The preprocessor is pp_guards()'s; its lines are left out
    here."""
    region = PP_CONDITIONAL.sub(lambda m: " " * len(m.group(0)), code[start:pos])
    depth = 0
    for ch in region:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                return "it lies outside the block"
    if depth:
        return f"it sits {depth} brace level(s) inside the block, in a block of its own"
    before = region.rstrip()
    if before and before[-1] not in ";}":
        return f"it is not a statement of its own (it follows `{before[-12:].strip()}`): a condition or a branch controls it"
    jump = re.search(r"\b(?:return|goto)\b", region) if jumps else None
    if jump:
        return f"a `{jump.group(0)}` ahead of it in the block can leave before it runs"
    return None


def call_stmt(call: str) -> str:
    """A regex for the call statement `call` (`ns::f();`), any whitespace."""
    return r"\b" + re.escape(call[:-3]).replace(r"::", r"\s*::\s*") + r"\s*\(\s*\)\s*;"


def check_bluetooth_bringup_worker(files: dict[str, str], errors: list[str]) -> None:
    """Rule BV7 (the F167 review): after the channel's init() returns, the
    sketch's bring-up worker goes on registering GATT services on the same
    server (ble_status on DEV and FULL, Opera on FULL), and an advertising
    start starts that server (NimBLEServer::start() walks the service list
    the worker appends to). The channel holds every start of its own while
    the worker runs, and the sketch tells it when that is, on the loop
    task."""
    code = blank_comments_and_strings(files.get(BT_CPP, ""))
    spans = named_bodies(code)
    start = body_of(code, SIG_BT_START_ADV, f"{BT_CPP}: start_advertising()", errors)
    if start is not None:
        sq = squash(start)
        hold = sq.find(BT_ADV_HOLD)
        go = sq.find("g_advertising->start()")
        if hold < 0 or go < 0 or go < hold:
            errors.append(f"{BT_CPP}: start_advertising() holds the start while the sketch's bring-up worker "
                          f"runs, before it starts anything (`{BT_ADV_HOLD}`) — NimBLEAdvertising::start() "
                          "starts the server the worker is still adding services to (the F167 review)")
    restore = body_of(code, SIG_BT_RESTORE_RADIO, f"{BT_CPP}: restore_radio()", errors)
    if restore is not None and BT_RESTORE_HOLD not in squash(restore):
        errors.append(f"{BT_CPP}: restore_radio() puts a stopped advertiser back only when no bring-up worker "
                      f"runs, holding it otherwise (`{BT_RESTORE_HOLD}`) (the F167 review)")
    stop = body_of(code, SIG_BT_STOP_ADV, f"{BT_CPP}: stop_advertising()", errors)
    if stop is not None and not squash(stop).startswith("g_advertise_after_bringup=false;"):
        errors.append(f"{BT_CPP}: stop_advertising() withdraws a held start first, "
                      "`g_advertise_after_bringup = false;` — or the worker's end starts what the owner "
                      "stopped (the F167 review)")
    for m in re.finditer(r"\bg_advertising\s*->\s*start\s*\(", code):
        where = enclosing_function(spans, m.start())
        if where not in ("start_advertising", "restore_radio"):
            errors.append(f"{BT_CPP}: {where or 'file scope'}() starts the advertiser itself — every start "
                          "goes through start_advertising() or restore_radio(), which hold it while the "
                          "bring-up worker registers services (the F167 review)")
    for sig, body_want, what in ((SIG_BT_WORKER_STARTED, BT_WORKER_STARTED_BODY, "bringup_worker_started()"),
                                 (SIG_BT_WORKER_FINISHED, BT_WORKER_FINISHED_BODY, "bringup_worker_finished()")):
        body = body_of(code, sig, f"{BT_CPP}: {what}", errors)
        if body is not None and squash(body) != body_want:
            errors.append(f"{BT_CPP}: {what} must be exactly `{body_want}` (the F167 review)")
    for m in re.finditer(r"\bg_bringup_worker_running\s*=(?!=)", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where not in ("bringup_worker_started", "bringup_worker_finished"):
            errors.append(f"{BT_CPP}: {where or 'file scope'}() sets g_bringup_worker_running — only the "
                          "sketch's calls do (the F167 review)")
    # The sketch: started before the worker is created, finished when its
    # result is taken (or it could not be created), both on the loop task.
    ino = blank_comments_and_strings(files.get(INO, ""))
    ino_spans = named_bodies(ino)
    disc = body_of(ino, SIG_INO_DISCOVERY_START, f"{INO}: ble_discovery_start_if_due()", errors)
    if disc is not None:
        sq = squash(disc)
        st = sq.find(INO_WORKER_STARTED)
        cr = sq.find(INO_WORKER_CREATE)
        fin = sq.find(INO_WORKER_FINISHED)
        if sq.count(INO_WORKER_STARTED) != 1 or cr < 0 or st < 0 or st > cr or \
                sq.count(INO_WORKER_FINISHED) != 1 or fin < cr:
            errors.append(f"{INO}: ble_discovery_start_if_due() tells the pairing channel "
                          f"`{INO_WORKER_STARTED}` once, before `{INO_WORKER_CREATE}` (the worker may run "
                          f"before it returns), and `{INO_WORKER_FINISHED}` when the create fails "
                          "(the F167 review)")
        bf = sq.find(INO_BOOT_FINISHED)
        if sq.count(INO_BOOT_FINISHED) != 1 or fin < 0 or bf < fin:
            errors.append(f"{INO}: ble_discovery_start_if_due() tells the pairing channel "
                          f"`{INO_BOOT_FINISHED}` once, right after the failed create's "
                          f"`{INO_WORKER_FINISHED}` — no worker, no finalize stage: the boot bring-up is "
                          "over, and Start Advertising and Pair may bring the stack up (F235's review)")
        # Both calls inside the failed create's block, each run every time
        # the block is (the verifier's escapes, wave 16): after a create that
        # worked, the first lets an advertising start walk the services the
        # worker is adding (F167), and the second lets a tap run init() beside
        # the worker's inits and the Scout's (F235's review, B1).
        blocks = list(re.finditer(INO_FAILED_CREATE, disc))
        if len(blocks) != 1:
            errors.append(f"{INO}: ble_discovery_start_if_due() creates the worker in one `if "
                          "(xTaskCreate(ble_bringup_task, ...) != pdPASS) {` whose block is the failed "
                          f"create's (found {len(blocks)}) (the F167 review, F235's review)")
        else:
            open_at = blocks[0].end() - 1
            close_at = close_brace(disc, open_at)
            for call, why in ((INO_WORKER_FINISHED, "no worker: the advertising hold ends (the F167 review)"),
                              (INO_BOOT_FINISHED, "no worker, no finalize stage: the boot bring-up is over "
                                                  "(F235's review)")):
                at = [m.start() for m in re.finditer(call_stmt(call), disc)]
                inside = [a for a in at if open_at < a < close_at]
                problem = None
                if len(at) != 1 or len(inside) != 1:
                    problem = "it is not called exactly once, inside that block"
                else:
                    problem = unconditional_in(disc, open_at + 1, inside[0])
                    guards = pp_guards(disc, 0, inside[0])
                    if problem is None and guards != (INO_BLE_GUARD, INO_BT_GUARD):
                        problem = (f"it is compiled under {' / '.join(guards or ('an unbalanced #if',))}, not "
                                   f"the gate's `#{INO_BLE_GUARD}` with `#{INO_BT_GUARD}` alone inside it")
                if problem:
                    errors.append(f"{INO}: ble_discovery_start_if_due() calls `{call}` once, as a statement "
                                  "of the failed create's block (`if (xTaskCreate(ble_bringup_task, ...) != "
                                  f"pdPASS) {{`) that runs whenever the block does — {why}; here {problem}")
            # The start mark runs whenever the create does (the second
            # verify's escapes, wave 16): since it also opens the gate's
            # still-starting answer (C3's boot_bring_up_started()), a start
            # skipped before a create that ran held no advertising while the
            # worker registered services (F167) and told a tap after a refused
            # worker init that Bluetooth had not started; a start whose create
            # was then skipped held every tap at 503 "still starting" until a
            # reboot. A statement of the gate's body itself, under the gate's
            # guard with FEATURE_BLUETOOTH alone inside it, and nothing but its
            # `#endif` between it and the create: no return, goto, label or
            # statement fits there.
            st = list(re.finditer(call_stmt(INO_WORKER_STARTED), disc))
            problem = None
            if len(st) != 1:
                problem = f"it is called {len(st)} times"
            else:
                problem = unconditional_in(disc, 0, st[0].start(), jumps=False)
                guards = pp_guards(disc, 0, st[0].start())
                between = squash(disc[st[0].end():blocks[0].start()])
                if problem is None and guards != (INO_BLE_GUARD, INO_BT_GUARD):
                    problem = (f"it is compiled under {' / '.join(guards or ('an unbalanced #if',))}, not "
                               f"the gate's `#{INO_BLE_GUARD}` with `#{INO_BT_GUARD}` alone inside it")
                elif problem is None and between != "#endif":
                    problem = (f"`{between[:60]}` lies between it and the create, where only its `#endif` "
                               "may")
            if problem:
                errors.append(f"{INO}: ble_discovery_start_if_due() calls `{INO_WORKER_STARTED}` once, as a "
                              "statement of the gate's body itself, compiled under the gate's "
                              f"`#{INO_BLE_GUARD}` with `#{INO_BT_GUARD}` alone inside it, with nothing but "
                              "its `#endif` between it and `if (xTaskCreate(ble_bringup_task, ...) != pdPASS) "
                              "{` — it holds the advertising while the worker registers services (the F167 "
                              "review) and opens the still-starting answer (C3), so it runs whenever the "
                              f"create does and only then (the second verify, wave 16); here {problem}")
    fin_body = body_of(ino, SIG_INO_FINALIZE, f"{INO}: ble_bringup_finalize_if_done()", errors)
    if fin_body is not None:
        sq = squash(fin_body)
        if not sq.startswith(INO_FINALIZE_HEAD) or sq.count(INO_WORKER_FINISHED) != 1:
            errors.append(f"{INO}: ble_bringup_finalize_if_done() tells the pairing channel "
                          f"`{INO_WORKER_FINISHED}` once, after it has taken the worker's result "
                          "(the F167 review)")
        else:
            # On every finalize, whatever the worker's result (the second
            # verify's escapes, wave 16): skipped once the worker brought the
            # stack up, a tap was answered 200 "Advertising started" and
            # nothing went on air until a reboot (on every DEV device, where
            # FEATURE_BLE is 0, under the wrong guard).
            head = re.search(INO_FINALIZE_HEAD_END, fin_body)
            wf = re.search(call_stmt(INO_WORKER_FINISHED), fin_body)
            problem = None
            if head is None or wf is None:
                problem = "its head's `g_ble_bringup_finalized = true;` or the call is missing"
            else:
                problem = unconditional_in(fin_body, head.end(), wf.start())
                guards = pp_guards(fin_body, 0, wf.start())
                if problem is None and guards != (INO_BT_GUARD,):
                    problem = (f"it is compiled under {' / '.join(guards or ('an unbalanced #if',))}, not "
                               f"`#{INO_BT_GUARD}` alone")
            if problem:
                errors.append(f"{INO}: ble_bringup_finalize_if_done() runs `{INO_WORKER_FINISHED}` on every "
                              "finalize, whatever the worker's result: a statement of the body's own (no "
                              f"condition, branch or block around it), under `#{INO_BT_GUARD}` alone, with "
                              "no `return` or `goto` between the head and it — a finalize that skips it holds "
                              "the pairing channel's advertising (a tap's start among it) until a reboot "
                              f"(the F167 review; the second verify, wave 16); here {problem}")
        # Read on the blanked body, not the squashed one: squashing glues a
        # preprocessor line's last word to the call after it.
        stack_calls = list(re.finditer(INO_FINALIZE_STACK_CALL, fin_body))
        last_is_end = bool(stack_calls) and re.match(r"bluetooth_channel\s*::\s*boot_bring_up_finished\s*\(",
                                                     fin_body[stack_calls[-1].start():])
        if sq.count(INO_BOOT_FINISHED) != 1 or not last_is_end:
            errors.append(f"{INO}: ble_bringup_finalize_if_done() tells the pairing channel "
                          f"`{INO_BOOT_FINISHED}` once, as its last call into the channel, the Scout or "
                          "ble_manager — after `ble_scout::ble_scout_init();`, whose nimble_scan_init() runs "
                          "NimBLEDevice::init() on the loop task when the stack is down: a Start Advertising "
                          "or Pair tap's init() on the HTTP task must not run beside it (F235's review)")
        else:
            # And it runs on every finalize, whatever the worker's result
            # (the verifier's escapes, wave 16): a finalize that skipped it
            # left a device saved on answering every Start Advertising and
            # Pair tap 503 "has not started yet" until a reboot, F235's own
            # symptom. No exit after the head, the mark a statement of the
            # body's own, under `#if FEATURE_BLUETOOTH` alone.
            head = re.search(INO_FINALIZE_HEAD_END, fin_body)
            mark = stack_calls[-1].start()
            problem = None
            if head is None:
                problem = "its head's `g_ble_bringup_finalized = true;` is missing"
            else:
                jump = re.search(r"\b(?:return|goto)\b", fin_body[head.end():])
                if jump:
                    problem = f"a `{jump.group(0)}` after its head can leave before it runs"
                else:
                    problem = unconditional_in(fin_body, head.end(), mark)
            guards = pp_guards(fin_body, 0, mark)
            if problem is None and guards != (INO_BT_GUARD,):
                problem = (f"it is compiled under {' / '.join(guards or ('an unbalanced #if',))}, not "
                           f"`#{INO_BT_GUARD}` alone")
            if problem:
                errors.append(f"{INO}: ble_bringup_finalize_if_done() runs `{INO_BOOT_FINISHED}` on every "
                              "finalize, whatever the worker's result: a statement of the body's own (no "
                              f"condition, branch or block around it), under `#{INO_BT_GUARD}` alone, and no "
                              "`return` or `goto` after the head — a finalize that skips it leaves a device "
                              "saved on answering every Start Advertising and Pair tap 503 until a reboot "
                              f"(F235's review); here {problem}")
    # The worker hands its result to the finalize stage on every path (the
    # second verify's escapes, wave 16): a worker that ended without the
    # store left the finalize stage, and with it both bringup_worker_finished()
    # and the boot bring-up's end, unrun until a reboot (every tap 503 "still
    # starting", advertising held). Its last statement before its one
    # `vTaskDelete(NULL);` is the release store, a statement of the body's
    # own outside every #if, with no `return` or `goto` in the worker; and no
    # other code writes the flag (one written early lets the finalize stage
    # run beside the worker's registrations and inits).
    task = body_of(ino, SIG_INO_BRINGUP_TASK, f"{INO}: ble_bringup_task()", errors)
    if task is not None:
        problem = None
        deletes = len(re.findall(r"\bvTaskDelete\s*\(", task))
        jump = re.search(r"\b(?:return|goto)\b", task)
        if not squash(task).endswith(INO_WORKER_END):
            problem = "it does not end with that store and `vTaskDelete(NULL);`"
        elif deletes != 1:
            problem = f"it calls vTaskDelete {deletes} times"
        elif jump:
            problem = f"a `{jump.group(0)}` in it can end it before the store"
        else:
            at = [m.start() for m in re.finditer(r"\b__atomic_store_n\s*\(", task)][-1]
            problem = unconditional_in(task, 0, at)
            guards = pp_guards(task, 0, at)
            if problem is None and guards != ():
                problem = f"it is compiled under {' / '.join(guards or ('an unbalanced #if',))}"
        if problem:
            errors.append(f"{INO}: ble_bringup_task() ends with `__atomic_store_n(&g_ble_bringup_done, true, "
                          "__ATOMIC_RELEASE);` then its one `vTaskDelete(NULL);`, the store a statement of its "
                          "body's own outside every #if, and no `return` or `goto` in it — the finalize stage "
                          "runs only on that store, and it is where the advertising hold and the boot "
                          f"bring-up end (the F167 review, F235's review; the second verify, wave 16); here "
                          f"{problem}")
    task_span = next((sp for sp in ino_spans if sp[0] == "ble_bringup_task"), None)
    for path, src in files.items():
        blanked = ino if path == INO else blank_comments_and_strings(src)
        for m in re.finditer(r"\bg_ble_bringup_done\b", blanked):
            if path == INO:
                before = squash(blanked[max(0, m.start() - 48):m.start()])
                after = squash(blanked[m.end():m.end() + 48])
                where = enclosing_function(ino_spans, m.start())
                if before.endswith("staticvolatilebool") and after.startswith("=false;") and where is None:
                    continue                          # its definition
                if before.endswith("__atomic_load_n(&") and after.startswith(",__ATOMIC_ACQUIRE)"):
                    continue                          # an acquire read
                if before.endswith("__atomic_store_n(&") and after.startswith(",true,__ATOMIC_RELEASE);") \
                        and task_span is not None and task_span[1] <= m.start() < task_span[2] \
                        and squash(blanked[m.start():task_span[2]]).startswith(INO_WORKER_END[18:]):
                    continue                          # the worker's handoff, its end
            else:
                where = enclosing_function(named_bodies(blanked), m.start())
            errors.append(f"{path}: {where or 'file scope'}() names g_ble_bringup_done other than as its "
                          "definition, an acquire read, or the worker's handoff at its end — only "
                          "ble_bringup_task() writes it, once, last (the second verify, wave 16)")
            break
    for call, allowed in (("bringup_worker_started", ("ble_discovery_start_if_due",)),
                          ("bringup_worker_finished", ("ble_discovery_start_if_due",
                                                       "ble_bringup_finalize_if_done")),
                          ("boot_bring_up_finished", ("ble_discovery_start_if_due",
                                                      "ble_bringup_finalize_if_done"))):
        for path, src in files.items():
            if path in (BT_CPP, BT_H):
                continue
            blanked = ino if path == INO else blank_comments_and_strings(src)
            starts = call_starts(blanked, call)
            if not starts:
                continue
            pspans = ino_spans if path == INO else named_bodies(blanked)
            for at in starts:
                where = enclosing_function(pspans, at)
                if path != INO or where not in allowed:
                    errors.append(f"{path}: {where or 'file scope'}() calls bluetooth_channel::{call}() — "
                                  f"only canary_wap.ino's {', '.join(allowed)} may, on the loop task "
                                  "(the F167 review)")


# BV6 (F196, bluetooth_api.h): every answer goes out at its own length.
SIG_BT_SEND_DOC = r"\bstatic\s+inline\s+esp_err_t\s+send_doc\s*\([^)]*\)"
BT_SEND_DOC_BODY = ("Stringout;if(!out.reserve(measureJson(doc)+1)){returnsend_json_response(req,\"\");}"
                    "serializeJson(doc,out);returnsend_json_response(req,out.c_str());")


def check_bluetooth_answer_lengths(api_src: str, errors: list[str]) -> None:
    """Rule BV6 (F196): every answer bluetooth_api.h sends is serialized in
    one place, send_doc(), into a String reserved to measureJson()'s length.
    A fixed char buffer that a longer answer fills is left unterminated by
    serializeJson() and httpd_resp_sendstr() then sends whatever follows it;
    a measured String cannot be outgrown. A failed reservation answers the
    fixed allocation error rather than a truncated document."""
    code = blank_comments_and_strings(api_src)
    spans = named_bodies(code)
    body = body_of(code, SIG_BT_SEND_DOC, f"{BT_API}: send_doc()", errors)
    if body is not None and squash(body) != BT_SEND_DOC_BODY:
        errors.append(f"{BT_API}: send_doc() must measure the answer and serialize it into a String "
                      "reserved to that length, answering a failed allocation with its fixed error "
                      "(`String out; if (!out.reserve(measureJson(doc) + 1)) { return "
                      "send_json_response(req, \"...\"); } serializeJson(doc, out); return "
                      "send_json_response(req, out.c_str());`) — serializeJson() into a char array "
                      "leaves a full one unterminated and httpd_resp_sendstr() sends what follows (F196)")
    for m in re.finditer(r"\bserialize(?:Json|JsonPretty|MsgPack)\s*\(", code):
        where = enclosing_function(spans, m.start())
        if where != "send_doc":
            errors.append(f"{BT_API}: {where or 'file scope'}() serializes an answer itself — every "
                          "Bluetooth answer goes out through send_doc(), at its own measured length (F196)")


# BV8 (F210): whether Bluetooth is on, as another task reads it.
SIG_BT_READ_ENABLED = r"\bbool\s+read_enabled\s*\(\s*\)"
BT_READ_ENABLED_BODY = "returnread_settings().enabled;"
# Who reads the loop task's own flag: the commands that auto-enable.
BT_IS_ENABLED_CALLERS = ("run_command",)


def check_bluetooth_enabled_read(files: dict[str, str], errors: list[str]) -> None:
    """Rule BV8 (F210): the Start Advertising and Pair handlers decided from
    is_enabled(), the loop task's g_settings.enabled read in place on the
    httpd task, whether to bring the stack up; the loop task's commands and
    its first pass's load_saved() write it. They read read_enabled(), the
    published settings' flag (since F235 only through C3's
    waits_for_boot_bring_up(): a bring-up handler's own body reads no
    setting);
    is_enabled() is the loop task's alone: static, named by no other file,
    called only by run_command() (BV3's BT_LIVE_READERS keeps it out of the
    header and out of every handler)."""
    code = blank_comments_and_strings(files[BT_CPP])
    spans = named_bodies(code)
    body = body_of(code, SIG_BT_READ_ENABLED, f"{BT_CPP}: read_enabled()", errors)
    if body is not None and squash(body) != BT_READ_ENABLED_BODY:
        errors.append(f"{BT_CPP}: read_enabled() must be exactly `return read_settings().enabled;` — "
                      "another task reads whether Bluetooth is on from the settings the loop task "
                      "published, never its flag in place (F210)")
    decls = list(re.finditer(r"(\bstatic\s+)?\bbool\s+is_enabled\s*\(\s*\)\s*([{;])", code))
    if sum(1 for m in decls if m.group(2) == "{") != 1 or any(m.group(1) is None for m in decls):
        errors.append(f"{BT_CPP}: is_enabled() must be defined once, `static bool is_enabled()` — the "
                      "flag is the loop task's; another task calls read_enabled() (F210)")
    for m in re.finditer(r"(?<![\w:.>])is_enabled\s*\(", code):
        where = enclosing_function(spans, m.start())
        if where is not None and where not in BT_IS_ENABLED_CALLERS:
            errors.append(f"{BT_CPP}: {where}() calls is_enabled() — only "
                          f"{', '.join(BT_IS_ENABLED_CALLERS)} (the loop task's commands) reads the flag in "
                          "place; another task reads read_enabled() (F210)")
    for path, src in files.items():
        if path == BT_CPP:
            continue
        hit = re.search(r"\bbluetooth_channel::is_enabled\s*\(", blank_comments_and_strings(src))
        if hit:
            errors.append(f"{path}: names bluetooth_channel::is_enabled( — the loop task's flag; read "
                          "bluetooth_channel::read_enabled(), the published settings (F210)")


# BV9 (F191, F209): no pairing completes, and no bond stays, without the owner.
SIG_BT_PASSKEY_DISPLAY = r"\buint32_t\s+onPassKeyDisplay\s*\(\s*\)\s*override"
SIG_BT_APPLY_PASSKEY = r"\bstatic\s+void\s+apply_passkey_display\s*\([^)]*\)"
SIG_BT_OWNER_PAIRING_OPEN = r"\bstatic\s+bool\s+owner_pairing_open\s*\(\s*\)"
SIG_BT_PUBLISH_VIEWS = r"\bstatic\s+void\s+publish_views\s*\(\s*\)"
BT_PASSKEY_GATE = "if(!__atomic_load_n(&g_owner_pairing_open,__ATOMIC_ACQUIRE)){passkey+=PASSKEY_REFUSED;}"
BT_PASSKEY_POST = "Evente=make_event(BT_EV_PASSKEY_DISPLAY);"
BT_OWNER_PAIRING_OPEN_BODY = ("returng_settings.enabled&&g_settings.allow_pairing&&"
                              "(g_pairing.state==PAIR_INITIATED||g_pairing.state==PAIR_PIN_DISPLAYED);")
BT_OWNER_PAIRING_PUBLISH = "__atomic_store_n(&g_owner_pairing_open,owner_pairing_open(),__ATOMIC_RELEASE);"
BT_APPLY_PASSKEY_GATE = "if(e.u.passkey.pin>=PASSKEY_REFUSED||!owner_pairing_open()){g_passkey_refused_count++;return;}"
# Who names the passkey gate's flag: its writer (publish_views(), the loop
# task) and its reader (onPassKeyDisplay(), the NimBLE host task).
BT_OWNER_PAIRING_NAMERS = ("publish_views", "onPassKeyDisplay")
# A pairing's digits, as an identifier names them (blanked code: the
# literals are gone, so a message saying "PIN" is not one).
BT_DIGITS_NAME = r"\b(?:pin|pin_\w+|\w+_pin|passkey\w*|\w+_passkey|numcmp)\b"
BT_UNAUTH_BRANCH = "}elseif(link.bonded){"
BT_UNAUTH_FORGET = "forget_bond(NimBLEAddress(link.id_address))"
BT_UNAUTH_DROP = "g_server->disconnect(link.handle);"
SIG_BT_REFRESH_BONDS = r"\bstatic\s+void\s+refresh_bond_store\s*\(\s*\)"
SIG_BT_PUBLISH_STATUS = r"\bstatic\s+void\s+publish_status_view\s*\(\s*\)"
# The bond store's counts (F209): written by the one function that reads the
# store, read by the status view's builder.
BT_BOND_COUNTS = ("g_bonds_held", "g_bonds_unlisted")
# The roads F191's and F209's review found.
SIG_BT_COMPARISON_OPEN = r"\bstatic\s+bool\s+owner_comparison_open\s*\(\s*\)"
BT_COMPARISON_OPEN_BODY = ("returng_settings.enabled&&g_settings.allow_pairing&&(g_pairing.state==PAIR_INITIATED||"
                           "g_pairing.state==PAIR_PIN_DISPLAYED||g_pairing.state==PAIR_CONFIRMING);")
SIG_BT_CONFIRM_PASSKEY_APPLY = r"\bstatic\s+void\s+apply_confirm_passkey\s*\([^)]*\)"
BT_COMPARISON_GATE = ("if(!owner_comparison_open()){NimBLEDevice::injectConfirmPasskey(*e.u.passkey.conn,false);"
                      "deletee.u.passkey.conn;g_passkey_refused_count++;return;}")
BT_PENDING_TAKE = "g_pending_pair_info=e.u.passkey.conn;"
BT_UNAUTH_DROP_WHOLE = "if(g_server){g_server->disconnect(link.handle);}"
BT_UNAUTH_KEEP = "keep_for_retry(link.id_address);"
BT_UNAUTH_DROP_ENTRIES = "drop_entries_without_bond();"
BT_AUTH_DROP_KEPT = "drop_kept(link.id_address);"
SIG_BT_REFUSE_LINKS = r"\bstatic\s+void\s+refuse_unauthenticated_links\s*\(\s*\)"
BT_REFUSE_LINKS_TEST = ("if(live.getConnHandle()!=handle||!live.isBonded()||live.isAuthenticated())continue;"
                        "if(!NimBLEDevice::isBonded(live.getIdAddress()))continue;"
                        "apply_auth_complete(link_event(BT_EV_AUTH_COMPLETE,live));")
BT_REFUSE_LINKS_CALLERS = ("reconcile_link", "adopt_init_result")
SIG_BT_STORE_FULL = r"\bstatic\s+void\s+apply_store_full\s*\([^)]*\)"
SIG_BT_RECONCILE_LINK = r"\bstatic\s+void\s+reconcile_link\s*\(\s*\)"
BT_DIRTY_MARK = "g_bond_store_dirty=true;"
BT_KEPT_CLEAR = "g_kept_bond_count=0;"
BT_KEPT_NAMERS = ("keep_for_retry", "drop_kept", "retry_kept_bonds", "reconcile_link")
BT_DISPATCH_NO_OWNER_PASSKEY = "return1000000u+esp_random()%1000000;"
SIG_DISPATCH_PASSKEY = r"\buint32_t\s+onPassKeyDisplay\s*\(\s*\)\s*override"


def check_bluetooth_owner_pairing(files: dict[str, str], errors: list[str]) -> None:
    """Rule BV9 (F191, F209): a passkey is shown only inside the owner's
    pairing mode, and NimBLE is handed one it cannot use outside it; no
    pairing digits reach the health log; a bond that is not authenticated is
    deleted with the radio quiet and its link dropped; the bond store's count
    beside the list is the loop task's, read by the status view."""
    code = blank_comments_and_strings(files[BT_CPP])
    spans = named_bodies(code)
    m = re.search(r"\bstatic\s+const\s+uint32_t\s+PASSKEY_REFUSED\s*=\s*(\d+)\s*;", code)
    if m is None or int(m.group(1)) < 1000000 or len(re.findall(r"\bPASSKEY_REFUSED\s*=", code)) != 1:
        errors.append(f"{BT_CPP}: PASSKEY_REFUSED must be defined once, `static const uint32_t PASSKEY_REFUSED "
                      "= <at least 1000000>;` — added to a refused passkey, it puts it past what "
                      "ble_sm_inject_io() takes (999999), so NimBLE sets no key (F191)")
    shown = body_of(code, SIG_BT_PASSKEY_DISPLAY, f"{BT_CPP}: ServerCallbacks::onPassKeyDisplay()", errors)
    if shown is not None:
        sq = squash(shown)
        gate, post = sq.find(BT_PASSKEY_GATE), sq.find(BT_PASSKEY_POST)
        mod = sq.find("passkey%=1000000;")
        if sq.count(BT_PASSKEY_GATE) != 1 or not (0 <= mod < gate < post) or \
                not sq.endswith("returnpasskey;") or sq.count("return") != 1:
            errors.append(f"{BT_CPP}: onPassKeyDisplay() must refuse, after its six random digits and "
                          f"before it posts them, with `{BT_PASSKEY_GATE}`, and return that passkey — "
                          "outside the owner's pairing mode NimBLE must get a passkey it cannot use, so "
                          "the pairing cannot complete for whoever reads the digits (F191)")
    body = body_of(code, SIG_BT_OWNER_PAIRING_OPEN, f"{BT_CPP}: owner_pairing_open()", errors)
    if body is not None and squash(body) != BT_OWNER_PAIRING_OPEN_BODY:
        errors.append(f"{BT_CPP}: owner_pairing_open() must be exactly `return g_settings.enabled && "
                      "g_settings.allow_pairing && (g_pairing.state == PAIR_INITIATED || g_pairing.state == "
                      "PAIR_PIN_DISPLAYED);` — the owner's pairing mode, Bluetooth on, new pairings "
                      "allowed, no Numeric Comparison pending (F191)")
    body = body_of(code, SIG_BT_PUBLISH_VIEWS, f"{BT_CPP}: publish_views()", errors)
    if body is not None and squash(body).count(BT_OWNER_PAIRING_PUBLISH) != 1:
        errors.append(f"{BT_CPP}: publish_views() must hand the passkey gate to the NimBLE host task, "
                      f"`{BT_OWNER_PAIRING_PUBLISH}`, once — a pairing mode a command starts or ends "
                      "reaches onPassKeyDisplay() before the command's handler answers (F191)")
    for hit in re.finditer(r"\bg_owner_pairing_open\b", code):
        where = enclosing_function(spans, hit.start())
        if where is not None and where not in BT_OWNER_PAIRING_NAMERS:
            errors.append(f"{BT_CPP}: {where}() names g_owner_pairing_open — publish_views() stores it "
                          "(release) and onPassKeyDisplay() reads it (acquire), nothing else (F191)")
            break
    body = body_of(code, SIG_BT_APPLY_PASSKEY, f"{BT_CPP}: apply_passkey_display()", errors)
    if body is not None and not squash(body).startswith(BT_APPLY_PASSKEY_GATE):
        errors.append(f"{BT_CPP}: apply_passkey_display() must start `{BT_APPLY_PASSKEY_GATE}` — a passkey "
                      "the host task refused, or one whose pairing mode ended before this pass, is shown "
                      "nowhere: not in the dashboard's PIN box, not in the status (F191)")
    for call in re.finditer(r"(?<![\w:.>])log_health\s*\(", code):
        close = matching_paren(code, call.end() - 1)
        args = code[call.end():close] if close > 0 else ""
        named = re.search(BT_DIGITS_NAME, args)
        if named:
            where = enclosing_function(spans, call.start())
            errors.append(f"{BT_CPP}: {where or 'file scope'}() logs `{named.group(0)}` — no pairing digits "
                          "reach the health log: it is durable (the SD card's /HEALTH, the BLE log "
                          "export), and its reader could complete a Passkey Entry (F191)")
            break
    auth = body_of(code, SIG_BT_AUTH_COMPLETE, f"{BT_CPP}: apply_auth_complete()", errors)
    if auth is not None:
        sq = squash(auth)
        at = sq.find(BT_UNAUTH_BRANCH)
        branch = sq[at:] if at >= 0 else ""
        branch = branch[:branch.find("}else{")] if "}else{" in branch else branch
        authed = sq[:at] if at >= 0 else sq
        if sq.count(BT_UNAUTH_BRANCH) != 1 or not sq.startswith("constLinkEvent&link=e.u.link;") or \
                BT_UNAUTH_FORGET not in branch or BT_UNAUTH_DROP not in branch or \
                re.search(r"\bdeleteBond\s*\(|\bble_gap_unpair\w*\s*\(", auth) or "forget_bond(" in authed:
            errors.append(f"{BT_CPP}: apply_auth_complete() must answer a bond that is not authenticated "
                          f"(`{BT_UNAUTH_BRANCH}` after the authenticated block) with "
                          f"`{BT_UNAUTH_FORGET}` (the radio quiet, as Remove does: a bare deleteBond() is "
                          f"refused busy for a bond with the phone's IRK) and `{BT_UNAUTH_DROP}`, and take no "
                          "authenticated bond: a Just Works bond no owner asked for held a slot of the "
                          "3-bond store, and three of them refused the owner (F191, F209)")
    for hit in re.finditer(r"(?<![\w:.>])refresh_bond_store\s*\(", code):
        where = enclosing_function(spans, hit.start())
        if where is not None and where != "publish_status_view":
            errors.append(f"{BT_CPP}: {where}() calls refresh_bond_store() — the status view's builder "
                          "reads the store, on the loop task, as it publishes (F209)")
            break
    for name in BT_BOND_COUNTS:
        for hit in re.finditer(r"\b" + name + r"\b", code):
            where = enclosing_function(spans, hit.start())
            if where not in (None, "refresh_bond_store", "publish_status_view"):
                errors.append(f"{BT_CPP}: {where}() names {name} — refresh_bond_store() counts the store "
                              "and publish_status_view() shows the count; a reader on another task reads "
                              "the published status (F209)")
                break
    builder = body_of(code, SIG_BT_PUBLISH_STATUS, f"{BT_CPP}: publish_status_view()", errors)
    if builder is not None and squash(builder).count("refresh_bond_store();") != 1:
        errors.append(f"{BT_CPP}: publish_status_view() must call refresh_bond_store() once, with the stack "
                      "up — the count GET /api/bluetooth shows beside the list (F209)")
    check_bluetooth_owner_pairing_review(files, code, spans, errors)


def check_bluetooth_owner_pairing_review(files: dict[str, str], code: str, spans: list, errors: list[str]) -> None:
    """Rule BV9's second half: the roads F191's and F209's review found."""
    body = body_of(code, SIG_BT_COMPARISON_OPEN, f"{BT_CPP}: owner_comparison_open()", errors)
    if body is not None and squash(body) != BT_COMPARISON_OPEN_BODY:
        errors.append(f"{BT_CPP}: owner_comparison_open() must be exactly `return g_settings.enabled && "
                      "g_settings.allow_pairing && (g_pairing.state == PAIR_INITIATED || g_pairing.state == "
                      "PAIR_PIN_DISPLAYED || g_pairing.state == PAIR_CONFIRMING);` — a Numeric Comparison asks "
                      "the owner only inside pairing mode, with new pairings allowed (F191's review)")
    body = body_of(code, SIG_BT_CONFIRM_PASSKEY_APPLY, f"{BT_CPP}: apply_confirm_passkey()", errors)
    if body is not None:
        sq = squash(body)
        gate, take = sq.find(BT_COMPARISON_GATE), sq.find(BT_PENDING_TAKE)
        if sq.count(BT_COMPARISON_GATE) != 1 or not (0 <= gate < take):
            errors.append(f"{BT_CPP}: apply_confirm_passkey() must answer no, before it takes the pending copy, "
                          f"with `{BT_COMPARISON_GATE}` — outside pairing mode a stranger's comparison waited "
                          "for the owner's confirm, which bonded and listed it with new pairings off (F191's review)")
    auth = body_of(code, SIG_BT_AUTH_COMPLETE, f"{BT_CPP}: apply_auth_complete()", errors)
    if auth is not None:
        sq = squash(auth)
        at = sq.find(BT_UNAUTH_BRANCH)
        open_at = at + len(BT_UNAUTH_BRANCH) - 1
        end = close_brace(sq, open_at) if at >= 0 else -1
        branch = sq[open_at + 1:end] if end > 0 else ""   # the whole branch, its inner blocks too
        authed = sq[:at] if at >= 0 else sq
        drops = re.findall(r"g_server->disconnect\(", branch)
        if branch.count(BT_UNAUTH_DROP_WHOLE) != 1 or len(drops) != 1 or "g_pairing" in branch or \
                BT_UNAUTH_KEEP not in branch or BT_UNAUTH_DROP_ENTRIES not in branch or \
                authed.count(BT_AUTH_DROP_KEPT) != 1:
            errors.append(f"{BT_CPP}: apply_auth_complete()'s unauthenticated branch must drop the link as "
                          f"`{BT_UNAUTH_DROP_WHOLE}` (on no other condition, by no other handle: a stranger beside "
                          "the recorded link stayed connected, or the owner's link went), name no g_pairing (that "
                          f"bond was never the owner's pairing), say `{BT_UNAUTH_DROP_ENTRIES}` and "
                          f"`{BT_UNAUTH_KEEP}`, and the authenticated block `{BT_AUTH_DROP_KEPT}` (F191's review)")
    body = body_of(code, SIG_BT_REFUSE_LINKS, f"{BT_CPP}: refuse_unauthenticated_links()", errors)
    if body is not None and squash(body).count(BT_REFUSE_LINKS_TEST) != 1:
        errors.append(f"{BT_CPP}: refuse_unauthenticated_links() must skip exactly a link that is not the stack's, "
                      "not bonded, authenticated, or whose bond is gone, and apply the rest as their report, "
                      f"`{BT_REFUSE_LINKS_TEST}` — a bond whose report never came is refused as F191 refuses one, "
                      "and nothing else is applied (F191's review)")
    calls = [enclosing_function(spans, m.start())
             for m in re.finditer(r"(?<![\w:.>])refuse_unauthenticated_links\s*\(", code)]
    calls = sorted(c for c in calls if c is not None)
    adopt = body_of(code, SIG_BT_ADOPT, f"{BT_CPP}: adopt_init_result()", errors)
    adopt_sq = squash(adopt) if adopt is not None else ""
    owner_at = adopt_sq.find("ble_server_dispatch::set_owner(")
    refuse_at = adopt_sq.find("refuse_unauthenticated_links();")
    if calls != sorted(BT_REFUSE_LINKS_CALLERS) or not (0 <= owner_at < refuse_at):
        errors.append(f"{BT_CPP}: refuse_unauthenticated_links() is called by {calls} — once by reconcile_link() "
                      "(a report the full queue dropped) and once by adopt_init_result() after its set_owner( (a "
                      "report that reached the dispatcher before the channel owned it) (F191's review)")
    calls = [enclosing_function(spans, m.start()) for m in re.finditer(r"(?<![\w:.>])retry_kept_bonds\s*\(", code)]
    update = body_of(code, SIG_UPDATE, f"{BT_CPP}: update()", errors)
    if [c for c in calls if c is not None] != ["update"] or update is None or \
            squash(update).count("retry_kept_bonds(now);") != 1:
        errors.append(f"{BT_CPP}: retry_kept_bonds(now); must be update()'s, once — a bond the stack kept is asked "
                      "again each minute (F191's review)")
    for sig, what, want in ((SIG_BT_STORE_FULL, "apply_store_full()", (BT_DIRTY_MARK,)),
                            (SIG_BT_RECONCILE_LINK, "reconcile_link()", (BT_DIRTY_MARK, BT_KEPT_CLEAR))):
        body = body_of(code, sig, f"{BT_CPP}: {what}", errors)
        if body is not None and any(w not in squash(body) for w in want):
            errors.append(f"{BT_CPP}: {what} must say {' and '.join('`' + w + '`' for w in want)} — the bond count "
                          "is read again when the full store refuses a pairing or a link event was dropped, and a "
                          "dropped event ends every kept bond's retry (its report may have been an owner's "
                          "pairing) (F209's and F191's review)")
    for name in ("g_kept_bonds", "g_kept_bond_count"):
        for hit in re.finditer(r"\b" + name + r"\b", code):
            where = enclosing_function(spans, hit.start())
            if where is not None and where not in BT_KEPT_NAMERS:
                errors.append(f"{BT_CPP}: {where}() names {name} — only {', '.join(BT_KEPT_NAMERS)} may (F191's "
                              "review)")
                break
    dispatch = files.get(BT_DISPATCH_H)
    if dispatch is not None:
        body = body_of(blank_comments_and_strings(dispatch), SIG_DISPATCH_PASSKEY,
                       f"{BT_DISPATCH_H}: Dispatcher::onPassKeyDisplay()", errors)
        if body is not None and not squash(body).endswith(BT_DISPATCH_NO_OWNER_PASSKEY):
            errors.append(f"{BT_DISPATCH_H}: Dispatcher::onPassKeyDisplay() with no pairing owner must return "
                          "`1000000u + esp_random() % 1000000;` — six digits nobody is shown are still six digits "
                          "a guess can match; past 999999 NimBLE sets no key (F191's review)")
    for path, src in files.items():
        if re.search(r"(?<![\w])setSecurityPasskey\s*\(", blank_comments_and_strings(src)):
            errors.append(f"{path}: calls setSecurityPasskey( — NimBLE-Arduino calls onPassKeyDisplay() only while "
                          "the static passkey is its default 123456 (NimBLEServer.cpp, 2.3.8 and 2.5.0), so a static "
                          "passkey is handed to every Passkey Entry, in or out of the owner's pairing mode, and F191's "
                          "refusal never runs (F191's review)")
            break


# BV10 (F259, F260): safe mode says so; a late bring-up says what it left out.
SIG_BT_SKIP_BOOT = r"\bvoid\s+skip_boot_bring_up_for_safe_mode\s*\(\s*\)"
SIG_BT_BOOT_SKIPPED = r"\bbool\s+boot_bring_up_skipped\s*\(\s*\)"
BT_SKIP_STORE = "__atomic_store_n(&g_boot_bring_up_skipped,true,__ATOMIC_RELEASE);"
BT_SKIPPED_LOAD = "return__atomic_load_n(&g_boot_bring_up_skipped,__ATOMIC_ACQUIRE);"
SIG_BT_SERVICE_REGISTERED = r"\bvoid\s+boot_service_registered\s*\(\s*uint8_t\s+\w+\s*\)"
BT_SERVICE_REGISTERED_BODY = "__atomic_fetch_or(&g_boot_services_registered,service,__ATOMIC_RELEASE);"
SIG_BT_LATE = r"\bstatic\s+bool\s+late_bring_up\s*\(\s*\)"
BT_LATE_BODY = "returng_initialized&&boot_bring_up_tried()&&!g_boot_stack_up;"
SIG_BT_NOTE_LATE = r"\bstatic\s+void\s+note_late_bring_up\s*\(\s*\)"
# What this build's worker registers, from the macros its blocks compile under.
BT_BOOT_SERVICES_DEF = ("staticconstuint8_tBOOT_SERVICES="
                        "#ifdefined(FEATURE_BLE_STATUS)&&FEATURE_BLE_STATUSBT_BOOT_SVC_STATUS|#endif"
                        "#ifdefined(FEATURE_BLE)&&FEATURE_BLEBT_BOOT_SVC_DISCOVERY|#endif0;")
# Who names each flag of the late bring-up's.
BT_LATE_NAMERS = {"g_boot_bring_up_skipped": ("skip_boot_bring_up_for_safe_mode", "boot_bring_up_skipped"),
                  "g_boot_services_registered": ("boot_service_registered", "boot_services_missing"),
                  "g_boot_stack_up": ("boot_bring_up_finished", "late_bring_up")}
INO_SKIP = "bluetooth_channel::skip_boot_bring_up_for_safe_mode();"
INO_SAFE_GATE = r"\bif\s*\(\s*!\s*in_safe_mode\s*\)\s*\{\s*g_ble_discovery_ready\s*="
# The worker's two registrations, each in the true block of the init that
# registered it: (the if's condition head, the statement, a FEATURE_BLUETOOTH
# guard needed inside that block).
INO_REGISTRATIONS = (
    (r"\bif\s*\(\s*ble_status\s*::\s*init\s*\(",
     "bluetooth_channel::boot_service_registered(bluetooth_channel::BT_BOOT_SVC_STATUS);", False),
    (r"\bif\s*\(\s*ble_manager\s*::\s*init\s*\(",
     "bluetooth_channel::boot_service_registered(bluetooth_channel::BT_BOOT_SVC_DISCOVERY);", True),
)


def stmt_re(stmt: str) -> str:
    """A regex for the squashed statement `stmt`, any whitespace between tokens."""
    return r"\s*".join(re.escape(tok) for tok in re.findall(r"\w+|[^\w\s]", stmt))


def true_block(code: str, head: re.Match) -> tuple[int, int] | None:
    """The `{ ... }` an `if (` whose condition starts at `head` runs when
    true, as (just after its `{`, its `}`), or None."""
    paren = code.find("(", head.start())
    close = matching_paren(code, paren)
    if close < 0:
        return None
    after = code[close + 1:].lstrip()
    if not after.startswith("{"):
        return None
    open_at = close + 1 + (len(code[close + 1:]) - len(after))
    end = close_brace(code, open_at)
    return None if end < 0 else (open_at + 1, end)


def check_bluetooth_late_bring_up(files: dict[str, str], errors: list[str]) -> None:
    """Rule BV10 (F259, F260): a safe-mode boot marks its bring-up skipped,
    so a tap is told Bluetooth stays off; the sketch's worker reports each
    service it registered, and a bring-up the worker did not perform says
    which are missing, registering none on a server that already started."""
    code = blank_comments_and_strings(files[BT_CPP])
    spans = named_bodies(code)
    for sig, want, what in ((SIG_BT_SKIP_BOOT, BT_SKIP_STORE, "skip_boot_bring_up_for_safe_mode()"),
                            (SIG_BT_BOOT_SKIPPED, BT_SKIPPED_LOAD, "boot_bring_up_skipped()"),
                            (SIG_BT_SERVICE_REGISTERED, BT_SERVICE_REGISTERED_BODY, "boot_service_registered()"),
                            (SIG_BT_LATE, BT_LATE_BODY, "late_bring_up()")):
        body = body_of(code, sig, f"{BT_CPP}: {what}", errors)
        if body is not None and squash(body) != want:
            errors.append(f"{BT_CPP}: {what} must be exactly `{want}` — any task's flag, or the loop task's "
                          "reading of what the boot bring-up left (F259, F260)")
    for name, allowed in BT_LATE_NAMERS.items():
        for hit in re.finditer(r"\b" + name + r"\b", code):
            where = enclosing_function(spans, hit.start())
            if where is not None and where not in allowed:
                errors.append(f"{BT_CPP}: {where}() names {name} — only {', '.join(allowed)} may (F259, F260)")
                break
    if squash(code).count(BT_BOOT_SERVICES_DEF) != 1:
        errors.append(f"{BT_CPP}: BOOT_SERVICES must be built from the macros the sketch's worker compiles "
                      "its registrations under (FEATURE_BLE_STATUS for ble_status, FEATURE_BLE for "
                      "ble_manager's) — a service the worker registers that the channel does not expect is "
                      "never reported missing (F260)")
    calls = [enclosing_function(spans, m.start()) for m in re.finditer(r"(?<![\w:.>])note_late_bring_up\s*\(", code)]
    if [c for c in calls if c is not None] != ["adopt_init_result"]:
        errors.append(f"{BT_CPP}: note_late_bring_up() is called by {calls} — once, by adopt_init_result(): the "
                      "loop task adopts a stack once per boot, so a late bring-up says what it left out once (F260)")
    note = body_of(code, SIG_BT_NOTE_LATE, f"{BT_CPP}: note_late_bring_up()", errors)
    if note is not None and re.search(r"\bcreateService\s*\(|(?<![\w.>])\w*::\s*init\s*\(|\bstart\s*\(", note):
        errors.append(f"{BT_CPP}: note_late_bring_up() registers or starts something — a late bring-up says "
                      "what it left out; nothing registers the worker's services on a server that already "
                      "started (F167's case, F260)")
    ino = blank_comments_and_strings(files.get(INO, ""))
    for path, src in files.items():
        if path in (INO, BT_CPP):
            continue
        hit = re.search(r"\b(skip_boot_bring_up_for_safe_mode|boot_service_registered)\s*\(",
                        blank_comments_and_strings(src))
        if hit and not path.endswith("bluetooth_channel.h"):
            errors.append(f"{path}: calls {hit.group(1)}() — the sketch's setup() and boot worker do, in "
                          "canary_wap.ino, and nothing else (F259, F260)")
    setup = the_body(ino, SIG_SETUP, f"{INO}: setup()", errors)
    if setup is not None:
        body = ino[setup[0]:setup[1]]
        hits = list(re.finditer(stmt_re(INO_SKIP), body))
        gate = re.search(INO_SAFE_GATE, body)
        ok = False
        if len(hits) == 1 and gate is not None:
            blk = true_block(body, gate)
            rest = body[blk[1] + 1:] if blk else ""
            other = re.match(r"\s*else\s*\{", rest)
            if blk and other:
                start = blk[1] + 1 + other.end()
                end = close_brace(body, start - 1)
                pos = hits[0].start()
                guards = pp_guards(body, start, pos)
                ok = start <= pos < end and guards == ("if FEATURE_BLUETOOTH",) and \
                    unconditional_in(body, start, pos) is None
        if not ok:
            errors.append(f"{INO}: setup() must say `{INO_SKIP}` once, a statement of the `else` of its "
                          "Bluetooth gate (`if (!in_safe_mode) { g_ble_discovery_ready = true; ...`) under `#if "
                          "FEATURE_BLUETOOTH` alone — a safe-mode boot never marks its bring-up over, and "
                          "without it a tap is told Bluetooth starts within five minutes (F259)")
    worker = the_body(ino, SIG_BT_BRINGUP_TASK, f"{INO}: ble_bringup_task()", errors)
    if worker is not None:
        body = ino[worker[0]:worker[1]]
        for head_re, stmt, guarded in INO_REGISTRATIONS:
            hits = list(re.finditer(stmt_re(stmt), ino))
            head = re.search(head_re, body)
            blk = true_block(body, head) if head else None
            ok = len(hits) == 1 and blk is not None
            if ok:
                pos = hits[0].start() - worker[0]
                guards = pp_guards(body, blk[0], pos)
                ok = blk[0] <= pos < blk[1] and unconditional_in(body, blk[0], pos) is None and \
                    guards == (("if FEATURE_BLUETOOTH",) if guarded else ())
            if not ok:
                errors.append(f"{INO}: ble_bringup_task() must say `{stmt}` once, a statement of the block "
                              f"that runs when its init succeeded{' (under #if FEATURE_BLUETOOTH alone)' if guarded else ''}"
                              " — the channel reports a service missing until the worker says it is on the "
                              "server (F260)")


def check_bluetooth_views(files: dict[str, str], errors: list[str]) -> None:
    """Rules BV1..BV10 and BD1: the Bluetooth channel's settings enable (F144),
    the NimBLE host task's events (F143), the status routes' reads (F138),
    the server callbacks' one dispatcher (F171), the bring-up's hand-over
    and its worker (F167), the bond store (F189), the answers' lengths (F196),
    whether Bluetooth is on as another task reads it (F210), no pairing or
    bond without the owner (F191, F209), and what safe mode and a late
    bring-up say (F259, F260)."""
    if BT_API not in files or BT_CPP not in files:
        errors.append(f"{SKETCH}: the Bluetooth channel's sources ({BT_API}, {BT_CPP}) are missing")
        return
    check_bluetooth_settings_enable(files[BT_API], files[BT_CPP], errors)
    check_bluetooth_callbacks(files[BT_CPP], errors)
    check_bluetooth_reads(files, errors)
    check_bluetooth_dispatch(files, errors)
    check_bluetooth_bringup(files[BT_CPP], errors)
    check_bluetooth_bond_store(files, errors)
    check_bluetooth_answer_lengths(files[BT_API], errors)
    check_bluetooth_bringup_worker(files, errors)
    check_bluetooth_enabled_read(files, errors)
    check_bluetooth_owner_pairing(files, errors)
    check_bluetooth_late_bring_up(files, errors)


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
    ("serve_reinit() marks a request served that a save kept from opening (F289's review)",
     on("mqtt", SIG_SERVE, r"\n[ \t]*if\s*\(busy\)\s*return;[^\n]*", "")),
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
     raw_other(CHIRP_CPP, "static bool unmute(MuteRefusal* why) {\n", "bool unmute(MuteRefusal* why) {\n")),
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
     raw_other(BT_H, "bool is_advertising();", "void disable();\nbool is_advertising();")),
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
              r"if\s*\(!bring_up\(\)\)\s*\{[^}]*\}", "")),
    ("the Bluetooth pair handler no longer brings the stack up",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"),
              r"if\s*\(!bring_up\(\)\)\s*\{[^}]*\}", "")),
    # Rule C3, the bring-up whenever the stack is down (F235).
    ("the advertise handler brings the stack up only while the setting says off (F235's premise)",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"), r"if\s*\(!bring_up\(\)\)",
              "if (!bluetooth_channel::read_enabled() && !bring_up())")),
    ("the pair handler brings the stack up only while the setting says off (F235's premise)",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"), r"if\s*\(!bring_up\(\)\)",
              "if (!bluetooth_channel::read_enabled() && !bring_up())")),
    ("the enable handler brings the stack up only while the published status says it is down",
     on_other(BT_API, api_handler("handle_bluetooth_enable"), r"if\s*\(!bring_up\(\)\)",
              "bluetooth_channel::BluetoothStatus st; bluetooth_channel::read_status(&st);\n"
              "  if (st.state == bluetooth_channel::BT_DISABLED && !bring_up())")),
    ("the pair handler decides its bring-up from the published settings",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"), r"if\s*\(!bring_up\(\)\)",
              "if (!bluetooth_channel::read_settings().enabled && !bring_up())")),
    ("the advertise handler reads the published status before its bring-up",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"), r"(if\s*\(!bring_up\(\)\))",
              r"bluetooth_channel::BluetoothStatus st;\n  bluetooth_channel::read_status(&st);\n  \1")),
    ("the advertise handler submits before its bring-up",
     lambda s: on_other(BT_API, api_handler("handle_bluetooth_advertise_start"),
                        r"(if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)[^;]*;)",
                        r'\1\n  if (!bring_up()) { return send_bring_up_refused(req, "x"); }')(
         on_other(BT_API, api_handler("handle_bluetooth_advertise_start"),
                  r"if\s*\(!bring_up\(\)\)\s*\{[^}]*\}", "")(s))),
    ("the enable handler answers a refused bring-up without the refusal's kind (send_bt_error)",
     on_other(BT_API, api_handler("handle_bluetooth_enable"), r"return\s+send_bring_up_refused\(req,",
              "return send_bt_error(req,")),
    ("the advertise handler answers a refused bring-up without the refusal's kind (send_bt_error)",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"),
              r"return\s+send_bring_up_refused\(req,", "return send_bt_error(req,")),
    ("the pair handler answers a bring-up in flight as a failure",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"),
              r"return\s+send_bring_up_refused\(req,\s*(\"[^\"]*\")\)", r"return send_error(req, \1)")),
    ("the settings POST answers a refused bring-up without the refusal's kind (send_bt_error)",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"(!bring_up\(\)\)\s*\{\s*)return\s+send_bring_up_refused\(req,", r"\1return send_bt_error(req,")),
    ("bring_up() decides from the setting",
     on_other(BT_API, SIG_BRING_UP, r"bluetooth_channel::is_initialized\(\)", "bluetooth_channel::read_enabled()")),
    ("bring_up() never retries a bring-up that failed",
     on_other(BT_API, SIG_BRING_UP, r"\s*\|\|\s*bluetooth_channel::init\(\)", "")),
    ("a handler calls bring_up() outside a guard",
     on_other(BT_API, api_handler("handle_bluetooth_disable"), r"(bluetooth_channel::Result r;)",
              r"(void)bring_up();\n  \1")),
    ("a handler returns bring_up() unguarded",
     on_other(BT_API, api_handler("handle_bluetooth_scan_clear"), r"(bluetooth_channel::Result r;)",
              r"if (false) return bring_up() ? ESP_OK : ESP_FAIL;\n  \1")),
    ("send_bring_up_refused() never answers still starting",
     on_other(BT_API, SIG_SEND_BRING_UP_REFUSED,
              r"if\s*\(bluetooth_channel::bring_up_starting\(\)\)\s*\{[^}]*\}", "")),
    ("send_bring_up_refused() answers still starting with 200",
     on_other(BT_API, SIG_SEND_BRING_UP_REFUSED, r"httpd_resp_set_status\(req,\s*http_status_line\(503\)\);",
              "")),
    ("send_bring_up_refused() answers still starting as busy (409)",
     on_other(BT_API, SIG_SEND_BRING_UP_REFUSED, r"http_status_line\(503\)", "http_status_line(409)")),
    ("send_bring_up_refused() answers a refusal without its name",
     on_other(BT_API, SIG_SEND_BRING_UP_REFUSED, r"return\s+send_bt_error\(req,\s*fallback\);",
              "return send_error(req, fallback);")),
    ("bring_up_starting() reads only the latch",
     on_other(BT_CPP, SIG_BT_BRING_UP_STARTING, r"return\s+init_running\(\)\s*\|\|\s*is_initialized\(\);",
              "return init_running();")),
    ("bring_up_starting() reads only the stack",
     on_other(BT_CPP, SIG_BT_BRING_UP_STARTING, r"return\s+init_running\(\)\s*\|\|\s*is_initialized\(\);",
              "return is_initialized();")),
    ("bring_up_starting() reads the stack before the latch",
     on_other(BT_CPP, SIG_BT_BRING_UP_STARTING, r"return\s+init_running\(\)\s*\|\|\s*is_initialized\(\);",
              "return is_initialized() || init_running();")),
    # Rule C3, the boot bring-up's go (the F235 review): a default device's
    # Start Advertising and Pair brought NimBLE up in the join window and in
    # safe mode, and beside the worker's own NimBLEDevice::init() on FULL.
    ("the advertise handler brings the stack up before the sketch's boot bring-up",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"),
              r"if\s*\(waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")),
    ("the pair handler brings the stack up before the sketch's boot bring-up",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"),
              r"if\s*\(waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")),
    ("the pair handler waits for the boot bring-up after its own bring-up",
     lambda s: on_other(BT_API, api_handler("handle_bluetooth_pair_start"),
                        r"(if\s*\(!bring_up\(\)\)\s*\{[^}]*\})",
                        r"\1\n  if (waits_for_boot_bring_up()) { return send_boot_bring_up_pending(req); }")(
         on_other(BT_API, api_handler("handle_bluetooth_pair_start"),
                  r"if\s*\(waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")(s))),
    ("the advertise handler answers the boot wait as a refusal",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"),
              r"return\s+send_boot_bring_up_pending\(req\);", "return send_bring_up_refused(req, \"x\");")),
    ("waits_for_boot_bring_up() lets a device saved off bring the stack up early again (F259)",
     on_other(BT_API, SIG_WAITS_FOR_BOOT, r"(!bluetooth_channel::boot_bring_up_tried\(\))",
              r"\1 && bluetooth_channel::read_enabled()")),
    ("waits_for_boot_bring_up() waits whether or not the boot bring-up is over",
     on_other(BT_API, SIG_WAITS_FOR_BOOT, r"\s*&&\s*!bluetooth_channel::boot_bring_up_tried\(\)", "")),
    ("waits_for_boot_bring_up() waits with the stack up",
     on_other(BT_API, SIG_WAITS_FOR_BOOT, r"!bluetooth_channel::is_initialized\(\)\s*&&\s*", "")),
    ("waits_for_boot_bring_up() waits only while the worker has not started (F259)",
     on_other(BT_API, SIG_WAITS_FOR_BOOT, r"!bluetooth_channel::boot_bring_up_tried\(\)",
              "!bluetooth_channel::boot_bring_up_started()")),
    # Sweep F259: Enable and the settings POST turning Bluetooth on wait too,
    # and safe mode says so.
    ("the enable handler brings the stack up before the sketch's boot bring-up (F259)",
     on_other(BT_API, api_handler("handle_bluetooth_enable"),
              r"if\s*\(waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")),
    ("the enable handler waits for the boot bring-up after its own bring-up (F259)",
     lambda s: on_other(BT_API, api_handler("handle_bluetooth_enable"),
                        r"(if\s*\(!bring_up\(\)\)\s*\{[^}]*\})",
                        r"\1\n  if (waits_for_boot_bring_up()) { return send_boot_bring_up_pending(req); }")(
         on_other(BT_API, api_handler("handle_bluetooth_enable"),
                  r"if\s*\(waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")(s))),
    ("the settings POST brings the stack up before the sketch's boot bring-up (F259)",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"if\s*\(\(cmd\.set_mask\s*&\s*bluetooth_channel::BT_SET_ENABLED\)\s*&&\s*settings\.enabled"
              r"\s*&&\s*waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")),
    ("the settings POST waits after its bring-up (F259)",
     lambda s: on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
                        r"(if\s*\(\(cmd\.set_mask[^{]*!bring_up\(\)\)\s*\{[^}]*\})",
                        r"\1\n  if ((cmd.set_mask & bluetooth_channel::BT_SET_ENABLED) && settings.enabled && "
                        r"waits_for_boot_bring_up()) { return send_boot_bring_up_pending(req); }")(
         on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
                  r"if\s*\(\(cmd\.set_mask\s*&\s*bluetooth_channel::BT_SET_ENABLED\)\s*&&\s*settings\.enabled"
                  r"\s*&&\s*waits_for_boot_bring_up\(\)\)\s*\{[^}]*\}\s*", "")(s))),
    ("the settings POST waits whatever it names (F259)",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"\(cmd\.set_mask\s*&\s*bluetooth_channel::BT_SET_ENABLED\)\s*&&\s*settings\.enabled"
              r"\s*&&\s*(waits_for_boot_bring_up\(\))", r"\1")),
    ("send_boot_bring_up_pending() promises five minutes in safe mode (F259)",
     on_other(BT_API, SIG_SEND_BOOT_PENDING,
              r"bluetooth_channel::boot_bring_up_skipped\(\)\s*\?\s*bluetooth_channel::boot_bring_up_skipped_error\(\)"
              r"\s*:\s*", "")),
    ("send_boot_bring_up_pending() answers 200",
     on_other(BT_API, SIG_SEND_BOOT_PENDING, r"httpd_resp_set_status\(req,\s*http_status_line\(503\)\);", "")),
    ("send_boot_bring_up_pending() never says still starting",
     on_other(BT_API, SIG_SEND_BOOT_PENDING,
              r"bluetooth_channel::boot_bring_up_started\(\)\s*\|\|\s*bluetooth_channel::bring_up_starting\(\)",
              "false")),
    ("send_boot_bring_up_pending() says not started after the worker's refused init (wave 16's verify)",
     on_other(BT_API, SIG_SEND_BOOT_PENDING, r"bluetooth_channel::boot_bring_up_started\(\)\s*\|\|\s*", "")),
    ("send_boot_bring_up_pending() reads the started bring-up and not the latch",
     on_other(BT_API, SIG_SEND_BOOT_PENDING, r"\s*\|\|\s*bluetooth_channel::bring_up_starting\(\)", "")),
    ("send_boot_bring_up_pending() says still starting before the worker started",
     on_other(BT_API, SIG_SEND_BOOT_PENDING, r"bluetooth_channel::boot_bring_up_started\(\)",
              "bluetooth_channel::read_enabled()")),
    ("boot_bring_up_started() reads relaxed",
     on_other(BT_CPP, SIG_BT_BOOT_STARTED, r"__ATOMIC_ACQUIRE", "__ATOMIC_RELAXED")),
    ("boot_bring_up_started() always answers yes",
     on_other(BT_CPP, SIG_BT_BOOT_STARTED, r"return\s+__atomic_load_n\([^;]*\);", "return true;")),
    ("bringup_worker_started() marks the start with a relaxed store",
     on_other(BT_CPP, SIG_BT_WORKER_STARTED, r"(&g_boot_bring_up_started, true, )__ATOMIC_RELEASE",
              r"\1__ATOMIC_RELAXED")),
    ("bringup_worker_started() marks the start with a plain store",
     on_other(BT_CPP, SIG_BT_WORKER_STARTED,
              r"__atomic_store_n\(&g_boot_bring_up_started, true, __ATOMIC_RELEASE\);",
              "g_boot_bring_up_started = true;")),
    ("bringup_worker_started() never marks the start",
     on_other(BT_CPP, SIG_BT_WORKER_STARTED,
              r"\n[ \t]*__atomic_store_n\(&g_boot_bring_up_started, true, __ATOMIC_RELEASE\);[^\n]*", "")),
    ("bringup_worker_finished() clears the start",
     on_other(BT_CPP, SIG_BT_WORKER_FINISHED, r"(g_bringup_worker_running = false;)",
              r"\1\n  __atomic_store_n(&g_boot_bring_up_started, false, __ATOMIC_RELEASE);")),
    ("deinit() clears the start",
     on_other(BT_CPP, SIG_BT_DEINIT, r"(set_state\(BT_DISABLED\);)",
              r"__atomic_store_n(&g_boot_bring_up_started, false, __ATOMIC_RELEASE);\n  \1")),
    ("send_boot_bring_up_pending() always says still starting",
     on_other(BT_API, SIG_SEND_BOOT_PENDING, r"\s*:\s*bluetooth_channel::boot_bring_up_pending_error\(\)",
              " : bluetooth_channel::bring_up_starting_error()")),
    ("the disable handler waits for the boot bring-up",
     on_other(BT_API, api_handler("handle_bluetooth_disable"), r"(bluetooth_channel::Result r;)",
              r"if (waits_for_boot_bring_up()) { return send_boot_bring_up_pending(req); }\n  \1")),
    ("boot_bring_up_tried() reads relaxed",
     on_other(BT_CPP, SIG_BT_BOOT_TRIED, r"__ATOMIC_ACQUIRE", "__ATOMIC_RELAXED")),
    ("boot_bring_up_tried() always answers yes",
     on_other(BT_CPP, SIG_BT_BOOT_TRIED, r"return\s+__atomic_load_n\([^;]*\);", "return true;")),
    ("the boot bring-up counts as tried once the worker starts",
     on_other(BT_CPP, SIG_BT_WORKER_STARTED, r"(g_bringup_worker_running = true;)",
              r"\1\n  __atomic_store_n(&g_boot_bring_up_tried, true, __ATOMIC_RELEASE);")),
    ("boot_bring_up_finished() never marks the boot bring-up over",
     on_other(BT_CPP, SIG_BT_BOOT_FINISHED, r"(&g_boot_bring_up_tried, )true(, __ATOMIC_RELEASE\);)",
              r"\1false\2")),
    ("boot_bring_up_finished() marks it with a relaxed store",
     on_other(BT_CPP, SIG_BT_BOOT_FINISHED, r"(&g_boot_bring_up_tried, true, )__ATOMIC_RELEASE",
              r"\1__ATOMIC_RELAXED")),
    ("the worker's end marks the boot bring-up over, before the finalize stage's Scout init",
     on_other(BT_CPP, SIG_BT_WORKER_FINISHED, r"$",
              "\n  __atomic_store_n(&g_boot_bring_up_tried, true, __ATOMIC_RELEASE);\n")),
    # Rule BV7, the sketch's two calls of the boot bring-up's end (the F235
    # review): last in the finalize stage, after the Scout's NimBLE init, and
    # where no worker could be created; nowhere else.
    ("the finalize stage marks the boot bring-up over before the Scout's NimBLE init",
     lambda s: on("ino", SIG_INO_FINALIZE, r"(ble_scout::ble_scout_allow_radio\(\);)",
                  r"bluetooth_channel::boot_bring_up_finished();\n  \1")(
         on("ino", SIG_INO_FINALIZE, r"bluetooth_channel::boot_bring_up_finished\(\);", "")(s))),
    ("the finalize stage marks the boot bring-up over before it takes the worker's result",
     lambda s: on("ino", SIG_INO_FINALIZE, r"(bluetooth_channel::bringup_worker_finished\(\);)",
                  r"bluetooth_channel::boot_bring_up_finished();\n  \1")(
         on("ino", SIG_INO_FINALIZE, r"bluetooth_channel::boot_bring_up_finished\(\);", "")(s))),
    ("the finalize stage never marks the boot bring-up over",
     on("ino", SIG_INO_FINALIZE, r"bluetooth_channel::boot_bring_up_finished\(\);", "")),
    ("a failed worker create never marks the boot bring-up over",
     on("ino", SIG_INO_DISCOVERY_START, r"bluetooth_channel::boot_bring_up_finished\(\);", "")),
    ("the gate marks the boot bring-up over as it starts the worker",
     on("ino", SIG_INO_DISCOVERY_START, r"(bluetooth_channel::bringup_worker_started\(\);)",
        r"\1\n  bluetooth_channel::boot_bring_up_finished();")),
    ("the sketch's loop() marks the boot bring-up over",
     on("ino", SIG_INO_LOOP, r"^", "\n  bluetooth_channel::boot_bring_up_finished();")),
    ("stop_advertising() marks the boot bring-up tried",
     on_other(BT_CPP, SIG_BT_STOP_ADV, r"$", "\n  __atomic_store_n(&g_boot_bring_up_tried, true, __ATOMIC_RELEASE);\n")),
    # The verifier's escapes (wave 16): each mark always runs where it is
    # reached, or sits in the failed create's block.
    ("the gate marks the boot bring-up over after any create, not only a failed one",
     lambda s: on("ino", SIG_INO_DISCOVERY_START, r"(\(out of memory\)\", nullptr\);\s*\})",
                  r"\1\n  #if FEATURE_BLUETOOTH\n  bluetooth_channel::boot_bring_up_finished();\n  #endif")(
         on("ino", SIG_INO_DISCOVERY_START, r"\n[ \t]*bluetooth_channel::boot_bring_up_finished\(\);[^\n]*", "")(s))),
    ("the gate tells the channel its worker finished after any create, not only a failed one",
     lambda s: on("ino", SIG_INO_DISCOVERY_START, r"(\(out of memory\)\", nullptr\);\s*\})",
                  r"\1\n  #if FEATURE_BLUETOOTH\n  bluetooth_channel::bringup_worker_finished();\n  #endif")(
         on("ino", SIG_INO_DISCOVERY_START, r"\n[ \t]*bluetooth_channel::bringup_worker_finished\(\);[^\n]*", "")(s))),
    ("a failed create returns before it marks the boot bring-up over",
     on("ino", SIG_INO_DISCOVERY_START, r"(!= pdPASS\) \{)", r"\1\n    if (g_wifi_ap_only) return;")),
    ("a failed create marks the boot bring-up over only under a condition",
     on("ino", SIG_INO_DISCOVERY_START, r"(bluetooth_channel::boot_bring_up_finished\(\);)",
        r"if (g_wifi_ap_only) \1")),
    ("a failed create marks the boot bring-up over under FEATURE_BLE",
     on("ino", SIG_INO_DISCOVERY_START,
        r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::bringup_worker_finished\(\);)", r"#if FEATURE_BLE\1")),
    ("the finalize stage returns early, before the Scout and the mark, when ble_manager failed",
     on("ino", SIG_INO_FINALIZE, r"(\n[ \t]*#if FEATURE_BLE_SCAN\n)", r"\n  if (g_ble_mgr_result == -1) return;\1")),
    ("the finalize stage jumps past the mark",
     lambda s: on("ino", SIG_INO_FINALIZE, r"(\n[ \t]*log_health\(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,\s*\"BLE bring-up finalized)",
                  r"\nfinalized:\1")(
         on("ino", SIG_INO_FINALIZE, r"(\n[ \t]*#if FEATURE_BLE_SCAN\n)", r"\n  if (g_ble_mgr_result == -1) goto finalized;\1")(s))),
    ("the finalize stage marks the boot bring-up over only when ble_manager came up",
     on("ino", SIG_INO_FINALIZE, r"(bluetooth_channel::boot_bring_up_finished\(\);)",
        r"if (g_ble_mgr_result == 1) \1")),
    ("the finalize stage marks the boot bring-up over in a block only ble_manager's success enters",
     on("ino", SIG_INO_FINALIZE, r"(bluetooth_channel::boot_bring_up_finished\(\);)",
        r"if (g_ble_mgr_result == 1) { \1 }")),
    ("the finalize stage marks the boot bring-up over in an else only ble_manager's success reaches",
     on("ino", SIG_INO_FINALIZE, r"(bluetooth_channel::boot_bring_up_finished\(\);)",
        r"if (g_ble_mgr_result != 1) {} else \1")),
    ("the finalize stage marks the boot bring-up over under FEATURE_BLE",
     on("ino", SIG_INO_FINALIZE, r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::boot_bring_up_finished\(\);)",
        r"#if FEATURE_BLE\1")),
    ("the finalize stage marks the boot bring-up over in an #else branch",
     on("ino", SIG_INO_FINALIZE, r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::boot_bring_up_finished\(\);)",
        r"#if FEATURE_BLUETOOTH\n  #else\1")),
    # The second verify's escapes (wave 16): the start mark, the finalize
    # stage's end of the worker, and the worker's handoff each run every time
    # they are reached, as the two end marks do.
    ("the gate returns between telling the channel its worker started and creating it",
     on("ino", SIG_INO_DISCOVERY_START, r"(\n[ \t]*if \(xTaskCreate\(ble_bringup_task)",
        r"\n  if (g_wifi_ap_only) return;\1")),
    ("the gate jumps from the start mark past the create",
     lambda s: on("ino", SIG_INO_DISCOVERY_START, r"(\n#endif\n)$", r"\n  no_worker:;\1")(
         on("ino", SIG_INO_DISCOVERY_START, r"(\n[ \t]*if \(xTaskCreate\(ble_bringup_task)",
            r"\n  if (g_wifi_ap_only) goto no_worker;\1")(s))),
    ("the gate runs a statement between the start mark and the create",
     on("ino", SIG_INO_DISCOVERY_START, r"(\n[ \t]*if \(xTaskCreate\(ble_bringup_task)",
        r"\n  g_ble_init_attempted = true;\1")),
    ("the gate tells the channel its worker started under FEATURE_BLE",
     on("ino", SIG_INO_DISCOVERY_START, r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::bringup_worker_started\(\);)",
        r"#if FEATURE_BLE\1")),
    ("the gate tells the channel its worker started only on the long path",
     on("ino", SIG_INO_DISCOVERY_START, r"(bluetooth_channel::bringup_worker_started\(\);)",
        r"if (long_due) \1")),
    ("the gate tells the channel its worker started in a block only the long path enters",
     on("ino", SIG_INO_DISCOVERY_START, r"(bluetooth_channel::bringup_worker_started\(\);)",
        r"if (long_due) { \1 }")),
    ("the gate tells the channel its worker started in an #else branch",
     on("ino", SIG_INO_DISCOVERY_START, r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::bringup_worker_started\(\);)",
        r"#if FEATURE_BLUETOOTH\n  #else\1")),
    ("the finalize stage tells the channel its worker finished only when ble_manager came up",
     on("ino", SIG_INO_FINALIZE, r"(bluetooth_channel::bringup_worker_finished\(\);)",
        r"if (g_ble_mgr_result != -1) \1")),
    ("the finalize stage tells the channel its worker finished under FEATURE_BLE",
     on("ino", SIG_INO_FINALIZE, r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::bringup_worker_finished\(\);)",
        r"#if FEATURE_BLE\1")),
    ("the finalize stage tells the channel its worker finished in a block only ble_manager's success enters",
     on("ino", SIG_INO_FINALIZE, r"(bluetooth_channel::bringup_worker_finished\(\);)",
        r"if (g_ble_mgr_result == 1) { \1 }")),
    ("the worker never hands its result to the loop",
     on("ino", SIG_INO_BRINGUP_TASK,
        r"__atomic_store_n\(&g_ble_bringup_done, true, __ATOMIC_RELEASE\);\s*(vTaskDelete\(NULL\);)", r"\1")),
    ("the worker ends early, before its handoff, when the channel's init failed",
     on("ino", SIG_INO_BRINGUP_TASK,
        r"(Serial\.println\(\"\[--\] Bluetooth init failed[^\n]*\n)", r"\1      vTaskDelete(NULL); return;\n")),
    ("the worker returns before its handoff when the channel's init failed",
     on("ino", SIG_INO_BRINGUP_TASK,
        r"(Serial\.println\(\"\[--\] Bluetooth init failed[^\n]*\n)", r"\1      return;\n")),
    ("the worker hands its result to the loop before its inits",
     lambda s: on("ino", SIG_INO_BRINGUP_TASK, r"^", "\n  __atomic_store_n(&g_ble_bringup_done, true, __ATOMIC_RELEASE);")(
         on("ino", SIG_INO_BRINGUP_TASK,
            r"__atomic_store_n\(&g_ble_bringup_done, true, __ATOMIC_RELEASE\);\s*(vTaskDelete\(NULL\);)",
            r"\1")(s))),
    ("the worker hands its result to the loop twice, once before its inits",
     on("ino", SIG_INO_BRINGUP_TASK, r"^", "\n  __atomic_store_n(&g_ble_bringup_done, true, __ATOMIC_RELEASE);")),
    ("the worker hands its result to the loop only when the channel's init was attempted",
     on("ino", SIG_INO_BRINGUP_TASK, r"(__atomic_store_n\(&g_ble_bringup_done, true, __ATOMIC_RELEASE\);)",
        r"if (g_ble_init_attempted) \1")),
    ("the worker hands its result to the loop with a relaxed store",
     on("ino", SIG_INO_BRINGUP_TASK, r"(__atomic_store_n\(&g_ble_bringup_done, true, )__ATOMIC_RELEASE",
        r"\1__ATOMIC_RELAXED")),
    ("the sketch's loop() marks the worker's result handed over itself",
     on("ino", SIG_INO_LOOP, r"^", "\n  g_ble_bringup_done = true;")),
    ("the finalize stage marks the worker's result handed over itself, with an atomic store",
     on("ino", SIG_INO_FINALIZE, r"(g_ble_bringup_finalized = true;)",
        r"\1\n  __atomic_store_n(&g_ble_bringup_done, true, __ATOMIC_RELEASE);")),
    # Rule BV4, init()'s latch (the F235 review): bring_up_starting() reads the
    # latch first because init() clears it only as it returns.
    ("init() clears its latch before the stack is published",
     on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);)",
              r"__atomic_store_n(&g_init_in_progress, false, __ATOMIC_RELEASE);\n  \1")),
    ("init()'s latch guard clears it with a relaxed store",
     on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_init_in_progress, false, )__ATOMIC_RELEASE",
              r"\1__ATOMIC_RELAXED")),
    ("init() takes its latch with relaxed orders",
     on_other(BT_CPP, SIG_BT_INIT, r"__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE\)",
              "__ATOMIC_RELAXED, __ATOMIC_RELAXED)")),
    ("init() takes its latch a second time",
     lambda s: on_other(BT_CPP, SIG_BT_INIT, r"(log_health\(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, \"Initializing BLE\")",
                        r"bool expected2 = false; (void)__atomic_compare_exchange_n(&g_init_in_progress, "
                        r"&expected2, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);\n  \1")(s)),
    ("init_running() reads the latch relaxed",
     on_other(BT_CPP, SIG_BT_INIT_RUNNING, r"__ATOMIC_ACQUIRE", "__ATOMIC_RELAXED")),
    ("init() destroys its latch guard by hand before the stack is published (wave 16's verify)",
     on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);)",
              r"latch_clear.~InitLatchClear();\n  \1")),
    ("init() clears its latch through an inner guard before the stack is published",
     on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);)",
              r"{ InitLatchClear early_clear; }\n  \1")),
    ("init() no longer looks at the stack again once it holds the latch (wave 16's verify)",
     on_other(BT_CPP, SIG_BT_INIT,
              r"(// Another caller brought it up between the first look and the latch\.\n)[ \t]*"
              r"if \(__atomic_load_n\(&g_stack_up, __ATOMIC_ACQUIRE\)\) return true;\n", r"\1")),
    ("init() looks at the stack again only after its first log",
     lambda s: on_other(BT_CPP, SIG_BT_INIT, r"(log_health\(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, \"Initializing BLE\", nullptr\);)",
                        r"\1\n  if (__atomic_load_n(&g_stack_up, __ATOMIC_ACQUIRE)) return true;")(
         on_other(BT_CPP, SIG_BT_INIT,
                  r"(// Another caller brought it up between the first look and the latch\.\n)[ \t]*"
                  r"if \(__atomic_load_n\(&g_stack_up, __ATOMIC_ACQUIRE\)\) return true;\n", r"\1")(s))),
    ("deinit() clears init()'s latch",
     on_other(BT_CPP, SIG_BT_DEINIT, r"(set_state\(BT_DISABLED\);)",
              r"__atomic_store_n(&g_init_in_progress, false, __ATOMIC_RELEASE);\n  \1")),
    # Rule C3, who calls bring_up() (the F235 review's escapes).
    ("the sketch's loop() calls bluetooth_api::bring_up()",
     on("ino", SIG_INO_LOOP, r"^", "\n  (void)bluetooth_api::bring_up();")),
    ("the sketch's loop() calls bring_up() unqualified",
     on("ino", SIG_INO_LOOP, r"^", "\n  (void)bring_up();")),
    ("a sketch file defines a bring_up() of its own, then loop() calls bluetooth_api::bring_up()",
     raw("ino", "void loop() {",
         "static bool bring_up() { return false; }\nvoid loop() {\n  (void)bluetooth_api::bring_up();")),
    ("the sketch's loop() calls bluetooth_api::bring_up() through a function pointer (wave 16's verify)",
     on("ino", SIG_INO_LOOP, r"^", "\n  { bool (*bu)() = &bluetooth_api::bring_up; (void)bu(); }")),
    # The second verify (wave 16): a name for the namespace that is not
    # `bluetooth_api`, or a namespace that brings its names in, spells
    # bring_up as `x::bring_up`, which neither pattern reads.
    ("the sketch's loop() calls bring_up() through a namespace alias",
     on("ino", SIG_INO_LOOP, r"^", "\n  { namespace ba = bluetooth_api; (void)ba::bring_up(); }")),
    ("the sketch aliases bluetooth_api at file scope and loop() calls bring_up() through it",
     lambda s: on("ino", SIG_INO_LOOP, r"^", "\n  (void)scv_bt::bring_up();")(
         raw("ino", '#include "bluetooth_api.h"', '#include "bluetooth_api.h"\nnamespace scv_bt = ::bluetooth_api;')(s))),
    ("the sketch's loop() calls bring_up() through a namespace that brings bluetooth_api's names in",
     lambda s: on("ino", SIG_INO_LOOP, r"^", "\n  (void)scv_btx::bring_up();")(
         raw("ino", '#include "bluetooth_api.h"',
             '#include "bluetooth_api.h"\nnamespace scv_btx { using namespace bluetooth_api; }')(s))),
    ("the Bluetooth status route calls bring_up() through a namespace alias",
     on_other(BT_API, api_handler("handle_bluetooth_status"), r"^",
              "\n  { namespace self = ::bluetooth_api; (void)self::bring_up(); }")),
    ("the sketch's loop() takes bring_up's address under a using-directive",
     on("ino", SIG_INO_LOOP, r"^", "\n  { using namespace bluetooth_api; bool (*bu)() = &bring_up; (void)bu(); }")),
    ("a bluetooth_api.h route takes bring_up's address and calls through it",
     on_other(BT_API, api_handler("handle_bluetooth_status"), r"^",
              "\n  { bool (*bu)() = &bring_up; (void)bu(); }")),
    ("the disable handler calls bluetooth_api::bring_up()",
     on_other(BT_API, api_handler("handle_bluetooth_disable"), r"(bluetooth_channel::Result r;)",
              r"(void)bluetooth_api::bring_up();\n  \1")),
    ("the disable handler brings the stack up in a guard",
     on_other(BT_API, api_handler("handle_bluetooth_disable"), r"(bluetooth_channel::Result r;)",
              r'if (!bring_up()) { return send_bring_up_refused(req, "x"); }\n  \1')),
    ("the status route brings the stack up through a qualified guard",
     on_other(BT_API, api_handler("handle_bluetooth_status"), r"^",
              "\n  if (!bluetooth_api::bring_up()) { return send_bring_up_refused(req, \"x\"); }")),
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
     on_other(CHIRP_CPP, r"\bstatic\s+bool\s+unmute\s*\([^)]*\)", r"(g_muted\s*=\s*false;)",
              r"\1 { StatusView z; memset(&z, 0, sizeof z); (void)g_status_view.publish(z); }")),
    ("chirp update() clears the tables' flag before it publishes them",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"\1 g_tables_changed = false;")),
    ("chirp publish_view() never clears the tables' flag (every pass rebuilds both tables)",
     on_other(CHIRP_CPP, SIG_CHIRP_PUBLISH, r"(return;)\s*g_tables_changed\s*=\s*false;", r"\1")),
    ("chirp publish_view() clears the tables' flag before its guard",
     on_other(CHIRP_CPP, SIG_CHIRP_PUBLISH,
              r"(if\s*\(!g_tables_changed[^)]*\)\s*return;)\s*g_tables_changed\s*=\s*false;",
              r"g_tables_changed = false; \1")),
    ("the nearby table's published copy is static again",
     raw_other(CHIRP_CPP, "static loop_snapshot::AttachedValue<NearbyTable,",
               "static loop_snapshot::Value<NearbyTable,")),
    ("the recent table's view is declared twice, once static",
     raw_other(CHIRP_CPP, "static loop_snapshot::AttachedValue<RecentTable, loop_command_ring::PortMuxLock> g_recent_view;",
               "static loop_snapshot::AttachedValue<RecentTable, loop_command_ring::PortMuxLock> g_recent_view;\n"
               "static loop_snapshot::Value<RecentTable, loop_command_ring::PortMuxLock> g_recent_copy;")),
    ("the tables' block comes from the internal heap",
     on_other(CHIRP_CPP, SIG_CHIRP_INIT, r"csi_large_calloc\(sizeof\(ViewTables\)\)",
              "calloc(1, sizeof(ViewTables))")),
    ("chirp init() never attaches the recent table's copy",
     on_other(CHIRP_CPP, SIG_CHIRP_INIT, r"\n[ \t]*g_recent_view\.attach\([^;]*\);", "")),
    ("chirp init() attaches the nearby copy to the scratch",
     on_other(CHIRP_CPP, SIG_CHIRP_INIT, r"g_nearby_view\.attach\(&g_view_tables->nearby\)",
              "g_nearby_view.attach(&g_view_tables->scratch.nearby)")),
    ("chirp update() re-attaches the recent view",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(uint32_t now = millis\(\);)",
              r"\1 g_recent_view.attach(nullptr);")),
    # Rule CV5: every change to the tables is marked for the view.
    ("a chirp frame no longer marks the tables",
     on_other(CHIRP_CPP, SIG_CHIRP_RECV, r"g_tables_changed\s*=\s*true;\s*(switch)", r"\1")),
    ("the prune no longer marks the tables",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(prune_old_chirps\(\);)\s*g_tables_changed\s*=\s*true;", r"\1")),
    ("chirp send_presence() empties the recent table",
     on_other(CHIRP_CPP, r"\bstatic\s+void\s+send_presence\s*\(\s*\)", r"(ChirpHeader\*\s+hdr\s*=)",
              r"g_recent_chirp_count = 0; \1")),
    ("chirp update() handles a presence frame itself",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(uint32_t now = millis\(\);)",
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
    # Rule CV7: what the routes put under each key (the reviewers' h01-h07).
    ("the send route answers every refusal as a cooldown (h01)",
     on_other(CHIRP_API, api_handler("handle_chirp_send"),
              r"doc\[\"error\"\]\s*=\s*chirp_channel::send_refusal_error\(r\.refusal\);",
              'doc["error"] = "cooldown";')),
    ("the send route puts the cooldown's fields on every refusal (h02)",
     on_other(CHIRP_API, api_handler("handle_chirp_send"),
              r"if\s*\(r\.refusal\s*==\s*chirp_channel::SEND_REFUSED_COOLDOWN\)\s*\{", "{")),
    ("the send route's message is its own string again",
     on_other(CHIRP_API, api_handler("handle_chirp_send"),
              r"doc\[\"message\"\]\s*=\s*chirp_channel::send_refusal_message\(r\.refusal\);",
              'doc["message"] = "Please wait before sending another chirp";')),
    ("the status route names its reason inline, without the clock (h03)",
     on_other(CHIRP_API, api_handler("handle_chirp_status"),
              r"const\s+char\s*\*\s*why\s*=\s*chirp_channel::cannot_send_reason\(v\);\s*"
              r"if\s*\(why\s*!=\s*nullptr\)\s*\{\s*doc\[\"cannot_send_reason\"\]\s*=\s*why;\s*\}",
              'if (!v.can_send) { if (v.state == chirp_channel::CHIRP_DISABLED) { '
              'doc["cannot_send_reason"] = "disabled"; } else if (v.state == chirp_channel::CHIRP_COOLDOWN) { '
              'doc["cannot_send_reason"] = "cooldown"; } else if (!v.presence_met) { '
              'doc["cannot_send_reason"] = "presence_required"; } }')),
    ("the status route answers can_send from the presence requirement (h04)",
     on_other(CHIRP_API, api_handler("handle_chirp_status"), r"=\s*v\.can_send;", "= v.presence_met;")),
    ("the status route answers the tier's whole cooldown as what is left (h05)",
     on_other(CHIRP_API, api_handler("handle_chirp_status"),
              r"chirp_channel::seconds_left\(v\.cooldown_remaining_ms\)", "v.cooldown_ms / 1000")),
    ("the status route rounds the cooldown left down again: its last second reads 0 (F178)",
     on_other(CHIRP_API, api_handler("handle_chirp_status"),
              r"chirp_channel::seconds_left\(v\.cooldown_remaining_ms\)", "v.cooldown_remaining_ms / 1000")),
    ("the send route rounds a cooldown refusal's time left down again (F178)",
     on_other(CHIRP_API, api_handler("handle_chirp_send"),
              r"chirp_channel::seconds_left\(r\.cooldown_remaining_ms\)", "r.cooldown_remaining_ms / 1000")),
    # Rule CV11: the dashboard shows it (F174, F178).
    ("the dashboard's confirm drops its answer again",
     raw_other(f"{SKETCH}/web_ui.h",
               "document.getElementById('chirpActionNote').textContent = WebUiLogic.chirpActionNote(data);\n"
               "      loadChirps();\n    }\n    async function dismissChirp", "loadChirps();\n    }\n    async function dismissChirp")),
    ("the dashboard's dismiss posts without reading its answer",
     raw_other(f"{SKETCH}/web_ui.h", "const data = await api('/api/chirp/dismiss', 'POST', { nonce });",
               "await api('/api/chirp/dismiss', 'POST', { nonce }); const data = {};")),
    ("the Chirp card's Send follows the presence requirement alone",
     raw_other(f"{SKETCH}/web_ui.h", "document.getElementById('chirpSendBtn').disabled = gate.sendDisabled;",
               "document.getElementById('chirpSendBtn').disabled = !data.presence_met;")),
    # Rules CV9, CV10: a refused confirm says why (F174).
    ("the confirm answer names every refusal not_found again",
     on_other(CHIRP_API, r"\binline\s+esp_err_t\s+send_confirm_answer\s*\([^)]*\)",
              r"doc\[\"error\"\]\s*=\s*chirp_channel::confirm_refusal_error\(r\.confirm_refusal\);",
              'doc["error"] = "not_found";')),
    ("the confirm answer drops its status (every refusal a 200)",
     on_other(CHIRP_API, r"\binline\s+esp_err_t\s+send_confirm_answer\s*\([^)]*\)",
              r"httpd_resp_set_status\(req,\s*http_status_line\(chirp_channel::confirm_refusal_status\("
              r"r\.confirm_refusal\)\)\);", "")),
    ("the dismiss answer says nothing of the vote",
     on_other(CHIRP_API, r"\binline\s+esp_err_t\s+send_dismiss_answer\s*\([^)]*\)",
              r"doc\[\"vote_sent\"\]\s*=\s*r\.vote_sent;", "")),
    ("the dismiss answer names a vote that went out as unsent",
     on_other(CHIRP_API, r"\binline\s+esp_err_t\s+send_dismiss_answer\s*\([^)]*\)",
              r"if\s*\(!r\.vote_sent\)", "if (true)")),
    ("the confirm route answers a bare success again",
     on_other(CHIRP_API, api_handler("handle_chirp_confirm"), r"return\s+send_confirm_answer\(req,\s*r\);",
              'JsonDocument doc; doc["success"] = r.ok; char buffer[64]; serializeJson(doc, buffer); '
              "return httpd_resp_sendstr(req, buffer);")),
    ("the ack answers a confirm as a dismiss",
     on_other(CHIRP_API, api_handler("handle_chirp_ack"),
              r"if\s*\(cmd\.type\s*==\s*chirp_channel::CHIRP_CMD_CONFIRM\)\s*return\s+send_confirm_answer\(req,\s*r\);",
              "")),
    ("the dismiss route answers through the confirm answer",
     on_other(CHIRP_API, api_handler("handle_chirp_dismiss"), r"return\s+send_dismiss_answer\(req,\s*r\);",
              "return send_confirm_answer(req, r);")),
    ("the confirm answer goes back to its 64-byte buffer",
     on_other(CHIRP_API, r"\binline\s+esp_err_t\s+send_confirm_answer\s*\([^)]*\)",
              r"char\s+buffer\[256\];", "char buffer[64];")),
    ("the mute route answers its refusal from a 64-byte buffer again",
     on_other(CHIRP_API, SIG_MUTE_ANSWER, r"char\s+buffer\[160\];", "char buffer[64];")),
    # Rule CV12: a refused mute or unmute says why (F192).
    ("the mute answer names every refusal invalid_duration again",
     on_other(CHIRP_API, SIG_MUTE_ANSWER,
              r"doc\[\"error\"\]\s*=\s*chirp_channel::mute_refusal_error\(r\.mute_refusal\);",
              'doc["error"] = "invalid_duration";')),
    ("the mute answer drops its status (the channel off a 200)",
     on_other(CHIRP_API, SIG_MUTE_ANSWER,
              r"if\s*\(status\s*!=\s*200\)\s*httpd_resp_set_status\(req,\s*http_status_line\(status\)\);", "")),
    ("the mute answer sets a status line for a 200 (http_status_line() sends it as 400)",
     on_other(CHIRP_API, SIG_MUTE_ANSWER, r"if\s*\(status\s*!=\s*200\)\s*", "")),
    ("the unmute route answers success whatever happened again",
     on_other(CHIRP_API, api_handler("handle_chirp_unmute"), r"return\s+send_mute_answer\(req,\s*r\);",
              'JsonDocument doc; doc["success"] = true; char buffer[64]; serializeJson(doc, buffer); '
              "return httpd_resp_sendstr(req, buffer);")),
    ("the mute route answers through the confirm answer",
     on_other(CHIRP_API, api_handler("handle_chirp_mute"), r"return\s+send_mute_answer\(req,\s*r\);",
              "return send_confirm_answer(req, r);")),
    ("the dashboard's mute drops its answer again",
     raw_other(f"{SKETCH}/web_ui.h", "const data = await api('/api/chirp/mute', 'POST', { duration_minutes: mins });",
               "await api('/api/chirp/mute', 'POST', { duration_minutes: mins }); const data = {};")),
    ("the dashboard's unmute shows nothing of its answer",
     raw_other(f"{SKETCH}/web_ui.h",
               "const data = await api('/api/chirp/unmute', 'POST');\n"
               "      document.getElementById('chirpActionNote').textContent = WebUiLogic.chirpActionNote(data);\n",
               "const data = await api('/api/chirp/unmute', 'POST');\n")),
    # Rule CV8: the send cooldown is a timer (F178).
    ("a send stores the cooldown as the state again (a mute overwrites it)",
     on_other(CHIRP_CPP, r"\bstatic\s+bool\s+send_chirp\s*\([^)]*\)", r"(cache_nonce\(hdr->nonce\);)",
              r"\1 set_state(CHIRP_COOLDOWN);")),
    ("the send's gate refuses on the cooldown state again",
     on_other(CHIRP_CPP, r"\bstatic\s+SendRefusal\s+send_gate\s*\([^)]*\)",
              r"if\s*\(left\s*>\s*0\)\s*return\s+SEND_REFUSED_COOLDOWN;",
              "if (g_state == CHIRP_COOLDOWN) return SEND_REFUSED_COOLDOWN;")),
    ("cannot_send_reason() names the cooldown from the state again",
     on_other(CHIRP_CPP, r"\bconst\s+char\s*\*\s*cannot_send_reason\s*\([^)]*\)",
              r"if\s*\(v\.cooldown_remaining_ms\s*>\s*0\)", "if (v.state == CHIRP_COOLDOWN)")),
    ("update() ends a cooldown state after its drain again",
     on_other(CHIRP_CPP, SIG_UPDATE, r"(uint32_t now = millis\(\);)",
              r"\1 if (g_state == CHIRP_COOLDOWN && get_cooldown_remaining_ms() == 0) set_state(CHIRP_ACTIVE);")),
    ("the recent route shows dismissed chirps (h06)",
     on_other(CHIRP_API, api_handler("handle_chirp_recent"), r"\n[ \t]*if\s*\(chirps\[i\]\.dismissed\)\s*continue;",
              "")),
    ("the nearby route frees its copy before it serializes (h07)",
     on_other(CHIRP_API, api_handler("handle_chirp_nearby"),
              r"(serializeJson\(doc,\s*buffer,\s*needed\);)(\s*)free\(t\);", r"free(t);\2\1")),
    ("the recent route frees its copy only after it answers",
     on_other(CHIRP_API, api_handler("handle_chirp_recent"),
              r"(serializeJson\(doc,\s*buffer,\s*needed\);)\s*free\(t\);", r"\1")),
    ("the recent route reads past the copy's count",
     on_other(CHIRP_API, api_handler("handle_chirp_recent"), r"const\s+size_t\s+count\s*=\s*t->count;",
              "const size_t count = chirp_channel::MAX_RECENT_CHIRPS;")),
    ("the nearby route answers a row's RSSI from its listening flag",
     on_other(CHIRP_API, api_handler("handle_chirp_nearby"), r"=\s*devices\[i\]\.rssi;",
              "= devices[i].listening;")),
    ("the recent route answers validated from suppressed",
     on_other(CHIRP_API, api_handler("handle_chirp_recent"), r"=\s*chirps\[i\]\.validated;",
              "= chirps[i].suppressed;")),
    ("the status route answers the session emoji twice",
     on_other(CHIRP_API, api_handler("handle_chirp_status"), r"(doc\[\"session_emoji\"\]\s*=\s*v\.session_emoji;)",
              r'\1 doc["session_emoji"] = "";')),
]

# Rules BV1..: the Bluetooth channel's settings enable (F144).
BV_MUTATIONS: list[tuple[str, Mutation]] = [
    ("the Bluetooth settings handler no longer brings the stack up for enabled:true",
     on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
              r"if\s*\(\(cmd\.set_mask\s*&\s*bluetooth_channel::BT_SET_ENABLED\)\s*&&\s*settings\.enabled"
              r"\s*&&\s*!bring_up\(\)\)\s*\{[^}]*\}", "")),
    ("the Bluetooth settings handler brings the stack up after the command ran",
     lambda s: on_other(BT_API, api_handler("handle_bluetooth_settings_set"),
                        r"(if\s*\(r\.ok\))", r'if (!bring_up()) { return send_bt_error(req, "x"); } \1')(
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
     on_other(BT_CPP, r"\bbool\s+init\s*\(\s*\)", r"(__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);)",
              r"\1 (void)post_event(make_event(BT_EV_SCAN_END));")),
    ("update() never applies the events",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*g_events\.consume\(apply_event\);", "")),
    ("update() applies the events after its early return",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*g_events\.consume\(apply_event\);", "")(
         on_other(BT_CPP, SIG_UPDATE, r"(static\s+uint32_t\s+last_status_update\s*=\s*0;)",
                  r"g_events.consume(apply_event); \1")(s))),
    ("update() runs the commands before the events",
     on_other(BT_CPP, SIG_UPDATE,
              r"g_events\.consume\(apply_event\);(\s*reconcile_link\(\);[^\n]*\s*)g_commands\.drain\(run_command\);",
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
     on_other(BT_CPP, r"\bbool\s+init\s*\(\s*\)", r"(__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);)",
              r"\1 publish_views();")),
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
# Rule BV2's reconciliation (F169) and rule BD1 (F171).
SIG_BT_RECONCILE = r"\bstatic\s+void\s+reconcile_link\s*\(\s*\)"
BV_MUTATIONS += [
    ("update() never reconciles a dropped link event",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*reconcile_link\(\);[^\n]*", "")),
    ("update() reconciles before it applies the events",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*reconcile_link\(\);[^\n]*", "")(
         on_other(BT_CPP, SIG_UPDATE, r"(g_events\.consume\(apply_event\);)", r"reconcile_link(); \1")(s))),
    ("update() reconciles after the commands ran",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*reconcile_link\(\);[^\n]*", "")(
         on_other(BT_CPP, SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"\1 reconcile_link();")(s))),
    ("a scan's end reconciles the link (a second caller)",
     on_other(BT_CPP, r"\bstatic\s+void\s+apply_scan_end\s*\([^)]*\)", r"(if\s*\(!g_scanning\)\s*return;)",
              r"\1 reconcile_link();")),
    ("reconcile_link() applies another event kind",
     on_other(BT_CPP, SIG_BT_RECONCILE, r"(g_link_drops_reconciled\s*=\s*dropped;)",
              r"\1 apply_scan_end(make_event(BT_EV_SCAN_END));")),
    ("Opera installs its server callbacks with setCallbacks() again (F171's defect)",
     on_other(BLE_OPERA_H, SIG_OPERA_INIT,
              r"ble_server_dispatch::install\(g_pServer,\s*ble_server_dispatch::kLink,\s*&g_serverCallbacks\);",
              "g_pServer->setCallbacks(&g_serverCallbacks);")),
    ("the channel installs its server callbacks with setCallbacks() again",
     on_other(BT_CPP, SIG_BT_ADOPT,
              r"ble_server_dispatch::set_owner\(ble_server_dispatch::kPairing,\s*&g_server_callbacks\);",
              "g_server->setCallbacks(&g_server_callbacks);")),
    ("Opera installs through the dispatcher and replaces it too",
     on_other(BLE_OPERA_H, SIG_OPERA_INIT,
              r"(ble_server_dispatch::install\(g_pServer,\s*ble_server_dispatch::kLink,\s*&g_serverCallbacks\);)",
              r"\1 NimBLEDevice::getServer()->setCallbacks(new OperaServerCallbacks());")),
    ("Opera takes the pairing owner's role",
     on_other(BLE_OPERA_H, SIG_OPERA_INIT, r"ble_server_dispatch::kLink", "ble_server_dispatch::kPairing")),
    ("the dispatcher lets NimBLE delete it with the server",
     on_other(BT_DISPATCH_H, SIG_DISPATCH_ATTACH, r"setCallbacks\(&g_dispatcher,\s*false\)",
              "setCallbacks(&g_dispatcher)")),
    ("a new module installs server callbacks of its own",
     raw_other(f"{SKETCH}/ble_status_api.h", "static NimBLEServer* g_server = nullptr;",
               "static NimBLEServer* g_server = nullptr;\n"
               "class StatusServerCallbacks : public NimBLEServerCallbacks {};\n"
               "static StatusServerCallbacks g_status_server_callbacks;\n"
               "static void hook() { g_server->setCallbacks(&g_status_server_callbacks); }")),
    # The F171 review's four misses, and the other spellings of a null.
    ("the channel puts NimBLE's default server callbacks back (setCallbacks(nullptr))",
     on_other(BT_CPP, SIG_BT_ADOPT,
              r"(ble_server_dispatch::set_owner\(ble_server_dispatch::kPairing,\s*&g_server_callbacks\);)",
              r"\1 g_server->setCallbacks(nullptr);")),
    ("Opera puts the defaults back with NULL",
     on_other(BLE_OPERA_H, SIG_OPERA_INIT,
              r"(ble_server_dispatch::install\(g_pServer,\s*ble_server_dispatch::kLink,\s*&g_serverCallbacks\);)",
              r"\1 g_pServer->setCallbacks(NULL);")),
    ("a module puts the defaults back with 0",
     raw_other(f"{SKETCH}/ble_status_api.h", "static NimBLEServer* g_server = nullptr;",
               "static NimBLEServer* g_server = nullptr;\n"
               "static void hook() { g_server->setCallbacks(0, false); }")),
    ("Opera installs its server callbacks through a pointer variable",
     on_other(BLE_OPERA_H, SIG_OPERA_INIT,
              r"(ble_server_dispatch::install\(g_pServer,\s*ble_server_dispatch::kLink,\s*&g_serverCallbacks\);)",
              r"\1 NimBLEServerCallbacks* cbp = &g_serverCallbacks; g_pServer->setCallbacks(cbp);")),
    ("a module puts a bare NimBLEServerCallbacks on the server",
     raw_other(f"{SKETCH}/ble_status_api.h", "static NimBLEServer* g_server = nullptr;",
               "static NimBLEServer* g_server = nullptr;\n"
               "static void hook() { NimBLEDevice::getServer()->setCallbacks(new NimBLEServerCallbacks()); }")),
    ("a module installs an object of a class derived from Opera's",
     raw_other(f"{SKETCH}/ble_status_api.h", "static NimBLEServer* g_server = nullptr;",
               "static NimBLEServer* g_server = nullptr;\n"
               "class StatusSub : public ble_opera::OperaServerCallbacks {};\n"
               "static StatusSub g_status_sub;\n"
               "static void hook() { g_server->setCallbacks(&g_status_sub); }")),
]
# Rule BV4 (F167): init() hands its result to the loop task; and BD1's
# split of the dispatcher's attach from the channel's install.
BV_MUTATIONS += [
    ("init() sets the state on its own task again",
     on_other(BT_CPP, SIG_BT_INIT, r'(log_health\(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Initializing BLE", nullptr\);)',
              r"\1 set_state(BT_INITIALIZING);")),
    ("init() loads the settings into the loop task's g_settings",
     on_other(BT_CPP, SIG_BT_INIT, r"load_settings\(&b\.applied\);", "load_settings(&g_settings);")),
    ("init() turns Bluetooth on and advertises itself again",
     on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_bringup_ready, true, __ATOMIC_RELEASE\);)",
              r"enable(); start_advertising(); \1")),
    ("init() marks the channel initialized itself",
     on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);)",
              r"\1 g_initialized = true;")),
    ("init() rebuilds the paired list on its own task",
     on_other(BT_CPP, SIG_BT_INIT, r"(NimBLEDevice::setMTU\(247\);)", r"\1 migrate_paired_devices();")),
    ("init() writes its hand-over after publishing it",
     lambda s: on_other(BT_CPP, SIG_BT_INIT, r"(__atomic_store_n\(&g_bringup_ready, true, __ATOMIC_RELEASE\);)",
                        r"\1 b.scanner = scanner;")(
         on_other(BT_CPP, SIG_BT_INIT, r"\n[ \t]*b\.scanner = scanner;", "")(s))),
    ("init() publishes its hand-over with a relaxed store",
     on_other(BT_CPP, SIG_BT_INIT, r"__atomic_store_n\(&g_bringup_ready, true, __ATOMIC_RELEASE\)",
              "__atomic_store_n(&g_bringup_ready, true, __ATOMIC_RELAXED)")),
    ("init() sets the stack's flag before the hand-over",
     lambda s: on_other(BT_CPP, SIG_BT_INIT, r"(Bringup& b = g_bringup;)",
                        r"__atomic_store_n(&g_stack_up, true, __ATOMIC_RELEASE); \1")(
         on_other(BT_CPP, SIG_BT_INIT, r"\n[ \t]*__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);", "")(s))),
    ("init() sets the stack's flag with a plain store",
     on_other(BT_CPP, SIG_BT_INIT, r"__atomic_store_n\(&g_stack_up, true, __ATOMIC_RELEASE\);", "g_stack_up = true;")),
    ("the loop task takes the hand-over with a relaxed load",
     on_other(BT_CPP, SIG_BT_ADOPT, r"__atomic_load_n\(&g_bringup_ready, __ATOMIC_ACQUIRE\)",
              "__atomic_load_n(&g_bringup_ready, __ATOMIC_RELAXED)")),
    ("the loop task reads the hand-over before the acquire",
     on_other(BT_CPP, SIG_BT_ADOPT, r"(if \(g_initialized \|\| !__atomic_load_n)",
              r"g_server = g_bringup.server; \1")),
    ("update() never takes the bring-up's result",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*adopt_init_result\(\);", "")),
    ("update() takes the bring-up's result after the commands ran",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"\1 adopt_init_result();")(
         on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*adopt_init_result\(\);", "")(s))),
    ("a command takes the bring-up's result too (a second caller)",
     on_other(BT_CPP, r"\bstatic\s+Result\s+run_command\s*\([^)]*\)", r"(case BT_CMD_ENABLE:)",
              r"\1 adopt_init_result();")),
    ("is_initialized() reads the loop task's flag",
     on_other(BT_CPP, SIG_BT_IS_INIT, r"return __atomic_load_n\(&g_stack_up, __ATOMIC_ACQUIRE\);",
              "return g_initialized;")),
    ("a refusal is published by a plain store",
     on_other(BT_CPP, SIG_BT_SET_FAIL,
              r"__atomic_store_n\(&g_init_fail_reason, \(const char\*\)text, __ATOMIC_RELEASE\);",
              "g_init_fail_reason = text;")),
    ("a refusal is rewritten in the one slot a reader holds",
     on_other(BT_CPP, SIG_BT_SET_FAIL, r"g_init_fail_slot = \(g_init_fail_slot \+ 1\) % INIT_FAIL_SLOTS;",
              "g_init_fail_slot = 0;")),
    ("init_fail_reason() reads the address with no acquire",
     on_other(BT_CPP, SIG_BT_FAIL_REASON, r"return __atomic_load_n\(&g_init_fail_reason, __ATOMIC_ACQUIRE\);",
              "return g_init_fail_reason;")),
    ("load_paired_devices() fills the loop task's list",
     on_other(BT_CPP, SIG_BT_LOAD_PAIRED, r"nvs->getBytes\(NVS_KEY_BT_PAIRED, out, data_len\);",
              "nvs->getBytes(NVS_KEY_BT_PAIRED, g_paired_devices, data_len);")),
    ("init() installs the channel's server callbacks itself again",
     lambda s: on_other(BT_CPP, SIG_BT_INIT, r"ble_server_dispatch::attach\(server\);",
                        "ble_server_dispatch::install(server, ble_server_dispatch::kPairing, &g_server_callbacks);")(
         on_other(BT_CPP, SIG_BT_ADOPT,
                  r"\n[ \t]*ble_server_dispatch::set_owner\(ble_server_dispatch::kPairing, &g_server_callbacks\);",
                  "")(s))),
    ("init() leaves the server on NimBLE's default callbacks until the loop task installs",
     on_other(BT_CPP, SIG_BT_INIT, r"\n[ \t]*ble_server_dispatch::attach\(server\);", "")),
    ("install() puts the dispatcher on the server without recording the owner",
     on_other(BT_DISPATCH_H, SIG_DISPATCH_INSTALL, r"g_dispatcher\.set\(role, owner\);", "")),
    # The F167 review: the loop task loads the saved settings and list, the
    # adoption keeps them, the dispatcher's owner is named without a second
    # setCallbacks(), and Remove and Clear wait for the stack.
    ("the adoption overwrites the settings with init()'s copy (a Disable comes back on)",
     on_other(BT_CPP, SIG_BT_ADOPT, r"(g_initialized = true;)", r"g_settings = b.applied; \1")),
    ("the adoption empties the loop task's paired list",
     on_other(BT_CPP, SIG_BT_ADOPT, r"(g_initialized = true;)", r"g_paired_count = 0; \1")),
    ("the adoption reloads the saved settings over the owner's commands",
     on_other(BT_CPP, SIG_BT_ADOPT, r"(g_initialized = true;)", r"load_settings(&g_settings); \1")),
    ("init() loads the paired list again",
     on_other(BT_CPP, SIG_BT_INIT, r"(load_settings\(&b\.applied\);)",
              r"\1 load_paired_devices(g_paired_devices, &g_paired_count, &g_paired_by_identity);")),
    ("update() never loads the saved settings",
     on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*load_saved\(\);", "")),
    ("update() takes the bring-up's result before loading the saved settings",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"(adopt_init_result\(\);)", r"\1 load_saved();")(
         on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*load_saved\(\);", "")(s))),
    ("update() loads the saved settings after the commands ran",
     lambda s: on_other(BT_CPP, SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"load_saved(); \1")(
         on_other(BT_CPP, SIG_UPDATE, r"\n[ \t]*load_saved\(\);", "")(s))),
    ("load_saved() loads on every pass (over the owner's commands)",
     on_other(BT_CPP, SIG_BT_LOAD_SAVED, r"if \(g_saved_loaded\) return;\s*", "")),
    ("a command reloads the saved settings (a second caller)",
     on_other(BT_CPP, r"\bstatic\s+Result\s+run_command\s*\([^)]*\)", r"(case BT_CMD_ENABLE:)",
              r"\1 g_saved_loaded = false;")),
    ("Remove runs before the stack is up",
     on_other(BT_CPP, SIG_BT_REMOVE, r"if \(!g_initialized\) \{", "if (false) {")),
    ("Clear all runs before the stack is up",
     on_other(BT_CPP, SIG_BT_CLEAR, r"if \(!g_initialized\) \{", "if (false) {")),
    ("the adoption installs the channel's callbacks with install() (a second setCallbacks)",
     on_other(BT_CPP, SIG_BT_ADOPT,
              r"ble_server_dispatch::set_owner\(ble_server_dispatch::kPairing, &g_server_callbacks\);",
              "ble_server_dispatch::install(g_server, ble_server_dispatch::kPairing, &g_server_callbacks);")),
    ("set_owner() puts the dispatcher on the server too",
     on_other(BT_DISPATCH_H, SIG_DISPATCH_SET_OWNER, r"(g_dispatcher\.set\(role, owner\);)",
              r"\1 attach(NimBLEDevice::getServer());")),
    ("Opera names its owner without attaching the dispatcher",
     on_other(BLE_OPERA_H, SIG_OPERA_INIT,
              r"ble_server_dispatch::install\(g_pServer,\s*ble_server_dispatch::kLink,\s*&g_serverCallbacks\);",
              "ble_server_dispatch::set_owner(ble_server_dispatch::kLink, &g_serverCallbacks);")),
]
# Rule BV7 (the F167 review): no advertising start while the sketch's
# bring-up worker registers services.
BV_MUTATIONS += [
    ("start_advertising() starts while the bring-up worker registers services",
     on_other(BT_CPP, SIG_BT_START_ADV, r"if \(g_bringup_worker_running\) \{[^}]*\}", "")),
    ("restore_radio() puts the advertiser back during the bring-up",
     on_other(BT_CPP, SIG_BT_RESTORE_RADIO,
              r"if \(g_bringup_worker_running\) \{\s*g_advertise_after_bringup = true;\s*\} else \{\s*"
              r"g_advertising->start\(\);\s*\}", "g_advertising->start();")),
    ("a link's end starts the advertiser itself",
     on_other(BT_CPP, r"\bstatic\s+void\s+apply_disconnect\s*\([^)]*\)",
              r"(ble_presence::notify_console_connected\(false\);)",
              r"\1 if (g_advertising) g_advertising->start();")),
    ("stop_advertising() leaves a held start for the worker's end",
     on_other(BT_CPP, SIG_BT_STOP_ADV, r"g_advertise_after_bringup = false;[^\n]*\n", "\n")),
    ("the worker's end drops a held start",
     on_other(BT_CPP, SIG_BT_WORKER_FINISHED, r"g_advertise_after_bringup = false;\s*start_advertising\(\);",
              "g_advertise_after_bringup = false;")),
    ("the sketch never tells the channel its worker started",
     on("ino", SIG_INO_DISCOVERY_START, r"bluetooth_channel::bringup_worker_started\(\);", "")),
    ("the sketch tells the channel after creating the worker",
     lambda s: on("ino", SIG_INO_DISCOVERY_START, r"(!= pdPASS\) \{)",
                  r"\1 bluetooth_channel::bringup_worker_started();")(
         on("ino", SIG_INO_DISCOVERY_START, r"bluetooth_channel::bringup_worker_started\(\);", "")(s))),
    ("the finalize stage never tells the channel the worker finished",
     on("ino", SIG_INO_FINALIZE, r"bluetooth_channel::bringup_worker_finished\(\);", "")),
    ("a failed worker create leaves the channel waiting",
     on("ino", SIG_INO_DISCOVERY_START, r"bluetooth_channel::bringup_worker_finished\(\);[^\n]*\n", "\n")),
    ("the worker tells the channel it finished, from its own task",
     on("ino", r"\bstatic\s+void\s+ble_bringup_task\s*\(\s*void\s*\*\s*\)",
        r"(__atomic_store_n\(&g_ble_bringup_done, true, __ATOMIC_RELEASE\);)",
        r"bluetooth_channel::bringup_worker_finished(); \1")),
]
# Rule BV6 (F196): every Bluetooth answer at its own length.
BV_MUTATIONS += [
    ("send_success() serializes into a 128-byte buffer again",
     on_other(BT_API, r"\bstatic\s+inline\s+esp_err_t\s+send_success\s*\([^)]*\)", r"return send_doc\(req, doc\);",
              "char buffer[128]; serializeJson(doc, buffer); return send_json_response(req, buffer);")),
    ("the settings route serializes into its own buffer",
     on_other(BT_API, api_handler("handle_bluetooth_settings_get"), r"return send_doc\(req, doc\);",
              "char buffer[512]; serializeJson(doc, buffer, sizeof(buffer)); return send_json_response(req, buffer);")),
    ("the status route goes back to a 2048-byte reserve",
     on_other(BT_API, api_handler("handle_bluetooth_status"), r"return send_doc\(req, doc\);",
              "String buffer; if (!buffer.reserve(2048)) { return send_error(req, \"x\"); } "
              "serializeJson(doc, buffer); return send_json_response(req, buffer.c_str());")),
    ("send_doc() reserves a fixed size",
     on_other(BT_API, SIG_BT_SEND_DOC, r"out\.reserve\(measureJson\(doc\) \+ 1\)", "out.reserve(128)")),
    ("send_doc() goes on after a failed allocation",
     on_other(BT_API, SIG_BT_SEND_DOC, r"if \(!out\.reserve\(measureJson\(doc\) \+ 1\)\) \{[^}]*\}",
              "(void)out.reserve(measureJson(doc) + 1);")),
    ("send_doc() serializes into a stack buffer of the measured size",
     on_other(BT_API, SIG_BT_SEND_DOC, r"serializeJson\(doc, out\);\s*return send_json_response\(req, out\.c_str\(\)\);",
              "char small[64]; serializeJson(doc, small); return send_json_response(req, small);")),
]
# Rule BV5 (F189): the bond store's status.
BV_MUTATIONS += [
    ("init() leaves the bond store's status to NimBLE's default (it evicts)",
     on_other(BT_CPP, SIG_BT_INIT, r"\n[ \t]*NimBLEDevice::setDeviceCallbacks\(&g_store_callbacks\);", "")),
    ("another module sets the device callbacks",
     raw_other(f"{SKETCH}/ble_status_api.h", "static NimBLEServer* g_server = nullptr;",
               "static NimBLEServer* g_server = nullptr;\n"
               "static void hook() { NimBLEDevice::setDeviceCallbacks(nullptr); }")),
    ("a full store makes room by unpairing the oldest bond",
     on_other(BT_CPP, SIG_BT_STORE_STATUS, r"(if \(event->event_code == BLE_STORE_EVENT_OVERFLOW\) \{)",
              r"\1 if (ble_gap_unpair_oldest_peer() == 0) return 0;")),
    ("a full store defers to NimBLE's default answer",
     on_other(BT_CPP, SIG_BT_STORE_STATUS, r"return BLE_HS_EUNKNOWN;",
              "return NimBLEDeviceCallbacks::onStoreStatus(event, nullptr);")),
    ("a full store lets a new pairing start",
     on_other(BT_CPP, SIG_BT_STORE_STATUS, r"(e\.u\.store\.obj_type = event->full\.obj_type;\s*"
              r"\(void\)post_event\(e, EVENT_LOSSY_LIMIT\);\s*)return BLE_HS_ESTORE_CAP;", r"\1return 0;")),
    ("the list takes a bond the store did not keep",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"if \(!found && !NimBLEDevice::isBonded\(identity\)\) \{",
              "if (false) {")),
    ("a store refusal posts at the full limit",
     on_other(BT_CPP, SIG_BT_STORE_STATUS, r"(e\.u\.store\.obj_type = event->overflow\.obj_type;\s*"
              r"\(void\)post_event\(e), EVENT_LOSSY_LIMIT\)", r"\1)")),
]
# Rule BV8 (F210): whether Bluetooth is on, as another task reads it.
BV_MUTATIONS += [
    ("the advertise handler reads the loop task's enabled flag in place again",
     on_other(BT_API, api_handler("handle_bluetooth_advertise_start"), r"if\s*\(!bring_up\(\)\)",
              "if (!bluetooth_channel::is_enabled() && !bring_up())")),
    ("the pair handler reads the loop task's enabled flag in place again",
     on_other(BT_API, api_handler("handle_bluetooth_pair_start"), r"if\s*\(!bring_up\(\)\)",
              "if (!bluetooth_channel::is_enabled() && !bring_up())")),
    ("bluetooth_channel.h declares is_enabled again",
     raw_other(BT_H, "bool is_advertising();", "bool is_enabled();\nbool is_advertising();")),
    ("read_enabled() reads the flag in place",
     on_other(BT_CPP, SIG_BT_READ_ENABLED, r"return read_settings\(\)\.enabled;", "return g_settings.enabled;")),
    ("read_enabled() reads the boot defaults",
     on_other(BT_CPP, SIG_BT_READ_ENABLED, r"return read_settings\(\)\.enabled;",
              "return kDefaultSettings.enabled;")),
    ("is_enabled() is not static",
     raw_other(BT_CPP, "static bool is_enabled() {", "bool is_enabled() {")),
    ("a reader answers from is_enabled() (it runs on the httpd task)",
     on_other(BT_CPP, r"\bvoid\s+read_status\s*\(\s*BluetoothStatus\s*\*\s*out\s*\)",
              r"v\.status\.enabled\s*=\s*kDefaultSettings\.enabled;", "v.status.enabled = is_enabled();")),
    ("the self-test reads bluetooth_channel::is_enabled()",
     raw_other(f"{SKETCH}/selftest_api.h", "any_active = any_active || bluetooth_channel::is_advertising();",
               "any_active = any_active || bluetooth_channel::is_advertising() || "
               "bluetooth_channel::is_enabled();")),
]
# Rule BV9 (F191, F209): no pairing completes, and no bond stays, without the owner.
SIG_BT_START_PAIRING = r"\bstatic\s+bool\s+start_pairing\s*\(\s*\)"
SIG_BT_CONFIRM_PASSKEY = r"\bstatic\s+void\s+apply_confirm_passkey\s*\([^)]*\)"
SIG_BT_READ_STATUS = r"\bvoid\s+read_status\s*\(\s*BluetoothStatus\s*\*\s*out\s*\)"
BV_MUTATIONS += [
    ("onPassKeyDisplay() hands NimBLE a usable passkey outside pairing mode",
     on_other(BT_CPP, SIG_BT_PASSKEY_DISPLAY, r"passkey \+= PASSKEY_REFUSED;", "")),
    ("onPassKeyDisplay() reads the gate relaxed",
     on_other(BT_CPP, SIG_BT_PASSKEY_DISPLAY, r"&g_owner_pairing_open, __ATOMIC_ACQUIRE",
              "&g_owner_pairing_open, __ATOMIC_RELAXED")),
    ("onPassKeyDisplay() refuses after it posted the digits",
     lambda s: on_other(BT_CPP, SIG_BT_PASSKEY_DISPLAY, r"(\(void\)post_event\(e\);)",
                        r"\1 if (!__atomic_load_n(&g_owner_pairing_open, __ATOMIC_ACQUIRE)) "
                        r"{ passkey += PASSKEY_REFUSED; }")(
         on_other(BT_CPP, SIG_BT_PASSKEY_DISPLAY,
                  r"if \(!__atomic_load_n\(&g_owner_pairing_open, __ATOMIC_ACQUIRE\)\) \{\s*"
                  r"passkey \+= PASSKEY_REFUSED;\s*\}", "")(s))),
    ("a refused passkey stays in NimBLE's range",
     raw_other(BT_CPP, "static const uint32_t PASSKEY_REFUSED = 1000000;",
               "static const uint32_t PASSKEY_REFUSED = 0;")),
    ("the passkey gate is open while a Numeric Comparison waits",
     on_other(BT_CPP, SIG_BT_OWNER_PAIRING_OPEN, r"g_pairing\.state == PAIR_PIN_DISPLAYED\)",
              "g_pairing.state == PAIR_PIN_DISPLAYED || g_pairing.state == PAIR_CONFIRMING)")),
    ("the passkey gate ignores allow_pairing",
     on_other(BT_CPP, SIG_BT_OWNER_PAIRING_OPEN, r"g_settings\.allow_pairing &&\s*", "")),
    ("publish_views() never hands the gate over",
     on_other(BT_CPP, SIG_BT_PUBLISH_VIEWS,
              r"__atomic_store_n\(&g_owner_pairing_open, owner_pairing_open\(\), __ATOMIC_RELEASE\);", "")),
    ("pairing mode opens the gate itself",
     on_other(BT_CPP, SIG_BT_START_PAIRING, r"(g_pairing\.state = PAIR_INITIATED;)",
              r"\1 __atomic_store_n(&g_owner_pairing_open, true, __ATOMIC_RELEASE);")),
    ("the loop task shows a passkey the host task refused",
     on_other(BT_CPP, SIG_BT_APPLY_PASSKEY, r"e\.u\.passkey\.pin >= PASSKEY_REFUSED \|\| ", "")),
    ("the loop task shows a passkey outside pairing mode",
     on_other(BT_CPP, SIG_BT_APPLY_PASSKEY, r" \|\| !owner_pairing_open\(\)", "")),
    ("the passkey's digits go to the health log again",
     on_other(BT_CPP, SIG_BT_APPLY_PASSKEY, r"\"Pairing PIN displayed\", nullptr\)",
              '"Pairing PIN displayed", String(e.u.passkey.pin).c_str())')),
    ("the Numeric Comparison's digits go to the health log again",
     on_other(BT_CPP, SIG_BT_CONFIRM_PASSKEY, r"(\"BLE pairing PIN — awaiting user confirmation\"), nullptr\)",
              r'\1, pin_str)')),
    ("an unauthenticated bond is kept, its link up",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"\} else if \(link\.bonded\) \{", "} else if (false) {")),
    ("an unauthenticated bond is deleted with the radio busy",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"forget_bond\(NimBLEAddress\(link\.id_address\)\)",
              "NimBLEDevice::deleteBond(NimBLEAddress(link.id_address))")),
    ("an unauthenticated bond's link stays up",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"g_server->disconnect\(link\.handle\);", "(void)link.handle;")),
    ("an authenticated bond is forgotten too",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"(if \(link\.authenticated\) \{)",
              r"\1 (void)forget_bond(NimBLEAddress(link.id_address));")),
    ("the bond store is counted off the status view",
     on_other(BT_CPP, SIG_UPDATE, r"(g_events\.consume\(apply_event\);)", r"\1 refresh_bond_store();")),
    ("the status view never counts the bond store",
     on_other(BT_CPP, SIG_BT_PUBLISH_STATUS, r"refresh_bond_store\(\);", "")),
    ("a reader on the httpd task reads the bond count in place",
     on_other(BT_CPP, SIG_BT_READ_STATUS, r"(\*out = v\.status;)", r"\1 out->bonds_held = g_bonds_held;")),
]
# Rule BV9's second half: the roads F191's and F209's review found.
SIG_BT_RETRY_KEPT = r"\bstatic\s+void\s+retry_kept_bonds\s*\([^)]*\)"
BV_MUTATIONS += [
    ("a comparison asks the owner after a completed pairing",
     on_other(BT_CPP, SIG_BT_COMPARISON_OPEN, r"g_pairing\.state == PAIR_CONFIRMING\)",
              "g_pairing.state == PAIR_CONFIRMING || g_pairing.state == PAIR_COMPLETE)")),
    ("a comparison asks the owner with new pairings off",
     on_other(BT_CPP, SIG_BT_COMPARISON_OPEN, r"g_settings\.allow_pairing &&\s*", "")),
    ("a comparison outside pairing mode asks the owner",
     on_other(BT_CPP, SIG_BT_CONFIRM_PASSKEY_APPLY,
              r"if \(!owner_comparison_open\(\)\) \{.*?return;\s*\}", "")),
    ("a comparison outside pairing mode waits instead of a no",
     on_other(BT_CPP, SIG_BT_CONFIRM_PASSKEY_APPLY,
              r"(if \(!owner_comparison_open\(\)\) \{)\s*NimBLEDevice::injectConfirmPasskey\(\*e\.u\.passkey\.conn, false\);",
              r"\1")),
    ("a comparison is refused only after the pending copy is taken",
     lambda s: on_other(BT_CPP, SIG_BT_CONFIRM_PASSKEY_APPLY, r"(g_pending_pair_active = true;)",
                        r"\1 if (!owner_comparison_open()) { NimBLEDevice::injectConfirmPasskey(*e.u.passkey.conn, "
                        r"false); delete e.u.passkey.conn; g_passkey_refused_count++; return; }")(
         on_other(BT_CPP, SIG_BT_CONFIRM_PASSKEY_APPLY,
                  r"if \(!owner_comparison_open\(\)\) \{.*?return;\s*\}", "")(s))),
    ("an unauthenticated bond's link is dropped only when recorded",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"if \(g_server\) \{(\s*g_server->disconnect\(link\.handle\);)",
              r"if (g_server && recorded) {\1")),
    ("an unauthenticated bond drops the recorded link instead of its own",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"g_server->disconnect\(link\.handle\);",
              "g_server->disconnect(g_connection_handle);")),
    ("an unauthenticated bond fails the owner's pairing again",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"(keep_for_retry\(link\.id_address\);\s*\})",
              r"\1 if (recorded) { g_pairing.state = PAIR_FAILED; }")),
    ("a bond the stack kept is never retried",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"keep_for_retry\(link\.id_address\);", "(void)0;")),
    ("a listed phone's Just Works re-pair keeps its bondless entry",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"drop_entries_without_bond\(\);", "(void)0;")),
    ("an owner's re-pair of a kept identity is retried away",
     on_other(BT_CPP, SIG_BT_AUTH_COMPLETE, r"drop_kept\(link\.id_address\);", "")),
    ("the reconcile refuses an authenticated link too",
     on_other(BT_CPP, SIG_BT_REFUSE_LINKS, r" \|\| live\.isAuthenticated\(\)", "")),
    ("the reconcile refuses a link whose bond is gone again",
     on_other(BT_CPP, SIG_BT_REFUSE_LINKS, r"if \(!NimBLEDevice::isBonded\(live\.getIdAddress\(\)\)\) continue;", "")),
    ("the reconcile never refuses a dropped report's bond",
     on_other(BT_CPP, SIG_BT_RECONCILE_LINK, r"refuse_unauthenticated_links\(\);", "")),
    ("the adoption never refuses a pre-owner bond",
     on_other(BT_CPP, SIG_BT_ADOPT, r"refuse_unauthenticated_links\(\);", "")),
    ("the adoption refuses before the channel owns the callbacks",
     lambda s: on_other(BT_CPP, SIG_BT_ADOPT, r"(ble_server_dispatch::set_owner\()", r"refuse_unauthenticated_links(); \1")(
         on_other(BT_CPP, SIG_BT_ADOPT, r"refuse_unauthenticated_links\(\);", "")(s))),
    ("update() never retries a kept bond",
     on_other(BT_CPP, SIG_UPDATE, r"retry_kept_bonds\(now\);", "")),
    ("a kept bond is retried by the reconcile too",
     on_other(BT_CPP, SIG_BT_RECONCILE_LINK, r"(g_kept_bond_count = 0;)", r"\1 retry_kept_bonds(millis());")),
    ("a dropped event leaves the kept bonds retried",
     on_other(BT_CPP, SIG_BT_RECONCILE_LINK, r"g_kept_bond_count = 0;", "")),
    ("the reconcile leaves the bond count stale",
     on_other(BT_CPP, SIG_BT_RECONCILE_LINK, r"g_bond_store_dirty = true;", "")),
    ("a refused pairing leaves the bond count stale",
     on_other(BT_CPP, SIG_BT_STORE_FULL, r"g_bond_store_dirty = true;", "")),
    ("the kept list is cleared from outside its owners",
     on_other(BT_CPP, SIG_BT_START_PAIRING, r"(g_pairing\.state = PAIR_INITIATED;)", r"\1 g_kept_bond_count = 0;")),
    ("the dispatcher hands NimBLE six digits with no pairing owner",
     on_other(BT_DISPATCH_H, SIG_DISPATCH_PASSKEY, r"return 1000000u \+ esp_random\(\) % 1000000;",
              "return esp_random() % 1000000;")),
    ("the channel sets a static passkey",
     on_other(BT_CPP, SIG_BT_PUBLISH_VIEWS, r"(publish_status_view\(\);)", r"NimBLEDevice::setSecurityPasskey(123456); \1")),
    ("the sketch sets a static passkey",
     on("ino", SIG_SETUP, r"(bluetooth_channel::skip_boot_bring_up_for_safe_mode\(\);)",
        r"\1 NimBLEDevice::setSecurityPasskey(246810);")),
]
# Rule BV10 (F259, F260): safe mode says so; a late bring-up says what it left out.
BV_MUTATIONS += [
    ("setup() never marks a safe-mode boot's bring-up skipped",
     on("ino", SIG_SETUP, r"bluetooth_channel::skip_boot_bring_up_for_safe_mode\(\);", "")),
    ("setup() marks the bring-up skipped on a normal boot",
     lambda s: on("ino", SIG_SETUP, r"(g_ble_discovery_ready\s*=\s*true;)",
                  r"\1\n    bluetooth_channel::skip_boot_bring_up_for_safe_mode();")(
         on("ino", SIG_SETUP, r"bluetooth_channel::skip_boot_bring_up_for_safe_mode\(\);", "")(s))),
    ("setup() marks the skip under FEATURE_BLE",
     on("ino", SIG_SETUP, r"#if FEATURE_BLUETOOTH(\s*bluetooth_channel::skip_boot_bring_up_for_safe_mode)",
        r"#if FEATURE_BLE\1")),
    ("setup() marks the skip under a condition",
     on("ino", SIG_SETUP, r"(bluetooth_channel::skip_boot_bring_up_for_safe_mode\(\);)",
        r"if (g_hw.safe_mode) \1")),
    ("skip_boot_bring_up_for_safe_mode() stores relaxed",
     on_other(BT_CPP, SIG_BT_SKIP_BOOT, r"__ATOMIC_RELEASE", "__ATOMIC_RELAXED")),
    ("boot_bring_up_skipped() reads relaxed",
     on_other(BT_CPP, SIG_BT_BOOT_SKIPPED, r"__ATOMIC_ACQUIRE", "__ATOMIC_RELAXED")),
    ("the worker's end clears the safe-mode mark",
     on_other(BT_CPP, SIG_BT_WORKER_FINISHED, r"(g_bringup_worker_running = false;)",
              r"\1\n  __atomic_store_n(&g_boot_bring_up_skipped, false, __ATOMIC_RELEASE);")),
    ("another file marks the bring-up skipped",
     raw_other(f"{SKETCH}/selftest_api.h", "any_active = any_active || bluetooth_channel::is_advertising();",
               "any_active = any_active || bluetooth_channel::is_advertising();\n"
               "  bluetooth_channel::skip_boot_bring_up_for_safe_mode();")),
    ("the worker never reports ble_status",
     on("ino", SIG_BT_BRINGUP_TASK,
        r"bluetooth_channel::boot_service_registered\(bluetooth_channel::BT_BOOT_SVC_STATUS\);", "")),
    ("the worker reports ble_status before it registered",
     lambda s: on("ino", SIG_BT_BRINGUP_TASK, r"(if \(ble_status::init\()",
                  r"bluetooth_channel::boot_service_registered(bluetooth_channel::BT_BOOT_SVC_STATUS);\n          \1")(
         on("ino", SIG_BT_BRINGUP_TASK,
            r"bluetooth_channel::boot_service_registered\(bluetooth_channel::BT_BOOT_SVC_STATUS\);", "")(s))),
    ("the worker reports ble_discovery when ble_manager failed",
     lambda s: on("ino", SIG_BT_BRINGUP_TASK, r"(g_ble_mgr_result = -1;)",
                  r"\1\n      #if FEATURE_BLUETOOTH\n      bluetooth_channel::boot_service_registered("
                  r"bluetooth_channel::BT_BOOT_SVC_DISCOVERY);\n      #endif")(
         on("ino", SIG_BT_BRINGUP_TASK,
            r"bluetooth_channel::boot_service_registered\(bluetooth_channel::BT_BOOT_SVC_DISCOVERY\);", "")(s))),
    ("the worker reports ble_discovery outside FEATURE_BLUETOOTH",
     on("ino", SIG_BT_BRINGUP_TASK,
        r"#if FEATURE_BLUETOOTH(\s*//[^\n]*\n\s*bluetooth_channel::boot_service_registered\(bluetooth_channel::BT_BOOT_SVC_DISCOVERY\);)"
        r"\s*#endif", r"\1")),
    ("the worker reports ble_status under a condition",
     on("ino", SIG_BT_BRINGUP_TASK,
        r"(bluetooth_channel::boot_service_registered\(bluetooth_channel::BT_BOOT_SVC_STATUS\);)",
        r"if (ble_server) \1")),
    ("boot_service_registered() reports relaxed",
     on_other(BT_CPP, SIG_BT_SERVICE_REGISTERED, r"__ATOMIC_RELEASE", "__ATOMIC_RELAXED")),
    ("boot_service_registered() overwrites the services already reported",
     on_other(BT_CPP, SIG_BT_SERVICE_REGISTERED, r"__atomic_fetch_or\(&g_boot_services_registered, service, __ATOMIC_RELEASE\);",
              "__atomic_store_n(&g_boot_services_registered, service, __ATOMIC_RELEASE);")),
    ("the channel does not expect ble_manager's services on FULL",
     raw_other(BT_CPP, "#if defined(FEATURE_BLE) && FEATURE_BLE\n    BT_BOOT_SVC_DISCOVERY |\n#endif\n", "")),
    ("a late bring-up is read from the clock of the worker, not the stack the boot left",
     on_other(BT_CPP, SIG_BT_LATE, r"\s*&&\s*!g_boot_stack_up", "")),
    ("boot_bring_up_finished() records nothing of the stack",
     on_other(BT_CPP, SIG_BT_BOOT_FINISHED, r"g_boot_stack_up = is_initialized\(\);[^\n]*", "")),
    ("boot_bring_up_finished() records the stack after its release",
     lambda s: on_other(BT_CPP, SIG_BT_BOOT_FINISHED, r"(__atomic_store_n\(&g_boot_bring_up_tried, true, __ATOMIC_RELEASE\);)",
                        r"\1 g_boot_stack_up = is_initialized();")(
         on_other(BT_CPP, SIG_BT_BOOT_FINISHED, r"g_boot_stack_up = is_initialized\(\);[^\n]*", "")(s))),
    ("adopt_init_result() reads the boot's stack itself",
     on_other(BT_CPP, SIG_BT_ADOPT, r"(note_late_bring_up\(\);)", r"g_boot_stack_up = true; \1")),
    ("a late bring-up registers ble_status's service on the running server",
     on_other(BT_CPP, SIG_BT_NOTE_LATE, r"(if \(missing == 0\) return;)",
              r"\1 if (g_server) (void)g_server->createService(SERVICE_UUID);")),
    ("a late bring-up says so every pass",
     on_other(BT_CPP, SIG_UPDATE, r"(g_events\.consume\(apply_event\);)", r"note_late_bring_up(); \1")),
    ("a late bring-up says nothing",
     on_other(BT_CPP, SIG_BT_ADOPT, r"\n[ \t]*note_late_bring_up\(\);", "")),
    ("GET /api/bluetooth drops services_missing",
     on_other(BT_API, api_handler("handle_bluetooth_status"),
              r"JsonArray missing = doc\[\"services_missing\"\]\.to<JsonArray>\(\);",
              "JsonArray missing = doc[\"missing\"].to<JsonArray>();")),
    ("GET /api/bluetooth reads the worker's report in place",
     on_other(BT_API, api_handler("handle_bluetooth_status"), r"(doc\[\"late_bring_up\"\] = status\.late_bring_up;)",
              r"\1 (void)bluetooth_channel::boot_bring_up_skipped();")),
]
MUTATIONS += BV_MUTATIONS


# The sources every mutation starts from, set by self_test() before the pool
# forks, so a worker inherits them (and MUTATIONS, whose mutators are closures
# that cannot be pickled) instead of receiving 5 MB per task.
_SELF_TEST_INPUT: tuple[dict, dict[str, str]] | None = None


def _mutation_problem(i: int) -> str | None:
    """Apply MUTATIONS[i] and say what is wrong if the check does not bite."""
    srcs, others = _SELF_TEST_INPUT
    name, mutate = MUTATIONS[i]
    try:
        m = mutate(srcs)
    except AnchorMissing as missing:
        return (f"self-test: mutation '{name}' no longer applies (anchor {missing}) — "
                "the source changed shape; update this guard's mutations with it")
    if m == srcs:
        return f"self-test: mutation '{name}' changed nothing"
    if not check(m["ino"], m["mesh_h"], m["mesh_cpp"], m["mqtt"], m.get("others", others)):
        return f"self-test: the check did not bite on mutation '{name}'"
    return None


def self_test_jobs() -> int:
    """Worker count: LOOP_CMD_JOBS if set, else the CPUs this process may use."""
    env = os.environ.get("LOOP_CMD_JOBS", "").strip()
    if env:
        try:
            return max(1, int(env))
        except ValueError:
            raise SystemExit(f"LOOP_CMD_JOBS={env!r} is not a number")
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except AttributeError:  # not Linux
        return max(1, os.cpu_count() or 1)


def self_test(srcs: dict, others: dict[str, str]) -> list[str]:
    global _SELF_TEST_INPUT
    srcs = dict(srcs)
    srcs["others"] = others
    _SELF_TEST_INPUT = (srcs, others)
    indices = range(len(MUTATIONS))
    jobs = min(self_test_jobs(), len(MUTATIONS))
    ctx = None
    if jobs > 1:
        try:
            ctx = multiprocessing.get_context("fork")
        except ValueError:  # no fork on this platform: run them here
            ctx = None
    if ctx is None:
        results = map(_mutation_problem, indices)
    else:
        # Small chunks keep the workers evenly loaded (the mutations' costs
        # differ by area); map() returns the answers in submission order.
        with ProcessPoolExecutor(max_workers=jobs, mp_context=ctx) as pool:
            results = list(pool.map(_mutation_problem, indices,
                                    chunksize=max(1, len(MUTATIONS) // (jobs * 8))))
    return [p for p in results if p]


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


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
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
    # `--no-self-test`: the check alone (regression_check.sh's path, inside its
    # ten-minute budget); CI runs the mutation self-test in its own job.
    problems = [] if "--no-self-test" in argv else self_test(srcs, others)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print("canary-wap loop-task ownership holds: the mesh's, Chirp's and Bluetooth's owner "
          "commands are internal to their channels and run from update()'s drain, the REST "
          "handlers only submit, the mesh, Chirp and Bluetooth status routes read only what the "
          "loop task published, the NimBLE host task's callbacks only post events update() "
          "applies, "
          "the MQTT client is replaced only by loop()'s re-init, which never stops a client "
          "(the retire_task worker does), and every client's network timeout keeps a "
          "loop-task publish under the watchdog "
          + ("(self-test skipped)." if "--no-self-test" in argv
             else f"({len(MUTATIONS)} mutations refused)."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
