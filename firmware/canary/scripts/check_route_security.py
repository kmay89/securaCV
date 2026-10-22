#!/usr/bin/env python3
"""Route-security conformance check for the canary (PlatformIO) firmware.

Every HTTP route firmware/canary registers must be credential-gated — the
device bearer token (auth_gate / auth_check), or the physical BOOT-tap
provisioning gate — unless it is on the explicit PUBLIC allowlist below
with a written reason. This pins the fail-closed property statically, so a
new endpoint cannot silently widen the unauthenticated surface. It is the
canary-tree sibling of
firmware/projects/canary-wap/tests_host/check_route_security.py (same
string-aware brace matcher, same allowlist-cannot-rot rule), and it closes
the standing ESP32S3_OPTIMIZATION_ROADMAP.md §3.8 P2 sweep "no new handler
is added without auth_gate".

Threat model, fixed and narrow. In scope: a careless or mistaken firmware
edit (a refactor, a new route, a new feature, a moved line, a widened
helper, a new file) that would ship a credential on a public route, ungate
a route, open the SoftAP or open the BOOT tap without a press. Out of scope:
a deliberate attempt to evade this checker (preprocessor tricks aimed at
it, token pasting, symbol aliasing, inline assembly, injected build flags),
and a committer who edits the checker itself, which could `return 0`. The
rules below that refuse those classes (the paste, symbol-binding and
build-input rules, and the macro rule's flag and C++-keyword clauses) are
defense in depth over the spellings they name, not a claim that every
spelling is held: gcc's long aliases (`--prefix`, which the driver rewrites
to `-B`, `--for-linker`, `--for-assembler`, and `--define-macro`, the long
form of `-D`, which the flag clause does not read), a letter-led glued
`-B<dir>`, and the `-L`/`-l` library options pass them.

The canary registers every route in ONE place,
lib/securacv_network/src/securacv_network.cpp's registerHttpHandlers(),
as `httpd_uri_t x = {...}; register_route(...)`. The captive-portal probes
are registered from a string array in a loop (`.uri = p`), so the parser
reads that array too — otherwise the six probe routes would be invisible
here and an unauthenticated route could hide in the same loop.

Beyond "every route reaches a gate", six wiring rules the marker scan alone
cannot see (each added after a review mutation, or a review finding, passed
every other gate), and the premise the last one stands on:

  * a gate marker counts only as a whole name, as the census spells its rows
    (a helper whose name ends in one, `lan_auth_gate(req)` or
    `setup_auth_gate(req)`, is no gate, so the route it guards fails as
    ungated rather than passing a gate the census never sees), and only
    where its RESULT decides something — inside an
    `if (...)` / `while (...)` / `switch (...)` condition or a `return`
    expression, or initializing a `const` local that one of those later
    reads. A bare `auth_gate(req);`, `(void)bearer_present_and_valid(req);`
    or a local nobody tests ignores the answer and is not a gate, and a
    `return` inside a lambda body is the lambda deciding, not the handler.
    auth_bearer_grant() answers one of four (NONE / OK / WRONG / HELD), so
    it counts only where its call is compared `== AuthManager::BearerResult::OK`
    (`!= ...::WRONG` would serve a request with no header or a held one);
  * every `send_html_with_token(...)` call passes `page_token_inject(req)` as
    its third argument — never a literal `true`/`false` or anything else —
    and page_token_inject goes through provisioning_gate::page_token_decide
    and the take hook, never a peek (a peek let one BOOT tap unlock every
    home-LAN page load for 30 s);
  * inside registerHttpHandlers(server) every registration uses the
    `server` parameter: the table goes on whichever server is primary (TLS
    or plain), and on FEATURE_HTTPS builds m_http_server is still nullptr
    there, so `register_route(m_http_server, ...)` drops the route with only
    a serial line (the shape two sibling branches add routes in); and every
    registration call, or registration function taken as a pointer, hands
    over an `httpd_uri_t` initializer the parser reads, and register_route()
    registers only the route it is handed (check_registration_count): an
    assignment form, a cast `.method` or a lambda `.handler` is a route no
    rule would read, so it fails;
  * no function registers more routes than the max_uri_handlers budget it
    sets (registerHttpHandlers is held to kRouteTableSlots, with and without
    the mesh block; every #if branch is counted, i.e. the worst case);
  * the Host comes first on every path that can hand out the bearer token or
    spend the BOOT tap, and its answer is used (check_host_first).
    host_is_foreign(req) — the Host must name this device unless the request
    came over the SoftAP — is asked before any bearer is read, any tap is
    taken or any token is emitted, and it counts only in two shapes: an
    `if (host_is_foreign(req))` whose branch returns (the refusal), or the
    FIRST argument of a host-tested decision (receipt_decide,
    page_token_decide), directly or through a `const bool` local set from
    it. Any other first argument to those decisions fails. A gated route
    gets the Host from auth_gate (whose own body is held to that order) or
    asks it itself; the provisioning receipt, gated by a bearer or the tap,
    binds receipt_decide's verdict once, sends the receipt only under an
    explicit SERVE_BEARER / SERVE_TAP test and ends in the Host refusal, so
    REFUSE_HOST, a dropped branch or a verdict added later refuses;
    page_token_inject hands the Host to page_token_decide before any grant
    or tap; send_html_with_token streams the token only in
    `(<verdict> == PageToken::INJECT) ? auth_get_token() : ""`. This rule
    checks shapes, not meaning: the decisions' order is proved by
    firmware/tests_host/test_provisioning_gate.cpp, and this rule proves the
    glue hands them the Host and obeys their verdict (added with the
    provisioning receipt's own Host check; the four rules above were green
    without it);
  * every answer that carries a credential holds to a bearer-only grant, on
    the flagship's own gates (check_bearer_only_answers, sweep F338; the
    canary-wap checker's F328 rule, ported to what this tree has). The four
    credentials are the bearer token, the SoftAP password, the TLS pin
    (public in every TLS handshake, held because the apps trust it as the
    pin) and the TLS key (the private key behind that certificate). Each is
    read where SECRET_READS spells it (its stores: the token's buffer and its
    NVS record, the SoftAP password's two buffers, begin()'s parameter and
    the Wi-Fi driver's AP config and its read-back, the TLS key's buffer and
    its NVS record; its accessor and its deriver, by bare name, so taking
    one's address is a read) and carried up the call graph to the route
    handlers, the carry stopping at a device-side use and at a comparer. No
    string literal under ROOTS is the value of the token's, the TLS key's,
    the first-boot flag's or the private key's NVS record (NVS_LITERAL_KEYS,
    read from include/canary_config.h; adjacent literals joined, escapes
    decoded), so a record is reached only by the name the reads follow.
    Every file under ROOTS is read (tree_sources: an `.inc`, `.hpp` or `.ino`
    of code is code, and any other kind fails, but a library manifest by its
    name, library.json or library.properties, which is read for the
    libraries it fetches instead, and a file git ignores, an editor's backup
    or an OS's .DS_Store, which is never committed), and read
    as the compiler reads it (strip_comments): a raw string literal is one
    literal, so code after the web UI's HTML is code; a backslash-newline is
    spliced away, so the accessor's name split across two lines by one is the
    accessor; and a digraph is the token it spells, so `%:define` is a #define
    and `<% %>` a block. Outside ROOTS the check reads what the flagship
    reaches (build_reach): each .c/.cpp build_src_filter compiles from
    firmware/common, and each file an #include reaches from ROOTS or from
    those, beside the including file or in include/ and the -I folders the
    flag files name (another product's source, setup_portal.cpp, or a host
    test there is not read). Those files name nothing the check holds under
    ROOTS but INCLUDE_MENTIONS (the NVS keys where canary_config.h defines
    them and reset_scope_keys.h lists them, ap_password.h's deriver and
    provisioning_gate.h's two Host-first decisions): no credential's
    spelling, no pinned, census, accessor, emitter or comparer name, and no
    census row's spelling (WiFi.softAP, the radio's mode, the driver's
    config, the AP class, the tap's open, another HTTP server), so a
    function there that reads a credential, opens the tap, starts the
    network or raises the SoftAP open, called from under ROOTS, fails. A
    build input the check cannot read fails too: an #include through a
    macro, a file the build compiles that is not C
    or C++, a build_src_filter entry that matches nothing, a file of this
    repository outside ROOTS and the include roots that the flagship
    #includes or compiles, and in the flag files the build inputs
    _BUILD_INPUT spells (build_input_problems; defense in depth, see the
    threat model): a linker option through `-Wl,`, `-Xlinker`, `--wrap`,
    `--defsym` or `-T` (`-Wl,--wrap=<symbol>` sends every call of a function
    to a name no rule reads), a forced include (`-include`/`--include`/
    `-imacros`/`--imacros`, in the space, glued and `=` forms), an include or
    sysroot path the reach does not read (`-isystem`, `-iquote`,
    `-idirafter`, `-iprefix`, `--sysroot`; only `-I` is read), a
    compiler-driver injector (`-specs`, `-fplugin`, `-wrapper`, `-B` before a
    path, `-Xpreprocessor`/`-Wp,`, `-Xassembler`/`-Wa,`), a response file
    (`@file`), a build script, a local library, a moved source folder or
    PlatformIO's `!command` dynamic value (a command run at build time whose
    stdout becomes flags), a key of those in any case (configparser
    lowercases it), or a `${sysenv.*}` reference anywhere in the flag files
    (a value the build-time environment sets, which can name a library or a
    flag the check cannot read; their text is read, so an empty environment
    at check time hides none). Every library an env of the flagship's
    configuration fetches, or a library manifest under ROOTS declares, is a
    REGISTRY_LIBS row with its reason, or a library under ROOTS by its name
    (library_problems). The configuration is read with PlatformIO's own
    reader, never by imitating it, at the version the builds that ship the
    flagship pin (PLATFORMIO_PIN, 6.1.19): platformio_lib_deps hands
    platformio.ini to ProjectConfig from the project's folder, as `pio run`
    does, and takes each env's lib_deps as it resolves them ([env] and each
    `extends`, the extra_configs files, every interpolation, `${this.option}`
    included, the older key names `lib_use`, `lib_force` and `lib_install`,
    a key at any indent and in any case, a one-line list split on ", "); each
    entry is read by PlatformIO's PackageSpec, and each manifest by its
    manifest parser, each dependency the PackageSpec PlatformIO builds from
    it. A configuration PlatformIO refuses, an env whose lib_deps it cannot
    resolve and a file it reads that the flag files (flag_sources) do not
    hold fail, and without the module, or with another version of it, the
    check fails before any rule runs, naming `python3 -m pip install
    platformio==6.1.19`; it never falls back to a reading of its own. So a
    library that raises its own access point or serves its own routes
    (WiFiManager's autoConnect(), handed no AP password, raises an open
    SoftAP and serves a firmware upload) fails, naming every env that
    fetches it, until a reviewer adds its row. A row is an `owner/name` and
    vouches for the dependencies its library declares, which are not read
    (the four rows' libraries declare none); no row admits a bare registry
    name, a registry id, a URL or a path.
    Every reader is named: an accessor in its home (SECRET_ACCESSORS), an
    emitter in its home carrying exactly what TOKEN_EMITTERS says, its
    reading statements pinned (EMITTER_STATEMENTS), a comparer reading only
    the token, a device-side use whose reading statements are exactly its
    SECRET_LOCAL_USES row (statement pins, keyed by file and qualified
    function, in order), or a SECRET_ANSWERS handler, whose own reading
    statements are pinned the same way (ANSWER_STATEMENTS: the receipt reads
    none itself, so a serial or MQTT line under its served verdict fails);
    outside every function only OUTSIDE_FUNCTION_LINES may name one. Every
    pinned statement is held where it sits: the header of every block
    around it and every
    preprocessor conditional open around it in its function, and a goto (or
    an ESP_GOTO_* macro) before it, are folded in front of it as `[within
    ...]`, so a statement moved under `if (false) {` or `#if 0`, or past a
    goto, is not the statement it was (begin()'s copy of the SoftAP key,
    compiled out, would leave raiseAp() an empty key). A mint's statement
    runs through its refusal too, so the pin on main.cpp's
    `derive_ap_password(device.privkey, ...)` holds the call, its refusal
    and the halt, at setup()'s own level with no block or preprocessor
    conditional around it and no goto before it, and the token's
    `derive_api_token(privkey, ...)` is held inside its `if (!loaded) {`.
    Every call of a mint follows, in its function and at its own level, the
    statement that establishes the key it reads (MINT_AFTER: setup()'s
    witness_provision_device() refusal, witness_provision_device()'s
    establish_identity_key()), with nothing before that statement in its
    function able to leave (outside a lambda): the key is all zeros before,
    neither deriver refuses a zero key, and an early `return true;` in
    witness_provision_device() would hand setup() a success over that key.
    Every registered handler that carries one is a SECRET_ANSWERS row with
    exactly what it carries and its grant: 'bearer' (`if (!auth_gate(req))
    return ESP_OK;` at its own top level, before every answer site),
    'receipt' (receipt_decide handed exactly RECEIPT_GRANTS, every answer
    under a SERVE_* test, check_host_first's check_receipt_dispatch holding
    the dispatch) or 'page' (only through send_html_with_token(...,
    page_token_inject(req)) or another page handler; a public route may
    carry only this way). No handler's own body names the private key. A
    function defined twice (an `#if` body and its `#else` one) is every one
    of its bodies: their reads and calls are its own, and each body is held
    to its row and its grant. Pinned whole (GLUE_DEFS, each defined exactly
    once under ROOTS, in its file, so a second body or a shadow in another
    lib fails): the glue every grant runs through (host_is_foreign,
    auth_gate, bearer_present_and_valid, from_ap_subnet, page_token_inject
    with F341's auth_handed_token on the tap that served,
    provisioning_gate_take, the hook setter, main.cpp's take hook, and the
    lockout glue F340 and F341 left: auth_get_instance, AuthManager::check,
    grant, compare, handedToken and constantTimeCompare and the auth_check,
    auth_bearer_grant and auth_handed_token wrappers, with the lockout's
    source key, request_peer_ipv4), from_ap_subnet's helpers (netif_ip4,
    be_word_host_order, sockaddr_ip4_host_order), page_token_inject's setup
    input at its source (the setup lib's setup_init, setup_is_first_boot,
    setup_is_active, setup_mark_complete and setup_check_timeout), the BOOT
    tap at its source (handle_boot_button whole, the press read from its
    GPIO, and the peek hook), the three accessors, the token's deriver and
    its two helpers (hmac_sha256, format_api_token_string), the SoftAP
    password's way to the radio (announce_setup_network, apply_ap_security),
    and the port-80 path (handle_captive_probe and handle_https_redirect, with
    the two members the probe's hint branch decides on, isTlsEnabled and
    getHttpsServer, whose stores the census holds to their writers: in that
    text, the setup page is not served on plain HTTP beside HTTPS).
    The gate primitives are mentioned only where the census says, a call or
    an address taken alike, and defined only where it says
    (GATE_CALL_SITES): every auth_gate call is the whole refusal statement,
    at its handler's own top level in every body that holds it (a head split
    across `#if` / `#else` is two), under no block or conditional, with
    nothing able to leave before it (a return, goto, throw, co_return or an
    ESP_RETURN_* / ESP_GOTO_* macro) but the rate limit's refusal;
    auth_handed_token runs on the receipt only under a served verdict; the
    AuthManager's one instance (s_auth) is reached only by
    auth_get_instance(), and that only by the three wrappers, and the auth
    lib never names its own `this`; constantTimeCompare runs only in compare;
    the receipt is sent only from its route and the page's emitter only from
    the three page handlers; a device-side use that hands a credential out
    through a parameter, or takes one in, is called only where its row says
    (the TLS key's loader and writer from initTls, the private key's
    nvs_load_key from establish_identity_key, whose record only its three
    functions name); the boot hands auth_load_or_derive g_device.privkey,
    with its refusal; every WiFi.softAP() is begin()'s or raiseAp()'s
    statement, with its key, and the radio's mode (WiFi.mode, enableAP,
    softAPConfig, WiFiGenericClass::mode, esp_wifi_set_mode, any of which
    brings the SoftAP up on the open config dropAp() leaves, and
    softAPdisconnect and softAPbandwidth, which leave it up or raise it on
    such a config) is set only by begin(), dropAp() and raiseAp(), each
    statement where it sits (dropAp's softAPdisconnect with its `true`), and
    each raise is followed by the softAP() that keys it with nothing able to
    leave between them (outside a lambda); begin() is started once, by
    setup() with g_ap_password (network_init, which hands it on, is called
    by nothing; the manager's instance is named only by its accessor, the
    accessor only in a binding named net or network or with a member after
    it, every reference or pointer to the manager, typed or deduced, is
    named net or network, MANAGER_NAMES, the manager is never handed on as
    a value, no other function returns it, and a member's bare, `this->` or
    `(*this).` begin() counts, so the census sees each start); `WiFi` is
    named only with a member after it; the driver's config is written only
    by apply_ap_security (the SoftAP's) and ensure_sta_pmf_capable (the
    STA's), each its statement where it sits, and Arduino-ESP32 3.x's AP
    class is not named; the BOOT tap is opened only by handle_boot_button's
    short press, where it sits, g_prov_gate is named only by the two hooks
    and that handler (the peek hook's copy, s_gate_is_open, only by the hook
    setter and /api/status), its declaration holding its initial value, and
    that handler, whose sweep expires a tap nobody took, runs on every pass
    of loop(), its call there the bare statement at loop()'s own level with
    nothing before it in loop() able to leave (outside a lambda); none of
    these servers the core brings is named, nor ESPAsyncWebServer's
    (Arduino's WebServer, HTTPUpdateServer and ArduinoOTA, the raw
    WiFiServer, NetworkServer on core 3.x, WiFiProv, whose SoftAP scheme
    raises an access point of its own, with the ESP-IDF provisioning manager
    it wraps, and AsyncWebServer: routes, a firmware push or an access point
    no rule reads), and a registry library is held where the build fetches
    it (REGISTRY_LIBS, above); the setup state (s_active, s_first_boot,
    s_started_ms)
    and the stored flag first boot is read from (NVS_KEY_SETUP_OK) are named
    only by the setup lib's pinned functions, the credentials reset that also
    clears that flag (factory_reset) runs only from the BOOT hold's dispatch
    and that only from the boot-time hold and handle_boot_button, and the
    setup timeout runs from loop() while setup is active, with nothing before
    its call there able to leave; the TLS flag and
    the TLS server's handle the port-80 hint reads (m_tls_enabled,
    m_https_server) are written only by the constructor and startHttpServer(),
    each statement where it sits; and the token's alphabet is its
    declaration. Every AUTH_MARKERS name is a census row,
    and every PRE_HOST_STEPS name a row, a TOKEN_EMITTERS key or
    auth_get_token, so a deleted gate's name cannot linger in either list.
    No `#define` under ROOTS or in a file outside it the flagship compiles or
    #includes (build_reach), and no `-D` flag in platformio.ini or the files
    its extra_configs names (in the `-D` spelling: the flag clause is defense
    in depth, and gcc's `--define-macro` passes it; see the threat model),
    may name a pinned
    or census function, a device-side use, an accessor, a mint, an emitter,
    a comparer, a credential's spelling or the private key, or be named for
    a function a pinned body or statement calls (`#define auth_gate(r)
    (true)`, or `#define take(...) is_open(...)` turning the take hook into
    a peek, is the preprocessor's shadow of a pinned body); the NVS records'
    names canary_config.h defines are its only exception. No macro or flag
    is a C++ keyword (`#define false true` would make every `return false;`
    below it a `return true;`). Under ROOTS, a macro named for anything else
    a pinned text names (a value's name too: PROVISIONING_GATE_TTL_MS,
    BOOT_SHORT_PRESS_MS) is a ROOTS_DEFINES row, in its file as often as it
    says, and none is #undef'd; and PINNED_DEFINES' SETUP_TIMEOUT_MS is
    defined once, as its row says. No macro or flag pastes tokens (`##`, or
    the digraph `%:%:`) but GNU's `, ##__VA_ARGS__` comma elision, which
    pastes nothing: `#define CV_JOIN(a, b) a##b` names nothing held, and
    `CV_JOIN(auth_get_, token)()` calls the token's accessor, or
    `JOIN(auth_handed_, token)(...)` clears a lockout, under no spelling a rule
    reads (a `#` stringize makes a string, never a call, and is not refused).
    Nothing there binds a name to a symbol in the spellings
    symbol_binding_problems names (defense in depth, as the paste rule is:
    see the threat model), the
    class the empty compiler barrier aside: no asm (`__asm__("_Z14auth_get_tokenv")`
    a label, an asm call) but `asm volatile("" ::: "memory")`, no weakref,
    alias, ifunc or symver attribute and no __REDIRECT macro
    (`__attribute__((weakref("_Z14auth_get_tokenv")))` is the same binding an
    asm label is), no pragma but KNOWN_PRAGMAS' (`#pragma redefine_extname`,
    `#pragma weak t = ...`, as `#pragma` or `_Pragma("...")`), and no identifier
    spelled as a C++ symbol (`_Z...`, `extern "C" const char*
    _Z14auth_get_tokenv();` declaring the accessor by its mangled name) — each
    reaches a function by its symbol, a spelling SECRET_READS and the census do
    not read;
  * the premise the last rule stands on, checked (no_session_problems): the
    flagship has no session. No cookie is read or set, the request headers
    read are `Authorization` (the auth lib) and `Host` (the network lib)
    only, by literal name, a response header's name is a literal and never
    `Set-Cookie`, a URL query is read only by the witness log's paging, and
    a request's raw path (`->uri`, or `.uri` through any reference to the
    request, which keeps its query) only where URI_READERS says. A cookie, a
    session header, a query credential or a lockout keyed on a request
    header fails here, pointing
    at canary-wap's any-session class (ANY_SESSION_GATES, F328):
    CONSOLIDATION.md's Phase 6 cv_session port must bring that rule with it.

(The lambda clause of the first rule is why a receipt_decide(...) whose
verdict nobody reads fails it, although its bearer and tap lambdas return
the gates' answers.)

The parser names what it reads: one head parser (function_defs) names a
constructor for its class, its initializer list included, a member as
`Class::name`, and a function whose head ends in `const`, `noexcept`,
`override`, `final` or a trailing return type (`auto f(...) -> void {`); a
lambda names no function, so what it calls is its enclosing function's. A
census name outside every parsed function counts as a declaration only after
a type, never after a statement's word (`return`, `else`, `case`), and never
with an `=`, a `(` or a `return` before it in its statement (`static
AuthManager* const v = &auth_get_instance();` is a use). The route-gate and
Host-first rules read every definition of a handler, whatever its head
(`static int`, `IRAM_ATTR`, `noexcept`, a trailing return type).

What a text check cannot see, stated so nobody leans on it: names are
matched by spelling, so a copy of a credential under another name is not
followed, and neither is a value passed on as a parameter
(announce_setup_network()'s pass is held by the pins on the statements and
the body that handle it instead); the network manager's own spellings are
the exception above; the files outside ROOTS the flagship reaches are
read only for their #define lines, their asm and symbol bindings, and the
names above, so their
code is otherwise not followed, and a held name that is also an ordinary
word there (INCLUDE_COMMON_WORDS: begin, check, s_active and the like) is
not read in them; a library the build links is not read: the registry ones
are held to REGISTRY_LIBS by name, each row with its reason (and vouching for
its library's own dependencies), as PlatformIO 6.1.19 resolves the
configuration (a reading a newer PlatformIO adds, which the unpinned PR
builds would take, is held only once PLATFORMIO_PIN moves with the builds
that ship the flagship), and the core is held only by the spellings
the census names, so a server or an access point the core can start under
another name is not seen (a server written on the raw socket API, the core's
UDP classes, WiFiUDP, AsyncUDP and DNSServer, of which the flagship's own
captive DNS responder is a WiFiUDP, its mDNS and NetBIOS responders, of which
the flagship runs mDNS, a BLE GATT server, the core's BLEServer or NimBLE's,
which the flagship's status service runs, and an ESP-IDF component such as
esp_local_ctrl that starts one of its own); the linker is not read (a flag
that would bind a symbol fails instead, in the spellings _BUILD_INPUT names:
`-L`/`-l` and the driver's long aliases are not among them); a value is not
checked, only its spelling, but SETUP_TIMEOUT_MS's (a `memset` of the private key just before
a mint passes, and both credentials would become HMACs of a zero key over
public inputs; the values
include/canary_config.h and the shared headers define,
PROVISIONING_GATE_TTL_MS among them, are read only for the four NVS keys'
strings and for a macro's name, never as numbers, so a header's own value is
not checked, though a shadow of it under ROOTS is); trigraphs are not read
(the GNU language modes the cores build in ignore them); an NVS
record is followed by its name and its literal key, not by a key built at
run time (`snprintf`, a char array) or a walk of the whole namespace
(`nvs_entry_find`, `esp_partition_read`, a namespace clear); a call by bare
name reaches every definition of that name (Preferences' begin() reaches
ScvNetworkManager::begin), which errs toward failing; the private key is not
carried (it is read across the tree to sign and derive): only a handler's
own body naming it fails, not a helper that answers it; a statement before
a gated route's refusal that does the route's work itself, without leaving,
is not seen; an early `return` between a mint's MINT_AFTER statement and the
mint is not refused (the rest of its function goes with it: setup()'s start
of the network, or, in witness_provision_device(), the token's mint, which
leaves the bearer empty, so auth_gate refuses every gated route as
unprovisioned), though a goto before a mint is, and so is any way out before
its MINT_AFTER statement; macros are read only as `#define` lines and `-D`
flags (a PlatformIO extra_script, which could add one, fails the flag rule); of
the members the pinned glue calls, the hint's isTlsEnabled and
getHttpsServer are pinned and the AuthManager's are glue, but getStatus
(which picks the setup page or Apple's success body, not the token, and
which two classes define), WiFi.getMode, Preferences' reads and ResetHold's
step are not held, and the decisions in firmware/common (provisioning_gate.h,
tls_policy.h, host_guard.h, ap_security_policy.h, reset_hold.h,
auth_backoff.h) are their host tests', not this check's; the
flagship's derive_api_token and its two helpers are held by their text, not
run (no host test compiles them; test_securacv_auth stubs derive_api_token);
the shared firmware/common/network/auth_backoff.h is outside ROOTS and its
arithmetic is test_auth_backoff.cpp's; and a pinned body or statement means
an edit to that glue updates its row in the same review (the failure prints
the row as it now is, so the update is mechanical). Each rule function is
self-tested with its own fixtures, but main()'s own plumbing is not run by a
self-test: a one-token edit to main() that dropped `+ reach_failures` (or
`+ pio_failures`) from the failure list, or passed `includes={}` (or
`manifests={}`, or `lib_deps={}`) to check_bearer_only_answers rather than
build_reach's (or library_manifests', or platformio_lib_deps') output, would
leave the self-test green (the real tree has no reach failure, clean
headers, no file PlatformIO reads that flag_sources does not, and no library
that fails). The compositions those mutations would hide — build_reach's
`includes` fed to check_bearer_only_answers, where a header reader is
caught, and flag_sources', platformio_lib_deps' and library_manifests'
output fed to it, where an env's library, a `${sysenv.*}` and a manifest's
dependency are held — are each held by a temp-dir fixture in the self-test;
main()'s four wiring lines themselves are not, as the threat model here is a
careless firmware edit, not a malicious edit to this checker (which could
`return 0`).

Run from the repo root, with PlatformIO 6.1.19 installed (CI: firmware.yml
job "Mesh + Scout Host Tests", its "Set up PlatformIO 6.1.19" step, then
step "Check canary (PIO) route security", beside the WAP's audit):
    python3 -m pip install platformio==6.1.19
    python3 firmware/canary/scripts/check_route_security.py
"""

import contextlib
import functools
import glob
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# PlatformIO's own readers, which the library rule reads the flagship's
# configuration and its libraries' manifests with, at PLATFORMIO_PIN (below).
# Without the module, main() fails naming the install command; nothing
# guesses in its place.
try:
    import platformio
    from platformio.package.manifest.parser import ManifestFileType, ManifestParserFactory
    from platformio.package.meta import PackageSpec
    from platformio.project.config import ProjectConfig
except ImportError:
    platformio = None

# Every .cpp/.h under the canary tree's libs and src. Only the network lib
# registers routes today; scanning the whole tree means a second lib that
# starts registering routes is audited without editing this script.
ROOTS = (Path("firmware/canary/lib"), Path("firmware/canary/src"))
# What the check reads under ROOTS: every C and C++ source and header spelling,
# so an `.inc`, `.hpp` or `.ino` #included from a .cpp is code like any other.
# Any other file under ROOTS fails (tree_sources), but a library manifest by
# its name (PlatformIO's library.json, Arduino's library.properties; read for
# the libraries it fetches, library_problems) and a file git ignores (an
# editor's backup, an OS's .DS_Store: never committed).
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".h++",
                   ".inc", ".inl", ".ipp", ".tpp", ".ino")
NON_SOURCE_NAMES = ("library.json", "library.properties")

# The calls whose presence in a handler body counts as a credential gate, each
# matched as a whole name (marker_at), as the census spells its rows: a helper
# whose name ends in one (`lan_auth_gate(`) is not that gate, and the census,
# which would never see the helper, cannot then be bypassed by it. Each is a
# real call that enforces access:
AUTH_MARKERS = (
    "auth_gate(",               # bearer, fail-closed 503 when unprovisioned
    "auth_check(",              # AuthManager bearer check (401/403/429), per source address
    "auth_bearer_grant(",       # the accounted grant (NONE/OK/WRONG/HELD), sends nothing;
                                # the caller tests for OK and answers the rest (F340):
                                # it counts only compared `== AuthManager::BearerResult::OK`
    "bearer_present_and_valid(",  # auth_bearer_grant plus refuses an unprovisioned credential
    "provisioning_gate_take(",  # physical BOOT tap, one tap = one consumer
    "receipt_decide(",          # provisioning_gate::receipt_decide: the Host, then
                                # the bearer, then the tap (host-tested). It runs the
                                # last two as lambdas, whose `return` is not the
                                # handler deciding, so the verdict itself must be
                                # bound and tested — and check_host_first holds the
                                # handler to serving only on a SERVE_* verdict
)

# (method, uri) pairs that are intentionally unauthenticated. Every entry
# needs a reason; an entry that stops matching a registered route fails the
# check so the list cannot rot.
PUBLIC_ALLOWLIST = {
    # The two HTML pages. The page itself carries no secret EXCEPT the
    # bearer token injected into its placeholder, and that injection is
    # decided per request by provisioning_gate::page_token_decide
    # (host-tested in firmware/tests_host/test_provisioning_gate.cpp):
    # first-boot setup, bearer-authenticated, SoftAP-subnet peer, or a BOOT
    # tap the load then spends — a home-LAN request with none of those gets
    # the page with an EMPTY token. Every API the page calls is auth_gate'd.
    # check_page_token_wiring() below is what makes this reason true: it
    # fails a page call that does not pass page_token_inject(req).
    # check_host_first() holds page_token_inject to asking the Host before
    # the grants and the tap.
    ("GET", "/"): "dashboard shell; token injection gated by page_token_decide (F20 gap #11), Host first",
    ("GET", "/setup"): "setup wizard shell; token injection gated by page_token_decide (F20 gap #11), Host first",
    # OS captive-portal probes must answer plainly or the OS disconnects.
    # While setup is active (or the STA is down) they serve the setup page
    # through the same page_token_policy; otherwise a fixed success body.
    ("GET", "/hotspot-detect.html*"): "Apple captive probe",
    ("GET", "/library/test/success.html*"): "Apple captive probe (legacy)",
    ("GET", "/generate_204*"): "Android captive probe",
    ("GET", "/gen_204*"): "Android captive probe (legacy)",
    ("GET", "/connecttest.txt*"): "Windows NCSI probe",
    ("GET", "/ncsi.txt*"): "Windows NCSI probe (legacy)",
    # The roll-call the Witness Wall polls (tvos/discovery/DISCOVERY.md): the
    # same coarse body the display and the WAP serve open, read cross-origin
    # by a television — name, product, online, chain, hub; never a key, a
    # MAC or an address. Rate-limited like every route. docs/TRUST_MODEL.md.
    ("GET", "/api/fleet"): "Witness Wall roll-call: coarse by construction, CORS-open (DISCOVERY.md)",
    ("OPTIONS", "/api/fleet"): "CORS preflight for the roll-call; serves nothing",
    # Two registrations share GET /*: the main table's captive wildcard
    # fallback (registered LAST: 302 → /setup while setup is active, else
    # 404), and, with FEATURE_HTTPS, the port-80 server's handle_https_redirect,
    # which POST /* is too. That server starts only once HTTPS is up
    # (startRedirectServer, after httpd_ssl_start), so a plain_http_exempt
    # path it hands to handle_captive_probe gets the probe's plain body or,
    # off the TLS server, the TLS hint (send_tls_captive_hint), never the
    # page token. Everything else is a 307 to https:// (F15), or a 500 when
    # the target would not fit. Both handlers are pinned whole (GLUE_DEFS),
    # and so are the two members the hint branch decides on (isTlsEnabled,
    # getHttpsServer), whose stores the census holds to their writers: that
    # text is what holds it (the hint branch, and a redirect that hands
    # nothing but a probe path to a page). Its SECRET_ANSWERS row ('page')
    # holds it to the page grant because its text reaches send_html_with_token
    # through the probe.
    ("GET", "/*"): "captive wildcard fallback (302/404) / port-80 https redirect: "
                   "a probe path gets the probe's plain answer or the TLS hint, else 307 "
                   "(500 on an oversize target)",
    ("POST", "/*"): "FEATURE_HTTPS port-80 server: a probe path gets the probe's plain answer "
                    "or the TLS hint, else 307 to https:// (F15; 500 on an oversize target)",
}

# Handlers that gate internally in ways the marker scan cannot attribute to
# a single route. Name → reason. Keep this empty unless a handler genuinely
# needs it: a marker in the body is always the better proof.
SELF_GATING_HANDLERS = {}

# Floor on parsed registrations. Today's table is ~60 (fewer without the
# mesh feature, but the parser reads every #if branch). A parse that finds
# far fewer means the registration style changed and the parser went blind.
MIN_ROUTES = 50


def raw_literal_end(text: str, i: int) -> int:
    """When the `"` at `i` opens a raw string literal (`R"delim(...)delim"`,
    with an optional `u8`, `u`, `U` or `L` before the `R` and no identifier
    character before that), the offset just past its closing quote, else -1.
    A raw literal holds `"`, `//`, `/*` and braces as text (the web UI's HTML
    and JavaScript), so both literal walkers below must skip it whole: read as
    an ordinary string it ends at its first `"`, and everything after the
    literal, to the end of its file, is read inside a string instead of as
    code."""
    if i == 0 or text[i - 1] != "R":
        return -1
    j = i - 1
    for prefix in ("u8", "u", "U", "L"):
        if text[max(0, j - len(prefix)):j] == prefix:
            j -= len(prefix)
            break
    if j > 0 and (text[j - 1].isalnum() or text[j - 1] == "_"):
        return -1  # an identifier ending in R, then an ordinary string
    open_ = text.find("(", i + 1, i + 18)
    if open_ < 0 or any(c in text[i + 1:open_] for c in ' )\\\t\v\f\n"'):
        return -1
    close = text.find(")" + text[i + 1:open_] + '"', open_ + 1)
    return close + (open_ - i) + 1 if close >= 0 else -1


# A backslash, any blanks after it (GCC accepts them, with a warning), and the
# newline it splices away.
_SPLICE = re.compile(r"\\[ \t\f\v]*\r?\n")


def splice_lines(text: str) -> str:
    """Join every backslash-newline, as the compiler does before it reads a
    single token (translation phase 2): `auth_get_\\<newline>token()` is a call
    of auth_get_token and `#\\<newline>define` a #define, which every spelling
    rule here would otherwise read as two words or as none. The newlines it
    takes out go back at the end of the joined line, so every line after it
    keeps its number. (A raw string literal's splices, which the compiler
    reverts, are joined too: that only makes a literal's text longer.)"""
    if "\\" not in text or not _SPLICE.search(text):
        return text
    out, pending, at = [], 0, 0
    for m in _SPLICE.finditer(text):
        piece = text[at:m.start()]
        if pending and "\n" in piece:
            nl = piece.index("\n")
            piece = piece[:nl + 1] + "\n" * pending + piece[nl + 1:]
            pending = 0
        out.append(piece)
        pending += 1
        at = m.end()
    rest = text[at:]
    nl = rest.find("\n")
    rest = (rest[:nl + 1] + "\n" * pending + rest[nl + 1:]) if nl >= 0 else rest + "\n" * pending
    return "".join(out) + rest


# The C++ digraphs (always on in C++) and the tokens they are, each padded to
# the digraph's length so offsets keep their lines: `%:define auth_gate(r)
# (true)` is a #define and `if (c) <% ... %>` a block, which the #define rule
# and every brace matcher here would otherwise not read as one. The operators
# that start with the same character are taken whole first (`<<%` is `<<`
# then `%`), and `<::` not followed by `:` or `>` is `<` then `::` (C++11).
_DIGRAPHS = {"%:%:": "##  ", "<%": "{ ", "%>": "} ", "<:": "[ ", ":>": "] ", "%:": "# "}
_NOT_DIGRAPH = ("<<=", "<=>", "<<", "<=", "%=", "::")


@functools.lru_cache(maxsize=None)
def strip_comments(text: str) -> str:
    """Remove C/C++ comments, honoring string and char literals, raw string
    literals included (copied whole, their `//` and `/*` being text), after
    joining the lines a backslash continues (splice_lines) and with every
    digraph in code read as the token it is (_DIGRAPHS): what is left is what
    the compiler reads.

    A naive regex swallows code when a string literal contains */ or //,
    so we walk the source tracking literal state. Newlines are preserved
    (comments become spaces) so nothing merges across lines.
    """
    text = splice_lines(text)
    out = []
    i, n = 0, len(text)
    state = "code"  # code | line_comment | block_comment | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code" and c in "<%:":
            if text.startswith("<::", i) and text[i + 3:i + 4] not in (":", ">"):
                op = "<"  # `<::x` is `<` then `::x`
            else:
                op = next((o for o in _NOT_DIGRAPH if text.startswith(o, i)), None)
            if op:
                out.append(op)
                i += len(op)
                continue
            dg = next((d for d in _DIGRAPHS if text.startswith(d, i)), None)
            if dg:
                out.append(_DIGRAPHS[dg])
                i += len(dg)
                continue
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"
                i += 2
            elif c == "/" and nxt == "*":
                state = "block_comment"
                i += 2
            elif c == '"' and raw_literal_end(text, i) > 0:
                end = raw_literal_end(text, i)
                out.append(text[i:end])
                i = end
            elif c == '"':
                out.append(c)
                state = "string"
                i += 1
            elif c == "'":
                out.append(c)
                state = "char"
                i += 1
            else:
                out.append(c)
                i += 1
        elif state == "line_comment":
            if c == "\n":
                out.append("\n")
                state = "code"
            i += 1
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        elif state in ("string", "char"):
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
            else:
                if (state == "string" and c == '"') or (state == "char" and c == "'"):
                    state = "code"
                i += 1
    return "".join(out)


# Pure and called for the same few files hundreds of times (every handler
# lookup, every call/definition scan), so it is memoized: without the cache
# the check spends most of its time re-walking securacv_network.cpp.
@functools.lru_cache(maxsize=None)
def blank_string_contents(text: str) -> str:
    """Replace the *contents* of string/char literals with spaces, keeping
    the quotes and the original length so byte offsets still align with
    `text`. Used only for brace matching: a `{` or `}` inside a JSON literal
    (`"{\\"ok\\":true}"`) must not shift the depth counter.

    A raw string literal (`R"delim(...)delim"`, raw_literal_end) keeps its
    `"delim(` and `)delim"` and has everything between them blanked: the web
    UI's HTML and JavaScript (securacv_webui, securacv_setup_page) is text,
    never a function this check reads, and code after the literal is code.
    """
    out = []
    i, n = 0, len(text)
    state = "code"  # code | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code" and c == '"' and raw_literal_end(text, i) > 0:
            end = raw_literal_end(text, i)
            open_ = text.index("(", i)
            delim = text[i + 1:open_]
            body = text[open_ + 1:end - len(delim) - 2]
            out.append('"' + delim + "(")
            out.append("".join("\n" if ch == "\n" else " " for ch in body))
            out.append(")" + delim + '"')
            i = end
            continue
        if state == "code":
            out.append(c)
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            i += 1
        else:
            if c == "\\":
                out.append("  " if nxt else " ")
                i += 2 if nxt else 1
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                out.append(c)
                state = "code"
            else:
                out.append("\n" if c == "\n" else " ")
            i += 1
    return "".join(out)


def function_bodies(source: str, name: str) -> list:
    """Every brace-matched `esp_err_t <name>(...) {...}` in `source`, in
    order: a handler defined twice (an `#if` body and its `#else` one) is two
    bodies, and every rule that reads a handler reads both.

    Brace matching runs over a string-blanked view; the returned slices come
    from the real `source` (blanking is length-preserving) so the marker
    scan sees the true handler text.
    """
    blanked = blank_string_contents(source)
    out = []
    for m in re.finditer(r"esp_err_t\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", blanked):
        depth, i = 1, m.end()
        while i < len(blanked) and depth:
            if blanked[i] == "{":
                depth += 1
            elif blanked[i] == "}":
                depth -= 1
            i += 1
        out.append(source[m.start():i])
    return out


def function_body(source: str, name: str):
    """The first of function_bodies(source, name), or None."""
    bodies = function_bodies(source, name)
    return bodies[0] if bodies else None


def handler_bodies(sources: dict, name: str) -> list:
    """Every definition of the function `name` (bare) in `sources`, from its
    name to its closing brace, whatever its return type, attributes or head
    (`static int`, `IRAM_ATTR`, `noexcept`, a trailing return type): the
    route-gate and Host-first rules read each one, so an `#else` body under
    another head is a body like the first."""
    return [t[s:e] for t in sources.values() for s, e in definition_spans(t, name)]


def parse_routes(fname: str, text: str):
    """(uri, method, handler, fname) for every httpd_uri_t initializer
    (parse_routes_at, without the offsets)."""
    return [(u, m, h, f) for _, u, m, h, f in parse_routes_at(fname, text)]


def parse_routes_at(fname: str, text: str):
    """(offset, uri, method, handler, fname) for every httpd_uri_t initializer.

    `.uri = "<literal>"` is the common form. `.uri = <identifier>` inside a
    range-for over a `static const char* <array>[] = {...}` is the probe
    loop: every string in that array becomes a route with the loop's
    method and handler. Any other non-literal `.uri` is returned with the
    uri None so main() fails loudly instead of skipping it, and so is a
    `.method` that is not a bare `HTTP_<NAME>` (a cast) or a `.handler` that
    is not a function's name (`&name` is one; a lambda is not): the method
    or the handler None. The initializer is `httpd_uri_t x = {...};` or
    `httpd_uri_t x{...};`. check_registration_count holds every
    registration call to one of these initializers.
    """
    routes = []
    uri_decl = re.compile(r"httpd_uri_t\s+\w+\s*(?:=\s*)?\{(.*?)\}\s*;", re.S)
    for m in uri_decl.finditer(text):
        fields = m.group(1)
        method_text = re.search(r"\.method\s*=\s*([^,}]*)", fields)
        handler_text = re.search(r"\.handler\s*=\s*([^,}]*)", fields)
        method = re.fullmatch(r"\s*HTTP_(\w+)\s*", method_text.group(1)) if method_text else None
        handler = (re.fullmatch(r"\s*&?\s*([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)\s*",
                                handler_text.group(1)) if handler_text else None)
        if not (method and handler):
            routes.append((m.start(), None, method.group(1) if method else None,
                           re.sub(r"\s+", "", handler.group(1)) if handler else None, fname))
            continue
        method = method.group(1)
        handler = re.sub(r"\s+", "", handler.group(1))
        lit = re.search(r'\.uri\s*=\s*"([^"]+)"', fields)
        if lit:
            routes.append((m.start(), lit.group(1), method, handler, fname))
            continue
        ident = re.search(r"\.uri\s*=\s*([A-Za-z_]\w*)\s*[,}]", fields)
        uris = None
        if ident:
            # Find the nearest preceding `for (... <ident> : <array>)`, then
            # that array's initializer.
            head = text[:m.start()]
            loops = list(re.finditer(
                r"for\s*\(\s*(?:const\s+)?char\s*(?:const\s*)?\*\s*"
                + re.escape(ident.group(1)) + r"\s*:\s*(\w+)\s*\)", head))
            if loops:
                arr = loops[-1].group(1)
                init = re.search(
                    r"\b" + re.escape(arr) + r"\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;",
                    head, re.S)
                if init:
                    uris = re.findall(r'"([^"]+)"', init.group(1))
        if uris:
            for u in uris:
                routes.append((m.start(), u, method, handler, fname))
        else:
            routes.append((m.start(), None, method, handler, fname))
    return routes


def _name_pattern(name: str) -> str:
    """`name` as a regex: a bare name, or a qualified one (`AuthManager::compare`)
    with any spacing around each `::`. Anchored at a word boundary, so
    `compare` is not the tail of `constantTimeCompare` and `AuthManager::compare`
    is not `XAuthManager::compare`."""
    return r"\b" + r"\s*::\s*".join(map(re.escape, name.split("::"))) + r"\s*\("


# What may stand between a definition's parameter list and its body: a
# member's `const`, and `noexcept`, `override`, `final`, then a trailing
# return type (`auto f(uint32_t s) -> void {`). A helper written either way is
# a function like any other, not code outside every function, and a shadow
# written either way is a second definition.
_HEAD_TAIL = (r"(?:\s*\b(?:const|noexcept|override|final)\b)*"
              r"(?:\s*->\s*[\w:<>,*&\s]+?)?\s*")
_HEAD_SUFFIX = re.compile(_HEAD_TAIL + r"(?=\{)")


def _body_open(blanked: str, i: int) -> int:
    """The offset of the `{` that opens a definition's body, when the text
    just past a parameter list's `)` at `i` (after any _HEAD_SUFFIX) is one,
    else -1 (a call)."""
    m = _HEAD_SUFFIX.match(blanked, i)
    return m.end() if m else -1


def definition_spans(text: str, name: str) -> list:
    """[(start, end)] of every brace-matched DEFINITION of function `name`
    (any return type, member or free; `Class::name` for one member only). A
    call is `name(...)` followed by `;` or `)`; only a definition is followed
    by `{` (after a member's `const` or a `noexcept`). The span starts at the
    name (its qualifier, for a qualified one)."""
    if name.split("::")[-1] not in text:
        return []
    blanked = blank_string_contents(text)
    out = []
    for m in re.finditer(_name_pattern(name), blanked):
        depth, i = 1, m.end()
        while i < len(blanked) and depth:
            if blanked[i] == "(":
                depth += 1
            elif blanked[i] == ")":
                depth -= 1
            i += 1
        j = _body_open(blanked, i)
        if j >= 0:
            depth, k = 1, j + 1
            while k < len(blanked) and depth:
                if blanked[k] == "{":
                    depth += 1
                elif blanked[k] == "}":
                    depth -= 1
                k += 1
            out.append((m.start(), k))
    return out


def definition_span(text: str, name: str):
    """(start, end) of the first definition of `name` (definition_spans), or
    None."""
    spans = definition_spans(text, name)
    return spans[0] if spans else None


def call_args(text: str, name: str):
    """[(offset, [arg, ...])] for every CALL of `name` in `text` — a
    definition (followed by `{`) is skipped. Arguments are split on
    top-level commas over a string-blanked view and returned stripped, from
    the real text."""
    out = []
    if name not in text:
        return out
    blanked = blank_string_contents(text)
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", blanked):
        depth, i, cuts = 1, m.end(), [m.end()]
        while i < len(blanked) and depth:
            c = blanked[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1:
                cuts.append(i + 1)
            i += 1
        close = i - 1
        if _body_open(blanked, i) >= 0:
            continue  # the definition, not a call
        bounds = cuts + [close + 1]
        args = [text[bounds[k]:bounds[k + 1] - 1].strip()
                for k in range(len(bounds) - 1)]
        out.append((m.start(), [a for a in args if a != ""] if args != [""] else []))
    return out


def _paren_open(blanked: str, close: int) -> int:
    """Offset of the `(` matching the `)` at `close`, or -1."""
    depth = 0
    for k in range(close, -1, -1):
        if blanked[k] == ")":
            depth += 1
        elif blanked[k] == "(":
            depth -= 1
            if depth == 0:
                return k
    return -1


def _paren_close(blanked: str, open_: int) -> int:
    """Offset of the `)` matching the `(` at `open_`, or -1."""
    depth = 0
    for k in range(open_, len(blanked)):
        if blanked[k] == "(":
            depth += 1
        elif blanked[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    return -1


def _opens_lambda(blanked: str, brace: int) -> bool:
    """True when the `{` at `brace` opens a lambda body: `[...] {`,
    `[...](...) {`, with optional mutable / noexcept / `-> T`."""
    head = blanked[max(0, brace - 300):brace]
    m = re.search(r"(?:\s*\b(?:mutable|noexcept|constexpr)\b)*"
                  r"(?:\s*->\s*[\w:<>,*&\s]+?)?\s*$", head)
    j = (m.start() if m else len(head)) - 1
    while j >= 0 and head[j].isspace():
        j -= 1
    if j >= 0 and head[j] == ")":
        o = _paren_open(head, j)
        if o < 0:
            return False
        j = o - 1
        while j >= 0 and head[j].isspace():
            j -= 1
    return j >= 0 and head[j] == "]"


def in_lambda(blanked: str, offset: int) -> bool:
    """True when `offset` sits inside a lambda's body. A `return` there
    decides the lambda's value, not the enclosing handler's — a gate called
    in `[req]() { return bearer_present_and_valid(req); }` decides nothing
    until the lambda's caller uses the answer."""
    depth = 0
    for k in range(offset - 1, -1, -1):
        c = blanked[k]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth:
                depth -= 1
            elif _opens_lambda(blanked, k):
                return True
    return False


def _statement_prefix(blanked: str, i: int) -> str:
    """The statement's text before offset `i`, back to the previous
    `;`, `{` or `}`."""
    stmt = max(blanked.rfind(";", 0, i), blanked.rfind("{", 0, i),
               blanked.rfind("}", 0, i))
    return blanked[stmt + 1:i].strip()


_DECIDING_PREFIX = re.compile(r"^(?:else\s+)?(?:if|while|switch)\s*\(|^return\b")
_CONST_LOCAL = re.compile(r"^const\s+[\w:<>]+\s*[*&]?\s*([A-Za-z_]\w*)\s*=")
# The markers whose result is not a yes or no: the comparison their call must
# stand in to say yes. auth_bearer_grant()'s four values (NONE / OK / WRONG /
# HELD) say yes only as OK, so `!= ...::WRONG` or `== ...::NONE` would serve a
# request with no header, or a held one.
_MARKER_YES = {
    "auth_bearer_grant(": re.compile(r"\s*==\s*(?:AuthManager\s*::\s*)?BearerResult\s*::\s*OK\b"),
}


def _says_yes(blanked: str, marker: str, i: int) -> bool:
    """True when the call of `marker` at `i` says yes or no by itself, or
    stands in the one comparison _MARKER_YES names for it."""
    yes = _MARKER_YES.get(marker)
    if yes is None:
        return True
    close = _paren_close(blanked, i + len(marker) - 1)
    return close >= 0 and yes.match(blanked, close + 1) is not None


@functools.lru_cache(maxsize=None)
def _marker_rx(marker: str):
    return re.compile(r"(?<![\w$])" + re.escape(marker))


def marker_at(text: str, marker: str, start: int = 0) -> int:
    """Offset of the first `marker` in `text` from `start` that is a whole
    name (no identifier character before it), or -1: `auth_gate(` is not the
    tail of `lan_auth_gate(`."""
    m = _marker_rx(marker).search(text, start)
    return m.start() if m else -1


def first_deciding_marker(body: str, marker: str) -> int:
    """Offset of the first occurrence of `marker` whose result decides
    something, or -1. It decides when the statement's text before it (back
    to the previous ';', '{' or '}') opens with `if (`, `else if (`,
    `while (`, `switch (` or `return`, or when it initializes a `const`
    local that a later if / while / switch condition or return expression
    reads. A bare call, a `(void)` cast, a non-const or unread local, and a
    `return` inside a lambda body (the lambda deciding, not the function)
    discard the answer and do not count, and neither does a marker whose
    answer is not a yes or no outside the comparison _MARKER_YES names, or
    one that is the tail of a longer name (marker_at)."""
    blanked = blank_string_contents(body)
    start = 0
    while True:
        i = marker_at(blanked, marker, start)
        if i < 0:
            return -1
        start = i + 1
        if in_lambda(blanked, i) or not _says_yes(blanked, marker, i):
            continue
        prefix = _statement_prefix(blanked, i)
        if _DECIDING_PREFIX.match(prefix):
            return i
        local = _CONST_LOCAL.match(prefix)
        if local:
            end = blanked.find(";", i)
            for use in re.finditer(r"\b" + re.escape(local.group(1)) + r"\b",
                                   blanked[end + 1:] if end >= 0 else ""):
                at = end + 1 + use.start()
                if (not in_lambda(blanked, at)
                        and _DECIDING_PREFIX.match(_statement_prefix(blanked, at))):
                    return i


def marker_decides(body: str, marker: str) -> bool:
    return first_deciding_marker(body, marker) >= 0


def returns_before(body: str, limit: int) -> bool:
    """True when the function body has an UNCONDITIONAL return — a statement
    that starts with `return` directly in the function's own block — before
    offset `limit`. Anything after it is unreachable, so a gate there is no
    gate. `if (x) return y;` is conditional (its statement starts with
    `if`), as is any return nested in a deeper block."""
    blanked = blank_string_contents(body)
    open_brace = blanked.find("{")
    if open_brace < 0:
        return False
    depth = 0
    for m in re.finditer(r"[{}]|\breturn\b", blanked[:limit]):
        tok = m.group(0)
        if tok == "{":
            depth += 1
        elif tok == "}":
            depth -= 1
        elif depth == 1 and m.start() > open_brace:
            stmt = max(blanked.rfind(";", 0, m.start()), blanked.rfind("{", 0, m.start()),
                       blanked.rfind("}", 0, m.start()))
            if blanked[stmt + 1:m.start()].strip() == "":
                return True
    return False


def gate_guards(body: str) -> bool:
    """A handler body is gated when some marker decides something AND no
    unconditional return comes before the first such marker."""
    firsts = [i for i in (first_deciding_marker(body, mk) for mk in AUTH_MARKERS) if i >= 0]
    return bool(firsts) and not returns_before(body, min(firsts))


def check_page_token_wiring(sources: dict) -> list:
    """Every page that can carry the bearer must ask page_token_inject(req),
    and page_token_inject must spend the tap (take), never peek."""
    failures = []
    sites = 0
    for fname, text in sorted(sources.items()):
        for off, args in call_args(text, "send_html_with_token"):
            sites += 1
            line = text.count("\n", 0, off) + 1
            third = args[2] if len(args) >= 3 else None
            if third != "page_token_inject(req)":
                failures.append(
                    f"{fname}:{line}: send_html_with_token(...) passes "
                    f"{third!r} as the page-token verdict — it must be "
                    f"page_token_inject(req) (a literal PageToken::INJECT hands "
                    f"the bearer to every home-LAN caller; that reverts F20 "
                    f"gap #11)")
        span = definition_span(text, "page_token_inject")
        if span:
            body = text[span[0]:span[1]]
            if "page_token_decide(" not in body:
                failures.append(f"{fname}: page_token_inject no longer calls "
                                f"provisioning_gate::page_token_decide (host-tested)")
            if "provisioning_gate_take" not in body:
                failures.append(f"{fname}: page_token_inject does not spend the "
                                f"tap through provisioning_gate_take")
            for peek in ("is_open", "provisioning_gate_is_open", "s_gate_is_open"):
                if re.search(r"\b" + peek + r"\b", body):
                    failures.append(
                        f"{fname}: page_token_inject reads the gate with a peek "
                        f"({peek}) — one tap would unlock every page load for "
                        f"the whole TTL; take it (page_token_decide)")
                    break
            if re.search(r"\breturn\s+(?:true|1)\s*;", body):
                failures.append(f"{fname}: page_token_inject has an unconditional "
                                f"`return true`")
    if sites < 3:
        failures.append(f"found only {sites} send_html_with_token call(s) (floor 3: "
                        f"/, /setup, the Apple probe) — parser regression?")
    if not any(definition_span(t, "page_token_inject") for t in sources.values()):
        failures.append("page_token_inject definition not found")
    return failures


# ── The Host first ────────────────────────────────────────────────────────────

# The Host check: the Host a request targeted must name this device, unless
# the request arrived over the Canary's own SoftAP (securacv_network's
# host_is_foreign over firmware/common/network/host_guard.h), asked of THIS
# request.
_HOST_CALL = r"host_is_foreign\s*\(\s*req\s*\)"

# The host-tested decisions that take the Host verdict as their FIRST
# argument (firmware/common/network/provisioning_gate.h) and decide it before
# anything else they are given.
HOST_DECISIONS = ("receipt_decide", "page_token_decide")

# Functions that put a credential into a response: what each carries (the
# SECRET_READS kinds, which the bearer-only answer rule holds it to exactly)
# and what it emits. Every call of one must sit behind the Host check (or,
# for the page, behind page_token_inject, which asks it).
TOKEN_EMITTERS = {
    "send_provisioning_receipt": (
        "token ap pin",
        "the provisioning receipt (the API token, the SoftAP password and the TLS pin)"),
    "send_html_with_token": (
        "token", "a page with the API token in its __CV_TOKEN__ placeholder"),
}

# Functions that read auth_get_token() only to compare a presented credential
# against it, never into a response.
TOKEN_COMPARERS = {
    "auth_gate": "the API gate: the Host, then the bearer compare",
    "bearer_present_and_valid": "the accounted bearer grant the receipt and the pages use",
}

# ── The bearer-only answers: what the rule reads ─────────────────────────────

# The four credentials, by the spellings this tree reads them with (over
# comment-stripped, string-blanked text): each store, its accessor and its
# deriver. The bearer token lives in s_bearer and in its NVS record
# (NVS_KEY_TOKEN, defined in include/canary_config.h); the SoftAP password in
# the network manager's buffer, main.cpp's, begin()'s parameter that carries
# it there, and the Wi-Fi driver's AP config (`<wifi_config_t>.ap.password`,
# and the read-back itself, `esp_wifi_get_config(WIFI_IF_AP, ...)`). The TLS
# pin is public (every TLS handshake shows the certificate it hashes); it is
# held because the apps trust it as the pin. The TLS key is the private key
# behind that certificate (the network manager's DER buffer and its NVS
# record): whoever holds it serves the pinned certificate and collects the
# bearer the apps send. An accessor or a deriver is spelled by its bare name,
# so taking its address (`&auth_get_token`, `&ScvNetworkManager::getApPassword`)
# is a read like calling it.
SECRET_READS = {
    "token": re.compile(r"\bauth_get_token\b|\bs_bearer\b|\bderive_api_token\b"
                        r"|\bNVS_KEY_TOKEN\b"),
    "ap": re.compile(r"\bgetApPassword\b|\bm_ap_password\b|\bg_ap_password\b|\bap_password\b"
                     r"|\bderive_ap_password\b|\bap\s*\.\s*password\b"
                     r"|\besp_wifi_get_config\s*\(\s*WIFI_IF_AP\b"),
    "pin": re.compile(r"\bgetTlsCertFp\b|\bm_tls_cert_fp_hex\b"),
    "tlskey": re.compile(r"\bm_tls_key_der\b|\bNVS_KEY_TLS_KEY\b"),
}
SECRET_NAMES = {"token": "the bearer token", "ap": "the SoftAP password",
                "pin": "the TLS pin", "tlskey": "the TLS private key"}
# The device's private key, which every credential above derives from (the
# token and the SoftAP password are HMACs of it). It is read across the tree
# to sign and to derive, so it is not carried; a registered route handler's
# own body never names it (_answer_problems).
_PRIVATE_KEY = re.compile(r"\bprivkey\b")
# The file suffixes the tables key rows by (a file is named by its path's
# tail, so a fixture's file and the tree's match alike).
_NET = "securacv_network.cpp"
_NET_H = "securacv_network.h"
_AUTH = "securacv_auth.cpp"
_CRYPTO = "securacv_crypto.cpp"
_WITNESS = "securacv_witness.cpp"
_MAIN = "src/main.cpp"
_SETUP = "securacv_setup.cpp"

# The accessors and the derivers under ROOTS: reading the store (or making
# the value) is their job, so their own body is not a reader; a call of one
# is a read by its caller (SECRET_READS spells each call). Each is defined
# once, in its home file: a second definition by the same name elsewhere is
# a reader like any other. Each body is pinned whole too (GLUE_DEFS), so a
# sink added inside an accessor, or a deriver keyed on anything but the
# private key, fails. derive_ap_password lives outside ROOTS
# (firmware/common/network/ap_password.h, host-tested by test_ap_password.cpp).
SECRET_ACCESSORS = {
    "auth_get_token": (_AUTH, "token"),
    "derive_api_token": (_CRYPTO, "token"),
    "getApPassword": (_NET_H, "ap"),
    "getTlsCertFp": (_NET_H, "pin"),
}
# The mints: a statement that calls one in an if condition is the whole if,
# its branch included, so a statement pin holds the call, its refusal and
# what the refusal does (the F350 review: a pin on the argument alone would
# pass a deleted call, a discarded result or a dropped halt). A mint's pin
# also holds where it sits: the header of every block around it (none for
# setup()'s, `if (!loaded) {` for the token's) and every preprocessor
# conditional open around it in its function, folded in front as
# `[within ...]`, so a mint moved under `if (false) {` or `#if 0` (the call
# compiled out, the key left "" and the SoftAP raised open) fails too.
SECRET_MINTS = ("derive_api_token", "derive_ap_password")
# Where the key a mint reads is established, and so what every call of the
# mint must follow in its function: (file suffix, qualified function, mint,
# the anchor statement, folded, which must sit at the function's own level
# with no block or preprocessor conditional around it and no goto before it).
# The private key starts as zeros (g_device is a zero-initialized static until
# witness_provision_device() runs establish_identity_key()), and neither
# deriver refuses an all-zero key: a mint moved above its anchor, its text
# and its row's order intact, would make the SoftAP key or the bearer a
# function of zeros and public inputs, the same on every Canary.
# establish_identity_key() itself is test_nvs_load_key.cpp's, cut verbatim.
MINT_AFTER = (
    (_MAIN, "setup", "derive_ap_password",
     'if (!witness_provision_device()) { Serial.println("[!!] Device provisioning failed - '
     'HALTING"); while (true) { delay(1000); } }'),
    (_WITNESS, "witness_provision_device", "auth_load_or_derive",
     "if (!establish_identity_key()) return false;"),
)
# The macros whose value a grant leans on, each defined exactly once, in its
# file, as this folded line (and never by a -D flag): SETUP_TIMEOUT_MS ends the
# first-boot unlock (setup_check_timeout), so widening it keeps every
# non-foreign page load's token for as long as it says.
PINNED_DEFINES = {
    "SETUP_TIMEOUT_MS": ("securacv_setup.h",
                         "#define SETUP_TIMEOUT_MS (15UL * 60UL * 1000UL)"),
}
# The NVS records a credential, the first-boot flag and the private key live
# in, by their include/canary_config.h names. The reads follow a record by
# that name (SECRET_READS, the census); a string literal equal to one of their
# values (NVS_KEY_TOKEN's string, say) is the same record under no name the
# rule reads, so none may appear under ROOTS (adjacent literals joined).
NVS_LITERAL_KEYS = ("NVS_KEY_TOKEN", "NVS_KEY_TLS_KEY", "NVS_KEY_SETUP_OK", "NVS_KEY_PRIV")
NVS_KEY_HOME = Path("firmware/canary/include/canary_config.h")

# The declarations outside every function that name a credential, as
# (file suffix, folded statement). Anything else outside a function fails:
# the rule reads credentials per function.
OUTSIDE_FUNCTION_LINES = (
    (_AUTH,
     "static char s_bearer[36];"),
    ("securacv_auth.h",
     "const char* auth_get_token();"),
    ("securacv_crypto.h",
     "bool derive_api_token(const uint8_t privkey[32], char* token_str, size_t token_str_len);"),
    ("securacv_network.h",
     "char m_ap_password[65];"),
    ("securacv_network.h",
     "char m_tls_cert_fp_hex[65];"),
    ("securacv_network.h",
     "uint8_t* m_tls_key_der;"),
    ("securacv_network.h",
     "bool begin(const char* ap_ssid, const char* ap_password, const char* device_id = nullptr);"),
    ("securacv_network.h",
     "bool network_init(const char* ap_ssid, const char* ap_password, const char* device_id = "
     "nullptr);"),
    (_MAIN,
     "static char g_ap_password[16];"),
    (_MAIN,
     'static_assert(sizeof(g_ap_password) >= canary::net::AP_PASSWORD_LEN + 1, "g_ap_password '
     'must hold the cv- key and its NUL");'),
)

# Device-side uses: the functions that read a credential and use it on the
# device (a buffer, the radio, the serial line the owner and the flashers
# read), never in an answer. Keyed by (file suffix, qualified function): a
# function of the same name in another file or class is not excused. Each
# row is (reason, every statement of the function that reads a credential,
# whitespace folded, literals intact, in order). A statement runs from just
# after the nearest `;`, `{` or `}` at its block's level to the first `;`
# or `{` at that level (a mint's, SECRET_MINTS, through its branch and with
# the blocks and conditionals around it). The carry up the call graph stops
# here. A new statement that reads a credential (a serial, BLE or MQTT sink),
# an edited one, or a dropped one fails until the row says it, and the
# failure prints the row as it now is. A function defined twice (an `#if`
# body and its `#else` one) is held body by body, so a stub that reads
# nothing fails as a row it no longer has.
# Where two pinned statements sit: begin()'s start in setup(), and initTls()'s
# body on an HTTPS build.
_BEGIN_IN_SETUP = ("[within { > #if FEATURE_WIFI_AP] if (net.begin(ap_ssid, g_ap_password, "
                   "device.device_id)) {")
_TLS_BUILD = "#if !FEATURE_HTTPS ... #elif !SECURACV_HAS_HTTPS_SERVER ... #else"
SECRET_LOCAL_USES = {
    (_NET, "ScvNetworkManager::ScvNetworkManager"): (
        "the constructor clears the SoftAP password's and the pin's buffers and the TLS key's "
        "pointer",
        (
            "m_ap_password[0] = '\\0';",
            "m_tls_key_der = nullptr;",
            "m_tls_cert_fp_hex[0] = '\\0';",
        )),
    (_NET, "ScvNetworkManager::begin"): (
        "keeps the SoftAP password it is handed for raiseAp() (the copy where it sits: dead or "
        "compiled out, raiseAp() would re-raise on an empty key), raises the SoftAP with it, and "
        "prints the setup-network line both flashers parse",
        (
            "if (ap_password) {",
            "[within if (ap_password) {] strncpy(m_ap_password, ap_password, "
            "sizeof(m_ap_password) - 1);",
            "[within if (ap_password) {] m_ap_password[sizeof(m_ap_password) - 1] = '\\0';",
            "[within else {] m_ap_password[0] = '\\0';",
            "bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, "
            "AP_MAX_CONNECTIONS);",
            "announce_setup_network(m_ap_ssid, m_ap_password);",
        )),
    (_NET, "network_init"): (
        "hands begin() its key (a convenience nothing calls: its census row says so)",
        (
            "return network_get_instance().begin(ap_ssid, ap_password, device_id);",
        )),
    (_NET, "ScvNetworkManager::raiseAp"): (
        "re-raises the SoftAP with its own password after the home link drops, and prints the "
        "setup-network line again",
        (
            "bool ap_ok = WiFi.softAP(m_ap_ssid, m_ap_password, AP_CHANNEL, false, "
            "AP_MAX_CONNECTIONS);",
            "announce_setup_network(m_ap_ssid, m_ap_password);",
        )),
    (_NET, "ScvNetworkManager::initTls"): (
        "loads or makes the certificate and its key (kept for the TLS server), computes the pin "
        "from the certificate and logs its first 16 digits",
        (
            "[within " + _TLS_BUILD + "] if (m_tls_cert_der && m_tls_key_der) return true;",
            "[within " + _TLS_BUILD + "] m_tls_key_der = key;",
            "[within " + _TLS_BUILD + "] canary::net::tls_policy::fingerprint_hex(digest, "
            "m_tls_cert_fp_hex);",
            "[within " + _TLS_BUILD + '] Serial.printf("[TLS] Cert fingerprint: %.16s...\\n", '
            "m_tls_cert_fp_hex);",
        )),
    (_NET, "ScvNetworkManager::startHttpServer"): (
        "asks tls_policy whether a key is loaded, and hands the TLS server the key",
        (
            "const Mode mode = canary::net::tls_policy::decide( FEATURE_HTTPS != 0, "
            "SECURACV_HAS_HTTPS_SERVER != 0, m_tls_cert_der != nullptr && m_tls_key_der != "
            "nullptr, setup_active, &why);",
            "[within if (mode == Mode::HTTPS_REDIRECT) { > #if SECURACV_HAS_HTTPS_SERVER] "
            "ssl.prvtkey_pem = m_tls_key_der;",
        )),
    (_NET, "tls_load_der_from_nvs"): (
        "reads the certificate and its key back from NVS into the buffers it is handed",
        (
            "const size_t klen = nvs.getBytesLength(NVS_KEY_TLS_KEY);",
            "bool ok = c && k && nvs.getBytes(NVS_KEY_TLS_CERT, c, clen) == clen && "
            "nvs.getBytes(NVS_KEY_TLS_KEY, k, klen) == klen;",
        )),
    (_NET, "tls_store_der_to_nvs"): (
        "persists the certificate and its key to NVS",
        (
            "const bool ok = nvs.putBytes(NVS_KEY_TLS_CERT, cert, cert_len) == cert_len && "
            "nvs.putBytes(NVS_KEY_TLS_KEY, key, key_len) == key_len;",
        )),
    (_MAIN, "setup"): (
        "derives the SoftAP password from the private key (F350: the whole statement, the halt "
        "included), starts the network with it, and prints it and the pin's first 16 digits on "
        "the serial banner the owner and the flashers read",
        (
            "if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
            'sizeof(g_ap_password))) { Serial.println("[!!] Setup network key derivation '
            'failed - HALTING"); while (true) { delay(1000); } }',
            _BEGIN_IN_SETUP,
            '[within #if FEATURE_WIFI_AP] Serial.printf("║ Password : %-45s ║\\n", '
            "g_ap_password);",
            "[within { > if (network.isTlsEnabled()) { > #if FEATURE_WIFI_AP] snprintf(fp_short, "
            'sizeof(fp_short), "%.16s...", network.getTlsCertFp());',
        )),
    (_NET, "apply_ap_security"): (
        "reads back and measures the passphrase the driver holds for the SoftAP (WPA2 or the "
        "WPA2/WPA3 transition, ap_security_policy.h); the read-back is zeroed after",
        (
            "if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {",
            "[within if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {] pw_len = "
            "strnlen((const char*)c.ap.password, sizeof(c.ap.password));",
        )),
    (_AUTH, "auth_load_or_derive"): (
        "loads the bearer token from NVS or derives it from the private key (the whole mint "
        "statement, its refusal included, inside `if (!loaded)`) into the auth lib's own "
        "buffer, and persists it",
        (
            "[within if (nvs.beginReadOnly()) {] size_t tlen = "
            "nvs.getBytesLength(NVS_KEY_TOKEN);",
            "[within if (nvs.beginReadOnly()) {] if (tlen > 0 && tlen < sizeof(s_bearer)) {",
            "[within if (nvs.beginReadOnly()) { > if (tlen > 0 && tlen < sizeof(s_bearer)) {] "
            "size_t got = nvs.getBytes(NVS_KEY_TOKEN, s_bearer, tlen);",
            "[within if (nvs.beginReadOnly()) { > if (tlen > 0 && tlen < sizeof(s_bearer)) { > "
            "if (got == tlen) {] s_bearer[tlen] = '\\0';",
            "[within if (nvs.beginReadOnly()) { > if (tlen > 0 && tlen < sizeof(s_bearer)) { > "
            'if (got == tlen) {] loaded = (strncmp(s_bearer, "cv_", 3) == 0 && strlen(s_bearer) '
            ">= 35);",
            "[within if (nvs.beginReadOnly()) { > if (tlen > 0 && tlen < sizeof(s_bearer)) { > "
            "else {] s_bearer[0] = '\\0';",
            "[within if (!loaded) {] if (!derive_api_token(privkey, s_bearer, "
            "sizeof(s_bearer))) { s_bearer[0] = '\\0'; return false; }",
            "[within if (!loaded) { > if (nvs.beginReadWrite()) {] size_t wrote = "
            "nvs.putBytes(NVS_KEY_TOKEN, s_bearer, strlen(s_bearer));",
            "return s_bearer[0] != '\\0';",
        )),
}

# The emitters' reading statements (TOKEN_EMITTERS), pinned the way a
# device-side use's are: keyed by the emitter's home file, each emitter
# defined exactly once under ROOTS, there. The carry still runs through an
# emitter (its callers answer what it carries); this holds what it does with
# a credential besides putting it in the answer, so a serial or MQTT line
# added inside the receipt's builder fails, as it would in raiseAp().
EMITTER_STATEMENTS = {
    (_NET, "send_provisioning_receipt"): (
        'doc["token"] = auth_get_token();',
        'doc["ap_password"] = net.getApPassword();',
        'doc["tls_cert_fp"] = net.isTlsEnabled() ? net.getTlsCertFp() : "";',
    ),
    (_NET, "send_html_with_token"): (
        'const char* token = (verdict == PageToken::INJECT) ? auth_get_token() : "";',
    ),
}

# The route handlers whose answer carries a credential: exactly what it
# carries, and its grant.
#   bearer  — `if (!auth_gate(req)) return ESP_OK;` at the handler's own top
#             level, before every answer site (and the census holds that
#             refusal outside every block and conditional, with nothing but a
#             rate-limit refusal able to leave before it).
#   receipt — receipt_decide handed exactly RECEIPT_GRANTS; check_host_first's
#             check_receipt_dispatch holds the dispatch (a send only under a
#             SERVE_* test, then the Host refusal), and every answer site
#             sits under a SERVE_* test.
#   page    — every answer site is send_html_with_token(...) (whose verdict
#             check_page_token_wiring holds to page_token_inject(req)) or a
#             call of another page-grant handler. A PUBLIC_ALLOWLIST route may
#             carry only with this grant.
SECRET_ANSWERS = {
    "handle_provisioning_receipt": ("token ap pin", "receipt"),
    "handle_status": ("pin", "bearer"),
    "handle_ui": ("token", "page"),
    "handle_setup_page": ("token", "page"),
    "handle_captive_probe": ("token", "page"),
    # Port 80 beside HTTPS: a plain_http_exempt path goes to the probe.
    "handle_https_redirect": ("token", "page"),
}
# The refusal a 'bearer' grant stands after, folded (the census holds every
# auth_gate call to exactly this statement).
BEARER_REFUSAL = "if (!auth_gate(req)) return ESP_OK;"
# Each SECRET_ANSWERS handler's own statements that read a credential, pinned
# the way an emitter's are (keyed by its home file, in order, every body of
# it): its grant says where it may answer, and this says what else it does
# with a credential there. The receipt reads none itself (its builder,
# send_provisioning_receipt, does), so a serial or MQTT line added under its
# SERVE_TAP or SERVE_BEARER test, which the grant alone would pass, fails.
ANSWER_STATEMENTS = {
    (_NET, "handle_provisioning_receipt"): (),
    (_NET, "handle_status"): (
        '[within {] doc["tls_cert_fp"] = net.isTlsEnabled() ? net.getTlsCertFp() : "";',),
    (_NET, "handle_ui"): (),
    (_NET, "handle_setup_page"): (),
    (_NET, "handle_captive_probe"): (),
    (_NET, "handle_https_redirect"): (),
}

# ── The bearer-only answers: the grant glue, pinned whole ────────────────────

# Every piece of glue a grant runs through, and the credentials' accessors,
# the token's mint and its helpers, the SoftAP password's way to the radio and
# the first-boot unlock's source, keyed by (file suffix, qualified function):
# what its body guarantees, and the body itself from the name to the closing
# brace, comment-stripped and whitespace folded (a preprocessor line inside it
# included). The pin holds the text; firmware/tests_host holds some of the
# behavior too: test_securacv_auth.cpp compiles securacv_auth.cpp whole (the
# AuthManager and the three wrappers) and drives page_token_inject,
# bearer_present_and_valid, handle_provisioning_receipt and send_host_refusal
# cut verbatim out of securacv_network.cpp, so an edit to one of those
# updates its row here and may change that suite's answers; the decisions the
# glue calls are host-tested, and the lockout's arithmetic is auth_backoff.h's
# (test_auth_backoff.cpp). The rest is held by its pin alone. An edit to a
# pinned body is an edit to its row in the same review, and the failure
# prints the body as it now is. Each row's function is defined exactly once
# under ROOTS, in its file: a second definition of the name anywhere (an
# `#else` body, or a shadow in another lib that its callers there would
# reach) fails. host_is_foreign was the first row (HOST_IS_FOREIGN_DEF).
_PAGE_GRANTS = (
    "SECURITY_MODEL.md's four unlocks, and only them: first-boot setup, a real bearer "
    "(auth_bearer_grant through bearer_present_and_valid), a SoftAP peer (from_ap_subnet) "
    "and one BOOT tap taken, never peeked, with auth_handed_token only on the tap that "
    "served (F341). A home-LAN reload without a tap is CONSOLIDATION.md Phase 6's "
    "cv_session port, which must bring canary-wap's any-session class (ANY_SESSION_GATES, "
    "F328) with it: never widen an input here instead")
GLUE_DEFS = {
    (_NET, "host_is_foreign"): (
        "every token path trusts it: the SoftAP exemption (the interface, never the name), a "
        "missing or oversize Host is foreign, then host_guard.h's host_names_this_device",
        "host_is_foreign(httpd_req_t* req) { if (from_ap_subnet(req)) return false; char "
        'host[96]; if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) '
        "return true; return !canary::net::host_names_this_device(host); }"),
    (_NET, "auth_gate"): (
        "the gate of every auth_gate route: the Host first, fail closed on an empty token, "
        "then auth_check against request_peer_ipv4(req), the socket peer",
        "auth_gate(httpd_req_t* req) { if (host_is_foreign(req)) { send_host_refusal(req); "
        "return false; } const char* token = auth_get_token(); if (!token || token[0] == "
        '\'\\0\') { httpd_resp_set_status(req, "503 Service Unavailable"); '
        'httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, '
        '"{\\"error\\":\\"not_provisioned\\"}"); witness_get_health().http_errors++; return '
        "false; } if (!auth_check(req, token, request_peer_ipv4(req))) { "
        "witness_get_health().http_errors++; return false; } return true; }"),
    (_NET, "bearer_present_and_valid"): (
        "the receipt's and the pages' bearer grant: the empty-token guard, then "
        "auth_bearer_grant(...) == BearerResult::OK over the Authorization header and nothing "
        "else",
        "bearer_present_and_valid(httpd_req_t* req) { const char* token = auth_get_token(); if "
        "(!token || token[0] == '\\0') return false; return auth_bearer_grant(req, token, "
        "request_peer_ipv4(req)) == AuthManager::BearerResult::OK; }"),
    (_NET, "from_ap_subnet"): (
        "the SoftAP grant: provisioning_gate::request_on_softap over the socket's peer and "
        "local addresses, never a header or a name",
        "from_ap_subnet(httpd_req_t* req) { if (!(WiFi.getMode() & WIFI_AP)) return false; "
        "const int fd = httpd_req_to_sockfd(req); if (fd < 0) return false; struct "
        "sockaddr_storage peer_ss; struct sockaddr_storage local_ss; socklen_t plen = "
        "sizeof(peer_ss); socklen_t llen = sizeof(local_ss); memset(&peer_ss, 0, "
        "sizeof(peer_ss)); memset(&local_ss, 0, sizeof(local_ss)); if (getpeername(fd, (struct "
        "sockaddr*)&peer_ss, &plen) != 0) return false; if (getsockname(fd, (struct "
        "sockaddr*)&local_ss, &llen) != 0) return false; uint32_t ap_ip, ap_mask, sta_ip, "
        'sta_mask; netif_ip4("WIFI_AP_DEF", &ap_ip, &ap_mask); netif_ip4("WIFI_STA_DEF", '
        "&sta_ip, &sta_mask); return canary::net::provisioning_gate::request_on_softap( "
        "sockaddr_ip4_host_order(peer_ss), sockaddr_ip4_host_order(local_ss), ap_ip, ap_mask, "
        "sta_ip, sta_mask); }"),
    (_NET, "network_set_provisioning_gate_hooks"): (
        "the BOOT-tap hooks, stored as main.cpp hands them: the take hook, and the is_open "
        "hook /api/status reads as a wiring check",
        "network_set_provisioning_gate_hooks(network_gate_fn_t take, network_gate_fn_t "
        "is_open) { s_gate_take = take; s_gate_is_open = is_open; }"),
    (_NET, "provisioning_gate_take"): (
        "the tap: take, never peek; unregistered means closed",
        "provisioning_gate_take() { return s_gate_take ? s_gate_take() : false; }"),
    (_NET, "page_token_inject"): (
        _PAGE_GRANTS,
        "page_token_inject(httpd_req_t* req) { using "
        "canary::net::provisioning_gate::PageToken; using "
        "canary::net::provisioning_gate::page_token_decide; const bool foreign = "
        "host_is_foreign(req); const bool setup_active = setup_is_active() || "
        "setup_is_first_boot(); const bool bearer_ok = !foreign && "
        "bearer_present_and_valid(req); const bool on_ap = from_ap_subnet(req); bool tap_spent "
        "= false; const char* why = nullptr; const PageToken verdict = page_token_decide( "
        "foreign, setup_active, bearer_ok, on_ap, [&tap_spent]() { tap_spent = "
        "provisioning_gate_take(); return tap_spent; }, &why); if (verdict != "
        'PageToken::INJECT) { Serial.printf("[AUTH] page token withheld (%s)\\n", why ? why : '
        '""); } else if (tap_spent) { auth_handed_token(request_peer_ipv4(req)); '
        'Serial.println("[AUTH] page token handed to one page load on a BOOT tap. Gate '
        'closed."); log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Page token unlocked", "BOOT '
        'gate"); } return verdict; }'),
    # page_token_inject's setup input at its source: the setup lib's state,
    # set at boot from NVS, cleared by the wizard's completion or its timeout
    # (the census holds every other writer out of the file).
    (_SETUP, "setup_init"): (
        "the first-boot unlock's source: setup is active only when NVS has no setup_ok, from "
        "boot until the wizard completes or SETUP_TIMEOUT_MS passes",
        "setup_init(void) { using namespace setup_wiz; if (s_initialized) return true; "
        'Preferences prefs; if (prefs.begin("securacv", true)) { s_first_boot = '
        "!prefs.getBool(NVS_KEY_SETUP_OK, false); prefs.getString(NVS_KEY_DEV_NAME, "
        "s_device_name, sizeof(s_device_name)); prefs.end(); } else { s_first_boot = true; } "
        "if (s_first_boot) { s_active = true; s_started_ms = millis(); "
        'log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, "First boot detected", "setup mode '
        'active"); } s_initialized = true; return true; }'),
    (_SETUP, "setup_is_first_boot"): (
        "the first-boot unlock's source: its state returned, nothing else",
        "setup_is_first_boot(void) { return setup_wiz::s_first_boot; }"),
    (_SETUP, "setup_is_active"): (
        "the first-boot unlock's source: its state returned, nothing else",
        "setup_is_active(void) { return setup_wiz::s_active; }"),
    (_SETUP, "setup_mark_complete"): (
        "the first-boot unlock's end: setup_ok persisted, both flags cleared",
        "setup_mark_complete(void) { using namespace setup_wiz; Preferences prefs; if "
        '(prefs.begin("securacv", false)) { prefs.putBool(NVS_KEY_SETUP_OK, true); '
        "prefs.end(); } s_first_boot = false; s_active = false; log_health(LOG_LEVEL_INFO, "
        'LOG_CAT_SYSTEM, "Setup complete", "normal operation"); }'),
    (_SETUP, "setup_check_timeout"): (
        "the first-boot unlock's end by time: past SETUP_TIMEOUT_MS from boot, setup_ok "
        "persisted, both flags cleared and a restart",
        "setup_check_timeout(void) { using namespace setup_wiz; if (!s_active) return; if "
        "((millis() - s_started_ms) >= SETUP_TIMEOUT_MS) { Preferences prefs; if "
        '(prefs.begin("securacv", false)) { prefs.putBool(NVS_KEY_SETUP_OK, true); '
        "prefs.end(); } s_active = false; s_first_boot = false; setup_stop_captive_portal(); "
        'log_health(LOG_LEVEL_INFO, LOG_CAT_SYSTEM, "Setup timeout", "rebooting to normal '
        'operation"); delay(500); ESP.restart(); } }'),
    (_MAIN, "prov_gate_take_hook"): (
        "the take hook: provisioning_gate::take, never is_open",
        "prov_gate_take_hook() { return canary::net::provisioning_gate::take(g_prov_gate, "
        "millis(), PROVISIONING_GATE_TTL_MS); }"),
    (_AUTH, "AuthManager::check"): (
        "the bearer check of every auth_gate route: the lockout read before any compare, no "
        "Authorization header a 401 with no strike, every wrong header one strike, the right "
        "one clears",
        "AuthManager::check(httpd_req_t* req, const char* expected_token, uint32_t source) { "
        "if (!req || !expected_token) return false; const uint32_t now = millis(); if "
        "(canary::net::auth_backoff::locked(m_table, source, now)) { sendLockout(req, source); "
        "return false; } Presented why = Presented::WRONG; const bool presented = "
        'httpd_req_get_hdr_value_len(req, "Authorization") != 0; const BearerResult result = '
        "canary::net::auth_backoff::grant( m_table, source, now, presented, [&]() { why = "
        "compare(req, expected_token); return why == Presented::RIGHT; }); switch (result) { "
        "case BearerResult::OK: return true; case BearerResult::HELD: sendLockout(req, "
        'source); return false; case BearerResult::NONE: httpd_resp_set_status(req, "401 '
        'Unauthorized"); httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, '
        '"WWW-Authenticate", "Bearer realm=\\"securacv\\""); httpd_resp_sendstr(req, '
        '"{\\"error\\":\\"unauthorized\\"," "\\"hint\\":\\"Include \'Authorization: Bearer '
        'cv_...\' header\\"}"); return false; case BearerResult::WRONG: break; } switch (why) '
        '{ case Presented::TOO_LONG: send_json_error(req, "401 Unauthorized", '
        '"{\\"error\\":\\"unauthorized\\"," "\\"hint\\":\\"Authorization header too '
        'long\\"}"); break; case Presented::UNREADABLE: send_json_error(req, "401 '
        'Unauthorized", "{\\"error\\":\\"unauthorized\\"}"); break; case '
        'Presented::NOT_BEARER: send_json_error(req, "401 Unauthorized", '
        '"{\\"error\\":\\"unauthorized\\"," "\\"hint\\":\\"Use \'Bearer <token>\' '
        'format\\"}"); break; case Presented::RIGHT: case Presented::WRONG: '
        'send_json_error(req, "403 Forbidden", "{\\"error\\":\\"forbidden\\"}"); break; } '
        "return false; }"),
    (_AUTH, "AuthManager::grant"): (
        "the accounted grant: auth_backoff::grant with presented = a non-empty Authorization "
        "header and a lambda over compare, so a held source is never compared",
        "AuthManager::grant(httpd_req_t* req, const char* expected_token, uint32_t source) { "
        "if (!req || !expected_token) return BearerResult::NONE; const bool presented = "
        'httpd_req_get_hdr_value_len(req, "Authorization") != 0; return '
        "canary::net::auth_backoff::grant( m_table, source, millis(), presented, [&]() { "
        "return compare(req, expected_token) == Presented::RIGHT; }); }"),
    (_AUTH, "AuthManager::compare"): (
        "the one compare: the Authorization header only, the Bearer prefix, the trim, the "
        "length, then constantTimeCompare",
        "AuthManager::compare(httpd_req_t* req, const char* expected_token) { size_t auth_len "
        '= httpd_req_get_hdr_value_len(req, "Authorization"); if (auth_len >= '
        "SECURACV_AUTH_HEADER_BUF) return Presented::TOO_LONG; char "
        "auth_buf[SECURACV_AUTH_HEADER_BUF]; if (httpd_req_get_hdr_value_str(req, "
        '"Authorization", auth_buf, sizeof(auth_buf)) != ESP_OK) { return '
        'Presented::UNREADABLE; } if (strncmp(auth_buf, "Bearer ", 7) != 0) return '
        "Presented::NOT_BEARER; const char* token = auth_buf + 7; while (*token == ' ') "
        "token++; size_t token_len = strlen(token); while (token_len > 0 && token[token_len - "
        "1] == ' ') token_len--; size_t expected_len = strlen(expected_token); if "
        "(expected_len == 0) return Presented::WRONG; if (token_len != expected_len) return "
        "Presented::WRONG; return constantTimeCompare(token, expected_token, expected_len) ? "
        "Presented::RIGHT : Presented::WRONG; }"),
    (_AUTH, "AuthManager::handedToken"): (
        "the tap's clear: record_success for that source only",
        "AuthManager::handedToken(uint32_t source) { "
        "canary::net::auth_backoff::record_success(m_table, source); }"),
    (_AUTH, "auth_get_instance"): (
        "the AuthManager's one instance, returned and nothing else (the census holds s_auth to "
        "this body and this function to the three wrappers)",
        "auth_get_instance() { return s_auth; }"),
    (_AUTH, "auth_check"): (
        "a one-line delegation to the instance's check",
        "auth_check(httpd_req_t* req, const char* expected_token, uint32_t source) { return "
        "auth_get_instance().check(req, expected_token, source); }"),
    (_AUTH, "auth_bearer_grant"): (
        "a one-line delegation to the instance's grant",
        "auth_bearer_grant(httpd_req_t* req, const char* expected_token, uint32_t source) { "
        "return auth_get_instance().grant(req, expected_token, source); }"),
    (_AUTH, "auth_handed_token"): (
        "a one-line delegation to the instance's handedToken",
        "auth_handed_token(uint32_t source) { auth_get_instance().handedToken(source); }"),
    (_AUTH, "AuthManager::constantTimeCompare"): (
        "the compare every bearer goes through: every byte visited through volatile, no early "
        "exit (a memcmp answers the same and leaks the matching prefix's length in time)",
        "AuthManager::constantTimeCompare(const void* a, const void* b, size_t len) { const "
        "volatile uint8_t* pa = static_cast<const volatile uint8_t*>(a); const volatile "
        "uint8_t* pb = static_cast<const volatile uint8_t*>(b); volatile uint8_t diff = 0; for "
        "(size_t i = 0; i < len; i++) { diff |= pa[i] ^ pb[i]; } return diff == 0; }"),
    # The accessors (SECRET_ACCESSORS): each returns its store and does
    # nothing else, so a sink added inside one fails (the reader rule names
    # the accessor, not its statements).
    (_AUTH, "auth_get_token"): (
        "the token's accessor: its store returned, nothing else",
        "auth_get_token() { return s_bearer; }"),
    (_NET_H, "getApPassword"): (
        "the SoftAP password's accessor: its store returned, nothing else",
        "getApPassword() const { return m_ap_password; }"),
    (_NET_H, "getTlsCertFp"): (
        "the TLS pin's accessor: its store returned, nothing else",
        "getTlsCertFp() const { return m_tls_cert_fp_hex; }"),
    # The token's mint: keyed by the private key it is handed (its caller,
    # witness_provision_device, is held to g_device.privkey by the census).
    # The flagship's copy has no host test (tests_host stubs it), so the body
    # is pinned.
    (_CRYPTO, "derive_api_token"): (
        "the token's mint: an HMAC keyed by the private key it is handed (never the public "
        "key, the fingerprint or anything the device publishes), then the STA MAC; a failed "
        "HMAC refuses",
        "derive_api_token(const uint8_t privkey[32], char* token_str, size_t token_str_len) { "
        "if (!token_str || token_str_len < 36) return false; uint8_t token_key[32]; if "
        '(!hmac_sha256( privkey, 32, (const uint8_t*)"securacv:token-key-derive:v1", 28, '
        "token_key)) { return false; } uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA); "
        'uint8_t token_input[27]; memcpy(token_input, "securacv:api-token:v1", 21); '
        "memcpy(token_input + 21, mac, 6); uint8_t token_hash[32]; if (!hmac_sha256(token_key, "
        "32, token_input, 27, token_hash)) { secure_zero(token_key, sizeof(token_key)); return "
        "false; } format_api_token_string(token_hash, sizeof(token_hash), token_str, "
        "token_str_len); secure_zero(token_key, sizeof(token_key)); secure_zero(token_hash, "
        "sizeof(token_hash)); return strlen(token_str) >= 35; }"),
    # Its two helpers (the census holds the alphabet's declaration): the HMAC
    # keyed by what it is handed, and the rendering of every hash byte.
    (_CRYPTO, "hmac_sha256"): (
        "the token's HMAC (and the backup's): mbedtls HMAC-SHA256 keyed by the key it is "
        "handed, over the data it is handed, a failure refused",
        "hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t "
        "data_len, uint8_t out[32]) { const mbedtls_md_info_t* info = "
        "mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); if (!info) return false; "
        "mbedtls_md_context_t ctx; mbedtls_md_init(&ctx); int ret = mbedtls_md_setup(&ctx, "
        "info, 1); if (ret == 0) ret = mbedtls_md_hmac_starts(&ctx, key, key_len); if (ret == "
        "0) ret = mbedtls_md_hmac_update(&ctx, data, data_len); if (ret == 0) ret = "
        "mbedtls_md_hmac_finish(&ctx, out); mbedtls_md_free(&ctx); return ret == 0; }"),
    (_CRYPTO, "format_api_token_string"): (
        "the token's rendering: cv_ and 32 characters of the unambiguous alphabet, each from "
        "a byte of the hash it is handed (rejection-sampled)",
        "format_api_token_string(const uint8_t* input, size_t in_len, char* output, size_t "
        "out_len) { if (!output || out_len == 0) return; size_t out_idx = 0; if (out_len > 3) "
        "{ output[out_idx++] = 'c'; output[out_idx++] = 'v'; output[out_idx++] = '_'; } const "
        "size_t target_chars = 32; size_t chars_produced = 0; size_t i = 0; while "
        "(chars_produced < target_chars && out_idx + 1 < out_len) { uint8_t b; if (i < "
        "in_len) { b = input[i++]; } else { b = (uint8_t)(i ^ 0xA5); i++; } if (b < 216) { "
        "output[out_idx++] = UNAMBIGUOUS_ALPHABET[b % UNAMBIGUOUS_LEN]; chars_produced++; } } "
        "output[out_idx] = '\\0'; }"),
    # The SoftAP password on its way to the radio, after the mint and the
    # two raises (the census holds both WiFi.softAP statements): the line it
    # is printed on, and the driver config it is measured from.
    (_NET, "announce_setup_network"): (
        "the setup-network line both flashers parse and its QR, on the serial line only (the "
        "SoftAP password reaches it as a parameter, which the reader rule does not follow)",
        "announce_setup_network(const char* ssid, const char* pass) { namespace ob = "
        "canary::net::onboard; char line[256]; if (ob::announce_line(line, sizeof(line), ssid, "
        "pass, AP_CHANNEL) > 0) Serial.println(line); if (ob::qr_line(line, sizeof(line), "
        "ssid, pass) > 0) Serial.println(line); }"),
    (_NET, "apply_ap_security"): (
        "the SoftAP's security request: the read-back passphrase only measured, the WPA2/WPA3 "
        "transition and PMF exactly as ap_security_policy.h decides (never an open or "
        "weaker authmode), and the read-back zeroed",
        "apply_ap_security(WiFiStatus& st) { namespace aps = canary::net::ap_security; "
        "wifi_config_t c; memset(&c, 0, sizeof(c)); size_t pw_len = 0; if "
        "(esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) { pw_len = strnlen((const "
        "char*)c.ap.password, sizeof(c.ap.password)); } aps::Decision d = "
        "aps::decide(CANARY_AP_WPA3_TRANSITION != 0, CANARY_SOFTAP_SAE_IN_BUILD != 0, pw_len); "
        "#if CANARY_SOFTAP_SAE_IN_BUILD if (d.mode == aps::AuthMode::WPA2_WPA3_TRANSITION) { "
        "c.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK; c.ap.pairwise_cipher = "
        "WIFI_CIPHER_TYPE_CCMP; c.ap.pmf_cfg.capable = d.pmf_capable; c.ap.pmf_cfg.required = "
        "d.pmf_required; const bool accepted = (esp_wifi_set_config(WIFI_IF_AP, &c) == "
        "ESP_OK); d = aps::after_driver(d, accepted); if (!accepted) { "
        'log_health(LOG_LEVEL_WARNING, LOG_CAT_NETWORK, "WPA3 SoftAP refused by driver", '
        '"WPA2-PSK kept"); } } #endif secure_zero(&c, sizeof(c)); strncpy(st.ap_auth, d.label, '
        "sizeof(st.ap_auth) - 1); st.ap_auth[sizeof(st.ap_auth) - 1] = '\\0'; "
        'st.ap_auth_reason = d.reason; Serial.printf("[WIFI] SoftAP security: %s (%s)\\n", '
        'd.label, d.reason); log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "SoftAP security", '
        "d.label); }"),
    # from_ap_subnet's helpers and the lockout's source key: page_token_inject's
    # on_ap input and host_is_foreign's SoftAP exemption read the interfaces
    # through the first three, so a helper that read the other interface's
    # address would make every home-LAN peer a SoftAP one (the token on every
    # page load, the Host check skipped) with from_ap_subnet's text intact; and
    # a source key that changes per request would leave the lockout counting
    # nothing.
    (_NET, "netif_ip4"): (
        "the SoftAP grant's interface addresses: the netif its key names (WIFI_AP_DEF the AP's, "
        "never the STA's), both 0 when it is absent or has no address",
        "netif_ip4(const char* ifkey, uint32_t* ip, uint32_t* mask) { *ip = 0; *mask = 0; "
        "esp_netif_t* nif = esp_netif_get_handle_from_ifkey(ifkey); if (!nif) return; "
        "esp_netif_ip_info_t info; memset(&info, 0, sizeof(info)); if "
        "(esp_netif_get_ip_info(nif, &info) != ESP_OK) return; *ip = "
        "be_word_host_order(&info.ip.addr); *mask = be_word_host_order(&info.netmask.addr); }"),
    (_NET, "be_word_host_order"): (
        "an lwIP address word's four bytes, first octet first, and nothing else",
        "be_word_host_order(const void* word) { const uint8_t* b = (const uint8_t*)word; "
        "return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | "
        "(uint32_t)b[3]; }"),
    (_NET, "sockaddr_ip4_host_order"): (
        "a socket address's IPv4, unwrapped by the host-tested ipv4_host_order_from_addr, 0 "
        "for anything else",
        "sockaddr_ip4_host_order(const struct sockaddr_storage& ss) { using "
        "canary::net::provisioning_gate::AddrFamily; using "
        "canary::net::provisioning_gate::ipv4_host_order_from_addr; if (ss.ss_family == "
        "AF_INET) { const struct sockaddr_in* sin = (const struct sockaddr_in*)&ss; return "
        "ipv4_host_order_from_addr(AddrFamily::IPV4, (const uint8_t*)&sin->sin_addr.s_addr); } "
        "#if defined(LWIP_IPV6) && LWIP_IPV6 if (ss.ss_family == AF_INET6) { const struct "
        "sockaddr_in6* sin6 = (const struct sockaddr_in6*)&ss; return "
        "ipv4_host_order_from_addr(AddrFamily::IPV6, (const uint8_t*)&sin6->sin6_addr); } "
        "#endif return 0; }"),
    (_NET, "request_peer_ipv4"): (
        "the lockout's source key: the socket's peer (getpeername), never a header and never "
        "a value that changes per request",
        "request_peer_ipv4(httpd_req_t* req) { const int fd = httpd_req_to_sockfd(req); if (fd "
        "< 0) return 0; struct sockaddr_storage peer_ss; socklen_t plen = sizeof(peer_ss); "
        "memset(&peer_ss, 0, sizeof(peer_ss)); if (getpeername(fd, (struct sockaddr*)&peer_ss, "
        "&plen) != 0) return 0; return sockaddr_ip4_host_order(peer_ss); }"),
    # The BOOT tap at its source: the peek hook /api/status reads, and the
    # handler whose short press opens the gate, whole (the press is the tap: a
    # press faked by the clock or by a SoftAP peer would reopen the tap with
    # its open statement intact).
    (_MAIN, "prov_gate_is_open_hook"): (
        "the peek hook /api/status reads: provisioning_gate::is_open, which writes nothing",
        "prov_gate_is_open_hook() { return "
        "canary::net::provisioning_gate::is_open(g_prov_gate, millis(), "
        "PROVISIONING_GATE_TTL_MS); }"),
    (_MAIN, "handle_boot_button"): (
        "the BOOT tap's source: the stamp swept every pass, the press read from the button's "
        "GPIO and nothing else, the long hold's grammar first, a medium hold printing the "
        "status, an armed USB confirmation leaving the gate closed, and a short press "
        "opening it once",
        "handle_boot_button() { static uint32_t boot_btn_start = 0; static bool "
        "boot_btn_was_pressed = false; canary::net::provisioning_gate::expire(g_prov_gate, "
        "millis(), PROVISIONING_GATE_TTL_MS); bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == "
        "LOW); reset_hold_dispatch(g_reset_hold.step(pressed, millis())); if (pressed && "
        "!boot_btn_was_pressed) { boot_btn_start = millis(); boot_btn_was_pressed = true; } "
        "else if (!pressed && boot_btn_was_pressed) { uint32_t duration = millis() - "
        "boot_btn_start; boot_btn_was_pressed = false; if (duration >= BOOT_MEDIUM_PRESS_MS) { "
        "print_status(); } else { #if FEATURE_USB_ONBOARD const bool tap_is_usb_confirm = "
        "usb_onboard::state() == usb_onboard::State::Armed; #else const bool "
        "tap_is_usb_confirm = false; #endif if (duration >= BOOT_SHORT_PRESS_MS && "
        'tap_is_usb_confirm) { Serial.println("[AUTH] BOOT tap confirmed USB onboarding; '
        'provisioning gate left closed"); } else if (duration >= BOOT_SHORT_PRESS_MS) { '
        'canary::net::provisioning_gate::open(g_prov_gate, millis()); Serial.printf("[AUTH] '
        'Provisioning gate OPENED (one receipt fetch or one LAN page load, %lu seconds)\\n", '
        "(unsigned long)(PROVISIONING_GATE_TTL_MS / 1000)); log_health(LOG_LEVEL_INFO, "
        'LOG_CAT_USER, "Provisioning gate opened", "BOOT button"); #ifdef LED_BUILTIN bool '
        "led_ok = true; #if FEATURE_SD_STORAGE led_ok = !storage_mount_in_flight(); #endif if "
        "(led_ok) { for (int i = 0; i < 3; i++) { digitalWrite(LED_BUILTIN, HIGH); delay(100); "
        "digitalWrite(LED_BUILTIN, LOW); delay(100); } } #endif } #if FEATURE_USB_ONBOARD "
        "usb_onboard::confirm(); #endif } } }"),
    # The port-80 path (PUBLIC_ALLOWLIST's /* reasons): the probe answers its
    # plain bodies, the setup page only off the port-80 server (the TLS hint
    # there), and the redirect hands a probe path to the probe and everything
    # else a 307: in this text, and in the two members below and their
    # stores' writers, the page token does not ride plain HTTP on an HTTPS
    # build.
    (_NET, "handle_captive_probe"): (
        "a probe's plain answer, the setup page through the page grant only while setup is "
        "active or the home link is down and never on the port-80 server beside HTTPS (the "
        "TLS hint, F15), else Apple's success body",
        "handle_captive_probe(httpd_req_t* req) { witness_get_health().http_requests++; const "
        'char* uri = req->uri; if (probe_path_is(uri, "/generate_204") || probe_path_is(uri, '
        '"/gen_204")) { httpd_resp_set_status(req, "204 No Content"); return '
        'httpd_resp_send(req, NULL, 0); } if (probe_path_is(uri, "/connecttest.txt") || '
        'probe_path_is(uri, "/ncsi.txt")) { httpd_resp_set_type(req, "text/plain"); return '
        'httpd_resp_sendstr( req, strstr(uri, "ncsi") ? "Microsoft NCSI" : "Microsoft Connect '
        'Test"); } if (setup_is_active() || !network_get_instance().getStatus().sta_connected) '
        "{ if (network_get_instance().isTlsEnabled() && req->handle != "
        "network_get_instance().getHttpsServer()) { return send_tls_captive_hint(req); } "
        "return send_html_with_token(req, CANARY_SETUP_HTML, page_token_inject(req)); } "
        'httpd_resp_set_type(req, "text/html"); return httpd_resp_sendstr(req, '
        "kAppleSuccessBody); }"),
    (_NET, "handle_https_redirect"): (
        "the port-80 server beside HTTPS: a plain_http_exempt path to the probe, everything "
        "else a 307 to https:// (F15), never a page of its own",
        "handle_https_redirect(httpd_req_t* req) { witness_get_health().http_requests++; if "
        "(canary::net::tls_policy::plain_http_exempt(req->uri, setup_is_active())) { return "
        "handle_captive_probe(req); } char host[64]; host[0] = '\\0'; const size_t host_len = "
        'httpd_req_get_hdr_value_len(req, "Host"); if (host_len == 0 || host_len >= '
        'sizeof(host) || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != '
        "ESP_OK) { host[0] = '\\0'; } char arrived_on[16]; local_addr_of(req, arrived_on, "
        "sizeof(arrived_on)); char location[256]; if "
        "(!canary::net::tls_policy::build_redirect_location( location, sizeof(location), host, "
        "arrived_on, req->uri)) { return httpd_resp_send_err(req, "
        'HTTPD_500_INTERNAL_SERVER_ERROR, "https redirect target too long"); } '
        'httpd_resp_set_status(req, "307 Temporary Redirect"); httpd_resp_set_hdr(req, '
        '"Location", location); httpd_resp_set_hdr(req, "Cache-Control", "no-store"); return '
        'httpd_resp_sendstr(req, "Redirecting to https"); }'),
    # ...and the two members the probe's hint branch decides on (`isTlsEnabled()
    # && req->handle != getHttpsServer()`): each returns its store and nothing
    # else, and GATE_CALL_SITES holds every write of the two stores, so "HTTPS is
    # up" and "this request came off the TLS server" cannot be answered wrong
    # with both handlers' text intact (the page, and its token, on port 80).
    (_NET_H, "isTlsEnabled"): (
        "the TLS flag the port-80 hint decides on: its store returned, nothing else",
        "isTlsEnabled() const { return m_tls_enabled; }"),
    (_NET_H, "getHttpsServer"): (
        "the TLS server's handle the port-80 hint compares the request's with: its store "
        "returned, nothing else",
        "getHttpsServer() const { return m_https_server; }"),
}

# ── The bearer-only answers: the gate primitives' call sites ─────────────────

# The census: where each gate primitive may be mentioned, under ROOTS, outside
# its own definition and its declarations (a call, a function pointer or an
# address taken alike). Per name:
#   callers — the (file suffix, qualified function) set that may mention it,
#             every one of which does (None: any function);
#   home    — where it is defined, (file suffix, qualified function), or None
#             when it is defined outside ROOTS (the shared headers, the
#             Arduino core): every definition of the name under ROOTS (in
#             "file", when the row has one) must be exactly that, so a second
#             definition, or a shadow in another lib, which its callers there
#             would reach, fails (an ident row has none: its "decl" holds it);
#   file    — only mentions in this file, or these files (a tuple), count (a
#             common name: `compare`, `this`);
#   pattern — the spelling, when every whole-word mention of the name
#             (`\bname\b`, its qualifier's `::` spaced freely) is not it;
#   bare    — (class prefix, header suffix): a bare `begin(` the pattern's
#             `bare` group matched counts only inside that class's members or
#             a function defined in that header (another class's begin() is
#             not the row's);
#   whole   — every call is exactly this folded statement, where it sits
#             (_placed), or, as a dict, each listed caller's every call is
#             its entry;
#   whole_if — the statement held is the whole `if` through its braced branch
#             (a mint's caller: the call, its refusal and what the refusal does);
#   stmts   — {caller: (statement, ...)}: that caller's statements that
#             mention it are exactly these, folded where they sit, in order;
#   top     — every call sits at its function's own top level, outside every
#             block and every preprocessor conditional opened in the function,
#             in every body that holds it, and nothing before it in the
#             function can leave (a `return`, `goto`, `throw`, `co_return`, or
#             an ESP_RETURN_* / ESP_GOTO_* macro, _LEAVE) but a
#             RATE_LIMIT_REFUSAL;
#   lead    — nothing before a call in its function can leave (_LEAVE, outside
#             a lambda): the BOOT sweep and the setup timeout in loop(), with
#             `whole` holding where each call sits;
#   keyed   — {caller: (raise statement, keying row)}: in that caller, the
#             raise statement (folded where it sits) is followed by the keying
#             row's `whole` statement for it, with nothing able to leave
#             between them (_LEAVE, outside a lambda): a mode raise and the
#             softAP() that keys it;
#   once    — exactly one call (per caller, when it has callers);
#   served  — callers whose every call sits under a SERVE_* test of the
#             receipt_decide verdict (check_receipt_dispatch's _under_serve_test);
#   ident   — a variable: every mention counts, and outside a function only
#             the declarations in "decl" may name it (none, without "decl");
#   none    — the name must have no mention at all (callers is empty).
# A row whose name has no mention fails (but a `none` row), and so does a
# listed caller that no longer mentions it. Every AUTH_MARKERS entry names a
# row, and every PRE_HOST_STEPS entry a row, a TOKEN_EMITTERS key or
# auth_get_token.
# The census rows' statements, folded where they sit (_placed).
_AP_SET_CONFIG = ("[within if (d.mode == aps::AuthMode::WPA2_WPA3_TRANSITION) { > "
                  "#if CANARY_SOFTAP_SAE_IN_BUILD] const bool accepted = "
                  "(esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK);")
_STA_SET_CONFIG = ("[within if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) { > if "
                   "(!capable) { > #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) ... #else] "
                   "capable = (esp_wifi_set_config(WIFI_IF_STA, &c) == ESP_OK);")
_BOOT_GATE_STMTS = (
    "canary::net::provisioning_gate::expire(g_prov_gate, millis(), PROVISIONING_GATE_TTL_MS);",
    "[within else if (!pressed && boot_btn_was_pressed) { > else { > else if (duration >= "
    "BOOT_SHORT_PRESS_MS) {] canary::net::provisioning_gate::open(g_prov_gate, millis());")
_SETUP_TIMEOUT = ("[within if (setup_is_active()) { > #if FEATURE_SETUP_WIZARD] "
                  "setup_check_timeout();")
_SETUP_WRITERS = {(_SETUP, "setup_init"), (_SETUP, "setup_mark_complete"),
                  (_SETUP, "setup_check_timeout")}
_BOOT_HOLD_AT_BOOT = ("[within if (digitalRead(BOOT_BUTTON_GPIO) == LOW) { > for (;;) {] "
                      "reset_hold_dispatch(g_reset_hold.step(pressed, millis()));")
_WIFI_MODE = (r"\bWiFi\w*\s*(?:\.|::)\s*(?:mode|enableAP|softAPConfig|softAPdisconnect"
              r"|softAPbandwidth)\b|\besp_wifi_set_mode\b")
_SETUP_STATE = {"file": _SETUP, "ident": True}
# The TLS state's writers, and the two blocks startHttpServer() writes it in.
_TLS_WRITERS = {(_NET, "ScvNetworkManager::ScvNetworkManager"),
                (_NET, "ScvNetworkManager::startHttpServer")}
_TLS_FAILED = "[within if (mode == Mode::HTTPS_REDIRECT) { > #if SECURACV_HAS_HTTPS_SERVER] "
_TLS_UP = ("[within if (mode == Mode::HTTPS_REDIRECT) { > if (httpd_ssl_start(&m_https_server, "
           "&ssl) == ESP_OK) { > #if SECURACV_HAS_HTTPS_SERVER] ")
_ALPHABET = {"callers": {(_CRYPTO, "format_api_token_string"), (_CRYPTO, "unambiguous_suffix16")},
             "file": _CRYPTO, "ident": True}
GATE_CALL_SITES = {
    # The 62 gated routes' refusal: whole, at the handler's own top level,
    # never under a block or a conditional (`if (!from_ap_subnet(req)) {...}`,
    # `#if 0`), with nothing before it able to leave but the rate limit.
    "auth_gate": {"callers": None, "whole": BEARER_REFUSAL, "top": True,
                  "home": (_NET, "auth_gate")},
    "auth_check": {"callers": {(_NET, "auth_gate")}, "home": (_AUTH, "auth_check")},
    "auth_bearer_grant": {"callers": {(_NET, "bearer_present_and_valid")},
                          "home": (_AUTH, "auth_bearer_grant")},
    "auth_handed_token": {"callers": {(_NET, "page_token_inject"),
                                      (_NET, "handle_provisioning_receipt")},
                          "served": {(_NET, "handle_provisioning_receipt")},
                          "home": (_AUTH, "auth_handed_token")},
    # The AuthManager: its one instance, s_auth, is reached only through
    # auth_get_instance() (whose one-line body is GLUE_DEFS'), and that only
    # from the three wrappers, so no other function reaches s_auth. What the
    # AuthManager's own members do with it (sendLockout, reset) is
    # test_securacv_auth.cpp's, which compiles the auth lib whole.
    "auth_get_instance": {"callers": {(_AUTH, "auth_check"), (_AUTH, "auth_bearer_grant"),
                                      (_AUTH, "auth_handed_token")},
                          "home": (_AUTH, "auth_get_instance")},
    "s_auth": {"callers": {(_AUTH, "auth_get_instance")}, "ident": True,
               "decl": ("static AuthManager s_auth;",)},
    # ...and never by its own `this`: a constructor or member that saved it
    # (`s_self = this;`) would hand any function the instance.
    "this (the auth lib)": {"pattern": r"\bthis\b",
                            "file": ("securacv_auth.cpp", "securacv_auth.h"),
                            "callers": set(), "none": True, "home": None},
    "compare": {"callers": {(_AUTH, "AuthManager::check"), (_AUTH, "AuthManager::grant")},
                "file": _AUTH, "pattern": r"(?<![\w.])compare\b",
                "home": (_AUTH, "AuthManager::compare")},
    "constantTimeCompare": {"callers": {(_AUTH, "AuthManager::compare")},
                            "home": (_AUTH, "AuthManager::constantTimeCompare")},
    "bearer_present_and_valid": {"callers": {(_NET, "page_token_inject"),
                                             (_NET, "handle_provisioning_receipt")},
                                 "home": (_NET, "bearer_present_and_valid")},
    "provisioning_gate_take": {"callers": {(_NET, "page_token_inject"),
                                           (_NET, "handle_provisioning_receipt")},
                               "home": (_NET, "provisioning_gate_take")},
    "receipt_decide": {"callers": {(_NET, "handle_provisioning_receipt")}, "home": None},
    "page_token_decide": {"callers": {(_NET, "page_token_inject")}, "home": None},
    # The receipt's emitter: sent only from the receipt route, whose
    # dispatch check_receipt_dispatch holds (an `#else` body of another
    # handler calling it fails here, whatever that handler's grant).
    "send_provisioning_receipt": {"callers": {(_NET, "handle_provisioning_receipt")},
                                  "home": (_NET, "send_provisioning_receipt")},
    # The page's emitter: called only by the three page handlers (each held to
    # page_token_inject(req) as its verdict), and named nowhere else, so a
    # function pointer or a reference to it handed a literal INJECT fails.
    "send_html_with_token": {"callers": {(_NET, "handle_ui"), (_NET, "handle_setup_page"),
                                         (_NET, "handle_captive_probe")},
                             "home": (_NET, "send_html_with_token")},
    # A device-side use that hands a credential out through a parameter has
    # its callers here: the TLS key's loader fills the buffers it is handed,
    # and the carry stops at it (SECRET_LOCAL_USES), so its caller is held.
    "tls_load_der_from_nvs": {"callers": {(_NET, "ScvNetworkManager::initTls")}, "once": True,
                              "home": (_NET, "tls_load_der_from_nvs")},
    # ...and its writer, which takes a key in the same way: a route that
    # stored one of its own would have the device serve the asker's key.
    "tls_store_der_to_nvs": {"callers": {(_NET, "ScvNetworkManager::initTls")}, "once": True,
                             "home": (_NET, "tls_store_der_to_nvs")},
    # So does the private key's: nvs_load_key fills the buffer it is handed
    # (test_nvs_load_key.cpp drives it and establish_identity_key, cut
    # verbatim), and its NVS record is named only by the three functions
    # that size, load and store it.
    "nvs_load_key": {"callers": {(_WITNESS, "establish_identity_key")}, "once": True,
                     "home": (_CRYPTO, "nvs_load_key")},
    "NVS_KEY_PRIV": {"callers": {(_CRYPTO, "stored_key_length"), (_CRYPTO, "nvs_load_key"),
                                 (_CRYPTO, "nvs_store_key")}, "ident": True},
    "network_set_provisioning_gate_hooks": {
        "callers": {(_MAIN, "setup")}, "once": True,
        "whole": "[within { > #if FEATURE_WIFI_AP] network_set_provisioning_gate_hooks("
                 "prov_gate_take_hook, prov_gate_is_open_hook);",
        "home": (_NET, "network_set_provisioning_gate_hooks")},
    "s_gate_take": {"callers": {(_NET, "network_set_provisioning_gate_hooks"),
                                (_NET, "provisioning_gate_take"), (_NET, "handle_status")},
                    "ident": True, "decl": ("static network_gate_fn_t s_gate_take = nullptr;",)},
    # The peek hook's copy: set by the hook setter, read only by /api/status's
    # wiring check (its hook is pinned in main.cpp, GLUE_DEFS).
    "s_gate_is_open": {"callers": {(_NET, "network_set_provisioning_gate_hooks"),
                                   (_NET, "handle_status")},
                       "ident": True,
                       "decl": ("static network_gate_fn_t s_gate_is_open = nullptr;",)},
    # The token's mint input: the boot hands the auth lib the private key,
    # with its refusal (derive_api_token's body is GLUE_DEFS').
    "auth_load_or_derive": {
        "callers": {(_WITNESS, "witness_provision_device")}, "once": True, "whole_if": True,
        "whole": "if (!auth_load_or_derive(g_device.privkey)) { "
                 'Serial.println("[!!] Bearer credential provisioning failed"); return false; }',
        "home": (_AUTH, "auth_load_or_derive")},
    # Every SoftAP raise, with the key it raises: begin() at boot (its
    # ap_password parameter, which main.cpp hands g_ap_password) and raiseAp()
    # after a home-link drop. An empty or null key raises an open network.
    "softAP": {
        "callers": {(_NET, "ScvNetworkManager::begin"), (_NET, "ScvNetworkManager::raiseAp")},
        "whole": {
            (_NET, "ScvNetworkManager::begin"):
                "bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, "
                "AP_MAX_CONNECTIONS);",
            (_NET, "ScvNetworkManager::raiseAp"):
                "bool ap_ok = WiFi.softAP(m_ap_ssid, m_ap_password, AP_CHANNEL, false, "
                "AP_MAX_CONNECTIONS);"},
        "home": None},
    # The key begin() raises with is its parameter, so begin() is started
    # once, by setup() with g_ap_password (and by network_init, which nothing
    # calls): a second start handed "" or nullptr would raise the SoftAP open
    # and leave raiseAp() an empty key. Its spelling: the manager's names
    # (MANAGER_NAMES, which every reference to it must use), `this`, the
    # singleton, the member's own name, and a bare `begin(` inside one of its
    # members ("bare": a qualified member, or one defined in the header).
    "ScvNetworkManager::begin": {
        "pattern": r"(?:\b(?:net|network|this)\s*(?:\.|->)|\bnetwork_get_instance\s*\(\s*\)"
                   r"[\s)]*(?:\.|->)|\(\s*\*\s*this\s*\)\s*\.)\s*begin\b"
                   r"|\bScvNetworkManager\s*::\s*begin\b"
                   r"|(?P<bare>(?<![\w.>:~])begin(?=\s*\())",
        "bare": ("ScvNetworkManager::", _NET_H),
        "callers": {(_MAIN, "setup"), (_NET, "network_init")}, "once": True,
        "whole": {
            (_MAIN, "setup"): _BEGIN_IN_SETUP,
            (_NET, "network_init"):
                "return network_get_instance().begin(ap_ssid, ap_password, device_id);"},
        "home": (_NET, "ScvNetworkManager::begin")},
    "network_init": {"callers": set(), "none": True, "home": (_NET, "network_init")},
    # The driver's SoftAP config is written by apply_ap_security alone (the
    # STA's PMF request writes WIFI_IF_STA), each call its whole statement
    # where it sits: never an open or weaker authmode beside the raise.
    "esp_wifi_set_config": {
        "callers": {(_NET, "apply_ap_security"), (_NET, "ensure_sta_pmf_capable")},
        "whole": {
            (_NET, "apply_ap_security"): _AP_SET_CONFIG,
            (_NET, "ensure_sta_pmf_capable"): _STA_SET_CONFIG},
        "home": None},
    # Arduino-ESP32 3.x's AP class raises a SoftAP of its own: not used here,
    # not named (a reference to it, `auto& a = WiFi.AP;`, is a mention too).
    "WiFi.AP": {"pattern": r"\bWiFi\s*\.\s*AP\b|\bAPClass\b|\bAP\s*\.\s*(?:create|begin|enable)\b",
                "callers": set(), "none": True, "home": None},
    # A mode change raises the SoftAP on whatever config the driver holds:
    # dropAp()'s WiFi.softAPdisconnect(true) leaves it an OPEN, empty-SSID
    # one (Arduino-ESP32 2.0.17's wifi_softap_config), and WiFi.mode,
    # WiFi.enableAP, WiFi.softAPConfig (which enables the AP first) and
    # esp_wifi_set_mode set the mode without writing one (and mode() is a
    # static of WiFiGenericClass, so `WiFiGenericClass::mode(...)` is one
    # too, with no `WiFi` in it). softAPdisconnect() itself is one: its
    # default argument is `false`, so on 2.0.17 it writes that open config
    # and leaves a running SoftAP up on it, and on core 3.x (AP.cpp's
    # APClass::clear()) it raises the AP first, even from STA-only mode, and
    # leaves it up on a zeroed, open config; softAPbandwidth() raises it on
    # 3.x the same way (APClass::bandwidth()). So the mode is set only by
    # begin(), dropAp() and raiseAp(), each statement where it sits (dropAp's
    # disconnect with its `true`), each raise followed by the softAP() its
    # row holds with nothing able to leave between them ("keyed": a return
    # there leaves the SoftAP up, unkeyed, on whatever config the driver
    # loaded, and raiseAp() would never re-key it, since it returns while the
    # AP is up).
    "WiFi.mode": {
        "pattern": _WIFI_MODE,
        "callers": {(_NET, "ScvNetworkManager::begin"), (_NET, "ScvNetworkManager::dropAp"),
                    (_NET, "ScvNetworkManager::raiseAp")},
        "stmts": {(_NET, "ScvNetworkManager::begin"): ("WiFi.mode(WIFI_AP_STA);",),
                  (_NET, "ScvNetworkManager::dropAp"): ("WiFi.softAPdisconnect(true);",
                                                         "WiFi.mode(WIFI_STA);"),
                  (_NET, "ScvNetworkManager::raiseAp"): (
                      "WiFi.mode(WIFI_AP_STA);", "[within if (!ap_ok) {] WiFi.mode(WIFI_STA);")},
        "keyed": {(_NET, "ScvNetworkManager::begin"): ("WiFi.mode(WIFI_AP_STA);", "softAP"),
                  (_NET, "ScvNetworkManager::raiseAp"): ("WiFi.mode(WIFI_AP_STA);", "softAP")},
        "home": None},
    # The TLS state the port-80 hint reads (GLUE_DEFS' isTlsEnabled and
    # getHttpsServer): the flag and the TLS server's handle are written by the
    # constructor and by startHttpServer() alone, each statement where it sits
    # (the flag set only once httpd_ssl_start() has filled the handle, both
    # cleared when it fails), and read only by their getters and the TLS
    # registration. A write anywhere else (`m_tls_enabled = false;` once the
    # port-80 server is up, `m_https_server = m_http_server;`) would serve the
    # setup page, with its token, on plain HTTP beside HTTPS.
    "m_tls_enabled": {
        "callers": _TLS_WRITERS | {(_NET_H, "isTlsEnabled")}, "ident": True,
        "decl": ("bool m_tls_enabled;",),
        "stmts": {(_NET, "ScvNetworkManager::ScvNetworkManager"): ("m_tls_enabled = false;",),
                  (_NET, "ScvNetworkManager::startHttpServer"): (
                      _TLS_UP + "m_tls_enabled = true;", _TLS_FAILED + "m_tls_enabled = false;")}},
    "m_https_server": {
        "callers": _TLS_WRITERS | {(_NET_H, "getHttpsServer"), (_NET_H, "getHttpServer")},
        "ident": True, "decl": ("httpd_handle_t m_https_server;",),
        "stmts": {(_NET, "ScvNetworkManager::ScvNetworkManager"): ("m_https_server = nullptr;",),
                  (_NET, "ScvNetworkManager::startHttpServer"): (
                      _TLS_FAILED + "if (httpd_ssl_start(&m_https_server, &ssl) == ESP_OK) {",
                      _TLS_UP + "registerHttpHandlers(m_https_server);",
                      _TLS_FAILED + "m_https_server = nullptr;")}},
    # The network manager's one instance, reached only by its accessor (whose
    # every use manager_alias_problems holds to a member access or a binding
    # named net or network).
    "s_network": {"callers": {(_NET, "network_get_instance")}, "ident": True,
                  "decl": ("static ScvNetworkManager s_network;",)},
    # The BOOT tap's open side: a short press in handle_boot_button, once,
    # where it sits (g_prov_gate's row below holds the statement in place).
    "provisioning_gate::open": {
        "pattern": r"\bprovisioning_gate\s*::\s*open\b",
        "callers": {(_MAIN, "handle_boot_button")}, "once": True, "home": None},
    "g_prov_gate": {
        "callers": {(_MAIN, "prov_gate_take_hook"), (_MAIN, "prov_gate_is_open_hook"),
                    (_MAIN, "handle_boot_button")},
        "file": _MAIN, "ident": True,
        "decl": ("static canary::net::provisioning_gate::State g_prov_gate = {0};",),
        "stmts": {(_MAIN, "handle_boot_button"): _BOOT_GATE_STMTS}},
    # ...and the sweep in that handler runs on every pass of loop(): its call
    # there is the bare statement at loop()'s own level, with nothing before
    # it in loop() able to leave ("lead", a lambda's return aside). Polled
    # only on a GPIO edge, or skipped by an early `return` above it, a tap
    # nobody took would not be swept, and its 32-bit stamp would read fresh
    # again once millis() wrapped (about 49.7 days; F347).
    "handle_boot_button": {"callers": {(_MAIN, "loop")}, "once": True, "lead": True,
                           "whole": {(_MAIN, "loop"): "handle_boot_button();"},
                           "home": (_MAIN, "handle_boot_button")},
    # The page grant's first-boot unlock at its source: the setup state is
    # written only by the setup lib's pinned functions (GLUE_DEFS), and the
    # timeout that ends it runs from loop() while setup is active, with
    # nothing before its call in loop() able to leave.
    "s_active": dict(_SETUP_STATE, decl=("static bool s_active = false;",),
                     callers=_SETUP_WRITERS | {(_SETUP, "setup_is_active")}),
    "s_first_boot": dict(_SETUP_STATE, decl=("static bool s_first_boot = false;",),
                         callers=_SETUP_WRITERS | {(_SETUP, "setup_is_first_boot")}),
    "s_started_ms": dict(_SETUP_STATE, decl=("static uint32_t s_started_ms = 0;",),
                         callers={(_SETUP, "setup_init"), (_SETUP, "setup_check_timeout")}),
    "setup_check_timeout": {
        "callers": {(_MAIN, "loop")}, "once": True, "lead": True,
        "whole": {(_MAIN, "loop"): _SETUP_TIMEOUT},
        "home": (_SETUP, "setup_check_timeout")},
    # ...and the stored flag first boot is read from (setup_init): written or
    # removed only by those functions, so a route that clears it cannot reboot
    # the device into setup on the home LAN with its Wi-Fi kept.
    "NVS_KEY_SETUP_OK": {"callers": _SETUP_WRITERS, "ident": True},
    # The credentials-and-settings reset (which clears that flag with the Wi-Fi
    # keys, through reset_scope_keys.h's list): run only by the BOOT hold's
    # dispatch, which runs only from the boot-time hold and handle_boot_button.
    "factory_reset": {"callers": {(_MAIN, "reset_hold_dispatch")},
                      "home": (_MAIN, "factory_reset")},
    "reset_hold_dispatch": {"callers": {(_MAIN, "setup"), (_MAIN, "handle_boot_button")},
                            "whole": {(_MAIN, "setup"): _BOOT_HOLD_AT_BOOT},
                            "home": (_MAIN, "reset_hold_dispatch")},
    "g_reset_hold": {"callers": {(_MAIN, "setup"), (_MAIN, "reset_hold_dispatch"),
                                 (_MAIN, "handle_boot_button")},
                     "file": _MAIN, "ident": True,
                     "decl": ("static canary::io::ResetHold g_reset_hold;",)},
    # Every route is an esp_http_server registration, which the route rules
    # read wherever it is written. A server the core brings (Arduino's
    # WebServer, the HTTPUpdateServer firmware push, ArduinoOTA, the raw
    # WiFiServer of the SimpleWiFiServer idiom, which core 3.x calls
    # NetworkServer, and WiFiProv, whose SoftAP scheme raises an access point
    # of its own and serves its provisioning over HTTP, with the ESP-IDF
    # provisioning manager it wraps, wifi_prov_mgr_* on core 2.x and
    # network_prov_mgr_* on 3.x), or ESPAsyncWebServer's, would serve routes,
    # a firmware upload or an access point that no rule reads: none of these
    # is named, in a function or outside one (an object of one declared at
    # namespace scope is a mention, so the row reads every mention, as a
    # variable's does; an angled `#include <WebServer.h>` is one too, but a
    # quoted path is a string and `<ESPAsyncWebServer.h>` holds no
    # AsyncWebServer as a whole name, so it is the class's use that fails). A
    # registry library is held where the build fetches it instead
    # (REGISTRY_LIBS). Not named here: a server written on the raw socket API,
    # the core's UDP classes (WiFiUDP, AsyncUDP, DNSServer; the flagship's own
    # captive DNS responder is a WiFiUDP), its mDNS and NetBIOS responders
    # (the flagship runs mDNS), a BLE GATT server (the core's BLEServer, or
    # NimBLE's, which the flagship's status service runs), and an ESP-IDF
    # component that starts a server of its own (esp_local_ctrl).
    "another HTTP server": {
        "pattern": r"\b(?:WebServer|AsyncWebServer|HTTPUpdateServer|ArduinoOTA|WiFiServer"
                   r"|NetworkServer|WiFiProv(?:Class)?|wifi_prov_mgr_\w+|network_prov_mgr_\w+)\b",
        "callers": set(), "ident": True, "none": True, "home": None},
    # The token's alphabet, which format_api_token_string renders the HMAC
    # through (its declaration is the row's, so a narrowed alphabet fails).
    "UNAMBIGUOUS_ALPHABET": dict(_ALPHABET, decl=(
        'static const char UNAMBIGUOUS_ALPHABET[] = '
        '"23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";',
        "static const size_t UNAMBIGUOUS_LEN = sizeof(UNAMBIGUOUS_ALPHABET) - 1;")),
    "UNAMBIGUOUS_LEN": dict(_ALPHABET, decl=(
        "static const size_t UNAMBIGUOUS_LEN = sizeof(UNAMBIGUOUS_ALPHABET) - 1;",)),
}
# The one statement a `top` census call may follow out of its function: the
# rate limit's refusal, its second argument (when it has one) a literal or a
# plain local, never a call.
RATE_LIMIT_REFUSAL = re.compile(
    r"if \(!rate_limit_check\(req(?:, (?:true|false|[A-Za-z_]\w*))?\)\) return ESP_OK;")
# receipt_decide's grants after the Host, folded: the bearer, then the tap.
RECEIPT_GRANTS = ("[req]() { return bearer_present_and_valid(req); }",
                  "[]() { return provisioning_gate_take(); }")

# ── The bearer-only answers: the no-session premise ──────────────────────────

# The flagship has no session: no cookie, no session header, no credential
# in a URL query. The request headers each file may read, by literal name.
REQUEST_HEADERS = {_AUTH: ("Authorization",), _NET: ("Host",)}
# The functions that may read a URL query: the witness log's paging.
URL_QUERY_READERS = {(_NET, "handle_witness")}
_URL_QUERY = re.compile(r"\b(httpd_req_get_url_query_len|httpd_req_get_url_query_str"
                        r"|httpd_query_key_value)\s*\(")
# The functions that may read a request's raw path (`->uri` or `.uri` through
# any reference to the request, which keeps the query, so a `?token=` or
# `?cv_session=` parsed from it is a query credential the URL-query rule above
# would not see), and why. A route table's designated `.uri = ...` is a write.
URI_READERS = {
    (_NET, "handle_captive_probe"): "matches the probe's path (probe_path_is)",
    (_NET, "handle_https_redirect"): "the port-80 exemption and the 307's https:// target",
    (_NET, "handle_log_ack"): "the record seq in POST /api/logs/<seq>/ack, behind auth_gate",
    (_NET, "register_route"): "a route table entry's own path (httpd_uri_t), for the "
                              "full-table log line; no request",
}
_URI_READ = re.compile(r"->\s*uri\b|\.\s*uri\b(?!\s*=(?!=))")
_NO_SESSION = ("the flagship has no session today. A cookie, a session header or a query "
               "credential is canary-wap's any-session class (ANY_SESSION_GATES, F328): "
               "CONSOLIDATION.md Phase 6's cv_session port must bring that rule with it")

# What must not run before the Host is decided: reading the bearer, taking
# the tap, reading the token, emitting it.
PRE_HOST_STEPS = ("bearer_present_and_valid(", "auth_bearer_grant(", "auth_check(",
                  "provisioning_gate_take(", "auth_get_token(") + tuple(
                      name + "(" for name in TOKEN_EMITTERS)

_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined"}


_FUNC_HEAD = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;{}()]*\)" + _HEAD_TAIL + r"\{")
# A constructor's parameter list closing, then its initializer list's `:`
# (not a `::`).
_INIT_COLON = re.compile(r"\)\s*:(?!:)")
# One initializer-list entry's member, up to its `(` or `{`.
_INIT_ENTRY = re.compile(r"\s*[A-Za-z_][\w:]*\s*(?=[({])")
# The qualifier directly before a function's name: `Class::` (or `Class::~`).
_QUALIFIER = re.compile(r"\b([A-Za-z_]\w*)\s*::\s*(~\s*)?$")


def _bracket_close(blanked: str, open_: int) -> int:
    """Offset of the `)` or `}` closing the `(` or `{` at `open_`, or -1."""
    pair = {"(": ")", "{": "}"}[blanked[open_]]
    depth = 0
    for k in range(open_, len(blanked)):
        if blanked[k] == blanked[open_]:
            depth += 1
        elif blanked[k] == pair:
            depth -= 1
            if depth == 0:
                return k
    return -1


def _qualified(blanked: str, at: int, name: str):
    """(qualified name, start) of the function name `name` found at `at`:
    `Class::name` when a qualifier sits directly before it (only that one:
    `ns::Type Class::name(` is `Class::name`), `Class::~name` for a
    destructor, else the bare name."""
    head = blanked[max(0, at - 200):at]
    m = _QUALIFIER.search(head)
    if m:
        return (f"{m.group(1)}::{'~' if m.group(2) else ''}{name}",
                at - len(head) + m.start())
    return name, at


def _initializer_body(blanked: str, i: int) -> int:
    """The offset of the `{` that opens a constructor's body, when an
    initializer list (`a_(x), b_{y}, ...`) starts at `i` (just past its
    `:`), else -1 (a ternary's `:`, a label, anything that is not one)."""
    while True:
        m = _INIT_ENTRY.match(blanked, i)
        if not m:
            return -1
        close = _bracket_close(blanked, m.end())
        if close < 0:
            return -1
        k = close + 1
        while k < len(blanked) and blanked[k].isspace():
            k += 1
        if k < len(blanked) and blanked[k] == ",":
            i = k + 1
            continue
        return k if k < len(blanked) and blanked[k] == "{" else -1


@functools.lru_cache(maxsize=None)
def function_defs(text: str) -> tuple:
    """(name, start, name_at, code_start, end) for every brace-bodied
    function definition in `text` whose parameter list holds no parentheses
    (every handler and helper here). `name` is qualified when the head has a
    qualifier (`AuthManager::grant`, `ScvNetworkManager::begin`); `start` is
    the head (its qualifier); `name_at` is where the name itself is;
    `code_start` is the body's `{`, or a constructor's initializer-list `:`;
    `end` is just past the closing `}`.

    A constructor is named for its class (`ScvNetworkManager::
    ScvNetworkManager`), its initializer list included, never for its last
    initializer entry (`m_peers_last_browse_ms(0) {` read as a head was the
    old parser's name for that body). A lambda has no name before its `(`,
    so a statement inside one is attributed to the function that holds the
    lambda."""
    blanked = blank_string_contents(text)
    heads = []
    inits = []
    for m in _INIT_COLON.finditer(blanked):
        open_ = _paren_open(blanked, m.start())
        if open_ < 0:
            continue
        j = open_
        while j > 0 and blanked[j - 1].isspace():
            j -= 1
        name = re.search(r"[A-Za-z_]\w*$", blanked[max(0, j - 80):j])
        if not name or name.group(0) in _KEYWORDS:
            continue
        body = _initializer_body(blanked, m.end())
        if body < 0:
            continue
        at = j - len(name.group(0))
        qual, start = _qualified(blanked, at, name.group(0))
        inits.append((m.end(), body))
        heads.append((qual, start, at, m.end() - 1, body))
    for m in _FUNC_HEAD.finditer(blanked):
        if m.group(1) in _KEYWORDS or any(a <= m.start() < b for a, b in inits):
            continue
        qual, start = _qualified(blanked, m.start(1), m.group(1))
        heads.append((qual, start, m.start(1), m.end() - 1, m.end() - 1))
    out = []
    for qual, start, at, code_start, body in heads:
        close = _bracket_close(blanked, body)
        out.append((qual, start, at, code_start, close + 1 if close >= 0 else len(blanked)))
    return tuple(sorted(out, key=lambda d: d[1]))


def function_spans(text: str):
    """[(name, start, end)] for every function_defs() definition: the name
    (qualified for a member), the head, the end."""
    return [(name, start, end) for name, start, _at, _code, end in function_defs(text)]


def enclosing_function(spans, offset: int):
    """The innermost (name, start, end) of `spans` holding `offset`, or None."""
    best = None
    for name, start, end in spans:
        if start <= offset < end and (best is None or start > best[1]):
            best = (name, start, end)
    return best


_HOST_LOCAL = re.compile(r"\bconst\s+bool\s+([A-Za-z_]\w*)\s*=\s*" + _HOST_CALL + r"\s*;")


def refusal_branch(blanked: str, at: int):
    """The statement an if condition ending at `at` controls, as (start,
    end), when it always returns — `return ...;`, or a `{...}` block with a
    `return` directly in it (a return nested deeper, or in a lambda, does
    not count) — else None."""
    j = at
    while j < len(blanked) and blanked[j].isspace():
        j += 1
    if re.match(r"return\b", blanked[j:j + 7]):
        end = blanked.find(";", j)
        return (j, end + 1) if end >= 0 else None
    if j >= len(blanked) or blanked[j] != "{":
        return None
    depth, k, returns = 0, j, False
    while k < len(blanked):
        c = blanked[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return (j, k + 1) if returns else None
        elif (depth == 1 and blanked.startswith("return", k)
              and not (blanked[k - 1].isalnum() or blanked[k - 1] == "_")
              and not (blanked[k + 6:k + 7].isalnum() or blanked[k + 6:k + 7] == "_")
              and _statement_prefix(blanked, k) == ""):
            returns = True
        k += 1
    return None


def _written_once(blanked: str, name: str) -> bool:
    """True when `name` is assigned exactly once in `blanked` (its
    declaration): no shadowing redeclaration, no reassignment."""
    return len(re.findall(r"\b" + re.escape(name) + r"\s*=(?!=)", blanked)) == 1


def host_refusals(body: str) -> list:
    """(offset, branch_text) for every `if (host_is_foreign(req))` — or
    `if (<local>)` for a `const bool <local> = host_is_foreign(req);`
    written once — whose condition is the Host verdict ALONE, whose branch
    always returns, and whose branch reads no credential, takes no tap and
    emits nothing (PRE_HOST_STEPS): the refusal. A branch that only logs,
    one that serves, and a condition that mixes the verdict with anything
    else are not a Host check."""
    blanked = blank_string_contents(body)
    out = []
    def consider(cond_end, at):
        if in_lambda(blanked, at):
            return
        span = refusal_branch(blanked, cond_end)
        if span and not steps_before(blanked[span[0]:span[1]], span[1] - span[0]):
            out.append((at, blanked[span[0]:span[1]]))
    for m in re.finditer(r"\b(?:else\s+)?if\s*\(\s*" + _HOST_CALL + r"\s*\)", blanked):
        consider(m.end(), blanked.find("host_is_foreign", m.start()))
    for loc in _HOST_LOCAL.finditer(blanked):
        if not _written_once(blanked, loc.group(1)):
            continue
        for m in re.finditer(r"\b(?:else\s+)?if\s*\(\s*" + re.escape(loc.group(1)) +
                             r"\s*\)", blanked[loc.end():]):
            consider(loc.end() + m.end(), loc.end() + m.start())
    return sorted(out)


def host_refusal_ifs(body: str) -> list:
    """The offsets of host_refusals(body)."""
    return [at for at, _ in host_refusals(body)]


def host_decision_args(body: str, fns=HOST_DECISIONS):
    """([offset], [(fn, first_arg)]) over every call of a HOST_DECISIONS
    function in `body`. The first list holds, for each call whose FIRST
    argument is the Host verdict — `host_is_foreign(req)` itself, or a
    `const bool` local initialized from exactly that and declared before
    the call — the offset where the Host is read. The second list holds
    every call given anything else (a literal, a negation, a local that
    is not the Host, a mutable, shadowed or reassigned local)."""
    blanked = blank_string_contents(body)
    good, bad = [], []
    for fn in fns:
        for off, args in call_args(body, fn):
            first = args[0] if args else ""
            if re.fullmatch(_HOST_CALL, first):
                good.append(blanked.find("host_is_foreign", off))
                continue
            locs = [m for m in _HOST_LOCAL.finditer(blanked[:off]) if m.group(1) == first]
            if locs and _written_once(blanked, first):
                good.append(locs[-1].start() + locs[-1].group(0).find("host_is_foreign"))
            else:
                bad.append((fn, first))
    return good, bad


def first_host_decision(body: str) -> int:
    """Offset in `body` where the Host is first decided, or -1: an
    `if (host_is_foreign(req))` refusal that returns (host_refusal_ifs), or
    the Host verdict handed to a host-tested decision as its first argument
    (host_decision_args). A bare call, a local nobody reads, and an if that
    only logs are not a Host check."""
    found = host_refusal_ifs(body) + host_decision_args(body)[0]
    return min(found) if found else -1


# The receipt dispatch: the verdict bound once, the receipt sent only under
# an explicit SERVE_* test, and the function ending in the Host refusal.
_RECEIPT_BIND = re.compile(r"\bconst\s+(?:[\w:]*::)?ReceiptVerdict\s+([A-Za-z_]\w*)\s*=\s*"
                           r"(?:[\w:]*::)?receipt_decide\s*\(")


def _serve_condition(cond: str, verdict: str) -> bool:
    """True when `cond` is `verdict == ReceiptVerdict::SERVE_BEARER` or
    `... SERVE_TAP`, or a disjunction of exactly those."""
    one = (r"\(?\s*" + re.escape(verdict) +
           r"\s*==\s*(?:[\w:]*::)?ReceiptVerdict::SERVE_(?:BEARER|TAP)\s*\)?")
    return re.fullmatch(r"\s*" + one + r"(?:\s*\|\|\s*" + one + r")*\s*", cond) is not None


def _under_serve_test(blanked: str, off: int, verdict: str) -> bool:
    """True when the call at `off` runs only inside an `if (<serve>)`: either
    it is that if's unbraced statement, or some block enclosing it is opened
    by that if (an `else` block is not)."""
    stmt = max(blanked.rfind(";", 0, off), blanked.rfind("{", 0, off),
               blanked.rfind("}", 0, off)) + 1
    m = re.compile(r"\s*(?:else\s+)?if\s*\(").match(blanked, stmt)
    if m:
        o = m.end() - 1
        c = _paren_close(blanked, o)
        if (c > 0 and _serve_condition(blanked[o + 1:c], verdict)
                and re.fullmatch(r"\s*(?:return)?\s*", blanked[c + 1:off])):
            return True
    depth = 0
    for k in range(off - 1, -1, -1):
        ch = blanked[k]
        if ch == "}":
            depth += 1
        elif ch == "{":
            if depth:
                depth -= 1
                continue
            j = k - 1
            while j >= 0 and blanked[j].isspace():
                j -= 1
            if j < 0 or blanked[j] != ")":
                continue
            o = _paren_open(blanked, j)
            if o < 0:
                continue
            if (re.search(r"\bif\s*$", blanked[max(0, o - 12):o])
                    and _serve_condition(blanked[o + 1:j], verdict)):
                return True
    return False


def check_receipt_dispatch(body: str, where: str) -> list:
    """A function that calls receipt_decide fails closed: the verdict is a
    `const ReceiptVerdict` local, every receipt it sends sits under an
    explicit SERVE_BEARER / SERVE_TAP test, and after the last send, at
    the function's own level and reachable, it answers the Host refusal —
    so REFUSE_HOST, a dropped branch or a verdict added later refuses."""
    failures = []
    blanked = blank_string_contents(body)
    calls = call_args(body, "receipt_decide")
    if not calls:
        return failures
    binds = list(_RECEIPT_BIND.finditer(blanked))
    if len(binds) != 1 or len(calls) != 1:
        return [f"{where}: receipt_decide's verdict must be bound once, as "
                f"`const ReceiptVerdict <v> = receipt_decide(...)`, and tested — "
                f"found {len(calls)} call(s) and {len(binds)} binding(s)"]
    verdict = binds[0].group(1)
    writes = re.findall(r"\b" + re.escape(verdict) + r"\s*=(?!=)", blanked)
    if len(writes) != 1:
        failures.append(f"{where}: `{verdict}` is written {len(writes)} times — the "
                        f"receipt_decide verdict is bound once and never shadowed "
                        f"or reassigned")
    sends = [off for off, _ in call_args(body, "send_provisioning_receipt")]
    for off in sends:
        if not _under_serve_test(blanked, off, verdict):
            failures.append(f"{where}: sends the receipt outside an explicit "
                            f"`if ({verdict} == ReceiptVerdict::SERVE_BEARER)` / "
                            f"`SERVE_TAP` test — every other verdict (REFUSE_HOST "
                            f"above all) must refuse, so a dropped or mistyped "
                            f"branch cannot serve")
    open_brace = blanked.find("{")
    tails = []
    for off, _ in call_args(body, "send_host_refusal"):
        depth = blanked[open_brace:off].count("{") - blanked[open_brace:off].count("}")
        if (depth == 1 and _statement_prefix(blanked, off) == ""
                and off > max(sends, default=-1) and not returns_before(body, off)):
            tails.append(off)
    if not tails:
        failures.append(f"{where}: does not end in the Host refusal — after the last "
                        f"receipt send, at the function's own level, "
                        f"send_host_refusal(req) must answer REFUSE_HOST and every "
                        f"verdict the tests above do not name")
    return failures


def steps_before(body: str, limit: int, steps=PRE_HOST_STEPS) -> list:
    """The PRE_HOST_STEPS that occur in `body` before offset `limit`."""
    blanked = blank_string_contents(body)
    out = []
    for step in steps:
        i = blanked.find(step)
        # `auth_check(` must not match inside `bearer_auth_check(` and friends.
        while i > 0 and (blanked[i - 1].isalnum() or blanked[i - 1] == "_"):
            i = blanked.find(step, i + 1)
        if 0 <= i < limit:
            out.append(step[:-1])
    return out


def check_host_first(sources: dict, require_defs: bool = True) -> list:
    """The Host is asked FIRST on every path that can hand out the token or
    spend the BOOT tap (see the module docstring)."""
    failures = []
    routes = []
    for fname, text in sources.items():
        routes.extend(r for r in parse_routes(fname, text) if None not in r[1:3])
    all_spans = {fname: function_spans(text) for fname, text in sources.items()}

    def body_of(name):
        for fname, text in sorted(sources.items()):
            span = definition_span(text, name)
            if span:
                return fname, text[span[0]:span[1]]
        return None, None

    # 0. host_is_foreign's own body is GLUE_DEFS's (check_bearer_only_answers).

    # 1. auth_gate refuses a foreign Host before it reads or compares the token.
    fname, body = body_of("auth_gate")
    if body is None:
        if require_defs:
            failures.append("auth_gate definition not found — parser regression?")
    else:
        refusals = [at for at, branch in host_refusals(body)
                    if re.search(r"\breturn\s+false\s*;", branch)
                    and not re.search(r"\breturn\s+(?!false\b)", branch)]
        if not refusals:
            failures.append(f"{fname}: auth_gate no longer refuses a foreign Host "
                            f"(if (host_is_foreign(req)) {{ ... return false; }}) — every "
                            f"auth_gate route would serve under a rebound name")
        else:
            late = steps_before(body, refusals[0],
                                ("auth_get_token(", "auth_check(", "auth_bearer_grant("))
            if late:
                failures.append(f"{fname}: auth_gate reads the token ({', '.join(late)}) "
                                f"before its Host check")

    # Every host-tested decision is handed the Host verdict itself.
    for fname, text in sorted(sources.items()):
        for name, start, end in all_spans[fname]:
            if name in HOST_DECISIONS:
                continue
            for fn, first in host_decision_args(text[start:end])[1]:
                failures.append(f"{fname}: {name} gives {fn} {first!r} as its Host "
                                f"verdict — the first argument must be "
                                f"host_is_foreign(req), or a `const bool` local set from "
                                f"exactly that")

    # 2. Every gated route reaches the Host check before any credential step:
    #    through auth_gate, or by asking it itself. A public route reaches no
    #    credential step at all except the page path (rule 3).
    seen = set()
    for uri, method, handler, fname in routes:
        base = handler.split("::")[-1]
        if (method, uri, base) in seen or uri is None:
            continue
        seen.add((method, uri, base))
        # Every definition: an `#else` body is a body like the first.
        for body in handler_bodies(sources, base):
            if (method, uri) in PUBLIC_ALLOWLIST:
                steps = steps_before(body, len(body), tuple(
                    st for st in PRE_HOST_STEPS if st != "send_html_with_token("))
                if steps:
                    failures.append(f"{method} {uri}: public handler {handler} reaches "
                                    f"{', '.join(steps)} — a public route may hand out the "
                                    f"token only through send_html_with_token(..., "
                                    f"page_token_inject(req))")
                continue
            via_gate = first_deciding_marker(body, "auth_gate(")
            via_host = first_host_decision(body)
            covered = [i for i in (via_gate, via_host) if i >= 0]
            if not covered:
                failures.append(f"{method} {uri}: handler {handler} ({fname}) never asks the "
                                f"Host — run auth_gate, or decide host_is_foreign(req) before "
                                f"the bearer, the tap and the token (the receipt route's "
                                f"shape)")
                continue
            late = steps_before(body, min(covered))
            if late:
                failures.append(f"{method} {uri}: handler {handler} ({fname}) runs "
                                f"{', '.join(late)} before the Host check — a foreign Host "
                                f"must be refused before the bearer is read or the tap is "
                                f"spent")

    # 3. The page path: page_token_inject asks the Host before any grant and
    #    before the tap, and hands that verdict to page_token_decide.
    fname, body = body_of("page_token_inject")
    if body is None:
        if require_defs:
            failures.append("page_token_inject definition not found — parser regression?")
    else:
        fed = host_decision_args(body, ("page_token_decide",))[0]
        h = min(fed) if fed else -1
        if h < 0:
            failures.append(f"{fname}: page_token_inject does not decide the Host first "
                            f"(host_is_foreign(req) as page_token_decide's first "
                            f"argument) — a page load under a foreign Host would spend "
                            f"the owner's BOOT tap")
        else:
            late = steps_before(body, h)
            if late:
                failures.append(f"{fname}: page_token_inject runs {', '.join(late)} "
                                f"before the Host check")

    # 4. send_html_with_token streams the token only on PageToken::INJECT:
    #    `const char* token = (<verdict> == PageToken::INJECT) ? auth_get_token()
    #    : "";`, <verdict> being its PageToken parameter, never reassigned.
    fname, body = body_of("send_html_with_token")
    if body is None:
        if require_defs:
            failures.append("send_html_with_token definition not found — parser regression?")
    else:
        blanked = blank_string_contents(body)
        brace = blanked.find("{")
        param = re.search(r"\bPageToken\s+([A-Za-z_]\w*)\s*\)\s*$", blanked[:brace])
        if not param:
            failures.append(f"{fname}: send_html_with_token no longer takes the "
                            f"page-token verdict (a PageToken, last parameter)")
        else:
            v = re.escape(param.group(1))
            if re.search(r"\b" + v + r"\s*=(?!=)", blanked[brace:]):
                failures.append(f"{fname}: send_html_with_token reassigns its verdict")
            shape = re.compile(r"const\s+char\s*\*\s*\w+\s*=\s*\(?\s*" + v +
                               r"\s*==\s*(?:[\w:]*::)?PageToken::INJECT\s*\)?\s*\?\s*"
                               r"auth_get_token\s*\(\s*\)\s*:\s*\"\"")
            for m in re.finditer(r"\bauth_get_token\s*\(", blanked):
                stmt_start = max(blanked.rfind(";", 0, m.start()),
                                 blanked.rfind("{", 0, m.start()),
                                 blanked.rfind("}", 0, m.start())) + 1
                stmt_end = blanked.find(";", m.start())
                if not shape.fullmatch(blanked[stmt_start:stmt_end].strip()):
                    failures.append(f"{fname}: send_html_with_token reads auth_get_token() "
                                    f"outside a PageToken::INJECT test (`const char* token "
                                    f"= ({param.group(1)} == PageToken::INJECT) ? "
                                    f"auth_get_token() : \"\"`) — only that verdict (Host "
                                    f"first, host-tested) may put the token in a page")

    # 5. Every call of an emitter (other than the page's, rule 3) sits behind
    #    the Host check in its own function. (Which functions may read a
    #    credential at all is check_bearer_only_answers's reader rule.)
    for fname, text in sorted(sources.items()):
        spans = all_spans[fname]
        for emitter in TOKEN_EMITTERS:
            if emitter == "send_html_with_token":
                continue
            for off, _ in call_args(text, emitter):
                enc = enclosing_function(spans, off)
                if enc is None:
                    continue  # a declaration
                name, start, end = enc
                body = text[start:end]
                covered = [i for i in (first_deciding_marker(body, "auth_gate("),
                                       first_host_decision(body)) if i >= 0]
                rel = off - start
                line = text.count("\n", 0, off) + 1
                if not covered or min(covered) > rel:
                    failures.append(f"{fname}:{line}: {name} calls {emitter}, which emits "
                                    f"{TOKEN_EMITTERS[emitter][1]}, without deciding "
                                    f"host_is_foreign(req) (or auth_gate) first")

    # 6. Every function that asks receipt_decide serves only on SERVE_* and
    #    ends in the Host refusal (check_receipt_dispatch).
    for fname, text in sorted(sources.items()):
        for name, start, end in all_spans[fname]:
            if name in HOST_DECISIONS or "receipt_decide" not in text[start:end]:
                continue
            failures.extend(check_receipt_dispatch(text[start:end], f"{fname}: {name}"))

    if require_defs:
        for table in (TOKEN_EMITTERS, TOKEN_COMPARERS):
            for name in table:
                if not any(definition_span(t, name) for t in sources.values()):
                    failures.append(f"{name} is listed as a token reader but is not "
                                    f"defined — remove or fix the entry")
    return failures


# ── The bearer-only answers ───────────────────────────────────────────────────
#
# check_bearer_only_answers() and its helpers are named for the gates they
# hold, not for what the gates protect: CodeQL's
# py/clear-text-logging-sensitive-data reads a call of a function (or a
# local) whose name says "secret", "password", "cert" or "lock" as a
# credential, and main() prints what these return. This script reads C++
# source text and never holds a credential, so that would be a false alert
# on every run (the canary-wap checker's #1832 lesson). Keep those words out
# of every function and local name below.


def _fold(text: str) -> str:
    return " ".join(text.split())


def _in_file(fname: str, suffix) -> bool:
    """True when `fname` is the file `suffix` names (its path's tail), or one
    of them when `suffix` is a tuple."""
    if isinstance(suffix, tuple):
        return any(_in_file(fname, s) for s in suffix)
    return fname == suffix or fname.endswith("/" + suffix)


def _kinds(kinds) -> str:
    return ", ".join(SECRET_NAMES[k] for k in SECRET_NAMES if k in kinds) or "nothing"


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _times(n: int) -> str:
    return {0: "0 times", 1: "once", 2: "twice"}.get(n, f"{n} times")


def _innermost(defs, pos: int):
    """The innermost function_defs() entry whose span holds `pos`, or None."""
    best = None
    for d in defs:
        if d[1] <= pos < d[4] and (best is None or d[1] > best[1]):
            best = d
    return best


def _statement_at(blanked: str, lo: int, hi: int, pos: int, whole_if: bool = False):
    """(start, end, level) of the statement holding `pos`, scanning the code
    from `lo` to `hi` (a function's code, or a whole file): it runs from just
    after the last `;`, `{` or `}` at the level of the innermost brace block
    holding `pos` (parentheses and brackets nest inside a statement; a
    lambda's braces open a block of their own) to the first `;` or `{` at
    that level, inclusive. With `whole_if`, a statement that is an `if` /
    `else if` header runs on through its braced branch. `level` is that
    block's nesting from `lo` (1 is a function body's own level)."""
    stack, starts = [], {0: lo}
    for k in range(lo, pos):
        c = blanked[k]
        if c in "([{":
            stack.append(c)
            if c == "{":
                starts[len(stack)] = k + 1
        elif c in ")]}":
            if stack:
                stack.pop()
            if c == "}":
                starts[len(stack)] = k + 1
        elif c == ";":
            starts[len(stack)] = k + 1
    level = max((i + 1 for i, c in enumerate(stack) if c == "{"), default=0)
    start = starts.get(level, lo)
    while start < pos and (blanked[start].isspace() or blanked[start] == "#"):
        if blanked[start] == "#":  # a preprocessor line before the statement is not of it
            nl = blanked.find("\n", start)  # (its continuation lines are spliced already)
            start = pos if nl < 0 or nl > pos else nl + 1
        else:
            start += 1
    depth, k = len(stack), pos
    while k < hi:
        c = blanked[k]
        if depth == level and c in ";{}":
            break
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        k += 1
    if k >= hi or blanked[k] == "}":
        return start, k, level
    if (blanked[k] == "{" and whole_if
            and re.match(r"(?:else\s+)?if\s*\(", blanked[start:k])):
        close = _bracket_close(blanked, k)
        return start, (close + 1 if close >= 0 else hi), level
    return start, k + 1, level


def _outside_statement(text: str, blanked: str, pos: int):
    """(start, end, folded) of the statement holding `pos` outside every
    function (a declaration): from just after the nearest `;`, `{` or `}`
    before it to the `;` that ends it, a brace initializer included
    (`State g = {0};` whole, so its initial value is part of it)."""
    lo = max(blanked.rfind(c, 0, pos) for c in ";{}") + 1
    start, end, _level = _statement_at(blanked, lo, len(blanked), pos)
    if blanked[end - 1:end] == "{" and "=" in blanked[start:end]:
        close = _bracket_close(blanked, end - 1)
        semi = blanked.find(";", close) if close >= 0 else -1
        if semi >= 0 and not blanked[close + 1:semi].strip():
            end = semi + 1
    return start, end, _fold(text[start:end])


def _function_index(sources: dict):
    """({(fname, qualified name): (def, ...)}, {bare name: [keys]}) over every
    function_defs() definition. A file that defines one name twice (an `#if`
    body and its `#else` one) keeps both: the reads and calls of every body
    are the function's, and every rule that holds a body holds each."""
    defs, by_name = {}, {}
    for fname, text in sorted(sources.items()):
        for d in function_defs(text):
            key = (fname, d[0])
            if key not in defs:
                defs[key] = ()
                by_name.setdefault(d[0].split("::")[-1], []).append(key)
            defs[key] += (d,)
    return defs, by_name


_ANY_CALL = re.compile(r"(?<![\w~])([A-Za-z_]\w*)\s*\(")


def _reads_in(sources: dict, defs: dict, reads_tbl: dict):
    """({key: {kind: [pos]}}, {key: {called bare names}}, [(fname, pos, kind)]
    for each read outside every function), over every body of every key. A
    call by bare name is any `name(`: free, member (`net.begin(`) or
    qualified (`ns::name(`)."""
    reads, calls, outside = {}, {}, []
    per_file = {}
    for key, ds in defs.items():
        per_file.setdefault(key[0], []).extend(ds)
    for fname, text in sorted(sources.items()):
        blanked = blank_string_contents(text)
        fdefs = per_file.get(fname, [])
        for kind, rx in reads_tbl.items():
            for m in rx.finditer(blanked):
                d = _innermost(fdefs, m.start())
                if d is None:
                    outside.append((fname, m.start(), kind))
                elif m.start() != d[2]:  # not the definition's own name
                    reads.setdefault((fname, d[0]), {}).setdefault(kind, []).append(m.start())
        for d in fdefs:
            calls.setdefault((fname, d[0]), set()).update(
                n for n in _ANY_CALL.findall(blanked[d[3]:d[4]]) if n not in _KEYWORDS)
    return reads, calls, outside


def _carry_map(defs, by_name, reads, calls, stops) -> dict:
    """{key: kinds}: what each function carries — what it reads, and to a
    fixed point what every function it calls carries (a call by bare name
    reaches every definition of that name) — the carry stopping at `stops`."""
    carried = {k: set(reads.get(k, {})) for k in defs}
    changed = True
    while changed:
        changed = False
        for key in defs:
            if key in stops:
                continue
            for name in calls.get(key, ()):
                for g in by_name.get(name, ()):
                    if g != key and g not in stops and not carried[g] <= carried[key]:
                        carried[key] |= carried[g]
                        changed = True
    return carried


_COND_OPEN = re.compile(r"#\s*if(?:n?def)?\b")
_COND_ALT = re.compile(r"#\s*(?:elif|else)\b")
_COND_CLOSE = re.compile(r"#\s*endif\b")


def _context(text: str, blanked: str, lo: int, start: int) -> list:
    """Where the statement at `start` sits in the function whose code starts
    at `lo`: the folded header of every brace block open there but the body's
    own, outermost first (`if (!loaded) {`, `if (false) {`, a lambda's), then
    every preprocessor conditional opened in the function and still open
    there (`#if 0`, `#ifndef X`; an `#elif` or `#else` is added to its `#if`).
    [] for a statement at the body's own level with no conditional open."""
    opens = []
    for k in range(lo, start):
        if blanked[k] == "{":
            opens.append(k)
        elif blanked[k] == "}" and opens:
            opens.pop()
    out = []
    for b in opens[1:]:
        s, _e, _lv = _statement_at(blanked, lo, b + 1, b)
        out.append(_fold(text[s:b + 1]))
    conds = []
    for line, bline in zip(text[lo:start].split("\n"), blanked[lo:start].split("\n")):
        code = bline.strip()
        if _COND_OPEN.match(code):
            conds.append(_fold(line))
        elif _COND_ALT.match(code) and conds:
            conds[-1] += " ... " + _fold(line)
        elif _COND_CLOSE.match(code) and conds:
            conds.pop()
    return out + conds


# A jump that can skip a statement: a `goto`, or ESP-IDF's esp_check.h macros
# that expand to one (ESP_GOTO_ON_ERROR, ESP_GOTO_ON_FALSE, ...).
_GOTO = re.compile(r"\bgoto\b|\bESP_GOTO_\w+\s*\(")
# A way out of a function before a statement: those, the keywords, and
# esp_check.h's early-return macros (ESP_RETURN_ON_FALSE(cond, value, ...)
# returns `value`).
_LEAVE = re.compile(r"\b(?:return|goto|throw|co_return)\b|\bESP_(?:RETURN|GOTO)_\w+\s*\(")


def _placed(text: str, blanked: str, d, start: int, end: int) -> str:
    """The statement text[start:end] of function body `d`, folded, with where
    it sits in front of it when it sits anywhere but its function's own level
    with nothing able to jump past it: `[within if (!loaded) { > #if 0] ...`,
    `[within after a goto] ...` (_context; a goto or an ESP_GOTO_* macro
    before it in its function)."""
    around = _context(text, blanked, d[3], start)
    if _GOTO.search(blanked, d[3], start):
        around.append("after a goto")
    folded = _fold(text[start:end])
    return "[within " + " > ".join(around) + "] " + folded if around else folded


def _leaves(text: str, blanked: str, d, lo: int, hi: int) -> list:
    """[(start, folded statement)] for each way out of function body `d`
    between offsets `lo` and `hi` (_LEAVE: a return, goto, throw, co_return or
    an ESP_RETURN_* / ESP_GOTO_* macro), at any depth but inside a lambda,
    whose `return` leaves the lambda, not the function."""
    code = blanked[d[3]:d[4]]
    out = []
    for x in _LEAVE.finditer(blanked, max(lo, d[3]), min(hi, d[4])):
        if in_lambda(code, x.start() - d[3]):
            continue
        s, e, _lv = _statement_at(blanked, d[3], d[4], x.start())
        out.append((s, _fold(text[s:e])))
    return out


def _first_placed(text: str, blanked: str, d, rx, want: str):
    """(start, end) of the first statement of body `d` holding a match of `rx`
    that is `want`, folded where it sits (_placed), or None."""
    for m in rx.finditer(blanked, d[3], d[4]):
        a, b, _lv = _statement_at(blanked, d[3], d[4], m.start())
        if _placed(text, blanked, d, a, b) == want:
            return a, b
    return None


def _keyed_problems(sources: dict, census: dict, defs_of: dict) -> list:
    """Each census row's `keyed` raise is followed, in its function, by the
    statement that keys it, with nothing able to leave between them."""
    problems = []
    for name, row in sorted(census.items()):
        for (suffix, qual), (raise_stmt, key_row) in sorted(row.get("keyed", {}).items()):
            whole = census.get(key_row, {}).get("whole")
            key_stmt = whole.get((suffix, qual)) if isinstance(whole, dict) else whole
            rx_raise, rx_key = _census_rx(name, row), _census_rx(key_row, census.get(key_row, {}))
            for fname, text in sorted(sources.items()):
                if not _in_file(fname, suffix):
                    continue
                blanked = blank_string_contents(text)
                for d in defs_of[fname]:
                    if d[0] != qual:
                        continue
                    raised = _first_placed(text, blanked, d, rx_raise, raise_stmt)
                    if raised is None:
                        continue  # no raise: the row's `stmts` pin names the change
                    keyed = _first_placed(text, blanked, d, rx_key, key_stmt) if key_stmt else None
                    if keyed is None or keyed[0] < raised[1]:
                        problems.append(f"{fname}:{_line_of(text, raised[0])}: {qual}'s SoftAP "
                                        f"raise `{raise_stmt}` is not followed by the softAP() "
                                        f"that keys it (`{key_stmt}`), so the SoftAP comes up on "
                                        f"whatever config the driver holds")
                        continue
                    for at, stmt in _leaves(text, blanked, d, raised[1], keyed[0]):
                        problems.append(f"{fname}:{_line_of(text, at)}: {qual} can leave between "
                                        f"its SoftAP raise `{raise_stmt}` and the softAP() that "
                                        f"keys it: {stmt!r} — the SoftAP stays up, never keyed, "
                                        f"on whatever config the driver loaded (raiseAp() returns "
                                        f"while the AP is up), and every SoftAP peer is handed the "
                                        f"page token")
    return problems


def _statements(sources, key, d, positions, T) -> list:
    """[(start, end, folded, kinds)] for the statements of function body `d`
    holding the reads at `positions`, in order, one per statement, each
    folded with where it sits (_placed): a statement moved under `if (false)
    {` or `#if 0`, or past a goto, is not the statement it was. A mint's
    statement (SECRET_MINTS) runs through its branch: `[within if (!loaded) {]
    if (!derive_api_token(...)) { ...; return false; }`."""
    text = sources[key[0]]
    blanked = blank_string_contents(text)
    mint = (re.compile(r"\b(?:" + "|".join(map(re.escape, T["mints"])) + r")\b")
            if T["mints"] else None)
    out = []
    for pos in sorted(positions):
        if any(a <= pos < b for a, b, _f, _k in out):
            continue
        start, end, _level = _statement_at(blanked, d[3], d[4], pos)
        if mint and mint.search(blanked, start, end):
            start, end, _level = _statement_at(blanked, d[3], d[4], pos, whole_if=True)
        kinds = {k for k, rx in T["reads"].items() if rx.search(blanked, start, end)}
        out.append((start, end, _placed(text, blanked, d, start, end), kinds))
    return out


def _positions(reads: dict, key, d) -> list:
    """The read offsets of `key` that fall in body `d`."""
    return [p for ps in reads.get(key, {}).values() for p in ps if d[3] <= p < d[4]]


def _pin_problems(sources, T, key, d, positions, want, table) -> list:
    """Body `d` of a pinned function: its statements that read a credential
    are exactly `want`, its `table` row, in order."""
    fname, qual = key
    text = sources[fname]
    got = _statements(sources, key, d, positions, T)
    folds = tuple(f for _a, _b, f, _k in got)
    if folds == tuple(want):
        return []
    problems = []
    for start, _end, stmt, kinds in got:
        if stmt not in want:
            problems.append(f"{fname}:{_line_of(text, start)}: {qual}() reads {_kinds(kinds)} "
                            f"in a statement not in its {table} row: {stmt!r}")
    for stmt in want:
        if stmt not in folds:
            problems.append(f"{fname}: {qual}()'s {table} row holds {stmt!r}, which it no "
                            f"longer has (a deleted call, a discarded result, a dropped "
                            f"refusal, or a mint moved under a block or a conditional, fails "
                            f"here)")
    problems.append(f"{fname}:{_line_of(text, d[1])}: {qual}()'s statements that read a "
                    f"credential are not its {table} row. If the change is meant, update the "
                    f"row in the same review. Now: {folds!r}")
    return problems


def _home_problems(defs: dict, name: str, home: str, table: str) -> list:
    """`name` is defined exactly once under ROOTS, in its `home` file."""
    found = sorted(f for (f, q), ds in defs.items() if q.split("::")[-1] == name for _d in ds)
    if len(found) == 1 and _in_file(found[0], home):
        return []
    where = ", ".join(found) or "no file the check reads"
    return [f"{table}: {name}() is defined in {where}; it is defined once, in {home} — remove "
            f"or fix the row"]


def _answer_sites(code: str, reads_tbl: dict, carriers: set, self_name: str) -> list:
    """(offset, what) for each place `code` (a handler's string-blanked
    body) reaches a credential: a read, or a call of a function that
    carries one."""
    sites = {(m.start(), re.match(r"\w+", m.group(0)).group(0))
             for rx in reads_tbl.values() for m in rx.finditer(code)}
    sites |= {(m.start(), m.group(1)) for m in _ANY_CALL.finditer(code)
              if m.group(1) != self_name and m.group(1) in carriers}
    return sorted(sites)


def _reader_problems(sources, T, defs, reads, carried, outside, handlers) -> list:
    """(c) Every function that reads a credential is named: an accessor in
    its home, an emitter in its home with exactly its carries and its reading
    statements (EMITTER_STATEMENTS), a comparer reading only the token, a
    local use (its statements: _local_use_problems) or a SECRET_ANSWERS
    handler (_answer_problems). Outside every function only
    OUTSIDE_FUNCTION_LINES may name one."""
    problems = []
    accessors = T["accessors"]
    for name, (home, _kind) in sorted(accessors.items()):
        problems.extend(_home_problems(defs, name, home, "SECRET_ACCESSORS"))
    rows = T["emitter_stmts"]
    for name in sorted(T["emitters"]):
        homes = [f for f, q in rows if q == name]
        if len(homes) != 1:
            problems.append(f"TOKEN_EMITTERS names {name}(), which has {len(homes)} "
                            f"EMITTER_STATEMENTS rows; it takes one, keyed by its home file")
        else:
            problems.extend(_home_problems(defs, name, homes[0], "EMITTER_STATEMENTS"))
    for f, q in sorted(rows):
        if q not in T["emitters"]:
            problems.append(f"EMITTER_STATEMENTS names {q}() ({f}), which is no TOKEN_EMITTERS "
                            f"key — remove or fix the row")
    for key in sorted(reads):
        fname, qual = key
        got = set(reads[key])
        bare = qual.split("::")[-1]
        line = _line_of(sources[fname], defs[key][0][1])
        if bare in accessors and _in_file(fname, accessors[bare][0]):
            if got - {accessors[bare][1]}:
                problems.append(f"{fname}:{line}: accessor {qual}() reads {_kinds(got)}; it is "
                                f"the accessor of {_kinds({accessors[bare][1]})} only")
        elif bare in T["emitters"]:
            want = set(T["emitters"][bare][0].split())
            if carried[key] != want:
                problems.append(f"{fname}:{line}: emitter {qual}() carries "
                                f"{_kinds(carried[key])}; its TOKEN_EMITTERS entry says "
                                f"{_kinds(want)}")
            row = next((stmts for (f, q), stmts in rows.items()
                        if q == qual and _in_file(fname, f)), None)
            for d in (defs[key] if row is not None else ()):
                problems.extend(_pin_problems(sources, T, key, d, _positions(reads, key, d), row,
                                              "EMITTER_STATEMENTS"))
        elif bare in T["comparers"]:
            if got != {"token"}:
                problems.append(f"{fname}:{line}: comparer {qual}() reads {_kinds(got)}; a "
                                f"TOKEN_COMPARERS entry reads the bearer token only, to "
                                f"compare a presented one")
        elif not (any(_in_file(fname, f) and q == qual for f, q in T["local"])
                  or bare in T["answers"]):
            problems.append(f"{fname}:{line}: {qual}() reads {_kinds(got)} and is no "
                            f"TOKEN_EMITTERS, TOKEN_COMPARERS, SECRET_LOCAL_USES or "
                            f"SECRET_ANSWERS entry: name it, and if it answers, put it "
                            f"behind a bearer-only grant")
    for table, label in ((T["emitters"], "TOKEN_EMITTERS"), (T["comparers"], "TOKEN_COMPARERS")):
        for name in sorted(table):
            if not any(k[1].split("::")[-1] == name for k in reads):
                problems.append(f"{label} names {name}(), which reads no credential — "
                                f"remove it")
    seen = set()
    for fname, pos, _kind in sorted(outside):
        if (fname, pos) in seen:
            continue
        text = sources[fname]
        start, end, stmt = _outside_statement(text, blank_string_contents(text), pos)
        seen.update((fname, p) for f, p, _k in outside if f == fname and start <= p < end)
        if not any(_in_file(fname, f) and stmt == s for f, s in T["outside"]):
            problems.append(f"{fname}:{_line_of(text, pos)}: a credential named outside every "
                            f"function ({stmt[:90]!r}): the rule reads credentials per "
                            f"function, so only OUTSIDE_FUNCTION_LINES may")
    for suffix, stmt in T["outside"]:
        hit = False
        for fname, pos, _kind in outside:
            if _in_file(fname, suffix):
                text = sources[fname]
                hit = hit or _outside_statement(text, blank_string_contents(text), pos)[2] == stmt
        if not hit:
            problems.append(f"OUTSIDE_FUNCTION_LINES holds {stmt!r} ({suffix}), which no "
                            f"longer names a credential there — remove or fix the row")
    return problems


def _local_use_problems(sources, T, defs, reads, handlers) -> list:
    """Each SECRET_LOCAL_USES row: defined where it says, reading a
    credential, not a route handler, and the reading statements of every
    body of it exactly the row's."""
    problems = []
    for (suffix, qual), (_why, want) in sorted(T["local"].items()):
        keys = [k for k in defs if _in_file(k[0], suffix) and k[1] == qual]
        if not keys:
            problems.append(f"SECRET_LOCAL_USES names {qual}() in {suffix}, which does not "
                            f"define it — remove or fix the row")
            continue
        for key in keys:
            fname = key[0]
            if qual.split("::")[-1] in handlers:
                problems.append(f"{fname}: {qual}() is a route handler, so it answers: not a "
                                f"SECRET_LOCAL_USES entry")
            if not reads.get(key):
                problems.append(f"SECRET_LOCAL_USES names {qual}() in {suffix}, which reads no "
                                f"credential — remove it")
                continue
            for d in defs[key]:
                problems.extend(_pin_problems(sources, T, key, d, _positions(reads, key, d),
                                              want, "SECRET_LOCAL_USES"))
    return problems


def _refusal_end(code: str, blanked: str, lo: int, hi: int) -> int:
    """End offset of the first BEARER_REFUSAL statement at a function body's
    own level within blanked[lo:hi], or -1."""
    for m in re.finditer(r"\bauth_gate\s*\(", blanked[lo:hi]):
        start, end, level = _statement_at(blanked, lo, hi, lo + m.start())
        if level == 1 and _fold(code[start:end]) == BEARER_REFUSAL:
            return end
    return -1


def _answer_problems(sources, T, defs, by_name, reads, carried, stops, routes) -> list:
    """(d) Every registered handler that carries a credential is a
    SECRET_ANSWERS row with exactly that set, and (e) its grant holds, in
    every body of it. A handler's own body never names the private key."""
    problems = []
    handlers = {h.split("::")[-1] for _u, _m, h, _f in routes}
    public = {h.split("::")[-1] for u, m, h, _f in routes if (m, u) in T["public"]}
    carriers = {k[1].split("::")[-1] for k, s in carried.items() if s and k not in stops}

    def carries(h):
        got = set()
        for key in by_name.get(h, ()):
            if key not in stops:
                got |= carried[key]
        return got

    def bodies(h):
        for key in by_name.get(h, ()):
            if key[1] == h and key not in stops:
                text = sources[key[0]]
                for d in defs[key]:
                    yield key[0], text, blank_string_contents(text), d

    for h in sorted(handlers):
        for fname, text, blanked, d in bodies(h):
            m = _PRIVATE_KEY.search(blanked, d[3], d[4])
            if m:
                problems.append(f"{fname}:{_line_of(text, m.start())}: route handler {h}() "
                                f"names the private key (privkey), which the token and the "
                                f"SoftAP password are derived from: no route answers it")
        got = carries(h)
        if not got or h in T["answers"]:
            continue
        if h in public:
            problems.append(f"public route handler {h}() answers {_kinds(got)}: a credential's "
                            f"answer takes a bearer-only grant (SECRET_ANSWERS), never none")
        else:
            problems.append(f"route handler {h}() answers {_kinds(got)} and is not in "
                            f"SECRET_ANSWERS: name it there, behind a bearer-only grant")
    pages = {h for h, (_c, grant) in T["answers"].items() if grant == "page"}
    for h, (named, grant) in sorted(T["answers"].items()):
        if grant not in ("bearer", "receipt", "page"):
            problems.append(f"SECRET_ANSWERS gives {h}() the grant {grant!r}; a grant is "
                            f"'bearer', 'receipt' or 'page'")
            continue
        if h not in handlers:
            problems.append(f"SECRET_ANSWERS names {h}(), which is no registered route "
                            f"handler — remove or fix the row")
            continue
        got = carries(h)
        if set(named.split()) != got:
            problems.append(f"{h}() carries {_kinds(got)}; its SECRET_ANSWERS entry says "
                            f"{_kinds(named.split())}")
        if h in public and grant != "page":
            problems.append(f"{h}() is a PUBLIC_ALLOWLIST route handler with the {grant!r} "
                            f"grant: a public route may carry a credential only through the "
                            f"page path")
        for fname, text, blanked, d in bodies(h):
            code = blanked[d[3]:d[4]]
            sites = _answer_sites(code, T["reads"], carriers, h)

            def where(off, fname=fname, text=text, d=d):
                return f"{fname}:{_line_of(text, d[3] + off)}: {h}()"
            if grant == "bearer":
                end = _refusal_end(text, blanked, d[3], d[4])
                if end < 0:
                    problems.append(f"{fname}: {h}() answers {_kinds(got)} with no "
                                    f"`{BEARER_REFUSAL}` at its own top level")
                for off, what in sites:
                    if end < 0 or d[3] + off < end:
                        problems.append(where(off) +
                                        f" reaches a credential ({what}) before its refusal "
                                        f"`{BEARER_REFUSAL}`")
            elif grant == "receipt":
                for off, args in call_args(text[d[3]:d[4]], "receipt_decide"):
                    given = tuple(_fold(a) for a in args[1:])
                    if given != tuple(T["receipt"]):
                        problems.append(where(off) +
                                        f" hands receipt_decide the grants {given!r} — they "
                                        f"are exactly {tuple(T['receipt'])!r}: the bearer "
                                        f"grant takes a real bearer, the tap is the tap")
                bind = _RECEIPT_BIND.search(code)
                for off, what in sites:
                    if not (bind and _under_serve_test(code, off, bind.group(1))):
                        problems.append(where(off) +
                                        f" reaches a credential ({what}) outside a SERVE_* "
                                        f"test of its receipt_decide verdict")
            else:
                for off, what in sites:
                    if what != "send_html_with_token" and what not in pages:
                        problems.append(where(off) +
                                        f" reaches a credential through {what}, not the page "
                                        f"path (send_html_with_token(..., "
                                        f"page_token_inject(req)) or another page handler)")
    problems.extend(_answer_statement_problems(sources, T, defs, reads))
    return problems


def _answer_statement_problems(sources, T, defs, reads) -> list:
    """Each SECRET_ANSWERS handler's own reading statements are exactly its
    ANSWER_STATEMENTS row (keyed by its home file), in every body of it."""
    problems = []
    rows = T["answer_stmts"]
    for h in sorted(T["answers"]):
        homes = [(f, q) for f, q in rows if q.split("::")[-1] == h]
        if len(homes) != 1:
            problems.append(f"SECRET_ANSWERS names {h}(), which has {len(homes)} "
                            f"ANSWER_STATEMENTS rows; it takes one, keyed by its home file")
            continue
        suffix, qual = homes[0]
        for key in sorted(k for k in defs if k[1] == qual):
            if not _in_file(key[0], suffix):
                problems.append(f"ANSWER_STATEMENTS: {qual}() is defined in {key[0]}; its row "
                                f"says {suffix} — remove or fix the row")
                continue
            for d in defs[key]:
                problems.extend(_pin_problems(sources, T, key, d, _positions(reads, key, d),
                                              rows[homes[0]], "ANSWER_STATEMENTS"))
    for f, q in sorted(rows):
        if q.split("::")[-1] not in T["answers"]:
            problems.append(f"ANSWER_STATEMENTS names {q}() ({f}), which is no SECRET_ANSWERS "
                            f"key — remove or fix the row")
    return problems


def mint_order_problems(sources: dict, rows) -> list:
    """MINT_AFTER: every call of a mint follows, in its function, the anchor
    statement that establishes the key it reads, at the function's own level
    (whole, through its refusal, with nothing around it and no goto before
    it), and nothing before the anchor in its function can leave (_leaves: an
    early `return true;` skips the key and still answers success)."""
    problems = []
    defs, _by_name = _function_index(sources)
    for suffix, qual, mint, anchor in rows:
        keys = [k for k in defs if _in_file(k[0], suffix) and k[1] == qual]
        if not keys:
            problems.append(f"MINT_AFTER names {qual}() in {suffix}, which does not define it — "
                            f"remove or fix the row")
            continue
        call = next(n for n in _ANY_CALL.findall(anchor) if n not in _KEYWORDS)
        for key in keys:
            text = sources[key[0]]
            blanked = blank_string_contents(text)
            for d in defs[key]:
                ends, starts = [], []
                for m in re.finditer(r"\b" + re.escape(call) + r"\s*\(", blanked[d[3]:d[4]]):
                    s, e, _lv = _statement_at(blanked, d[3], d[4], d[3] + m.start(),
                                              whole_if=True)
                    if _placed(text, blanked, d, s, e) == anchor:
                        ends.append(e)
                        starts.append(s)
                mints = [d[3] + m.start() for m in
                         re.finditer(r"\b" + re.escape(mint) + r"\s*\(", blanked[d[3]:d[4]])]
                if not mints:
                    problems.append(f"MINT_AFTER: {qual}() ({key[0]}) no longer calls {mint}() — "
                                    f"remove or fix the row")
                if not ends:
                    problems.append(f"{key[0]}:{_line_of(text, d[1])}: {qual}() no longer holds "
                                    f"`{anchor}` at its own level (MINT_AFTER): {mint}() would "
                                    f"read a key nothing established, all zeros")
                    continue
                for at, stmt in _leaves(text, blanked, d, d[3], min(starts)):
                    problems.append(f"{key[0]}:{_line_of(text, at)}: {qual}() can leave before "
                                    f"`{anchor}` (MINT_AFTER): {stmt!r} — a way out there skips "
                                    f"the statement that establishes the key, and a caller "
                                    f"that reads success mints from an all-zero private key, "
                                    f"the same credential on every Canary")
                for pos in mints:
                    if pos < min(ends):
                        problems.append(f"{key[0]}:{_line_of(text, pos)}: {qual}() calls {mint}() "
                                        f"before `{anchor}` (MINT_AFTER): the key it reads is "
                                        f"all zeros until then, and the credential it mints "
                                        f"would be the same on every Canary")
    return problems


def _string_literals(text: str) -> list:
    """[(start, end, value)] for every string literal in comment-stripped
    `text`, a raw one included (its value as written), an ordinary one's
    escapes decoded; adjacent literals (only space between) are joined into
    one, as the compiler joins them."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            end = raw_literal_end(text, i)
            if end > 0:
                open_ = text.index("(", i)
                value = text[open_ + 1:end - (open_ - i) - 1]
            else:
                end = i + 1
                while end < n and text[end] != '"':
                    end += 2 if text[end] == "\\" else 1
                end += 1
                value = text[i + 1:end - 1]
                if "\\" in value:
                    try:
                        value = value.encode("latin-1", "replace").decode("unicode_escape")
                    except UnicodeDecodeError:
                        pass
            if out and not text[out[-1][1]:i].strip():
                out[-1] = (out[-1][0], end, out[-1][2] + value)
            else:
                out.append((i, end, value))
            i = end
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
        else:
            i += 1
    return out


def nvs_literal_problems(sources: dict, keys: dict) -> list:
    """No string literal under ROOTS is the value of an NVS_LITERAL_KEYS
    record (`keys`: name → its include/canary_config.h value). Every file's
    literals are read: a joined or escaped one (`"setup_" "ok"`,
    `"setup\\x5fok"`) holds no copy of the value in a file that names none."""
    problems = []
    names = {v: k for k, v in keys.items()}
    for fname, text in sorted(sources.items()):
        if not names or '"' not in text:
            continue
        for start, _end, value in _string_literals(text):
            if value in names:
                problems.append(f"{fname}:{_line_of(text, start)}: the string literal {value!r} "
                                f"is {names[value]}'s value — that NVS record is read and "
                                f"written by its name only, where the reads and the census "
                                f"follow it (the bearer, the TLS key, the first-boot flag or "
                                f"the private key under a name no rule reads)")
    return problems


def glue_def_problems(sources: dict, glue: dict) -> list:
    """Every GLUE_DEFS row: its function defined exactly once under ROOTS, in
    its file (a second definition anywhere, or a shadow in another lib,
    fails), its body exactly the row's."""
    problems = []
    for (suffix, qual), (why, want) in sorted(glue.items()):
        found = [(f, sp) for f, t in sorted(sources.items()) for sp in definition_spans(t, qual)]
        if len(found) != 1 or not _in_file(found[0][0], suffix):
            where = ", ".join(f for f, _sp in found) or "nowhere"
            problems.append(f"GLUE_DEFS pins {qual}() in {suffix}, which ROOTS defines "
                            f"{_times(len(found))} ({where}): pinned glue is defined once, in "
                            f"its file (a second body, or a shadow its callers there would "
                            f"reach, is not the pinned one) — remove or fix the row")
            continue
        fname, (a, b) = found[0]
        got = _fold(sources[fname][a:b])
        if got != want:
            problems.append(f"{fname}: {qual} changed — {why}. If the change is meant, update "
                            f"its GLUE_DEFS row in this script in the same review. Now: {got}")
    return problems


# A census name mentioned outside every function after one of these words is
# a statement in a body the head parser did not read (a namespace-scope
# lambda's `return ...`), never a declaration; and so is one whose statement
# has an `=`, a `(` or one of those words before it (`static AuthManager*
# const v = &auth_get_instance();`, a default argument): a declaration names
# its function after a type and nothing else.
_STATEMENT_WORD = re.compile(r"\b(?:return|throw|co_return|case|else|do|new|delete)\s*$")
_USE_BEFORE = re.compile(r"[=(]|\b(?:return|throw|co_return)\b")


def _is_row_def(qual: str, name: str) -> bool:
    """True when a definition named `qual` is census row `name`'s own: that
    member exactly for a qualified row (`ScvNetworkManager::begin`), else a
    definition by the bare name."""
    return qual == name if "::" in name else qual.split("::")[-1] == name


def _census_home_problems(name: str, row: dict, defs_of: dict) -> list:
    """A census primitive is defined exactly where its row's home says."""
    if "home" not in row:
        return [f"GATE_CALL_SITES row {name} names no home: say where it is defined (None: "
                f"outside ROOTS)"]
    home = row["home"]
    found = sorted((f, d[0]) for f, ds in defs_of.items()
                   if not row.get("file") or _in_file(f, row["file"])
                   for d in ds if _is_row_def(d[0], name))
    if home is None and not found:
        return []
    if (home is not None and len(found) == 1 and _in_file(found[0][0], home[0])
            and found[0][1] == home[1]):
        return []
    where = ", ".join(f"{q} ({f})" for f, q in found) or "nowhere"
    want = ("outside ROOTS only" if home is None else f"once, as {home[1]} in {home[0]}")
    return [f"GATE_CALL_SITES row {name}: ROOTS defines it {_times(len(found))} ({where}); it is "
            f"defined {want} (a second definition, or a shadow in another lib, is the "
            f"primitive its callers there reach)"]


def _top_problems(text: str, blanked: str, d, start: int, level: int, where: str,
                  name: str) -> list:
    """A `top` census call: at its function's own level, outside every block
    and preprocessor conditional opened in it, with nothing before it in the
    function able to leave but RATE_LIMIT_REFUSAL."""
    problems = []
    around = _context(text, blanked, d[3], start)
    if level != 1 or around:
        problems.append(f"{where}: {d[0]} calls {name} other than at its own top level "
                        f"(within {' > '.join(around)}): the refusal runs on every request, "
                        f"never under a block or a conditional")
    for x in _LEAVE.finditer(blanked, d[3], start):
        s, e, lv = _statement_at(blanked, d[3], d[4], x.start())
        stmt = _fold(text[s:e])
        if lv != 1 or not RATE_LIMIT_REFUSAL.fullmatch(stmt):
            problems.append(f"{where}: {d[0]} can leave before its {name} call: {stmt!r} — "
                            f"only the rate limit's refusal may come first (an early return "
                            f"or a goto skips the gate)")
    return problems


def gate_census_problems(sources: dict, census: dict, markers, steps, emitters) -> list:
    """GATE_CALL_SITES: every mention of a gate primitive is where its row
    says (a call, or its address taken: `&auth_get_token`, a function
    pointer); every primitive is defined where its row says; every
    AUTH_MARKERS name is a row, and every PRE_HOST_STEPS name a row, an
    emitter or auth_get_token."""
    problems = []
    for marker in markers:
        if marker.rstrip("(") not in census:
            problems.append(f"AUTH_MARKERS entry {marker!r} names no GATE_CALL_SITES row — a "
                            f"gate marker is a census primitive (a deleted gate's name "
                            f"cannot linger)")
    for step in steps:
        name = step.rstrip("(")
        if name not in census and name not in emitters and name != "auth_get_token":
            problems.append(f"PRE_HOST_STEPS entry {step!r} names no GATE_CALL_SITES row, "
                            f"TOKEN_EMITTERS key or auth_get_token")
    defs_of = {f: function_defs(t) for f, t in sources.items()}
    for name, row in sorted(census.items()):
        rx = _census_rx(name, row)
        callers = row.get("callers")
        if not row.get("ident"):
            problems.extend(_census_home_problems(name, row, defs_of))
        seen, count, per, held = set(), 0, {}, {}
        for fname, text in sorted(sources.items()):
            if row.get("file") and not _in_file(fname, row["file"]):
                continue
            blanked = blank_string_contents(text)
            for m in rx.finditer(blanked):
                d = _innermost(defs_of[fname], m.start())
                if d is not None and d[1] <= m.start() <= d[2] and _is_row_def(d[0], name):
                    continue  # the primitive's own definition
                if "bare" in row and "bare" in rx.groupindex and m.group("bare") and not (
                        d is not None and m.start() >= d[3]
                        and (d[0].startswith(row["bare"][0]) or _in_file(fname, row["bare"][1]))):
                    continue  # another class's begin(), or a declaration
                line = _line_of(text, m.start())
                if d is None or m.start() < d[3]:
                    ostart, _oend, stmt = _outside_statement(text, blanked, m.start())
                    before = blanked[max(0, m.start() - 40):m.start()]
                    if row.get("ident"):
                        if not row.get("decl"):
                            problems.append(f"{fname}:{line}: {name} named outside every "
                                            f"function: {stmt!r} — its GATE_CALL_SITES row "
                                            f"allows it nowhere")
                        elif stmt not in row["decl"]:
                            problems.append(f"{fname}:{line}: {name} named outside every "
                                            f"function other than in its declaration: {stmt!r}")
                    elif (not re.search(r"[\w*&>]\s*$", before)
                          or _STATEMENT_WORD.search(before)
                          or _USE_BEFORE.search(blanked, ostart, m.start())):
                        problems.append(f"{fname}:{line}: {name}() called outside every "
                                        f"function: {stmt!r}")
                    continue
                count += 1
                key = next(((f, q) for f, q in (callers or ())
                            if _in_file(fname, f) and q == d[0]), None)
                if callers is not None and key is None:
                    problems.append(
                        f"{fname}:{line}: {name} named in {d[0]}: its GATE_CALL_SITES row "
                        f"allows it in no function" if not callers else
                        f"{fname}:{line}: {name} called from {d[0]}, which its "
                        f"GATE_CALL_SITES row does not name — a gate primitive is "
                        f"called only where the census says")
                    continue
                seen.add(key)
                per[key] = per.get(key, 0) + 1
                whole = row.get("whole")
                if isinstance(whole, dict):
                    whole = whole.get(key)
                start, end, lv = _statement_at(blanked, d[3], d[4], m.start(),
                                               whole_if=bool(row.get("whole_if")))
                placed = _placed(text, blanked, d, start, end)
                if whole and placed != whole:
                    problems.append(f"{fname}:{line}: {d[0]} calls {name} other than as "
                                    f"the whole statement `{whole}`: {placed!r}")
                if key in row.get("stmts", {}):
                    held.setdefault(key, {})[start] = placed
                if row.get("top"):
                    # Every definition whose body holds the call: a head split
                    # across #if / #else puts the refusal in the outer one's
                    # conditional, which the innermost head does not show.
                    for dd in defs_of[fname]:
                        if dd[3] <= m.start() < dd[4]:
                            s2, _e2, lv2 = _statement_at(blanked, dd[3], dd[4], m.start())
                            problems.extend(_top_problems(text, blanked, dd, s2, lv2,
                                                          f"{fname}:{line}", name))
                if row.get("lead"):
                    for _s, stmt in _leaves(text, blanked, d, d[3], start):
                        problems.append(f"{fname}:{line}: {d[0]} can leave before its {name} "
                                        f"call: {stmt!r} — nothing above the call may leave "
                                        f"{d[0]}, or a pass that leaves there skips it")
                if key in row.get("served", ()):
                    code = blanked[d[3]:d[4]]
                    bind = _RECEIPT_BIND.search(code)
                    if not (bind and _under_serve_test(code, m.start() - d[3], bind.group(1))):
                        problems.append(f"{fname}:{line}: {d[0]} calls {name} outside a "
                                        f"SERVE_* test of its receipt_decide verdict — only "
                                        f"the tap that served hands the asker the token")
        if count == 0 and not row.get("none"):
            problems.append(f"GATE_CALL_SITES row {name} has no call under ROOTS — remove or "
                            f"fix the row")
        if row.get("once"):
            if callers is None and count > 1:
                problems.append(f"{name} is called {count} times; its GATE_CALL_SITES row says "
                                f"exactly once")
            for k, n in sorted((k, n) for k, n in per.items() if k is not None and n > 1):
                problems.append(f"{name} is called {n} times by {k[1]}; its GATE_CALL_SITES row "
                                f"says exactly once")
        for k, want in sorted(row.get("stmts", {}).items()):
            got = tuple(f for _s, f in sorted(held.get(k, {}).items()))
            if got != tuple(want):
                problems.append(f"GATE_CALL_SITES row {name}: {k[1]}'s statements that name it "
                                f"are not its row ({k[0]}). If the change is meant, update the "
                                f"row in the same review. Now: {got!r}")
        for f, q in sorted((callers or set()) - seen):
            problems.append(f"GATE_CALL_SITES row {name} names {q} ({f}), which no longer "
                            f"calls it — remove it")
    return problems + _keyed_problems(sources, census, defs_of)


def _census_rx(name: str, row: dict):
    """A census row's spelling: its pattern, else every mention of its name
    as a whole word (its qualifier's `::` spaced freely), so taking its
    address is a mention like calling it."""
    return re.compile(row.get("pattern") or
                      r"\b" + r"\s*::\s*".join(map(re.escape, name.split("::"))) + r"\b")


# The names a reference or pointer to the network manager may take, so
# begin()'s census row (which spells its calls by these names) sees every
# start of the network: `ScvNetworkManager& mgr = ...; mgr.begin(...)` fails
# here instead of passing there.
MANAGER_NAMES = ("net", "network")
# Its accessor: the one function that returns it.
MANAGER_ACCESSOR = "network_get_instance"
# A reference, a pointer or a function declared with the manager's type
# (`ScvNetworkManager& m(...)`, `ScvNetworkManager* p`, `ScvNetworkManager& mgr()`).
_MANAGER_TYPED = re.compile(
    r"\bScvNetworkManager\s*(?:const\s*)?[&*]+\s*(?:const\s+)?"
    r"((?:[A-Za-z_]\w*\s*::\s*)*~?[A-Za-z_]\w*)")
# A declaration whose type is deduced (`auto`, `auto&`, `auto*`, `decltype(...)`),
# the name it declares and how its initializer opens.
_DEDUCED = re.compile(
    r"\b(?:auto|decltype\s*\([^;{}]*?\))\s*(?:const\s*)?[&*]*\s*(?:const\s+)?"
    r"([A-Za-z_]\w*)\s*(=|\(|\{)")
# The manager named in an expression (never `canary::net::`, a namespace).
_MANAGER_WORD = r"(?<!::)(?<!:: )\b(?:network_get_instance|s_network|net|network)\b(?!\s*::)"
# The manager handed on as a value (an argument, an assignment's right side, a
# lambda's init-capture, a return), not used through a member: `f(net)`,
# `x = &network;`, `[&m = net]`, `return net;`, and in the network lib `this`
# / `*this` the same way.
# (Each optional token owns the space after it, so a long run of blanks, a
# raw literal's, is read once.)
_MANAGER_VALUE = re.compile(
    r"(?:[(,={]|\breturn\b)\s*(?:[&*]\s*)?(?:\(\s*)?(" + _MANAGER_WORD[:-len(r"(?!\s*::)")]
    + r"|(?:\*\s*)?\bthis)\s*(?:\)\s*)?(?=[,;)}\]])")
# The files whose `this` is the manager's.
_MANAGER_FILES = (_NET, _NET_H)


def manager_alias_problems(sources: dict) -> list:
    """Every way to the network manager is one begin()'s census row reads:
    a reference or pointer to it, a local, a parameter or a member, is named
    one of MANAGER_NAMES and so is a deduced (`auto`, `decltype`) one; no
    other function returns it; it is never handed on as a value (an argument,
    an assignment, a return) but through its accessor; its accessor is named
    only in its definition and declarations, as `ScvNetworkManager& net =
    network_get_instance();` (or `network`), or with a member after it; and
    the radio itself, `WiFi`, is named only with a member after it."""
    problems = []

    def bad(fname, text, at, what):
        problems.append(f"{fname}:{_line_of(text, at)}: {what} — a reference to the network "
                        f"manager is named {' or '.join(MANAGER_NAMES)} and reached only "
                        f"through {MANAGER_ACCESSOR}(), so begin()'s census row sees every start "
                        f"of the network (a second start handed \"\" raises the SoftAP open)")

    for fname, text in sorted(sources.items()):
        blanked = blank_string_contents(text)
        defs = function_defs(text)
        mine = _in_file(fname, _MANAGER_FILES)
        for m in _MANAGER_TYPED.finditer(blanked):
            name = re.sub(r"\s+", "", m.group(1))
            if name.split("::")[-1].startswith("operator"):
                continue  # an assignment operator's reference (its `return *this;` is read below)
            if name not in MANAGER_NAMES and name != MANAGER_ACCESSOR:
                bad(fname, text, m.start(), f"names the network manager {name!r}")
        for m in _DEDUCED.finditer(blanked):
            if m.group(1) in MANAGER_NAMES:
                continue
            open_ = m.end() - 1
            if m.group(2) == "=":
                end = blanked.find(";", open_)
                init = blanked[open_ + 1:end if end >= 0 else len(blanked)]
            else:
                close = _bracket_close(blanked, open_)
                init = blanked[open_ + 1:close if close >= 0 else len(blanked)]
                if m.group(2) == "(" and not re.fullmatch(
                        r"\s*[&*]?\s*(?:" + _MANAGER_WORD + r"\s*(?:\(\s*\))?|\*?\s*this)\s*",
                        init):
                    continue  # a parameter list, or a value that is not the manager
            if re.search(_MANAGER_WORD, init) or (mine and re.search(r"\bthis\b", init)):
                bad(fname, text, m.start(), f"names the network manager {m.group(1)!r} "
                                            f"(a deduced type)")
        for m in _MANAGER_VALUE.finditer(blanked):
            word = m.group(1)
            if "this" in word and not mine:
                continue
            d = _innermost(defs, m.start(1))
            if word.strip() == "s_network" and d is not None and d[0] == MANAGER_ACCESSOR:
                continue  # the accessor returning it
            if word.strip() == MANAGER_ACCESSOR:
                continue  # its accessor's own rule, below
            bad(fname, text, m.start(1), f"hands the network manager on as a value "
                                         f"({_fold(text[m.start():m.end()])!r})")
        for m in re.finditer(r"\b" + MANAGER_ACCESSOR + r"\b", blanked):
            d = _innermost(defs, m.start())
            if d is not None and d[2] == m.start():
                continue  # its definition
            after = re.match(r"\s*\(\s*\)\s*(?:\.|->)\s*[A-Za-z_]", blanked[m.end():])
            if after:
                continue  # a member of the manager
            if d is None or m.start() < d[3]:
                ostart, _oend, stmt = _outside_statement(text, blanked, m.start())
                if (re.search(r"[\w*&>]\s*$", blanked[max(0, m.start() - 40):m.start()])
                        and not _USE_BEFORE.search(blanked, ostart, m.start())):
                    continue  # a declaration
            s, e, _lv = (_statement_at(blanked, d[3], d[4], m.start()) if d is not None
                         and m.start() >= d[3] else (m.start(), m.end(), 0))
            if _fold(text[s:e]) in tuple(f"ScvNetworkManager& {n} = {MANAGER_ACCESSOR}();"
                                         for n in MANAGER_NAMES):
                continue
            bad(fname, text, m.start(), f"names {MANAGER_ACCESSOR}() other than in a binding "
                                        f"named {' or '.join(MANAGER_NAMES)} or with a member "
                                        f"after it ({_fold(text[s:e])[:80]!r})")
        for m in re.finditer(r"\bWiFi\b(?!\s*\.)", blanked):
            problems.append(f"{fname}:{_line_of(text, m.start())}: names WiFi other than with a "
                            f"member after it — the radio's mode and its SoftAP are read by "
                            f"their spelling on WiFi (the census rows WiFi.mode, softAP and "
                            f"WiFi.AP), so a reference to it under another name raises the "
                            f"SoftAP where no row reads")
    return problems


def no_session_problems(sources: dict, headers: dict, queries: set, uri_readers) -> list:
    """The premise: no cookie, request headers only REQUEST_HEADERS', no
    `Set-Cookie`, a URL query only in URL_QUERY_READERS, a request's raw path
    only in URI_READERS, and every row of those two still a reader."""
    problems = []
    used = set()
    for fname, text in sorted(sources.items()):
        blanked = blank_string_contents(text)
        defs = function_defs(text)
        allowed = next((names for suffix, names in headers.items() if _in_file(fname, suffix)),
                       ())
        for m in re.finditer(r"\bhttpd_req_get_cookie_val\s*\(", blanked):
            problems.append(f"{fname}:{_line_of(text, m.start())}: reads a cookie "
                            f"(httpd_req_get_cookie_val) — {_NO_SESSION}")
        for fn in ("httpd_req_get_hdr_value_len", "httpd_req_get_hdr_value_str"):
            for off, args in call_args(text, fn):
                hdr = args[1] if len(args) > 1 else ""
                lit = re.fullmatch(r'"([^"\\]*)"', hdr)
                if not lit or lit.group(1) not in allowed:
                    problems.append(f"{fname}:{_line_of(text, off)}: reads the request "
                                    f"header {hdr!r}, a header not in REQUEST_HEADERS for this "
                                    f"file ({', '.join(allowed) or 'none'}) — {_NO_SESSION}; a "
                                    f"lockout keyed on a request header (X-Forwarded-For) "
                                    f"is the client's to write")
        for off, args in call_args(text, "httpd_resp_set_hdr"):
            hdr = args[1] if len(args) > 1 else ""
            lit = re.fullmatch(r'"([^"\\]*)"', hdr)
            if not lit or lit.group(1).lower() == "set-cookie":
                problems.append(f"{fname}:{_line_of(text, off)}: sets the response header "
                                f"{hdr!r} (a header name is a literal, never Set-Cookie) — "
                                f"{_NO_SESSION}")
        for rx, allow, what, table in (
                (_URL_QUERY, queries, "the URL query", "URL_QUERY_READERS"),
                (_URI_READ, uri_readers, "a request's raw path (its uri, which keeps its query)",
                 "URI_READERS")):
            for m in rx.finditer(blanked):
                d = _innermost(defs, m.start())
                row = next(((f, q) for f, q in allow if d and _in_file(fname, f) and q == d[0]),
                           None)
                used.add((table, row))
                if row is None:
                    who = d[0] if d else "code outside every function"
                    how = f" ({m.group(1)})" if m.groups() else ""
                    problems.append(f"{fname}:{_line_of(text, m.start())}: {who} reads "
                                    f"{what}{how}, outside {table} — {_NO_SESSION}")
    for allow, table in ((queries, "URL_QUERY_READERS"), (uri_readers, "URI_READERS")):
        for f, q in sorted(allow):
            if (table, (f, q)) not in used:
                problems.append(f"{table} names {q} ({f}), which no longer reads it — remove "
                                f"or fix the row")
    return problems


# The build's flag files (platformio.ini and the files its extra_configs
# names): a `-D` flag is a `#define` too. PlatformIO's own reader
# (platformio_lib_deps) fails a file it reads that flag_sources does not.
FLAG_FILES = (Path("firmware/canary/platformio.ini"),)
# The folders outside ROOTS the flagship builds from (platformio.ini's
# -I${PROJECT_DIR}/include, -I${PROJECT_DIR}/../common and its subfolders, and
# build_src_filter's ../../common entries). What the check reads of them is what
# the flagship reaches (build_reach): each .c/.cpp build_src_filter compiles, and
# each file an #include reaches from ROOTS or from those, resolved as the
# compiler resolves it. A `#define` there, in a header #included after a pinned
# function, shadows it as one under ROOTS does; a file there no flagship source
# reaches (another product's source, a host test) is not read.
PROJECT_DIR = Path("firmware/canary")
INCLUDE_ROOTS = (Path("firmware/canary/include"), Path("firmware/common"))
# The defines those headers hold on purpose that name what the check holds:
# the NVS records' names (NVS_LITERAL_KEYS), each in canary_config.h.
INCLUDE_DEFINES = {name: "include/canary_config.h" for name in NVS_LITERAL_KEYS}
# What the files outside ROOTS the flagship reaches (build_reach) may name of
# what the check holds under ROOTS (INCLUDE_MENTIONS), as (file suffix, name):
# the NVS records' names where canary_config.h defines them and
# reset_scope_keys.h lists them for the reset, the SoftAP key's deriver where
# ap_password.h defines it, and the two Host-first decisions where
# provisioning_gate.h defines them. Code in those files is not read as code (a
# function there that read the bearer, opened the tap, started the network or
# raised the SoftAP, #included or linked and called from under ROOTS, would be
# followed by no rule), so a file naming anything else the check holds fails:
# a credential's spelling (SECRET_READS), a name the tables hold under ROOTS,
# or a census row's spelling (WiFi.softAP, the radio's mode, the driver's
# config, the AP class, the tap's open, another HTTP server).
INCLUDE_MENTIONS = (
    {("include/canary_config.h", n) for n in NVS_LITERAL_KEYS}
    | {("include/reset_scope_keys.h", n) for n in NVS_LITERAL_KEYS}
    | {("network/ap_password.h", "derive_ap_password")}
    | {("network/provisioning_gate.h", n) for n in HOST_DECISIONS})
# ...but the held names that are ordinary words in the shared code (a BLE
# server's begin(), an Improv state's s_active, a policy's check()): the
# spelling of a held name that the shared code also uses for its own.
INCLUDE_COMMON_WORDS = frozenset({"begin", "check", "grant", "compare", "setup", "s_active",
                                  "s_started_ms", "hmac_sha256", "from_ap_subnet"})
# The C++ keywords and literals. A macro of one rewrites every pinned body after
# it with every text intact (`#define false true` below the glue makes
# from_ap_subnet's `return false;` a `return true;`), and GCC takes it without a
# word: none may be defined, under ROOTS, in a header or by a flag.
_CXX_KEYWORDS = frozenset(
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t "
    "char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
    "continue co_await co_return co_yield decltype default delete do double dynamic_cast else "
    "enum explicit export extern false float for friend goto if inline int long mutable "
    "namespace new noexcept not not_eq nullptr operator or or_eq private protected public "
    "register reinterpret_cast requires return short signed sizeof static static_assert "
    "static_cast struct switch template this thread_local throw true try typedef typeid "
    "typename union unsigned using virtual void volatile wchar_t while xor xor_eq".split())
# The macros under ROOTS named for what a pinned text reads (any identifier in a
# GLUE_DEFS body, a pinned statement or a census row's held statement), each
# defined in its file this many times (its #if and #else spellings): the
# feature probes and the buffer size the glue reads. Any other #define under
# ROOTS of such a name, and any #undef of one, is a shadow of what the glue
# reads with its text intact (an `#undef PROVISIONING_GATE_TTL_MS` and a wider
# one in main.cpp, a BOOT press threshold of 0); PINNED_DEFINES' are held to
# their values too. (A header's value outside ROOTS is not read as a number.)
ROOTS_DEFINES = {
    "CANARY_SOFTAP_SAE_IN_BUILD": (_NET, 2),
    "SECURACV_HAS_HTTPS_SERVER": (_NET, 2),
    "SECURACV_AUTH_HEADER_BUF": ("securacv_auth.h", 1),
}
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", re.M)
# GNU's comma elision (`, ##__VA_ARGS__`, the CSI logging macros' form): the
# one `##` that pastes nothing. Every other `##` builds a token from pieces, so
# a name pasted together (`a##b` handed `auth_get_` and `token`) is a call no
# rule reads by its spelling.
_COMMA_ELISION = re.compile(r",\s*##\s*__VA_ARGS__\b")
_DEFINE_NAME = re.compile(r"\s*#\s*define\s+([A-Za-z_]\w*)")
_UNDEF = re.compile(r"^[ \t]*#[ \t]*undef\s+([A-Za-z_]\w*)", re.M)
_FLAG = re.compile(r"(?<![\w-])-D\s*(?:'([^']*)'|\"([^\"]*)\"|(\S+))")


def flag_sources(files=FLAG_FILES) -> tuple:
    """({path: text}, problems): the flag files and every file their
    extra_configs names (on its key's line or the indented lines after it,
    split on commas and newlines, a glob expanded as PlatformIO does), each
    text with its comment lines blanked so a line number is the file's. A
    named file that is missing is a problem: a flag in it would not be read."""
    out, problems, todo = {}, [], [(Path(f), None) for f in files]
    while todo:
        path, named_by = todo.pop(0)
        path = Path(os.path.normpath(path))
        if path.as_posix() in out:
            continue
        if not path.is_file():
            problems.append(f"{path.as_posix()}: a flag file the build reads"
                            f"{' (extra_configs in ' + named_by + ')' if named_by else ''} is "
                            f"missing, so its -D flags cannot be read — fix the path or the "
                            f"check's FLAG_FILES")
            continue
        text = "\n".join("" if line.lstrip().startswith((";", "#")) else line
                         for line in path.read_text(errors="replace").splitlines())
        out[path.as_posix()] = text
        for m in re.finditer(r"^extra_configs\s*=([^\n]*)\n?((?:(?:[ \t]+[^\n]*)?\n)*)", text,
                             re.M):
            for item in re.split(r"[,\n]", m.group(1) + "\n" + m.group(2)):
                item = item.split(";")[0].strip()
                if not item:
                    continue
                if any(c in item for c in "*?["):
                    hits = sorted(path.parent.glob(item))
                    if not hits:
                        problems.append(f"{path.as_posix()}: extra_configs names {item!r}, which "
                                        f"matches no file")
                    todo.extend((h, path.as_posix()) for h in hits)
                else:
                    todo.append((path.parent / item, path.as_posix()))
    return out, problems


# An #include as the compiler reads it: a quoted name, an angled one, or
# anything else (a macro, which names a file the check cannot follow).
_INCLUDE = re.compile(r'^[ \t]*#[ \t]*(include|include_next|import)\b[ \t]*'
                      r'(?:"([^"\n]*)"|<([^>\n]*)>|([^\n]*))', re.M)
_INCLUDE_DIR = re.compile(r"(?<![\w-])-I\s*(?:'([^']*)'|\"([^\"]*)\"|(\S+))")
_SRC_FILTER = re.compile(r"\+<([^>\n]*)>")


def _under(path: Path, roots) -> bool:
    return any(path == r or r in path.parents for r in roots)


def _inside(path: Path, repo: Path) -> bool:
    """True when `path` is in the repository at `repo` (not a toolchain's or a
    package's header)."""
    try:
        return ".." not in Path(os.path.relpath(path, repo)).parts
    except ValueError:
        return False


def build_reach(sources: dict, flags: dict, project=PROJECT_DIR, roots=INCLUDE_ROOTS,
                tree=ROOTS, repo=Path(".")) -> tuple:
    """({path: comment-stripped text}, problems): every file outside ROOTS
    the flagship compiles or #includes. Compiled: each build_src_filter
    `+<...>` entry in the flag files (`flags`, flag_sources()), relative to
    src/ (a folder is every file in it, a glob as PlatformIO expands it);
    reached: each file an #include names from ROOTS (`sources`) or from a
    reached file, looked up beside the including file (a quoted name) and in
    include/ and every -I folder the flag files name, every candidate that
    exists, so a name two folders hold is both. Only the files under the
    include roots are returned; a compiled file that is not C or C++, a
    filter that matches nothing, an #include the check cannot follow (a
    macro) and one that reaches a file of this repository outside ROOTS
    (`tree`) and the include roots are problems: code the build reads that
    no rule would."""
    problems, dirs = [], [project / "include"]
    for fname, text in sorted(flags.items()):
        for m in _INCLUDE_DIR.finditer(text):
            raw = next(g for g in m.groups() if g is not None)
            if raw.startswith("${PROJECT_DIR}"):
                d = project / raw[len("${PROJECT_DIR}"):].lstrip("/")
            elif "${" in raw or os.path.isabs(raw):
                continue  # a package's folder (the core, a library): not this repository
            else:
                d = project / raw
            d = Path(os.path.normpath(d))
            if d not in dirs:
                dirs.append(d)
    reached, queue = {}, list(sources)

    def take(path: Path, why: str):
        key = path.as_posix()
        if _under(path, tree) or key in reached:
            return
        if not _under(path, roots):
            problems.append(f"{why} {key}, outside ROOTS and the include roots, which the "
                            f"check does not read — keep the flagship's code where the check "
                            f"reads it, or teach the check the new folder")
            return
        reached[key] = strip_comments(path.read_text(errors="replace"))
        queue.append(key)
    for fname, text in sorted(flags.items()):
        for m in _SRC_FILTER.finditer(text):
            entry = m.group(1).strip()
            target = Path(os.path.normpath(project / "src" / entry))
            hits = (sorted(p for p in target.rglob("*") if p.is_file()) if target.is_dir()
                    else sorted(Path(h) for h in glob.glob(target.as_posix(), recursive=True)
                                if Path(h).is_file()))
            if not hits:
                problems.append(f"{fname}:{_line_of(text, m.start())}: build_src_filter's "
                                f"+<{entry}> matches no file — fix the entry (the check reads "
                                f"what the build compiles from it)")
            for h in hits:
                if _under(h, tree):
                    continue
                if h.suffix.lower() not in SOURCE_SUFFIXES:
                    problems.append(f"{fname}:{_line_of(text, m.start())}: build_src_filter "
                                    f"compiles {h.as_posix()}, which is not C or C++ the check "
                                    f"reads (an assembly file reaches a function by its symbol, "
                                    f"a spelling no rule reads)")
                    continue
                take(h, f"{fname}:{_line_of(text, m.start())}: build_src_filter compiles")
    while queue:
        fname = queue.pop(0)
        text = sources.get(fname, reached.get(fname, ""))
        for m in _INCLUDE.finditer(text):
            quoted, angled, other = m.group(2), m.group(3), m.group(4)
            if quoted is None and angled is None:
                problems.append(f"{fname}:{_line_of(text, m.start())}: #{m.group(1)} "
                                f"{other.strip()!r} names its file through a macro, which the "
                                f"check cannot follow — #include a quoted or angled name")
                continue
            name = quoted if quoted is not None else angled
            here = [Path(fname).parent] if quoted is not None else []
            for d in here + dirs:
                cand = Path(os.path.normpath(d / name))
                if cand.is_file() and _inside(cand, repo):
                    take(cand, f"{fname}:{_line_of(text, m.start())}: #includes")
    return reached, problems


def _pinned_texts(T: dict) -> list:
    """Every text the check pins: GLUE_DEFS' bodies, the SECRET_LOCAL_USES,
    EMITTER_STATEMENTS and ANSWER_STATEMENTS statements, the census rows'
    held statements and declarations, and MINT_AFTER's anchors."""
    texts = [want for _why, want in T["glue"].values()]
    texts += [s for _why, stmts in T["local"].values() for s in stmts]
    texts += [s for stmts in T["emitter_stmts"].values() for s in stmts]
    texts += [s for stmts in T["answer_stmts"].values() for s in stmts]
    for row in T["census"].values():
        whole = row.get("whole")
        texts += ([whole] if isinstance(whole, str) else list((whole or {}).values()))
        texts += [s for stmts in row.get("stmts", {}).values() for s in stmts]
        texts += list(row.get("decl", ()))
    texts += [anchor for *_where, anchor in T["mint_after"]]
    return [blank_string_contents(x) for x in texts]


def _called_names(T: dict) -> set:
    """Every function a pinned text calls (_pinned_texts): a macro by one of
    those names (`#define take(s, n, t) is_open(s, n, t)` turns the take hook
    into a peek) leaves every pinned text intact."""
    return {n for x in _pinned_texts(T) for n in _ANY_CALL.findall(x) if n not in _KEYWORDS}


def _pinned_ids(T: dict) -> set:
    """Every identifier a pinned text names (_pinned_texts), a value's name
    (`PROVISIONING_GATE_TTL_MS`, `BOOT_SHORT_PRESS_MS`) as well as a call's."""
    return {n for x in _pinned_texts(T) for n in re.findall(r"[A-Za-z_]\w*", x)}


def pastes(text: str) -> bool:
    """True when a macro's text (strings blanked) pastes tokens: a `##` that
    is not GNU's `, ##__VA_ARGS__` comma elision."""
    return "##" in _COMMA_ELISION.sub(",", text)


def macro_problems(sources: dict, T: dict, flags: dict, includes: dict) -> list:
    """No `#define` under ROOTS, and no `-D` build flag, names a pinned or
    census function, a device-side use, an accessor, a mint, an emitter, a
    comparer, a credential's spelling or the private key, in its name or its
    replacement: `#define auth_gate(r) (true)` after the gate's definition
    leaves every refusal's text and every pin intact and ungates every
    route, the preprocessor's shadow of a pinned body. No macro or flag is
    named for a function a pinned body or statement calls (_called_names:
    the take hook's `take`, from_ap_subnet's `request_on_softap`,
    host_is_foreign's `host_names_this_device`), and a `#define` in a header
    outside ROOTS (`includes`, INCLUDE_ROOTS) is held the same way, but
    INCLUDE_DEFINES' rows in their file. No macro or flag is a C++ keyword
    (`#define false true`). Under ROOTS, a macro named for anything else a
    pinned text names is ROOTS_DEFINES' row, in its file as often as it says,
    and none is #undef'd. PINNED_DEFINES' macros are each defined once, in
    their file, as their row says, never #undef'd and never set by a flag.
    No macro or flag pastes tokens (`##`, `%:%:`) but GNU's `, ##__VA_ARGS__`:
    `#define CV_JOIN(a, b) a##b` names nothing held, and
    `CV_JOIN(auth_get_, token)()` calls the token's accessor under no spelling
    a rule reads."""
    names = ({q.split("::")[-1] for _f, q in T["glue"]}
             | {q.split("::")[-1] for _f, q in T["local"]}
             | set(T["accessors"]) | set(T["mints"]) | set(T["emitters"]) | set(T["comparers"]))
    rxs = ([re.compile(r"\b(?:" + "|".join(map(re.escape, sorted(names))) + r")\b"),
            _PRIVATE_KEY] + list(T["reads"].values())
           + [_census_rx(n, row) for n, row in sorted(T["census"].items())])
    called = _called_names(T)
    ids = _pinned_ids(T)
    pinned = T["pinned_defines"]
    roots_defs = T["roots_defines"]

    def named(text):
        return next((m.group(0) for rx in rxs for m in [rx.search(text)] if m), None)

    def named_for(name):
        return (name if name in called or name in pinned or name in _CXX_KEYWORDS
                else named(name))
    problems = []
    defined, rooted = {}, {}
    for fname, text in sorted(sources.items()) + sorted(includes.items()):
        outside = fname in includes
        blanked = blank_string_contents(text)
        for m in _DEFINE.finditer(blanked):
            head = _DEFINE_NAME.match(m.group(0))
            folded = _fold(text[m.start():m.end()])
            if not head:
                problems.append(f"{fname}:{_line_of(text, m.start())}: a #define whose name the "
                                f"check cannot read ({folded[:90]!r}) — write `#define NAME ...`")
                continue
            name = head.group(1)
            if name in pinned:
                defined.setdefault(name, []).append((fname, folded))
                continue
            if outside and INCLUDE_DEFINES.get(name) and _in_file(fname, INCLUDE_DEFINES[name]):
                continue
            hit = named_for(name) or named(m.group(0))
            if not hit and not outside and name in ids:
                rooted.setdefault(name, []).append(fname)
                hit = None if name in roots_defs else name
            where = (" (outside ROOTS, in a file the flagship compiles or #includes: a "
                     "header #included after the function shadows it the same way)"
                     if outside else "")
            if hit:
                problems.append(f"{fname}:{_line_of(text, m.start())}: a macro names {hit!r} "
                                f"({folded[:90]!r}): no #define may stand in for, or wrap, the "
                                f"glue, the gates, what they call or a credential the check "
                                f"holds{where}")
            if pastes(m.group(0)):
                problems.append(f"{fname}:{_line_of(text, m.start())}: a macro pastes tokens "
                                f"with ## ({folded[:90]!r}): a name pasted together "
                                f"(`CV_JOIN(auth_get_, token)()`) is a call no rule reads by its "
                                f"spelling; only GNU's `, ##__VA_ARGS__` may be written{where}")
        for m in _UNDEF.finditer(blanked):
            if m.group(1) in pinned or m.group(1) in _CXX_KEYWORDS or (
                    not outside and m.group(1) in ids):
                problems.append(f"{fname}:{_line_of(text, m.start())}: #undef {m.group(1)} — a "
                                f"macro a pinned text reads is defined where it is and never "
                                f"undefined to be redefined (PINNED_DEFINES, ROOTS_DEFINES)")
    for name, (suffix, n) in sorted(roots_defs.items()):
        got = rooted.get(name, [])
        if len(got) != n or not all(_in_file(f, suffix) for f in got):
            problems.append(f"ROOTS_DEFINES: {name} is defined {_times(len(got))} under ROOTS "
                            f"({', '.join(got) or 'nowhere'}); its row says {_times(n)}, in "
                            f"{suffix}. If the change is meant, update the row in the same "
                            f"review")
    for name, (suffix, want) in sorted(pinned.items()):
        got = defined.get(name, [])
        if len(got) != 1 or not _in_file(got[0][0], suffix) or got[0][1] != want:
            problems.append(f"PINNED_DEFINES: {name} is defined {_times(len(got))} "
                            f"({'; '.join(f'{f}: {d}' for f, d in got) or 'nowhere'}); it is "
                            f"defined once, in {suffix}, as {want!r}. If the change is meant, "
                            f"update the row in the same review")
    for fname, text in sorted(flags.items()):
        for m in _FLAG.finditer(text):
            flag = next(g for g in m.groups() if g is not None)
            hit = named(flag) or named_for(re.match(r"[A-Za-z_]\w*", flag).group(0)
                                           if re.match(r"[A-Za-z_]", flag) else "")
            if hit:
                problems.append(f"{fname}:{_line_of(text, m.start())}: the build flag "
                                f"{m.group(0)!r} names {hit!r}: no -D may stand in for the "
                                f"glue, the gates, what they call or a credential the check "
                                f"holds")
            if pastes(flag):
                problems.append(f"{fname}:{_line_of(text, m.start())}: the build flag "
                                f"{m.group(0)!r} pastes tokens with ##: a name pasted together "
                                f"is a call no rule reads by its spelling")
    return problems


# What binds a name to a symbol, so that a call reaches a function by its
# symbol, not its name (`_Z14auth_get_tokenv` is auth_get_token's, and holds no
# `auth_get_token` on a word boundary): assembly (an asm label, an asm
# statement's call); an attribute that names a symbol (weakref, alias, ifunc,
# symver, in the GNU or the C++11 spelling, its underscores or not); glibc's
# __REDIRECT macros (an asm label by another name); a pragma that binds one
# (redefine_extname, weak), as `#pragma` or `_Pragma("...")`; and a name
# spelled as a C++ symbol (`extern "C" const char* _Z14auth_get_tokenv();`
# declares the accessor by its symbol: an identifier beginning `_Z`, which the
# implementation reserves). The one asm the flagship writes is the compiler
# barrier, an empty template; its pragmas are KNOWN_PRAGMAS'.
_ASM = re.compile(r"\b(?:asm|__asm__|__asm)\b")
_EMPTY_ASM = re.compile(r"(?:asm|__asm__|__asm)\s*(?:\b(?:volatile|__volatile__|inline|goto)\b\s*)*"
                        r"\(\s*\"\"\s*[:)]")
_SYMBOL_ATTRIBUTE = re.compile(r"\b(?:__)?(?:weakref|alias|ifunc|symver)(?:__)?\b|\b__REDIRECT\w*")
_MANGLED_NAME = re.compile(r"(?<![\w$])_Z\w*")
_PRAGMA_LINE = re.compile(r"^[ \t]*#[ \t]*pragma\b([^\n]*)", re.M)
_PRAGMA_OP = re.compile(r"\b_Pragma\s*\(\s*(?:L|u8|u|U)?")
# The pragmas the check knows bind no name to a symbol (a namespace's pragma
# by its first two words): what the flagship writes (once, GCC poison) and
# the diagnostics, packing and messages it may.
KNOWN_PRAGMAS = frozenset({"once", "pack", "message", "GCC poison", "GCC diagnostic",
                           "GCC warning", "GCC error", "GCC system_header", "GCC push_options",
                           "GCC pop_options", "GCC optimize"})
_BIND_WORDS = "binds a name to a symbol"


def _pragma_head(text: str) -> str:
    """A pragma's name as KNOWN_PRAGMAS spells it: its first word, or its
    namespace's two (`GCC diagnostic`)."""
    words = re.findall(r"[A-Za-z_]\w*", text)[:2]
    return " ".join(words) if words[:1] == ["GCC"] else "".join(words[:1])


def symbol_binding_problems(sources: dict, includes: dict) -> list:
    """Nothing under ROOTS, or in a file outside it the flagship reaches,
    binds a name to a symbol: no asm but the empty-template compiler barrier
    (`asm volatile("" ::: "memory")`), no weakref, alias, ifunc or symver
    attribute, no __REDIRECT macro, no pragma but KNOWN_PRAGMAS' (a
    `_Pragma` read from its string literal, and one handed anything else
    refused), and no identifier spelled as a C++ symbol (`_Z...`). One
    problem per line."""
    problems, seen = [], set()

    def add(fname, text, pos, what):
        line = _line_of(text, pos)
        if (fname, line) not in seen:
            seen.add((fname, line))
            problems.append(f"{fname}:{line}: {what}")
    for fname, text in sorted(sources.items()) + sorted(includes.items()):
        blanked = blank_string_contents(text)
        for m in _ASM.finditer(blanked):
            if _EMPTY_ASM.match(text, m.start()):
                continue
            end = blanked.find(";", m.start())
            add(fname, text, m.start(),
                f"assembly ({_fold(text[m.start():end + 1 if end >= 0 else None])[:90]!r}): "
                f"an asm label or statement reaches a function by its symbol "
                f"(`__asm__(\"_Z14auth_get_tokenv\")`), a spelling no rule reads; only the "
                f"empty compiler barrier `asm volatile(\"\" ::: \"memory\")` may be written")
        pragmas = [(m.start(), m.group(1)) for m in _PRAGMA_LINE.finditer(blanked)]
        literals = {start: value for start, _end, value in _string_literals(text)}
        for m in _PRAGMA_OP.finditer(blanked):
            if m.end() in literals:
                pragmas.append((m.start(), literals[m.end()]))
            else:
                add(fname, text, m.start(),
                    f"a _Pragma handed no string literal ({_fold(text[m.start():m.end() + 40])!r}) "
                    f"is a pragma the check cannot read, and a pragma {_BIND_WORDS} "
                    f"(redefine_extname, weak) — write `_Pragma(\"...\")` or `#pragma`")
        for pos, body in pragmas:
            head = _pragma_head(body)
            if head not in KNOWN_PRAGMAS:
                add(fname, text, pos,
                    f"the pragma {head or body.strip()!r} is not one the check knows binds "
                    f"nothing (KNOWN_PRAGMAS): `#pragma redefine_extname t "
                    f"_Z14auth_get_tokenv` or `#pragma weak t = ...` {_BIND_WORDS}, a "
                    f"spelling no rule reads")
        for m in _SYMBOL_ATTRIBUTE.finditer(blanked):
            add(fname, text, m.start(),
                f"{m.group(0)!r} {_BIND_WORDS} (`__attribute__((weakref(\"_Z14auth_get_tokenv\")))` "
                f"is the token's accessor under no spelling a rule reads); no weakref, alias, "
                f"ifunc or symver attribute, and no __REDIRECT macro, may be written")
        for m in _MANGLED_NAME.finditer(blanked):
            add(fname, text, m.start(),
                f"{m.group(0)!r} is spelled as a C++ symbol (a name beginning `_Z`, which the "
                f"implementation reserves): `extern \"C\" const char* _Z14auth_get_tokenv();` "
                f"{_BIND_WORDS}, the token's accessor under no spelling a rule reads")
    return problems


# What the flag files may not hold, because each is code or a binding the
# check does not read. A driver or compiler flag token (anchored at a token
# start, so `-DBOARD` is not `-B`), in its space form, its glued one and its
# `=`/`--` long one alike. This is defense in depth over the spellings below,
# outside the docstring's threat model (a deliberate flag aimed at the
# check), not every spelling the driver honors: gcc's long aliases
# (`--prefix`, rewritten to `-B`; `--for-linker`, `--for-assembler`,
# `--define-macro`), a letter-led glued `-B<dir>` and the `-L`/`-l` library
# options are not read.
#   - a linker option (`-Wl,`, `-Xlinker`, `--wrap`, `--defsym`, a `-T` linker
#     script): `-Wl,--wrap=<symbol>` sends every call of a function to
#     `__wrap_<symbol>`, a name no rule reads;
#   - a forced include (`-include`, `--include`, `-imacros`, `--imacros`): a
#     file no `#include` names, so macro_problems never scans it;
#   - an include or sysroot path the check does not read as a reach root
#     (`-isystem`, `-iquote`, `-idirafter`, `-iprefix`, `-iwithprefix`,
#     `-isysroot`, `--sysroot`; only `-I` is read, by build_reach): a header
#     reached through one is read by no rule;
#   - a compiler driver that injects code or changes the toolchain
#     (`-specs`/`--specs`, `-fplugin`, `-wrapper`, `-B<prefix>`,
#     `-Xpreprocessor`/`-Wp,` and `-Xassembler`/`-Wa,`, each a way to smuggle a
#     forced include or a `--defsym` past the tokens above);
#   - a response file (`@file`), which splices in flags from a file no rule reads.
# A PlatformIO section key that moves a source/library/include folder or runs
# code (extra_scripts, lib_extra_dirs, a lib_deps path, src_dir, lib_dir,
# include_dir, a linker script; a key in any case, as configparser lowercases
# it), PlatformIO's own `!command` dynamic value (a command run at build time
# whose stdout becomes flags) and a `${sysenv.*}` reference (the build-time
# environment's value, which can bring a library or a flag the check cannot
# read: the file text is read, so an empty environment at check time hides
# none) are refused the same way. (The `=`/`--` long forms, the include-path
# flags, `@file` and `!command` were added in F338's fifth review, which found
# each slipped the first version of this rule; `${sysenv.*}` anywhere, and a
# key in any case, in its eleventh round.)
_BUILD_INPUT = re.compile(
    r"(?<![\w-])(?:-Wl,|-Xlinker\b|--wrap\b|--defsym\b|-T(?=\s*[\w$./])"
    r"|--?include\b|--?imacros\b"
    r"|-i(?:system|quote|dirafter|prefix|withprefix(?:before)?|sysroot|plugindir|multilib)\b"
    r"|--sysroot\b|--?specs\b|-fplugin\b|-wrapper\b|-Xpreprocessor\b|-Xassembler\b"
    r"|-Wp,|-Wa,|-B(?=[\s/$.=~]))"
    r"|^[ \t]*(?i:extra_scripts?|lib_extra_dirs|src_dir|lib_dir|include_dir|"
    r"board_build\.ldscript)[ \t]*[=:]"
    r"|(?:symlink|file)://"
    r"|\$\{\s*(?i:sysenv)\s*\."
    r"|(?:^[ \t]*|[=:,][ \t]*)![ \t]*(?=\S)"
    r"|(?:^[ \t]*|[=:,][ \t]*)@(?=[\w/$.~])", re.M)


def build_input_problems(flags: dict) -> list:
    """No flag file holds a build input the check does not read
    (_BUILD_INPUT): a linker option, a forced include, an include-path or
    sysroot flag, a compiler-driver injector, a response file, a build
    script, a local library, a moved source folder, a `!command` dynamic
    value or a `${sysenv.*}` reference. One problem per line."""
    problems, seen = [], set()
    for fname, text in sorted(flags.items()):
        for m in _BUILD_INPUT.finditer(text):
            line_no = _line_of(text, m.start())
            if (fname, line_no) in seen:
                continue
            seen.add((fname, line_no))
            end = text.find("\n", m.start())
            line = text[text.rfind("\n", 0, m.start()) + 1:end if end >= 0 else len(text)]
            problems.append(f"{fname}:{line_no}: {line.strip()!r} is a build "
                            f"input the check does not read (a linker option, a forced "
                            f"include, an include-path or sysroot flag, a compiler-driver "
                            f"injector, a response file, a build script, a local library, a "
                            f"moved folder, a `!command` run at build time or a `${{sysenv.*}}` "
                            f"value the build-time environment sets) — a symbol bound there "
                            f"(`-Wl,--wrap=...`), a header force-included (`--include=...`), a "
                            f"flag a command emits (`!python3 gen.py`) or a library an "
                            f"environment variable names (`${{sysenv.LIB_DEPS}}`) is reached by "
                            f"no spelling the rules read; teach the check before the build "
                            f"takes one")
    return problems


# The registry libraries the build may fetch, each with why it is safe to link
# unread. A library is code no rule reads, so one that brings its own server or
# access point serves past every rule here: tzapu/WiFiManager's autoConnect(),
# handed no AP password, raises an OPEN SoftAP and serves a firmware upload
# (`/update`, which posts to `/u`) and `/erase` to anyone who joins it (v2.0.17:
# WiFi.softAP(name, "") and its WebServer's routes), and ESPAsyncWebServer,
# PsychicHttp or ESPUI serve routes no route rule reads. So every library an
# env of the flagship's configuration fetches (its lib_deps as PlatformIO's own
# reader resolves it, platformio_lib_deps), and every dependency a library
# manifest under ROOTS declares (library.json's "dependencies",
# library.properties' `depends=`, read by PlatformIO's own manifest parser), is
# a row here, matched by `owner/name` whatever version range it names, or a
# library under ROOTS by its name. A row is an `owner/name` and admits only
# that: a library no row names fails until a reviewer adds its row with a
# reason, as PUBLIC_ALLOWLIST's routes are added, and a bare registry name
# (which the registry resolves by search), a registry id, a URL or a path fails
# whatever the rows say (library_problems). A row vouches for its library's own
# dependencies too, which PlatformIO fetches and the check does not read: the
# four rows' libraries declare none (no dependency in the manifests of Crypto
# 0.4.0, ArduinoJson 7.4.3, PubSubClient 2.8 and NimBLE-Arduino 2.5.1, versions
# within the ranges the flag files name). A core library is not fetched; the
# core's servers are the "another HTTP server" census row's to name (it names
# some and lists those it does not).
REGISTRY_LIBS = {
    "rweather/Crypto": "Ed25519, Curve25519 and ChaChaPoly, with the hashes they use: no "
                       "network and no server",
    "bblanchon/ArduinoJson": "JSON encoding and decoding: no network and no server",
    "knolleary/PubSubClient": "the MQTT client: one outbound connection to the configured "
                              "broker, no server and no access point",
    "h2zero/NimBLE-Arduino": "the BLE stack ([env:full]'s Scout and status service): no "
                             "Wi-Fi, no HTTP server and no access point",
}
# The PlatformIO whose own readers the library rule reads with (ProjectConfig
# for the configuration, PackageSpec for each lib_deps entry and manifest
# dependency, its manifest parser for library.json and library.properties):
# the version the builds that ship the flagship pin (firmware-release.yml and
# flasher-release.yml hand .github/actions/setup-platformio
# `platformio-version: "6.1.19"` and build release_ha with it). firmware.yml's
# PlatformIO Build jobs pin none and take the newest release, 6.2.0 on
# 2026-10-22, whose ProjectConfig and PackageSpec are this version's text and
# whose manifest parser adds one reading (an owner written into a list
# dependency's "name", which this version reads as a bare name and so fails as
# no row). firmware.yml's route-check job installs this version; main()
# refuses any other, and a missing module, with the install command.
PLATFORMIO_PIN = "6.1.19"
PLATFORMIO_INSTALL = f"python3 -m pip install platformio=={PLATFORMIO_PIN}"


def _shown(path: Path) -> str:
    """A path as the problems name it: relative to the working directory (the
    repo root, as main() runs) when it is inside it, else absolute."""
    rel = os.path.relpath(path)
    return Path(rel).as_posix() if not rel.startswith("..") else path.as_posix()


def platformio_lib_deps(flags: dict, ini=FLAG_FILES[0], project=PROJECT_DIR) -> tuple:
    """({env: [lib_deps entry]}, problems): every env of the configuration at
    `ini` and the libraries it fetches, as PlatformIO's own reader resolves
    them (ProjectConfig at PLATFORMIO_PIN, read from the project's folder as
    `pio run` reads it: the extra_configs files, [env] and each `extends`,
    every interpolation, the older key names lib_use, lib_force and
    lib_install, a key at any indent and in any case, a one-line list split
    on ", "). Nothing here reads the ini text. A configuration PlatformIO
    refuses and an env whose lib_deps it cannot resolve are problems, and so
    is a file it reads that the flag files (`flags`, flag_sources()) do not
    hold: the -D flags and build inputs there would be read by no rule."""
    ini, project = Path(ini), Path(project)
    where = _shown(ini)
    if not ini.is_file():
        return {}, [f"{where}: the configuration PlatformIO reads is missing, so the libraries "
                    f"its envs fetch are not held to REGISTRY_LIBS — fix the path or the "
                    f"check's FLAG_FILES"]
    base, path, deps, problems, read = project.resolve(), ini.resolve(), {}, [], []
    # PlatformIO reads a project from its folder: extra_configs' globs and
    # ${PROJECT_DIR} are the working directory's. (Nothing in the block names a
    # path for a problem: those are the caller's working directory's.)
    with contextlib.chdir(base):
        try:
            config = ProjectConfig(str(path))
        except Exception as e:  # PlatformIO's own refusal (a parse error, a duplicate key)
            config, refused = None, e
        if config is not None:
            read = [Path(os.path.normpath(base / p)) for p in config._parsed]
            for env in config.envs():
                try:
                    deps[env] = [str(entry) for entry in config.get(f"env:{env}", "lib_deps")]
                except Exception as e:  # an interpolation PlatformIO cannot resolve
                    problems.append(f"{where}: PlatformIO {PLATFORMIO_PIN}'s reader cannot "
                                    f"resolve env:{env}'s lib_deps ({type(e).__name__}: {e}), "
                                    f"so the libraries it fetches are not held to REGISTRY_LIBS")
    if config is None:
        return {}, [f"{where}: PlatformIO {PLATFORMIO_PIN}'s reader refuses the configuration "
                    f"({refused}), so the libraries its envs fetch are not held to "
                    f"REGISTRY_LIBS"]
    held = {Path(f).resolve() for f in flags}
    problems.extend(f"{_shown(p)}: PlatformIO reads it as part of {where}'s configuration "
                    f"(extra_configs), but the flag files (flag_sources) do not hold it, so its "
                    f"-D flags and build inputs are read by no rule — name it in extra_configs "
                    f"as flag_sources reads it, or teach flag_sources PlatformIO's spelling"
                    for p in read if p.resolve() not in held)
    return deps, problems


def _manifest_deps(fname: str, text: str) -> tuple:
    """([(line, shown, spec or None, why)], [declared library name], problems)
    of a library manifest, read by PlatformIO's own manifest parser (library.json's
    "dependencies" in any form it takes, library.properties' `depends=`), each
    dependency the PackageSpec PlatformIO builds from it (dependency_to_spec:
    its owner, its name and its version as the requirement, a version that is
    no semantic version range read as `name=version`, a URL or a path)."""
    try:
        data = ManifestParserFactory.new(text, ManifestFileType.from_uri(fname)).as_dict()
    except Exception as e:  # PlatformIO's own refusal (bad JSON, a malformed list)
        return [], [], [f"{fname}: a library manifest PlatformIO {PLATFORMIO_PIN} cannot read "
                        f"({type(e).__name__}: {e}), so the libraries it fetches are not held "
                        f"to REGISTRY_LIBS"]
    names = [data["name"]] if isinstance(data.get("name"), str) else []
    items = []
    for dep in data.get("dependencies") or []:
        dep = dep if isinstance(dep, dict) else {"name": dep}
        ident = "/".join(str(x) for x in (dep.get("owner"), dep.get("name")) if x)
        at = text.find(str(dep.get("name")))
        line = _line_of(text, at) if at >= 0 else 1
        try:
            spec = PackageSpec(owner=dep.get("owner"), name=dep.get("name"),
                               requirements=dep.get("version"))
        except Exception as e:  # a version PlatformIO reads as neither a range nor a URL
            items.append((line, ident or repr(dep), None, f"{type(e).__name__}: {e}"))
            continue
        shown = f"{ident}={dep.get('version')}" if spec.raw else ident
        items.append((line, shown, spec, None))
    return items, names, []


def _spec_admitted(spec, known: set, local: set) -> bool:
    """A PackageSpec names a REGISTRY_LIBS row (`known`, each owner/name
    casefolded) or a library under ROOTS by its name (`local`): no URL or
    path (PlatformIO fetches or copies what it names), no registry id, and a
    bare name only for a library under ROOTS."""
    if spec.external or spec.id is not None or not isinstance(spec.name, str):
        return False
    if spec.owner:
        return f"{spec.owner}/{spec.name}".casefold() in known
    return spec.name in local


def library_problems(sources: dict, lib_deps: dict, manifests: dict, registry: dict) -> list:
    """Every library the flagship's envs fetch (`lib_deps`, {env: [entry]},
    platformio_lib_deps(): each entry as PlatformIO's ProjectConfig resolves
    it, read by its PackageSpec) and every dependency a library manifest under
    ROOTS declares (`manifests`, library_manifests(), read by PlatformIO's
    manifest parser) is a REGISTRY_LIBS row, or a library under ROOTS by its
    name (a row vouching for its library's own dependencies). A row is an
    `owner/name`; no row admits a bare registry name, a registry id, a URL or
    a path. An entry that fails names every env that fetches it."""
    problems = []
    known = {k.casefold() for k in registry if k.count("/") == 1}
    problems.extend(f"REGISTRY_LIBS: {k!r} is no owner/name row — a row admits the one "
                    f"library its owner/name names (a bare registry name is resolved by "
                    f"search, and a URL fetches what it names)"
                    for k in sorted(registry) if k.count("/") != 1)
    local = set()
    for path in list(sources) + list(manifests):
        parts = path.split("/")
        local |= {parts[i + 1] for i, part in enumerate(parts[:-2]) if part == "lib"}
    specs = []
    for fname, text in sorted(manifests.items()):
        items, names, bad = _manifest_deps(fname, text)
        local |= set(names)
        problems.extend(bad)
        specs.extend((f"{fname}:{n}: its dependency list fetches {shown!r}", spec, why)
                     for n, shown, spec, why in items)
    fetched = {}
    for env, entries in sorted(lib_deps.items()):
        for entry in entries:
            fetched.setdefault(entry, []).append(f"env:{env}")
    for entry, envs in fetched.items():
        try:
            spec, why = PackageSpec(entry), None
        except Exception as e:  # a spec PlatformIO cannot read as a library
            spec, why = None, f"{type(e).__name__}: {e}"
        specs.append((f"{', '.join(envs)}: lib_deps, as PlatformIO resolves it, fetches "
                      f"{entry!r}", spec, why))
    for where, spec, why in specs:
        if spec is not None and _spec_admitted(spec, known, local):
            continue
        read = (f"PlatformIO cannot read it as a library spec ({why})" if spec is None
                else "a URL or a path" if spec.external
                else f"PlatformIO reads it as {spec.humanize()!r}")
        problems.append(f"{where}, a library no REGISTRY_LIBS row names ({read}; a row is "
                        f"an owner/name, and no row admits a bare registry name, a registry "
                        f"id, a URL or a path) — its code is read by no rule, so one that "
                        f"raises its own access point or serves its own routes "
                        f"(WiFiManager's autoConnect() raises an open SoftAP and serves a "
                        f"firmware upload) ships past every rule here; add its row with why it "
                        f"raises no access point and serves no route, or drop it")
    return problems


def library_manifests(roots=ROOTS) -> dict:
    """{path: text} of every library manifest under `roots` (NON_SOURCE_NAMES),
    whose dependencies library_problems holds to REGISTRY_LIBS."""
    return {p.as_posix(): p.read_text(errors="replace")
            for root in roots for p in sorted(root.rglob("*"))
            if p.is_file() and p.name in NON_SOURCE_NAMES}


def include_mention_problems(T: dict, includes: dict) -> list:
    """The files outside ROOTS the flagship reaches (`includes`, build_reach)
    name nothing the check holds under ROOTS but INCLUDE_MENTIONS: no
    credential's spelling, no pinned, census, accessor, emitter or comparer
    name, the network manager's type and accessor and the setup lib's
    namespace included (INCLUDE_COMMON_WORDS aside), and no census row's
    spelling for a primitive defined outside ROOTS (a header's `WiFi.softAP(`,
    `WiFi.mode(` or `esp_wifi_set_config(` raises the SoftAP open as one under
    ROOTS would), but a row confined to its own files."""
    held = ({q.split("::")[-1] for _f, q in T["glue"]}
            | {q.split("::")[-1] for _f, q in T["local"]}
            | set(T["accessors"]) | set(T["emitters"]) | set(T["comparers"])
            | {"ScvNetworkManager", MANAGER_ACCESSOR, "setup_wiz"})
    held |= {n.split("::")[-1] for n, row in T["census"].items()
             if (row.get("ident") or row.get("home") is not None) and " " not in n}
    held -= INCLUDE_COMMON_WORDS
    rxs = [re.compile(r"\b(?:" + "|".join(map(re.escape, sorted(held))) + r")\b")]
    rxs += list(T["reads"].values())
    rxs += [_census_rx(n, row) for n, row in sorted(T["census"].items())
            if row.get("home", "") is None and not row.get("file")]
    problems = []
    for fname, text in sorted(includes.items()):
        blanked = blank_string_contents(text)
        for rx in rxs:
            for m in rx.finditer(blanked):
                word = re.match(r"[A-Za-z_]\w*", m.group(0)).group(0) if re.match(
                    r"[A-Za-z_]", m.group(0)) else m.group(0)
                if any(_in_file(fname, f) and word == n for f, n in INCLUDE_MENTIONS):
                    continue
                problems.append(f"{fname}:{_line_of(text, m.start())}: a file outside ROOTS "
                                f"the flagship compiles or #includes names {m.group(0)!r}, "
                                f"which the check holds under ROOTS — its code is not read, so "
                                f"a function there that reads a credential, opens the tap, "
                                f"starts the network or raises the SoftAP is followed by no "
                                f"rule (INCLUDE_MENTIONS)")
    return problems


def check_bearer_only_answers(sources: dict, tables=None, flags=None, includes=None,
                              manifests=None, lib_deps=None) -> list:
    """Every answer that carries a credential holds to its grant, on the
    flagship's own gates (see the module docstring). `tables` overrides the
    module tables by name (the self-test's fixtures pass their own; main()
    passes "nvs_keys", the NVS_LITERAL_KEYS values canary_config.h holds);
    `flags` is the build's flag files (flag_sources()); `includes` the
    files outside ROOTS the flagship compiles or #includes (build_reach());
    `manifests` the library manifests under ROOTS (library_manifests());
    `lib_deps` each env's libraries as PlatformIO resolves them
    (platformio_lib_deps())."""
    T = {"reads": SECRET_READS, "accessors": SECRET_ACCESSORS, "mints": SECRET_MINTS,
         "emitters": TOKEN_EMITTERS, "emitter_stmts": EMITTER_STATEMENTS,
         "comparers": TOKEN_COMPARERS, "local": SECRET_LOCAL_USES, "answers": SECRET_ANSWERS,
         "answer_stmts": ANSWER_STATEMENTS, "outside": OUTSIDE_FUNCTION_LINES,
         "glue": GLUE_DEFS, "census": GATE_CALL_SITES, "mint_after": MINT_AFTER,
         "pinned_defines": PINNED_DEFINES, "roots_defines": ROOTS_DEFINES, "nvs_keys": {},
         "receipt": RECEIPT_GRANTS, "headers": REQUEST_HEADERS, "queries": URL_QUERY_READERS,
         "uri": URI_READERS, "public": PUBLIC_ALLOWLIST, "markers": AUTH_MARKERS,
         "steps": PRE_HOST_STEPS, "registry": REGISTRY_LIBS}
    T.update(tables or {})
    routes = []
    for fname, text in sorted(sources.items()):
        routes.extend(r for r in parse_routes(fname, text) if None not in r[1:3])
    handlers = {h.split("::")[-1] for _u, _m, h, _f in routes}
    defs, by_name = _function_index(sources)
    # (a) the reads and the calls, per (file, qualified function), every body.
    reads, calls, outside = _reads_in(sources, defs, T["reads"])
    # (b) the carry, stopping at the local uses and the comparers.
    stops = {k for k in defs
             if any(_in_file(k[0], f) and k[1] == q for f, q in T["local"])
             or k[1].split("::")[-1] in T["comparers"]}
    carried = _carry_map(defs, by_name, reads, calls, stops)
    return (_reader_problems(sources, T, defs, reads, carried, outside, handlers)   # (c)
            + _local_use_problems(sources, T, defs, reads, handlers)
            + _answer_problems(sources, T, defs, by_name, reads, carried, stops,     # (d), (e)
                               routes)
            + glue_def_problems(sources, T["glue"])
            + gate_census_problems(sources, T["census"], T["markers"], T["steps"],
                                   T["emitters"])
            + mint_order_problems(sources, T["mint_after"])
            + nvs_literal_problems(sources, T["nvs_keys"])
            + no_session_problems(sources, T["headers"], T["queries"], T["uri"])
            + macro_problems(sources, T, flags or {}, includes or {})
            + build_input_problems(flags or {})
            + library_problems(sources, lib_deps or {}, manifests or {}, T["registry"])
            + symbol_binding_problems(sources, includes or {})
            + include_mention_problems(T, includes or {})
            + manager_alias_problems(sources))


def check_registration_sites(sources: dict) -> list:
    """Inside registerHttpHandlers(server) every registration goes to the
    `server` parameter — never a member handle."""
    failures = []
    found = False
    for fname, text in sorted(sources.items()):
        span = definition_span(text, "registerHttpHandlers")
        if not span:
            continue
        found = True
        body = text[span[0]:span[1]]
        for fn in ("register_route", "httpd_register_uri_handler"):
            for off, args in call_args(body, fn):
                if not args or args[0] != "server":
                    line = text.count("\n", 0, span[0] + off) + 1
                    failures.append(
                        f"{fname}:{line}: {fn}({args[0] if args else ''}, ...) inside "
                        f"registerHttpHandlers — use the `server` parameter: the "
                        f"table goes on whichever server is primary, and on "
                        f"FEATURE_HTTPS builds m_http_server is still nullptr here "
                        f"(the route would vanish with only a serial line)")
    if not found:
        failures.append("registerHttpHandlers definition not found — parser regression?")
    return failures


# The calls that put a route on a server. A registration the parser cannot read
# (an `httpd_uri_t w; w.uri = ...;` assignment, a route a helper builds) is a
# route no rule reads, so no function calls these more often than it writes
# the initializers parse_routes_at reads (the probe loop registers six routes
# from one initializer and one call). register_route, the wrapper every route
# goes through, hands httpd_register_uri_handler its own parameter.
REGISTER_CALLS = ("register_route", "httpd_register_uri_handler")
REGISTER_WRAPPER = "register_route"


def check_registration_count(sources: dict) -> list:
    """Every registration call hands over an initializer the parser reads:
    per function, no more REGISTER_CALLS mentions (a call, or the function
    taken as a pointer) than parsed `httpd_uri_t` initializers, none outside
    every function, and the wrapper registering only the route it is handed
    (`httpd_register_uri_handler(server, uri)`, once)."""
    failures = []
    for fname, text in sorted(sources.items()):
        if not any(fn in text for fn in REGISTER_CALLS):
            continue
        inits = {off for off, *_ in parse_routes_at(fname, text)}
        blanked = blank_string_contents(text)
        defs = function_defs(text)
        per, wrapped = {}, {}
        for fn in REGISTER_CALLS:
            for m in re.finditer(r"\b" + fn + r"\b", blanked):
                off = m.start()
                d = _innermost(defs, off)
                if d is None or off < d[3]:
                    before = blanked[max(blanked.rfind(c, 0, off) for c in ";{}") + 1:off]
                    if re.search(r"[\w*&>]\s*$", before) and not re.search(r"[=(]", before):
                        continue  # a declaration
                    failures.append(f"{fname}:{_line_of(text, off)}: {fn}() called outside "
                                    f"every function — a route is registered in a function, "
                                    f"from an initializer the parser reads")
                    continue
                if d[0].split("::")[-1] == REGISTER_WRAPPER:
                    wrapped.setdefault(d, []).append(
                        bool(fn == "httpd_register_uri_handler" and re.match(
                            r"httpd_register_uri_handler\s*\(\s*server\s*,\s*uri\s*\)",
                            blanked[off:])))
                    continue
                per.setdefault(d, []).append(off)
        for d, calls in sorted(wrapped.items(), key=lambda kv: kv[0][1]):
            if calls != [True]:
                failures.append(f"{fname}:{_line_of(text, d[1])}: {d[0]}() names a registration "
                                f"call {len(calls)} time(s), not once as "
                                f"`httpd_register_uri_handler(server, uri)` — the wrapper "
                                f"registers only the route it is handed")
        for d, offs in sorted(per.items(), key=lambda kv: kv[0][1]):
            n = sum(1 for off in inits if d[1] <= off < d[4])
            if len(offs) > n:
                failures.append(f"{fname}:{_line_of(text, d[1])}: {d[0]}() registers "
                                f"{len(offs)} times but writes {n} `httpd_uri_t x = {{ .uri = "
                                f"..., .method = HTTP_..., .handler = ... }};` initializer(s) — "
                                f"a route registered any other way (an assignment form, a "
                                f"route a helper builds) is one no rule reads")
    return failures


def check_route_budgets(sources: dict) -> list:
    """No function registers more routes than the max_uri_handlers it sets;
    registerHttpHandlers is held to kRouteTableSlots, with and without the
    mesh block. Every #if branch is counted (the worst case), so an #else /
    #elif inside a counted body is refused rather than double-counted."""
    failures = []
    slots_re = re.compile(
        r"#if\s+defined\(FEATURE_MESH_NETWORK\)\s*&&\s*FEATURE_MESH_NETWORK\s*\n"
        r"\s*static\s+const\s+uint16_t\s+kRouteTableSlots\s*=\s*(\d+)\s*;\s*\n"
        r"\s*#else\s*\n"
        r"\s*static\s+const\s+uint16_t\s+kRouteTableSlots\s*=\s*(\d+)\s*;")
    mesh_block_re = re.compile(
        r"#if\s+defined\(FEATURE_MESH_NETWORK\)\s*&&\s*FEATURE_MESH_NETWORK\b(.*?)#endif", re.S)
    checked_table = False
    for fname, text in sorted(sources.items()):
        routes = parse_routes_at(fname, text)
        # Functions that set a literal budget and register in their own body.
        for m in re.finditer(r"max_uri_handlers\s*=\s*(\d+)\s*;", text):
            owner = None
            for d in re.finditer(r"\b(\w+)\s*\([^;{}]*\)\s*\{", text[:m.start()]):
                if d.group(1) not in ("if", "for", "while", "switch", "catch"):
                    owner = d.group(1)
            span = definition_span(text, owner) if owner else None
            if not span or not (span[0] <= m.start() < span[1]):
                continue
            n = sum(1 for off, *_ in routes if span[0] <= off < span[1])
            if n > int(m.group(1)):
                failures.append(f"{fname}: {owner}() registers {n} routes but sets "
                                f"max_uri_handlers = {m.group(1)}")
        span = definition_span(text, "registerHttpHandlers")
        if not span:
            continue
        slots = slots_re.search(text)
        if not slots:
            failures.append(f"{fname}: kRouteTableSlots is not the expected "
                            f"#if FEATURE_MESH_NETWORK / #else pair — teach "
                            f"check_route_budgets the new shape")
            continue
        checked_table = True
        mesh_slots, base_slots = int(slots.group(1)), int(slots.group(2))
        body = text[span[0]:span[1]]
        if re.search(r"^\s*#\s*(?:else|elif)\b", body, re.M):
            failures.append(f"{fname}: registerHttpHandlers has an #else/#elif — the "
                            f"worst-case counter sums every branch; teach it the "
                            f"alternative before adding one")
        total = sum(1 for off, *_ in routes if span[0] <= off < span[1])
        mesh = 0
        for mb in mesh_block_re.finditer(body):
            lo, hi = span[0] + mb.start(), span[0] + mb.end()
            mesh += sum(1 for off, *_ in routes if lo <= off < hi)
        if total > mesh_slots:
            failures.append(
                f"{fname}: registerHttpHandlers registers {total} routes (every "
                f"feature on) but kRouteTableSlots is {mesh_slots} with the mesh "
                f"— raise it (and its #else twin) and the comment above it")
        if total - mesh > base_slots:
            failures.append(
                f"{fname}: registerHttpHandlers registers {total - mesh} routes "
                f"without the mesh block but kRouteTableSlots is {base_slots} "
                f"without the mesh — raise it")
    if not checked_table:
        failures.append("registerHttpHandlers / kRouteTableSlots not found — "
                        "parser regression?")
    return failures


def _assert_parser_robust() -> None:
    """Guard the parser on every run, so a silent false pass cannot creep in.

    1. A handler whose body carries an UNBALANCED brace inside a string
       literal before its auth call must still extract fully (a naive
       matcher truncates there and mis-reports the route's gating).
    2. The probe-loop form must expand to one route per array entry.
    """
    fixture = (
        'esp_err_t handle_demo(httpd_req_t* req) {\n'
        '  httpd_resp_sendstr(req, "{\\"code\\":\\"bad_}\\"}");\n'
        '  if (!auth_gate(req)) return ESP_OK;\n'
        '  return ESP_OK;\n'
        '}\n'
        'esp_err_t handle_other(httpd_req_t* req) { return ESP_OK; }\n'
    )
    body = function_body(fixture, "handle_demo")
    assert body is not None, "fixture handler not found"
    assert "auth_gate(" in body, (
        "brace matcher truncated the body at a string-literal brace — the "
        "auth call was lost (this is the bug this guard prevents)")
    assert "handle_other" not in body, "body over-extended into the next fn"
    fixture2 = (
        "esp_err_t handle_c(httpd_req_t* req) {\n"
        "  char close = '}';\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  return ESP_OK;\n"
        "}\n"
    )
    b2 = function_body(fixture2, "handle_c")
    assert b2 is not None and "auth_gate(" in b2, "char-literal brace desync"

    loop = (
        'static const char* kPaths[] = { "/a*", "/b*" };\n'
        "for (const char* p : kPaths) {\n"
        "  httpd_uri_t probe = { .uri = p, .method = HTTP_GET, .handler = h };\n"
        "}\n"
        'httpd_uri_t x = { .uri = "/x", .method = HTTP_POST, .handler = hx };\n'
    )
    got = sorted((u, m, h) for u, m, h, _ in parse_routes("fixture", loop))
    assert got == [("/a*", "GET", "h"), ("/b*", "GET", "h"), ("/x", "POST", "hx")], (
        f"probe-loop expansion broke: {got}")
    # ...a brace-initialized route and an `&handler` are read; a cast method
    # or a lambda handler is a route the parser names as unreadable, which
    # check_route_gates fails.
    forms = ('httpd_uri_t a = { .uri = "/a", .method = HTTP_GET, .handler = &ha };\n'
             'httpd_uri_t b{ .uri = "/b", .method = HTTP_POST, .handler = hb };\n'
             'httpd_uri_t c = { .uri = "/c", .method = (httpd_method_t)HTTP_POST,\n'
             '                  .handler = hc };\n'
             'httpd_uri_t d = { .uri = "/d", .method = HTTP_GET,\n'
             '                  .handler = [](httpd_req_t* r) { return ESP_OK; } };\n')
    got = [(u, m, h) for u, m, h, _ in parse_routes("fixture", forms)]
    assert got == [("/a", "GET", "ha"), ("/b", "POST", "hb"), (None, None, "hc"),
                   (None, "GET", None)], got
    got = check_route_gates({}, parse_routes("fixture", forms))[0]
    assert sum("the parser cannot read this registration's" in f for f in got) == 2, got
    # ...and a registration the parser cannot read (an assignment form) is a
    # registration call with no initializer: check_registration_count.
    table = ("static void register_route(httpd_handle_t server, const httpd_uri_t* uri) {\n"
             "  httpd_register_uri_handler(server, uri);\n}\n"
             "void Mgr::registerHttpHandlers(httpd_handle_t server) {\n"
             '  httpd_uri_t a = { .uri = "/a", .method = HTTP_GET, .handler = ha };\n'
             "  register_route(server, &a);\n}\n")
    assert check_registration_count({"fx": table}) == []
    assigned = table.replace("  register_route(server, &a);\n",
                             "  register_route(server, &a);\n  httpd_uri_t w;\n"
                             '  w.uri = "/w";\n  w.method = HTTP_POST;\n  w.handler = hw;\n'
                             "  register_route(server, &w);\n")
    got = check_registration_count({"fx": assigned})
    assert any("registers 2 times but writes 1" in f for f in got), got
    # ...a registration through a pointer to the call is a mention too, and the
    # wrapper registers only the route it is handed.
    pointer = table.replace("  register_route(server, &a);\n",
                            "  register_route(server, &a);\n  auto reg = register_route;\n"
                            "  httpd_uri_t w;\n  w.uri = \"/w\";\n  reg(server, &w);\n")
    got = check_registration_count({"fx": pointer})
    assert any("registers 2 times but writes 1" in f for f in got), got
    # ...and one named outside every function (a pointer to the wrapper kept at
    # namespace scope, called with a route assigned field by field) fails too.
    kept = table + ("static void (*const s_register)(httpd_handle_t, const httpd_uri_t*) = "
                    "register_route;\n")
    got = check_registration_count({"fx": kept})
    assert any("register_route() called outside every function" in f for f in got), got
    extra = table.replace("  httpd_register_uri_handler(server, uri);\n",
                          "  httpd_register_uri_handler(server, uri);\n"
                          "  httpd_register_uri_handler(server, &s_extra);\n")
    got = check_registration_count({"fx": extra})
    assert any("the wrapper registers only the route it is handed" in f for f in got), got

    # 2b. A raw string literal is one literal: its `"`, `//`, `/*` and braces
    #     are text, and the code after it is code (a reader after the web UI's
    #     HTML would otherwise be read inside a string, to the end of its file).
    #     Each prefixed literal holds one `"`, so a reading that missed its prefix
    #     would end it there and read every function after it inside a string.
    raw = ('static const char kUi[] = R"html(<a href="//x">{"/*"}</a>)html";\n'
           'static const char kJs[] = u8R"(if (a) { b("); })";\n'
           "const char* after() { return auth_get_token(); }\n"
           'static const wchar_t kW[] = LR"(a " b)";\n'
           "const char* after_l() { return auth_get_token(); }\n"
           'static const char16_t kU[] = uR"(a " b)";\n'
           "const char* after_u() { return auth_get_token(); }\n"
           'static const char32_t kU32[] = UR"(a " b)";\n'
           "const char* after_u32() { return auth_get_token(); }\n"
           'static const char kNot[] = FOOR"x";\n')
    stripped = strip_comments(raw)
    assert stripped == raw, stripped
    blanked = blank_string_contents(stripped)
    assert len(blanked) == len(stripped) and blanked.count("auth_get_token") == 4 and (
        "href" not in blanked)
    assert [n for n, _s, _e in function_spans(stripped)] == [
        "after", "after_l", "after_u", "after_u32"], function_spans(stripped)
    # 2c. A backslash-newline is spliced away and a digraph read as its token,
    #     every later line keeping its number; `<::x`, `<<%`, `%=` and a
    #     literal's text are not digraphs.
    spliced = splice_lines("a\\\nb \\  \r\nc\nd\\\ne\nf")
    assert spliced == "ab c\n\n\nde\n\nf", repr(spliced)
    got = strip_comments('%:define X <% a<:0:> %> %:%: std::vector<::y> q<<%r; s%=2; "<%"\n')
    assert got == '# define X {  a[ 0]  }  ##   std::vector<::y> q<<%r; s%=2; "<%"\n', got

    # 3. A gate whose result is ignored is not a gate.
    for ok in ("if (!auth_gate(req)) return ESP_OK;",
               "} else if (auth_check(req, t)) {",
               "if (a && (b || bearer_present_and_valid(req))) {",
               "return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n"
               "       AuthManager::BearerResult::OK;"):
        assert marker_decides("{ " + ok, next(m for m in AUTH_MARKERS if m in ok)), ok
    for bad in ("(void)bearer_present_and_valid(req); return send(req);",
                "(void)auth_bearer_grant(req, t, request_peer_ipv4(req)); return send(req);",
                # The grant's four values say yes only as OK (F340).
                "if (auth_bearer_grant(req, t, request_peer_ipv4(req)) != "
                "AuthManager::BearerResult::WRONG) return send(req);",
                "if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
                "AuthManager::BearerResult::NONE) return send(req);",
                "const AuthManager::BearerResult r = auth_bearer_grant(req, t, request_peer_ipv4(req));\n"
                "  if (r != AuthManager::BearerResult::HELD) return send(req);",
                "if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
                "AuthManager::BearerResult::OKAY) return send(req);",
                "auth_gate(req); return ESP_OK;",
                "bool ok = auth_gate(req); return send(req);",
                "const bool ok = auth_gate(req); return send(req);",
                "bool ok = auth_gate(req); if (!ok) return ESP_OK;"):
        assert not marker_decides("{ " + bad, next(m for m in AUTH_MARKERS if m in bad)), bad
    # ...a const local the function then tests is the gate deciding...
    assert marker_decides("{ const bool ok = auth_gate(req);\n  if (!ok) return ESP_OK; }",
                          "auth_gate(")
    # ...but a `return` inside a lambda is the lambda deciding, not the
    # handler: a receipt_decide whose verdict is dropped decides nothing.
    dropped = ("{\n  receipt_decide(host_is_foreign(req),\n"
               "      [req]() { return bearer_present_and_valid(req); },\n"
               "      []() mutable -> bool { return provisioning_gate_take(); });\n"
               "  return send_provisioning_receipt(req);\n}")
    assert not any(marker_decides(dropped, m) for m in AUTH_MARKERS), dropped
    # ...and a deciding gate below an unconditional return is unreachable,
    # while an early `if (...) return` (rate limit) or a nested one is fine.
    assert not gate_guards("{\n  (void)auth_gate(req);\n  return send(req);\n"
                           "  if (!auth_gate(req)) return ESP_OK;\n}")
    assert gate_guards("{\n  if (!rate_limit_check(req)) return ESP_OK;\n"
                       "  if (!auth_gate(req)) return ESP_OK;\n  return send(req);\n}")
    assert gate_guards("{\n  if (x) {\n    return a;\n  }\n"
                       "  if (bearer_present_and_valid(req)) {\n    return b;\n  }\n}")
    # ...and a marker is a whole name, as the census spells its rows: a helper
    # whose name ends in one (`lan_auth_gate`, `setup_auth_gate`, canary-wap's
    # `api_auth_check`) is no gate, so a route it guards fails as ungated and
    # as never asking the Host, where the census would never see the helper.
    for helper in ("lan_auth_gate(", "setup_auth_gate(", "api_auth_check(",
                   "peer_bearer_present_and_valid(", "my_provisioning_gate_take("):
        body = "{\n  if (!" + helper + "req)) return ESP_OK;\n  return apply(req);\n}"
        assert not any(marker_decides(body, m) for m in AUTH_MARKERS), helper
        assert not gate_guards(body), helper
        weak = ('httpd_uri_t w = { .uri = "/api/w", .method = HTTP_POST, .handler = handle_w };\n'
                "static bool " + helper + "httpd_req_t* req) { return from_ap_subnet(req); }\n"
                "static esp_err_t handle_w(httpd_req_t* req) " + body + "\n")
        got = check_route_gates({"fx": weak}, parse_routes("fx", weak))[0]
        assert any("handler handle_w (fx) has NO credential gate" in f for f in got), (helper, got)
        got = check_host_first({"fx": weak}, require_defs=False)
        assert any("handle_w (fx) never asks the Host" in f for f in got), (helper, got)

    # 4. Argument splitting survives nested calls and a comma in a string.
    calls = call_args('x(); send_html_with_token(req, pick("a,b", 2), true);\n'
                      'static esp_err_t send_html_with_token(httpd_req_t* req, '
                      'const char* h, bool i) { return ESP_OK; }\n', "send_html_with_token")
    assert [a for _, a in calls] == [["req", 'pick("a,b", 2)', "true"]], calls

    # 5. Registration-site and budget rules see the two sibling-branch shapes.
    table = (
        "#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK\n"
        "static const uint16_t kRouteTableSlots = 3;\n#else\n"
        "static const uint16_t kRouteTableSlots = 2;\n#endif\n"
        "void Mgr::registerHttpHandlers(httpd_handle_t server) {\n"
        '  httpd_uri_t a = { .uri = "/a", .method = HTTP_GET, .handler = ha };\n'
        "  register_route(server, &a);\n"
        '  httpd_uri_t b = { .uri = "/b", .method = HTTP_GET, .handler = hb };\n'
        "  register_route(m_http_server, &b);\n"
        "#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK\n"
        '  httpd_uri_t c = { .uri = "/c", .method = HTTP_GET, .handler = hc };\n'
        "  register_route(server, &c);\n#endif\n}\n")
    site = check_registration_sites({"fx": table})
    assert len(site) == 1 and "m_http_server" in site[0], site
    assert check_route_budgets({"fx": table}) == [], check_route_budgets({"fx": table})
    over = table.replace("kRouteTableSlots = 3;", "kRouteTableSlots = 2;")
    assert any("with the mesh" in f for f in check_route_budgets({"fx": over})), over

    # 5b. A handler defined twice (an #if body and its #else one) is read
    #     body by body: the #else body without its gate fails both rules.
    twice = ('httpd_uri_t w = { .uri = "/api/w", .method = HTTP_POST, .handler = handle_w };\n'
             "#if FEATURE_X\nstatic esp_err_t handle_w(httpd_req_t* req) {\n"
             "  if (!auth_gate(req)) return ESP_OK;\n  return apply(req);\n}\n#else\n"
             "static esp_err_t handle_w(httpd_req_t* req) {\n  return apply(req);\n}\n#endif\n")
    got = check_route_gates({"fx": twice}, parse_routes("fx", twice))[0]
    assert any("(one of its 2 definitions) has NO credential gate" in f for f in got), got
    got = check_host_first({"fx": twice}, require_defs=False)
    assert any("handle_w (fx) never asks the Host" in f for f in got), got
    # ...whatever the #else body's head: `static int` (esp_err_t is an int, so
    # it matches the forward declaration), an attribute, a trailing return
    # type, noexcept.
    for head in ("static int handle_w(httpd_req_t* req) {",
                 "static esp_err_t IRAM_ATTR handle_w(httpd_req_t* req) {",
                 "static auto handle_w(httpd_req_t* req) -> esp_err_t {",
                 "static esp_err_t handle_w(httpd_req_t* req) noexcept {"):
        other = twice.replace("#else\nstatic esp_err_t handle_w(httpd_req_t* req) {",
                              "#else\n" + head)
        got = check_route_gates({"fx": other}, parse_routes("fx", other))[0]
        assert any("(one of its 2 definitions) has NO credential gate" in f for f in got), (
            head, got)
        got = check_host_first({"fx": other}, require_defs=False)
        assert any("handle_w (fx) never asks the Host" in f for f in got), (head, got)
    both = twice.replace("{\n  return apply(req);",
                         "{\n  if (!auth_gate(req)) return ESP_OK;\n  return apply(req);")
    assert check_route_gates({"fx": both}, parse_routes("fx", both))[0] == []
    assert check_host_first({"fx": both}, require_defs=False) == []
    for second, why in (("  (void)auth_gate(req);\n  return apply(req);", "ignores its result"),
                        ("  return apply(req);\n  if (!auth_gate(req)) return ESP_OK;",
                         "returns unconditionally before its first credential gate")):
        late = twice.replace("{\n  return apply(req);", "{\n" + second)
        got = check_route_gates({"fx": late}, parse_routes("fx", late))[0]
        assert any("(one of its 2 definitions)" in f and why in f for f in got), (second, got)

    # 6. The Host first on the receipt: the shapes the rule refuses (each a
    #    review mutation that the Host-first rule alone passed), and the fixed
    #    ones.
    reg = ('httpd_uri_t r = { .uri = "/api/provisioning-receipt", .method = HTTP_GET, '
           '.handler = handle_provisioning_receipt };\n')
    emit = ("static esp_err_t send_provisioning_receipt(httpd_req_t* req) {\n"
            "  doc[\"token\"] = auth_get_token();\n  return ESP_OK;\n}\n")
    def receipt(body):
        return {"fx": reg + emit + "static esp_err_t handle_provisioning_receipt("
                "httpd_req_t* req) {\n" + body + "}\n"}
    def red(body, *needles):
        got = check_host_first(receipt(body), require_defs=False)
        assert any(all(n in f for n in needles) for f in got), (needles, body, got)
    def green(body):
        got = check_host_first(receipt(body), require_defs=False)
        assert got == [], (body, got)
        handler = function_body(receipt(body)["fx"], "handle_provisioning_receipt")
        assert gate_guards(handler), ("main()'s gate rule refuses", body)

    # The if-form: a refusal that returns, then the bearer, then the tap.
    no_host = ("  if (bearer_present_and_valid(req)) return send_provisioning_receipt(req);\n"
               "  if (!provisioning_gate_take()) return ESP_OK;\n"
               "  return send_provisioning_receipt(req);\n")
    red(no_host, "never asks the Host")
    red("  if (bearer_present_and_valid(req)) return send_provisioning_receipt(req);\n"
        "  if (host_is_foreign(req)) return ESP_OK;\n"
        "  if (!provisioning_gate_take()) return ESP_OK;\n"
        "  return send_provisioning_receipt(req);\n",
        "before the Host check", "bearer_present_and_valid")
    # The accounted grant itself, read before the Host (F340): it strikes
    # a wrong bearer against its address, so a rebound page could spend a
    # host's backoff (or test a token) under a name that is not this device.
    red("  if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
        "AuthManager::BearerResult::OK)\n"
        "    return send_provisioning_receipt(req);\n"
        "  if (host_is_foreign(req)) return ESP_OK;\n"
        "  if (!provisioning_gate_take()) return ESP_OK;\n"
        "  return send_provisioning_receipt(req);\n",
        "before the Host check", "auth_bearer_grant")
    for not_a_check in (
            "  host_is_foreign(req);\n",                                # answer dropped
            "  const bool foreign = host_is_foreign(req);\n",           # local unread
            "  if (host_is_foreign(req)) { Serial.println(\"x\"); }\n",  # only logs
            "  const bool foreign = host_is_foreign(req);\n"
            "  if (foreign) Serial.println(\"x\");\n",                  # local only logs
            "  if (host_is_foreign(req) && false) return ESP_OK;\n",    # mixed condition
            "  if (!host_is_foreign(req)) return ESP_OK;\n",            # inverted
            "  if (host_is_foreign(req)) return send_provisioning_receipt(req);\n",  # serves
            "  if (host_is_foreign(req)) {\n    if (!provisioning_gate_take()) return ESP_OK;\n"
            "    return ESP_OK;\n  }\n",                                 # spends the tap
            "  const bool foreign = host_is_foreign(req);\n"
            "  {\n    const bool foreign = false;\n"
            "    if (foreign) return ESP_OK;\n  }\n"):                  # shadowed
        red(not_a_check + no_host, "never asks the Host")
    for gate in ("static bool auth_gate(httpd_req_t* req) {\n"
                 "  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n"
                 "    return true;\n  }\n  const char* token = auth_get_token();\n"
                 "  return auth_check(req, token);\n}\n",
                 "static bool auth_gate(httpd_req_t* req) {\n"
                 "  if (host_is_foreign(req)) return auth_check(req, auth_get_token());\n"
                 "  const char* token = auth_get_token();\n"
                 "  return auth_check(req, token);\n}\n"):
        got = check_host_first({"fx": gate}, require_defs=False)
        assert any("auth_gate no longer refuses a foreign Host" in f for f in got), (gate, got)
    good_gate = ("static bool auth_gate(httpd_req_t* req) {\n"
                 "  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n"
                 "    return false;\n  }\n  const char* token = auth_get_token();\n"
                 "  return auth_check(req, token, request_peer_ipv4(req));\n}\n")
    assert check_host_first({"fx": good_gate}, require_defs=False) == []
    early_grant = good_gate.replace(
        "  if (host_is_foreign(req)) {",
        "  if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
        "AuthManager::BearerResult::HELD) return false;\n  if (host_is_foreign(req)) {")
    got = check_host_first({"fx": early_grant}, require_defs=False)
    assert any("auth_gate reads the token (auth_bearer_grant) before its Host check" in f
               for f in got), (early_grant, got)
    green("  if (host_is_foreign(req)) return ESP_OK;\n" + no_host)
    green("  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n"
          "    return ESP_OK;\n  }\n" + no_host)
    green("  const bool foreign = host_is_foreign(req);\n"
          "  if (foreign) {\n    send_host_refusal(req);\n    return ESP_OK;\n  }\n" + no_host)

    # The decision form: receipt_decide gets the Host, and the dispatch fails
    # closed (the receipt only under SERVE_*, the Host refusal at the end).
    decide = ("  const ReceiptVerdict verdict = receipt_decide(host_is_foreign(req),\n"
              "      [req]() { return bearer_present_and_valid(req); },\n"
              "      []() { return provisioning_gate_take(); });\n")
    serve_bearer = ("  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n"
                    "    return send_provisioning_receipt(req);\n  }\n")
    serve_tap = ("  if (verdict == ReceiptVerdict::SERVE_TAP) {\n"
                 "    const esp_err_t result = send_provisioning_receipt(req);\n"
                 "    log(\"served\");\n    return result;\n  }\n")
    no_tap = ("  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n"
              "    return httpd_resp_sendstr(req, \"{}\");\n  }\n")
    tail = "  send_host_refusal(req);\n  return ESP_OK;\n"
    green(decide + serve_bearer + serve_tap + no_tap + tail)
    green(decide + "  if (verdict == ReceiptVerdict::SERVE_BEARER) "
          "return send_provisioning_receipt(req);\n" + serve_tap + no_tap + tail)
    green(decide + "  if (verdict == ReceiptVerdict::SERVE_BEARER ||\n"
          "      verdict == ReceiptVerdict::SERVE_TAP) {\n"
          "    return send_provisioning_receipt(req);\n  }\n" + no_tap + tail)
    # The reviewed shape: branches for three verdicts, the receipt for the rest.
    refuse_host = ("  if (verdict == ReceiptVerdict::REFUSE_HOST) {\n"
                   "    send_host_refusal(req);\n    return ESP_OK;\n  }\n")
    open_tail = "  return send_provisioning_receipt(req);\n"
    outside = "sends the receipt outside an explicit"
    red(decide + refuse_host + serve_bearer + no_tap + open_tail, outside)
    red(decide + serve_bearer + no_tap + open_tail, outside)  # the refusal dropped
    red(decide + refuse_host.replace("REFUSE_HOST", "SERVE_TAP") + serve_bearer +
        no_tap + open_tail, outside)                         # the wrong enumerator
    red(decide + "  (void)verdict;\n" + open_tail, outside)  # the verdict ignored
    body = decide + "  (void)verdict;\n" + open_tail
    handler = function_body(receipt(body)["fx"], "handle_provisioning_receipt")
    assert not any(marker_decides(handler, m) for m in AUTH_MARKERS), \
        "main()'s gate rule accepts a receipt_decide verdict nobody reads"
    # A logged Host, then the decision told the Host is fine.
    red("  if (host_is_foreign(req)) { Serial.println(\"foreign\"); }\n" +
        decide.replace("receipt_decide(host_is_foreign(req),", "receipt_decide(false,") +
        serve_bearer + serve_tap + no_tap + tail,
        "gives receipt_decide 'false' as its Host verdict")
    for wrong in ("!host_is_foreign(req)", "host_is_foreign(nullptr)", "foreign"):
        red(decide.replace("host_is_foreign(req),", wrong + ","),
            f"gives receipt_decide {wrong!r}")
    red("  bool foreign = host_is_foreign(req);\n" +
        decide.replace("host_is_foreign(req),", "foreign,") + serve_bearer + tail,
        "gives receipt_decide 'foreign'")                    # a mutable local
    # The fail-closed dispatch, broken one hunk at a time.
    red(decide + serve_bearer.replace("== ReceiptVerdict::SERVE_BEARER",
                                      "!= ReceiptVerdict::REFUSE_NO_TAP") + tail, outside)
    red(decide + serve_bearer.replace("SERVE_BEARER", "REFUSE_HOST") + tail, outside)
    red(decide + serve_bearer.replace("SERVE_BEARER) {", "SERVE_BEARER || true) {") +
        tail, outside)
    red(decide + "  if (verdict == ReceiptVerdict::SERVE_TAP) {\n    log(\"x\");\n"
        "  } else {\n    return send_provisioning_receipt(req);\n  }\n" + tail, outside)
    red(decide + "  if (verdict == ReceiptVerdict::SERVE_TAP) {\n  }\n" + open_tail, outside)
    red(decide + serve_bearer + serve_tap + no_tap + "  return ESP_OK;\n",
        "does not end in the Host refusal")
    red(decide + serve_bearer + serve_tap + no_tap +
        "  if (verdict == ReceiptVerdict::REFUSE_HOST) send_host_refusal(req);\n"
        "  return ESP_OK;\n", "does not end in the Host refusal")
    red(decide + serve_bearer + "  return ESP_OK;\n" + tail,
        "does not end in the Host refusal")                  # the tail unreachable
    red(decide.replace("const ReceiptVerdict", "ReceiptVerdict") + serve_bearer + tail,
        "must be bound once")
    red(decide + "  {\n    const ReceiptVerdict verdict = ReceiptVerdict::SERVE_BEARER;\n"
        "    if (verdict == ReceiptVerdict::SERVE_BEARER) "
        "return send_provisioning_receipt(req);\n  }\n" + tail, "is written 2 times")
    # ...an emitter called from an unregistered helper is held to it too.
    helper = {"fx": emit + "static esp_err_t other(httpd_req_t* req) {\n"
              "  return send_provisioning_receipt(req);\n}\n"}
    got = check_host_first(helper, require_defs=False)
    assert any("other calls send_provisioning_receipt" in f for f in got), got

    # 7. The page path: the Host is page_token_decide's first argument, read
    #    before the grants and the tap.
    def inject(body):
        return {"fx": "static PageToken page_token_inject(httpd_req_t* req) {\n" + body + "}\n"}
    def inject_red(body, *needles):
        got = check_host_first(inject(body), require_defs=False)
        assert any(all(n in f for n in needles) for f in got), (needles, body, got)
    tap = "      []() { return provisioning_gate_take(); });\n"
    inject_red("  const bool bearer_ok = bearer_present_and_valid(req);\n"
               "  return page_token_decide(false, bearer_ok, false,\n" + tap,
               "does not decide the Host first")
    inject_red("  const bool bearer_ok = bearer_present_and_valid(req);\n"
               "  const bool foreign = host_is_foreign(req);\n"
               "  return page_token_decide(foreign, bearer_ok, false,\n" + tap,
               "before the Host check")
    inject_red("  const bool bearer_ok = auth_bearer_grant(req, t, request_peer_ipv4(req)) ==\n"
               "      AuthManager::BearerResult::OK;\n"
               "  const bool foreign = host_is_foreign(req);\n"
               "  return page_token_decide(foreign, bearer_ok, false,\n" + tap,
               "page_token_inject runs auth_bearer_grant before the Host check")
    good_inject = ("  const bool foreign = host_is_foreign(req);\n"
                   "  const bool bearer_ok = !foreign && bearer_present_and_valid(req);\n"
                   "  return page_token_decide(foreign, bearer_ok, false,\n" + tap)
    assert check_host_first(inject(good_inject), require_defs=False) == []
    assert check_host_first(inject("  return page_token_decide(host_is_foreign(req), "
                                   "setup_active(), false,\n" + tap),
                            require_defs=False) == []
    # A Host local that only logs, or an early refusal-shaped if, does not
    # stand in for the decision being told.
    for logged in ("  if (foreign) Serial.println(\"foreign\");\n",
                   "  if (host_is_foreign(req)) return PageToken::WITHHOLD;\n"):
        inject_red(good_inject.replace("  const bool bearer_ok", logged + "  const bool bearer_ok")
                   .replace("page_token_decide(foreign,", "page_token_decide(false,"),
                   "does not decide the Host first")
    for wrong in ("!foreign", "bearer_ok"):
        inject_red(good_inject.replace("page_token_decide(foreign,",
                                       f"page_token_decide({wrong},"),
                   f"gives page_token_decide {wrong!r}")
    inject_red(good_inject.replace("const bool foreign", "bool foreign"),
               "gives page_token_decide 'foreign'")
    inject_red(good_inject.replace("  return page_token_decide(",
                                   "  {\n  const bool foreign = false;\n  return page_token_decide(")
               + "  }\n", "gives page_token_decide 'foreign'")      # shadowed

    # 8. The page streams the token only on INJECT. (A stray reader of the
    #    token is the bearer-only answer rule's: _assert_answer_gate_rule.)
    def send(cond, sig="PageToken verdict", pre=""):
        return {"fx": "static esp_err_t send_html_with_token(httpd_req_t* req, "
                f"const char* h, {sig}) {{\n" + pre +
                f"  const char* token = {cond} ? auth_get_token() : \"\";\n"
                "  return ESP_OK;\n}\n"}
    assert check_host_first(send("(verdict == PageToken::INJECT)"), require_defs=False) == []
    assert check_host_first(send("verdict == canary::net::provisioning_gate::PageToken::INJECT"),
                            require_defs=False) == []
    for wrong in ("(verdict != PageToken::WITHHOLD)", "(verdict != PageToken::INJECT)",
                  "(verdict == PageToken::FOREIGN_HOST || verdict == PageToken::INJECT)",
                  "(true || verdict == PageToken::INJECT)", "(other == PageToken::INJECT)",
                  "(inject && !foreign)"):
        got = check_host_first(send(wrong), require_defs=False)
        assert any("outside a PageToken::INJECT test" in f for f in got), (wrong, got)
    got = check_host_first(send("(verdict == PageToken::INJECT)",
                                pre="  verdict = PageToken::INJECT;\n"), require_defs=False)
    assert any("reassigns its verdict" in f for f in got), got
    got = check_host_first(send("(inject)", sig="bool inject"), require_defs=False)
    assert any("no longer takes the page-token verdict" in f for f in got), got

    # 9. host_is_foreign is pinned (its GLUE_DEFS row, held to the tree's
    #    text here): reformatting passes, any other edit fails.
    hif = ("static bool host_is_foreign(httpd_req_t* req) {\n"
           "  if (from_ap_subnet(req)) return false;\n  char host[96];\n"
           "  if (httpd_req_get_hdr_value_str(req, \"Host\", host,\n"
           "                                  sizeof(host)) != ESP_OK) return true;\n"
           "  return !canary::net::host_names_this_device(host);\n}\n")
    row = {("fx/" + _NET, "host_is_foreign"): GLUE_DEFS[(_NET, "host_is_foreign")]}
    assert glue_def_problems({"fx/" + _NET: hif}, row) == []
    for mutant in (hif.replace("{\n  if (from_ap", "{\n  return false;\n  if (from_ap"),
                   hif.replace("ESP_OK) return true;", "ESP_OK) return false;"),
                   hif.replace("return !canary", "return canary"),
                   hif.replace("from_ap_subnet(req)) return", "from_ap_subnet(req) || 1) return")):
        got = glue_def_problems({"fx/" + _NET: mutant}, row)
        assert any("host_is_foreign changed" in f for f in got), (mutant, got)


# ── The bearer-only answers: fixtures ─────────────────────────────────────────
#
# A small tree on the post-F340 auth API (check, grant, compare, handedToken,
# the three wrappers), with the receipt and its dispatch and SERVE_TAP clear,
# /api/status behind its refusal answering the pin, a page, a public roll-call,
# the bearer glue, the accessors and device-side uses (the two mints among
# them). The clean tree passes; every plant fails for its named reason.

_FIX_AUTH = "fx/lib/securacv_auth.cpp"
_FIX_CRYPTO = "fx/lib/securacv_crypto.cpp"
_FIX_NET_H = "fx/lib/securacv_network.h"
_FIX_NET = "fx/lib/securacv_network.cpp"
_FIX_MAIN = "fx/src/main.cpp"
_FIX_OTHER = "fx/lib/securacv_other.cpp"
_FIX_WITNESS = "fx/lib/securacv_witness.cpp"
_FIX_SETUP = "fx/lib/securacv_setup.cpp"
_FIX_SETUP_H = "fx/lib/securacv_setup.h"
_FIX_MINT_SRC = ("  if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
                 "sizeof(g_ap_password))) {\n"
                 "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
                 "    while (true) { delay(1000); }\n  }\n")
_FIX_MINT = ("if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
             "sizeof(g_ap_password))) { Serial.println(\"[!!] Setup network key derivation "
             "failed - HALTING\"); while (true) { delay(1000); } }")
# setup()'s start of the network, in the tree's block and conditional, and its
# serial banner.
_FIX_SETUP_NET = (
    "  {\n"
    "#if FEATURE_WIFI_AP\n"
    "    const char* ap_ssid = device.ap_ssid;\n"
    "    ScvNetworkManager& net = network_get_instance();\n"
    "    network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n"
    "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n"
    "      net.startHttpServer();\n    }\n"
    "#endif\n"
    "  }\n")
# Where the private key is established, before every mint (MINT_AFTER).
_FIX_PROVISION = ("  if (!witness_provision_device()) {\n"
                  "    Serial.println(\"[!!] Device provisioning failed - HALTING\");\n"
                  "    while (true) { delay(1000); }\n  }\n")
_FIX_SETUP_PRINT = ("#if FEATURE_WIFI_AP\n"
                    "  Serial.printf(\"Password: %s\\n\", g_ap_password);\n"
                    "#endif\n")
_FIX_BEGIN_COPY = ("  if (ap_password) {\n"
                   "    strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);\n"
                   "    m_ap_password[sizeof(m_ap_password) - 1] = '\\0';\n"
                   "  } else {\n"
                   "    m_ap_password[0] = '\\0';\n  }\n")
_FIX_FILES = {
    _FIX_AUTH: (
        "static AuthManager s_auth;\n"
        "AuthManager& auth_get_instance() {\n  return s_auth;\n}\n"
        "AuthManager::AuthManager() : m_table{} {}\n"
        "bool AuthManager::constantTimeCompare(const void* a, const void* b, size_t len) {\n"
        "  const volatile uint8_t* pa = (const volatile uint8_t*)a;\n"
        "  const volatile uint8_t* pb = (const volatile uint8_t*)b;\n"
        "  volatile uint8_t diff = 0;\n"
        "  for (size_t i = 0; i < len; i++) diff |= pa[i] ^ pb[i];\n"
        "  return diff == 0;\n}\n"
        "void AuthManager::handedToken(uint32_t source) {\n"
        "  canary::net::auth_backoff::record_success(m_table, source);\n}\n"
        "AuthManager::Presented AuthManager::compare(httpd_req_t* req,\n"
        "                                            const char* expected_token) {\n"
        "  char auth_buf[128];\n"
        "  if (httpd_req_get_hdr_value_str(req, \"Authorization\", auth_buf, sizeof(auth_buf)) "
        "!= ESP_OK) {\n    return Presented::UNREADABLE;\n  }\n"
        "  if (strncmp(auth_buf, \"Bearer \", 7) != 0) return Presented::NOT_BEARER;\n"
        "  const char* token = auth_buf + 7;\n"
        "  const size_t expected_len = strlen(expected_token);\n"
        "  if (expected_len == 0 || strlen(token) != expected_len) return Presented::WRONG;\n"
        "  return constantTimeCompare(token, expected_token, expected_len) ? Presented::RIGHT\n"
        "                                                                   : Presented::WRONG;\n"
        "}\n"
        "AuthManager::BearerResult AuthManager::grant(httpd_req_t* req,\n"
        "                                             const char* expected_token,\n"
        "                                             uint32_t source) {\n"
        "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
        "  return canary::net::auth_backoff::grant(\n"
        "      m_table, source, millis(), presented,\n"
        "      [&]() { return compare(req, expected_token) == Presented::RIGHT; });\n}\n"
        "bool AuthManager::check(httpd_req_t* req, const char* expected_token, uint32_t source) {\n"
        "  const uint32_t now = millis();\n"
        "  if (canary::net::auth_backoff::locked(m_table, source, now)) {\n"
        "    httpd_resp_set_status(req, \"429 Too Many Requests\");\n"
        "    return false;\n  }\n"
        "  Presented why = Presented::WRONG;\n"
        "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
        "  const BearerResult result = canary::net::auth_backoff::grant(\n"
        "      m_table, source, now, presented, [&]() {\n"
        "        why = compare(req, expected_token);\n"
        "        return why == Presented::RIGHT;\n      });\n"
        "  if (result == BearerResult::OK) return true;\n"
        "  httpd_resp_set_status(req, why == Presented::WRONG ? \"403 Forbidden\" "
        ": \"401 Unauthorized\");\n"
        "  return false;\n}\n"
        "bool auth_check(httpd_req_t* req, const char* expected_token, uint32_t source) {\n"
        "  return auth_get_instance().check(req, expected_token, source);\n}\n"
        "AuthManager::BearerResult auth_bearer_grant(httpd_req_t* req,\n"
        "                                            const char* expected_token,\n"
        "                                            uint32_t source) {\n"
        "  return auth_get_instance().grant(req, expected_token, source);\n}\n"
        "void auth_handed_token(uint32_t source) {\n"
        "  auth_get_instance().handedToken(source);\n}\n"
        "static char s_bearer[36];\n"
        "bool auth_load_or_derive(const uint8_t privkey[32]) {\n"
        "  bool loaded = false;\n"
        "  if (!loaded) {\n"
        "    if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) {\n"
        "      s_bearer[0] = '\\0';\n      return false;\n    }\n  }\n"
        "  return true;\n}\n"
        "const char* auth_get_token() {\n  return s_bearer;\n}\n"),
    _FIX_WITNESS: (
        "static bool establish_identity_key() {\n"
        "  return nvs_load_key(g_device.privkey) == KeyLoad::Loaded;\n}\n"
        "bool witness_provision_device() {\n"
        "  if (!establish_identity_key()) return false;\n"
        "  if (!auth_load_or_derive(g_device.privkey)) {\n"
        "    Serial.println(\"[!!] Bearer credential provisioning failed\");\n"
        "    return false;\n  }\n"
        "  return true;\n}\n"),
    _FIX_CRYPTO: (
        "bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data,\n"
        "                 size_t data_len, uint8_t out[32]) {\n"
        "  return mbed_hmac(key, key_len, data, data_len, out);\n}\n"
        "static const char UNAMBIGUOUS_ALPHABET[] =\n"
        "  \"23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz\";\n"
        "static const size_t UNAMBIGUOUS_LEN = sizeof(UNAMBIGUOUS_ALPHABET) - 1;\n"
        "void format_api_token_string(const uint8_t* input, size_t in_len, char* output,\n"
        "                             size_t out_len) {\n"
        "  for (size_t i = 0; i < in_len && i + 1 < out_len; i++) {\n"
        "    output[i] = UNAMBIGUOUS_ALPHABET[input[i] % UNAMBIGUOUS_LEN];\n  }\n}\n"
        "static void unambiguous_suffix16(uint16_t v, char out[5]) {\n"
        "  for (int i = 0; i < 4; i++) {\n"
        "    out[i] = UNAMBIGUOUS_ALPHABET[v % UNAMBIGUOUS_LEN];\n"
        "    v = (uint16_t)(v / UNAMBIGUOUS_LEN);\n  }\n}\n"
        "bool derive_api_token(const uint8_t privkey[32], char* token_str,\n"
        "                      size_t token_str_len) {\n"
        "  if (!token_str || token_str_len < 36) return false;\n"
        "  return hmac_into(privkey, token_str, token_str_len);\n}\n"
        "static esp_err_t stored_key_length(size_t* len) {\n"
        "  return nvs_get_blob(s_nvs, NVS_KEY_PRIV, nullptr, len);\n}\n"
        "KeyLoad nvs_load_key(uint8_t priv[32]) {\n"
        "  return nvs.getBytes(NVS_KEY_PRIV, priv, 32) == 32 ? KeyLoad::Loaded\n"
        "                                                     : KeyLoad::Absent;\n}\n"
        "bool nvs_store_key(const uint8_t priv[32]) {\n"
        "  return nvs.putBytes(NVS_KEY_PRIV, priv, 32) == 32;\n}\n"),
    _FIX_NET_H: (
        "struct ScvNetworkManager {\n"
        "  ScvNetworkManager();\n"
        "  const char* getTlsCertFp() const { return m_tls_cert_fp_hex; }\n"
        "  const char* getApPassword() const { return m_ap_password; }\n"
        "  bool isTlsEnabled() const { return m_tls_enabled; }\n"
        "  httpd_handle_t getHttpsServer() const { return m_https_server; }\n"
        "  httpd_handle_t getHttpServer() const {\n"
        "    return m_https_server ? m_https_server : m_http_server;\n  }\n"
        "  char m_ap_password[65];\n"
        "  char m_tls_cert_fp_hex[65];\n"
        "  httpd_handle_t m_http_server;\n"
        "  httpd_handle_t m_https_server;\n"
        "  bool m_tls_enabled;\n"
        "};\n"),
    _FIX_NET: (
        "#if FEATURE_HTTPS && \\\n    defined(CONFIG_ESP_HTTPS_SERVER_ENABLE)\n"
        "#define SECURACV_HAS_HTTPS_SERVER 1\n#endif\n"
        "static ScvNetworkManager s_network;\n"
        "ScvNetworkManager& network_get_instance() {\n  return s_network;\n}\n"
        "ScvNetworkManager::ScvNetworkManager() : m_http_server(nullptr) {\n"
        "  m_https_server = nullptr;\n  m_tls_enabled = false;\n}\n"
        "bool ScvNetworkManager::startHttpServer() {\n"
        "  const Mode mode = canary::net::tls_policy::decide(FEATURE_HTTPS != 0);\n"
        "#if SECURACV_HAS_HTTPS_SERVER\n"
        "  if (mode == Mode::HTTPS_REDIRECT) {\n"
        "    httpd_ssl_config_t ssl = HTTPD_SSL_CONFIG_DEFAULT();\n"
        "    if (httpd_ssl_start(&m_https_server, &ssl) == ESP_OK) {\n"
        "      m_tls_enabled = true;\n"
        "      registerHttpHandlers(m_https_server);\n"
        "      return true;\n    }\n"
        "    m_https_server = nullptr;\n    m_tls_enabled = false;\n  }\n"
        "#endif\n"
        "  return httpd_start(&m_http_server, nullptr) == ESP_OK;\n}\n"
        "static void netif_ip4(const char* ifkey, uint32_t* ip, uint32_t* mask) {\n"
        "  esp_netif_t* nif = esp_netif_get_handle_from_ifkey(ifkey);\n"
        "  *ip = nif ? ip_of(nif) : 0;\n  *mask = nif ? mask_of(nif) : 0;\n}\n"
        "static bool from_ap_subnet(httpd_req_t* req) {\n"
        "  if (!(WiFi.getMode() & WIFI_AP)) return false;\n"
        "  uint32_t ap_ip, ap_mask, sta_ip, sta_mask;\n"
        "  netif_ip4(\"WIFI_AP_DEF\", &ap_ip, &ap_mask);\n"
        "  netif_ip4(\"WIFI_STA_DEF\", &sta_ip, &sta_mask);\n"
        "  return canary::net::provisioning_gate::request_on_softap(\n"
        "      peer_of(req), local_of(req), ap_ip, ap_mask, sta_ip, sta_mask);\n}\n"
        "static bool host_is_foreign(httpd_req_t* req) {\n"
        "  if (from_ap_subnet(req)) return false;\n"
        "  char host[96];\n"
        "  if (httpd_req_get_hdr_value_str(req, \"Host\", host, sizeof(host)) != ESP_OK) "
        "return true;\n"
        "  return !canary::net::host_names_this_device(host);\n}\n"
        "static uint32_t request_peer_ipv4(httpd_req_t* req) {\n  return peer_of(req);\n}\n"
        "static void send_host_refusal(httpd_req_t* req) {\n"
        "  httpd_resp_set_status(req, \"403 Forbidden\");\n"
        "  httpd_resp_sendstr(req, \"{\\\"error\\\":\\\"host\\\"}\");\n}\n"
        "static bool auth_gate(httpd_req_t* req) {\n"
        "  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n    return false;\n  }\n"
        "  const char* token = auth_get_token();\n"
        "  if (!token || token[0] == '\\0') {\n    return false;\n  }\n"
        "  if (!auth_check(req, token, request_peer_ipv4(req))) {\n    return false;\n  }\n"
        "  return true;\n}\n"
        "static network_gate_fn_t s_gate_take = nullptr;\n"
        "static network_gate_fn_t s_gate_is_open = nullptr;\n"
        "void network_set_provisioning_gate_hooks(network_gate_fn_t take,\n"
        "                                         network_gate_fn_t is_open) {\n"
        "  s_gate_take = take;\n  s_gate_is_open = is_open;\n}\n"
        "static bool provisioning_gate_take() { return s_gate_take ? s_gate_take() : false; }\n"
        "static bool bearer_present_and_valid(httpd_req_t* req) {\n"
        "  const char* token = auth_get_token();\n"
        "  if (!token || token[0] == '\\0') return false;\n"
        "  return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n"
        "         AuthManager::BearerResult::OK;\n}\n"
        "static void announce_setup_network(const char* ssid, const char* pass) {\n"
        "  char line[256];\n"
        "  if (announce_line(line, sizeof(line), ssid, pass) > 0) Serial.println(line);\n}\n"
        "static void apply_ap_security(WiFiStatus& st) {\n"
        "  wifi_config_t c;\n  size_t pw_len = 0;\n"
        "  if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {\n"
        "    pw_len = strnlen((const char*)c.ap.password, sizeof(c.ap.password));\n  }\n"
        "  aps::Decision d = aps::decide(pw_len);\n"
        "#if CANARY_SOFTAP_SAE_IN_BUILD\n"
        "  if (d.mode == aps::AuthMode::WPA2_WPA3_TRANSITION) {\n"
        "    c.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;\n"
        "    const bool accepted = (esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK);\n"
        "    d = aps::after_driver(d, accepted);\n  }\n"
        "#endif\n"
        "  secure_zero(&c, sizeof(c));\n}\n"
        "static bool ensure_sta_pmf_capable() {\n"
        "#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)\n"
        "  return true;\n"
        "#else\n"
        "  wifi_config_t c;\n  bool capable = false;\n"
        "  if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK) {\n"
        "    capable = c.sta.pmf_cfg.capable;\n"
        "    if (!capable) {\n"
        "      c.sta.pmf_cfg.capable = true;\n"
        "      capable = (esp_wifi_set_config(WIFI_IF_STA, &c) == ESP_OK);\n    }\n  }\n"
        "  return capable;\n"
        "#endif\n}\n"
        "bool ScvNetworkManager::begin(const char* ap_ssid, const char* ap_password) {\n"
        + _FIX_BEGIN_COPY +
        "  WiFi.mode(WIFI_AP_STA);\n"
        "  bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, "
        "AP_MAX_CONNECTIONS);\n"
        "  apply_ap_security(m_status);\n"
        "  return ap_ok;\n}\n"
        "bool network_init(const char* ap_ssid, const char* ap_password, "
        "const char* device_id) {\n"
        "  return network_get_instance().begin(ap_ssid, ap_password, device_id);\n}\n"
        "void ScvNetworkManager::dropAp() {\n"
        "  WiFi.softAPdisconnect(true);\n  WiFi.mode(WIFI_STA);\n}\n"
        "void ScvNetworkManager::raiseAp() {\n"
        "  WiFi.mode(WIFI_AP_STA);\n"
        "  bool ap_ok = WiFi.softAP(m_ap_ssid, m_ap_password, AP_CHANNEL, false, "
        "AP_MAX_CONNECTIONS);\n"
        "  if (!ap_ok) return;\n"
        "  apply_ap_security(m_status);\n"
        "  announce_setup_network(m_ap_ssid, m_ap_password);\n}\n"
        "void ScvNetworkManager::idle() {\n  delay(1);\n}\n"
        "static bool tls_load_der_from_nvs(uint8_t** key, size_t* key_len) {\n"
        "  *key_len = nvs.getBytesLength(NVS_KEY_TLS_KEY);\n"
        "  *key = (uint8_t*)malloc(*key_len);\n"
        "  return *key && nvs.getBytes(NVS_KEY_TLS_KEY, *key, *key_len) == *key_len;\n}\n"
        "static bool tls_store_der_to_nvs(const uint8_t* key, size_t key_len) {\n"
        "  return nvs.putBytes(NVS_KEY_TLS_KEY, key, key_len) == key_len;\n}\n"
        "bool ScvNetworkManager::initTls() {\n"
        "  uint8_t* key = nullptr;\n  size_t key_len = 0;\n"
        "  if (!tls_load_der_from_nvs(&key, &key_len)) {\n"
        "    if (!make_key(&key, &key_len) || !tls_store_der_to_nvs(key, key_len)) return false;\n"
        "  }\n"
        "  m_tls_key_der = key;\n  return true;\n}\n"
        "static esp_err_t send_html_with_token(httpd_req_t* req, const char* html,\n"
        "                                      PageToken verdict) {\n"
        "  const char* token = (verdict == PageToken::INJECT) ? auth_get_token() : \"\";\n"
        "  return send_page(req, html, token);\n}\n"
        "static PageToken page_token_inject(httpd_req_t* req) {\n"
        "  const bool foreign      = host_is_foreign(req);\n"
        "  const bool setup_active = setup_is_active() || setup_is_first_boot();\n"
        "  const bool bearer_ok    = !foreign && bearer_present_and_valid(req);\n"
        "  const bool on_ap        = from_ap_subnet(req);\n"
        "  bool tap_spent = false;\n"
        "  const PageToken verdict = page_token_decide(\n"
        "      foreign, setup_active, bearer_ok, on_ap,\n"
        "      [&tap_spent]() {\n        tap_spent = provisioning_gate_take();\n"
        "        return tap_spent;\n      });\n"
        "  if (verdict != PageToken::INJECT) {\n    log_withheld();\n"
        "  } else if (tap_spent) {\n    auth_handed_token(request_peer_ipv4(req));\n  }\n"
        "  return verdict;\n}\n"
        "static esp_err_t handle_ui(httpd_req_t* req) {\n"
        "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n}\n"
        "static esp_err_t send_provisioning_receipt(httpd_req_t* req) {\n"
        "  ScvNetworkManager& net = network_get_instance();\n"
        "  JsonDocument doc;\n"
        "  doc[\"token\"] = auth_get_token();\n"
        "  doc[\"ap_password\"] = net.getApPassword();\n"
        "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n"
        "  return send_doc(req, doc);\n}\n"
        "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {\n"
        "  const ReceiptVerdict verdict = receipt_decide(\n"
        "      host_is_foreign(req),\n"
        "      [req]() { return bearer_present_and_valid(req); },\n"
        "      []() { return provisioning_gate_take(); });\n"
        "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n"
        "    return send_provisioning_receipt(req);\n  }\n"
        "  if (verdict == ReceiptVerdict::SERVE_TAP) {\n"
        "    auth_handed_token(request_peer_ipv4(req));\n"
        "    return send_provisioning_receipt(req);\n  }\n"
        "  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n"
        "    return httpd_resp_sendstr(req, \"{}\");\n  }\n"
        "  send_host_refusal(req);\n  return ESP_OK;\n}\n"
        "static esp_err_t handle_status(httpd_req_t* req) {\n"
        "  if (!rate_limit_check(req)) return ESP_OK;\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  ScvNetworkManager& net = network_get_instance();\n"
        "  JsonDocument doc;\n"
        "  doc[\"provisioning_gate\"] = (s_gate_take && s_gate_is_open) ? \"physical_button\" "
        ": \"unwired\";\n"
        "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n"
        "  return send_doc(req, doc);\n}\n"
        "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  return httpd_resp_sendstr(req, \"{}\");\n}\n"
        "static esp_err_t handle_witness(httpd_req_t* req) {\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  char query[64];\n"
        "  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) "
        "page_from(query);\n"
        "  return httpd_resp_sendstr(req, \"[]\");\n}\n"
        "static esp_err_t handle_fleet(httpd_req_t* req) {\n"
        "  prefs.begin(\"fleet\", true);\n"
        "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n"
        "  return httpd_resp_sendstr(req, fleet_body());\n}\n"
        "void ScvNetworkManager::registerHttpHandlers(httpd_handle_t server) {\n"
        "  httpd_uri_t a = { .uri = \"/\", .method = HTTP_GET, .handler = handle_ui };\n"
        "  httpd_uri_t b = { .uri = \"/api/provisioning-receipt\", .method = HTTP_GET,\n"
        "                    .handler = handle_provisioning_receipt };\n"
        "  httpd_uri_t c = { .uri = \"/api/status\", .method = HTTP_GET, "
        ".handler = handle_status };\n"
        "  httpd_uri_t d = { .uri = \"/api/wifi/connect\", .method = HTTP_POST,\n"
        "                    .handler = handle_wifi_connect };\n"
        "  httpd_uri_t e = { .uri = \"/api/witness\", .method = HTTP_GET, "
        ".handler = handle_witness };\n"
        "  httpd_uri_t f = { .uri = \"/api/fleet\", .method = HTTP_GET, "
        ".handler = handle_fleet };\n"
        "}\n"),
    _FIX_MAIN: (
        "static char g_ap_password[16];\n"
        "static canary::io::ResetHold g_reset_hold;\n"
        "static void factory_reset(canary::io::ResetScope scope) {\n"
        "  clear_scope(scope);\n  ESP.restart();\n}\n"
        "static void reset_hold_dispatch(canary::io::HoldEvent ev) {\n"
        "  if (ev == canary::io::HoldEvent::Released && g_reset_hold.released_stage == 2) {\n"
        "    factory_reset(canary::io::ResetScope::CredentialsAndSettings);\n  }\n}\n"
        "static canary::net::provisioning_gate::State g_prov_gate = {0};\n"
        "static bool prov_gate_take_hook() {\n"
        "  return canary::net::provisioning_gate::take(g_prov_gate, millis(), TTL);\n}\n"
        "static bool prov_gate_is_open_hook() {\n"
        "  return canary::net::provisioning_gate::is_open(g_prov_gate, millis(), TTL);\n}\n"
        "void setup() {\n"
        "  if (digitalRead(BOOT_BUTTON_GPIO) == LOW) {\n"
        "    for (;;) {\n"
        "      const bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n"
        "      reset_hold_dispatch(g_reset_hold.step(pressed, millis()));\n"
        "      if (!pressed) break;\n    }\n  }\n"
        + _FIX_PROVISION +
        "  DeviceIdentity& device = witness_get_device();\n"
        + _FIX_MINT_SRC + _FIX_SETUP_NET + _FIX_SETUP_PRINT +
        "}\n"
        "static void handle_boot_button() {\n"
        "  canary::net::provisioning_gate::expire(g_prov_gate, millis(), "
        "PROVISIONING_GATE_TTL_MS);\n"
        "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n"
        "  reset_hold_dispatch(g_reset_hold.step(pressed, millis()));\n"
        "  if (pressed && !boot_btn_was_pressed) {\n"
        "    boot_btn_was_pressed = true;\n"
        "  } else if (!pressed && boot_btn_was_pressed) {\n"
        "    if (duration >= BOOT_MEDIUM_PRESS_MS) {\n"
        "      print_status();\n"
        "    } else {\n"
        "      if (duration >= BOOT_SHORT_PRESS_MS && tap_is_usb_confirm) {\n"
        "        Serial.println(\"[AUTH] USB onboarding confirmed\");\n"
        "      } else if (duration >= BOOT_SHORT_PRESS_MS) {\n"
        "        canary::net::provisioning_gate::open(g_prov_gate, millis());\n"
        "      }\n    }\n  }\n}\n"
        "void loop() {\n"
        "  handle_boot_button();\n"
        "#if FEATURE_SETUP_WIZARD\n"
        "  if (setup_is_active()) {\n"
        "    setup_check_timeout();\n"
        "  }\n"
        "#endif\n"
        "}\n"),
    _FIX_SETUP: (
        "namespace setup_wiz {\n"
        "static bool s_initialized = false;\n"
        "static bool s_first_boot = false;\n"
        "static bool s_active = false;\n"
        "static uint32_t s_started_ms = 0;\n"
        "}\n"
        "bool setup_init(void) {\n"
        "  using namespace setup_wiz;\n"
        "  if (s_initialized) return true;\n"
        "  s_first_boot = !prefs.getBool(NVS_KEY_SETUP_OK, false);\n"
        "  if (s_first_boot) {\n    s_active = true;\n    s_started_ms = millis();\n  }\n"
        "  s_initialized = true;\n  return true;\n}\n"
        "bool setup_is_first_boot(void) {\n  return setup_wiz::s_first_boot;\n}\n"
        "bool setup_is_active(void) {\n  return setup_wiz::s_active;\n}\n"
        "void setup_mark_complete(void) {\n"
        "  using namespace setup_wiz;\n  prefs.putBool(NVS_KEY_SETUP_OK, true);\n"
        "  s_first_boot = false;\n  s_active = false;\n}\n"
        "void setup_check_timeout(void) {\n"
        "  using namespace setup_wiz;\n"
        "  if (!s_active) return;\n"
        "  if ((millis() - s_started_ms) >= SETUP_TIMEOUT_MS) {\n"
        "    prefs.putBool(NVS_KEY_SETUP_OK, true);\n"
        "    s_active = false;\n    s_first_boot = false;\n  }\n}\n"
        "bool setup_get_device_name(char* out, size_t len) {\n"
        "  return copy_name(out, len);\n}\n"),
    _FIX_SETUP_H: (
        "#define SETUP_DEVICE_NAME_MAX  32\n"
        "#define SETUP_TIMEOUT_MS       (15UL * 60UL * 1000UL)\n"),
}
_FIX_GLUE = ((_NET, "host_is_foreign"), (_NET, "auth_gate"), (_NET, "bearer_present_and_valid"),
             (_NET, "from_ap_subnet"), (_NET, "network_set_provisioning_gate_hooks"),
             (_NET, "provisioning_gate_take"), (_NET, "page_token_inject"),
             (_MAIN, "prov_gate_take_hook"), (_AUTH, "AuthManager::check"),
             (_AUTH, "AuthManager::grant"), (_AUTH, "AuthManager::compare"),
             (_AUTH, "AuthManager::handedToken"), (_AUTH, "auth_check"),
             (_AUTH, "auth_bearer_grant"), (_AUTH, "auth_handed_token"),
             (_AUTH, "AuthManager::constantTimeCompare"), (_AUTH, "auth_get_token"),
             (_NET_H, "getApPassword"), (_NET_H, "getTlsCertFp"),
             (_CRYPTO, "derive_api_token"), (_NET, "announce_setup_network"),
             (_NET, "apply_ap_security"), (_AUTH, "auth_get_instance"),
             (_CRYPTO, "hmac_sha256"), (_CRYPTO, "format_api_token_string"),
             (_SETUP, "setup_init"), (_SETUP, "setup_is_first_boot"),
             (_SETUP, "setup_is_active"), (_SETUP, "setup_mark_complete"),
             (_SETUP, "setup_check_timeout"), (_NET, "netif_ip4"), (_NET, "request_peer_ipv4"),
             (_MAIN, "prov_gate_is_open_hook"), (_MAIN, "handle_boot_button"),
             (_NET_H, "isTlsEnabled"), (_NET_H, "getHttpsServer"))
# The census rows whose callers or statements the fixture tree spells its own
# way (every other row is the tree's): one page handler, and raiseAp()'s one
# mode switch.
_FIX_CENSUS = {
    "send_html_with_token": dict(GATE_CALL_SITES["send_html_with_token"],
                                 callers={(_NET, "handle_ui")}),
    "WiFi.mode": dict(GATE_CALL_SITES["WiFi.mode"], stmts={
        (_NET, "ScvNetworkManager::begin"): ("WiFi.mode(WIFI_AP_STA);",),
        (_NET, "ScvNetworkManager::dropAp"): ("WiFi.softAPdisconnect(true);",
                                               "WiFi.mode(WIFI_STA);"),
        (_NET, "ScvNetworkManager::raiseAp"): ("WiFi.mode(WIFI_AP_STA);",)}),
}
# The NVS keys' values main() reads from include/canary_config.h.
_FIX_NVS_KEYS = {"NVS_KEY_TOKEN": "api_token", "NVS_KEY_TLS_KEY": "tls_key",
                 "NVS_KEY_SETUP_OK": "setup_ok", "NVS_KEY_PRIV": "privkey"}
_FIX_TABLES = {
    "local": {
        (_NET, "ScvNetworkManager::begin"): (
            "keeps the SoftAP password and raises the SoftAP with it",
            ("if (ap_password) {",
             "[within if (ap_password) {] strncpy(m_ap_password, ap_password, "
             "sizeof(m_ap_password) - 1);",
             "[within if (ap_password) {] m_ap_password[sizeof(m_ap_password) - 1] = '\\0';",
             "[within else {] m_ap_password[0] = '\\0';",
             "bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, "
             "AP_MAX_CONNECTIONS);")),
        (_NET, "network_init"): (
            "hands begin() its key",
            ("return network_get_instance().begin(ap_ssid, ap_password, device_id);",)),
        (_NET, "ScvNetworkManager::raiseAp"): (
            "raises the SoftAP",
            ("bool ap_ok = WiFi.softAP(m_ap_ssid, m_ap_password, AP_CHANNEL, false, "
             "AP_MAX_CONNECTIONS);",
             "announce_setup_network(m_ap_ssid, m_ap_password);")),
        (_NET, "apply_ap_security"): (
            "measures the driver's passphrase",
            ("if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {",
             "[within if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {] pw_len = "
             "strnlen((const char*)c.ap.password, sizeof(c.ap.password));")),
        (_MAIN, "setup"): (
            "mints the SoftAP password, starts the network with it, prints it",
            (_FIX_MINT, _BEGIN_IN_SETUP,
             "[within #if FEATURE_WIFI_AP] Serial.printf(\"Password: %s\\n\", g_ap_password);")),
        (_AUTH, "auth_load_or_derive"): (
            "mints the token",
            ("[within if (!loaded) {] if (!derive_api_token(privkey, s_bearer, "
             "sizeof(s_bearer))) { s_bearer[0] = '\\0'; return false; }",)),
        (_NET, "tls_load_der_from_nvs"): (
            "loads the TLS key into the buffer it is handed",
            ("*key_len = nvs.getBytesLength(NVS_KEY_TLS_KEY);",
             "return *key && nvs.getBytes(NVS_KEY_TLS_KEY, *key, *key_len) == *key_len;")),
        (_NET, "tls_store_der_to_nvs"): (
            "persists the TLS key it is handed",
            ("return nvs.putBytes(NVS_KEY_TLS_KEY, key, key_len) == key_len;",)),
        (_NET, "ScvNetworkManager::initTls"): (
            "keeps the TLS key for the server", ("m_tls_key_der = key;",)),
    },
    "answers": {"handle_provisioning_receipt": ("token ap pin", "receipt"),
                "handle_status": ("pin", "bearer"),
                "handle_ui": ("token", "page")},
    "answer_stmts": {(_NET, "handle_provisioning_receipt"): (),
                     (_NET, "handle_status"): ('doc["tls_cert_fp"] = net.getTlsCertFp();',),
                     (_NET, "handle_ui"): ()},
    "census": dict(GATE_CALL_SITES, **_FIX_CENSUS),
    "nvs_keys": _FIX_NVS_KEYS,
    "roots_defines": {"SECURACV_HAS_HTTPS_SERVER": (_NET, 1)},
    "outside": (("securacv_network.h", "char m_ap_password[65];"),
                ("securacv_network.h", "char m_tls_cert_fp_hex[65];"),
                (_AUTH, "static char s_bearer[36];"),
                (_MAIN, "static char g_ap_password[16];")),
    "public": {("GET", "/"): "the page", ("GET", "/api/fleet"): "the roll-call"},
    "emitter_stmts": {
        (_NET, "send_provisioning_receipt"): ('doc["token"] = auth_get_token();',
                                              'doc["ap_password"] = net.getApPassword();',
                                              'doc["tls_cert_fp"] = net.getTlsCertFp();'),
        (_NET, "send_html_with_token"): (
            'const char* token = (verdict == PageToken::INJECT) ? auth_get_token() : "";',),
    },
    "uri": {},
}


def _fixture_sources() -> dict:
    return {f: strip_comments(t) for f, t in _FIX_FILES.items()}


def _fixture_tables(sources: dict) -> dict:
    """_FIX_TABLES, with each _FIX_GLUE row pinned to the clean fixture's
    body under the tree's row's message (so a plant reads what the tree's
    failure says)."""
    glue = {}
    for suffix, qual in _FIX_GLUE:
        fname = next(f for f in sources if _in_file(f, suffix))
        a, b = definition_span(sources[fname], qual)
        glue[(suffix, qual)] = (GLUE_DEFS[(suffix, qual)][0], _fold(sources[fname][a:b]))
    return dict(_FIX_TABLES, glue=glue)


_PUBLIC_AP = "public route handler handle_fleet() answers the SoftAP password"
# handle_boot_button's body from its sweep to its end (a census row's order plant).
_FIX_BOOT_SWEEP = ("  canary::net::provisioning_gate::expire(g_prov_gate, millis(), "
                   "PROVISIONING_GATE_TTL_MS);\n")
_FIX_BOOT_BODY = _FIX_FILES[_FIX_MAIN][_FIX_FILES[_FIX_MAIN].index(_FIX_BOOT_SWEEP):
                                       _FIX_FILES[_FIX_MAIN].index("}\nvoid loop() {\n")]
_STATUS_REFUSAL = "handle_status calls auth_gate other than as the whole statement"
# The roll-call handler's head (a site to plant a reader or a macro before).
_FIX_FLEET = "static esp_err_t handle_fleet(httpd_req_t* req) {\n"
_PASTE = "a macro pastes tokens with ##"
_ASM_WORDS = "an asm label or statement reaches a function by its symbol"
# (what, file, old text, new text, words every one of which a problem names).
# An empty old text appends the new text to the file (a new file when absent).
_FIX_PLANTS = (
    ("M1: the public roll-call answers the SoftAP password", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  httpd_resp_set_hdr(req, \"X-AP\", network_get_instance().getApPassword());\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     (_PUBLIC_AP, "handle_fleet() reads the SoftAP password and is no")),
    ("M4: the roll-call answers it through a helper", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static void fill_ap(char* out, size_t cap) {\n"
     "  snprintf(out, cap, \"%s\", network_get_instance().getApPassword());\n}\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n  char buf[16];\n"
     "  fill_ap(buf, sizeof(buf));\n",
     (_PUBLIC_AP, "fill_ap() reads the SoftAP password and is no")),
    # A helper that re-derives a credential from the private key reads it by
    # its deriver alone, naming no buffer the credential is kept in.
    ("the roll-call answers the SoftAP key re-derived by a helper", _FIX_NET, _FIX_FLEET,
     "static bool fleet_setup_key(char* out, size_t cap) {\n"
     "  return canary::net::derive_ap_password(witness_get_device().privkey, out, cap);\n}\n"
     + _FIX_FLEET + "  char key[16];\n"
     "  if (fleet_setup_key(key, sizeof(key))) httpd_resp_set_hdr(req, \"X-Setup-Key\", key);\n",
     (_PUBLIC_AP, "fleet_setup_key() reads the SoftAP password and is no")),
    ("the roll-call answers the bearer re-derived by a helper", _FIX_NET, _FIX_FLEET,
     "static bool fleet_bearer(char* out, size_t cap) {\n"
     "  const DeviceIdentity& dev = witness_get_device();\n"
     "  return derive_api_token(dev.privkey, out, cap);\n}\n"
     + _FIX_FLEET + "  char t[40];\n"
     "  if (fleet_bearer(t, sizeof(t))) httpd_resp_set_hdr(req, \"X-T\", t);\n",
     ("public route handler handle_fleet() answers the bearer token",
      "fleet_bearer() reads the bearer token and is no")),
    # The carry runs to a fixed point: a two-hop chain whose helper is defined
    # after its caller carries what the far end answers.
    ("the roll-call answers the pin through a helper defined after it", _FIX_NET,
     "  return httpd_resp_sendstr(req, fleet_body());\n}\n",
     "  if (s_fleet_polled_ms == 0) return fleet_extra(req);\n"
     "  return httpd_resp_sendstr(req, fleet_body());\n}\n"
     "static esp_err_t fleet_extra(httpd_req_t* req) {\n  return handle_status(req);\n}\n",
     ("public route handler handle_fleet() answers the TLS pin",)),
    ("a gated route answers the pin through a gated one, unnamed", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n"
     "  if (s_fleet_polled_ms == 0) return handle_status(req);\n",
     ("route handler handle_wifi_connect() answers the TLS pin and is not in SECRET_ANSWERS",)),
    ("M7: /api/status answers the SoftAP password behind its refusal", _FIX_NET,
     "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n  return send_doc(req, doc);\n}\n"
     "static esp_err_t handle_wifi_connect",
     "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n  doc[\"ap\"] = net.getApPassword();\n"
     "  return send_doc(req, doc);\n}\nstatic esp_err_t handle_wifi_connect",
     ("handle_status() carries the SoftAP password, the TLS pin; its SECRET_ANSWERS entry "
      "says the TLS pin",)),
    ("M2: /api/status open during setup", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (!setup_is_active() && !auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     (_STATUS_REFUSAL, "handle_status() answers the TLS pin with no `if (!auth_gate(req))")),
    ("M3: /api/status serves under setup or a bearer, before its refusal", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (setup_is_active() || bearer_present_and_valid(req)) "
     "return send_provisioning_receipt(req);\n"
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     ("bearer_present_and_valid called from handle_status",
      "reaches a credential (send_provisioning_receipt) before its refusal")),
    ("W1: Wi-Fi connect open during setup", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!setup_is_active() && !auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect calls auth_gate other than as the whole statement",)),
    ("W2: Wi-Fi connect open to a SoftAP peer", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!from_ap_subnet(req) && !auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect calls auth_gate other than as the whole statement",)),
    ("M8: the receipt's bearer grant passes during setup", _FIX_NET,
     "[req]() { return bearer_present_and_valid(req); },",
     "[req]() { return bearer_present_and_valid(req) || setup_is_active(); },",
     ("hands receipt_decide the grants",)),
    ("M9: the receipt's tap grant passes a SoftAP peer", _FIX_NET,
     "      []() { return provisioning_gate_take(); });\n  if (verdict == ReceiptVerdict::",
     "      [req]() { return from_ap_subnet(req) || provisioning_gate_take(); });\n"
     "  if (verdict == ReceiptVerdict::",
     ("hands receipt_decide the grants",)),
    ("M10: the bearer grant takes a cv_session cookie", _FIX_NET,
     "  if (!token || token[0] == '\\0') return false;\n  return auth_bearer_grant(",
     "  if (!token || token[0] == '\\0') return false;\n  char c[64];\n"
     "  if (httpd_req_get_hdr_value_str(req, \"Cookie\", c, sizeof(c)) == ESP_OK &&\n"
     "      strstr(c, \"cv_session=\")) return true;\n  return auth_bearer_grant(",
     ("bearer_present_and_valid changed", "reads the request header '\"Cookie\"'",
      "ANY_SESSION_GATES")),
    ("M12: auth_gate passes during setup", _FIX_NET,
     "  if (!auth_check(req, token, request_peer_ipv4(req))) {",
     "  if (!auth_check(req, token, request_peer_ipv4(req)) && !setup_is_active()) {",
     ("auth_gate changed",)),
    ("G1: the bearer grant passes an unprovisioned token", _FIX_NET,
     "  if (!token || token[0] == '\\0') return false;\n  return auth_bearer_grant(",
     "  if (!token || token[0] == '\\0') return true;\n  return auth_bearer_grant(",
     ("bearer_present_and_valid changed",)),
    ("G2: auth_gate's compare made dead", _FIX_NET,
     "  if (!auth_check(req, token, request_peer_ipv4(req))) {",
     "  if (!auth_check(req, token, request_peer_ipv4(req)) && false) {",
     ("auth_gate changed",)),
    ("M11: the page's bearer grant widened", _FIX_NET,
     "!foreign && bearer_present_and_valid(req);",
     "!foreign && (bearer_present_and_valid(req) || other());",
     ("page_token_inject changed", "four unlocks", "Phase 6")),
    ("M14: the page's SoftAP grant takes the home Wi-Fi", _FIX_NET,
     "const bool on_ap        = from_ap_subnet(req);",
     "const bool on_ap        = from_ap_subnet(req) || sta_connected;",
     ("page_token_inject changed",)),
    ("M15: the page's SoftAP grant takes a broker link", _FIX_NET,
     "const bool on_ap        = from_ap_subnet(req);",
     "const bool on_ap        = from_ap_subnet(req) || mqtt_connected();",
     ("page_token_inject changed",)),
    ("M16: the page's SoftAP grant always", _FIX_NET,
     "const bool on_ap        = from_ap_subnet(req);", "const bool on_ap        = true;",
     ("page_token_inject changed",)),
    ("G3: the page's setup grant while the home link is down", _FIX_NET,
     "setup_is_active() || setup_is_first_boot();",
     "setup_is_active() || setup_is_first_boot() || !sta_connected;",
     ("page_token_inject changed",)),
    ("A2: the page's tap grant passes a SoftAP peer", _FIX_NET,
     "        return tap_spent;\n", "        return tap_spent || from_ap_subnet(req);\n",
     ("page_token_inject changed",)),
    ("A1: the SoftAP grant whenever the AP is up", _FIX_NET,
     "  if (!(WiFi.getMode() & WIFI_AP)) return false;\n",
     "  if (WiFi.getMode() & WIFI_AP) return true;\n",
     ("from_ap_subnet changed",)),
    ("A4: an unwired tap reads open", _FIX_NET,
     "s_gate_take ? s_gate_take() : false; }", "s_gate_take ? s_gate_take() : true; }",
     ("provisioning_gate_take changed",)),
    ("A5: the take peeks", _FIX_NET,
     "s_gate_take ? s_gate_take() : false; }", "s_gate_is_open ? s_gate_is_open() : false; }",
     ("provisioning_gate_take changed", "names provisioning_gate_take")),
    ("A6: the peek wired as the take hook", _FIX_MAIN,
     "network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);",
     "network_set_provisioning_gate_hooks(prov_gate_is_open_hook, prov_gate_is_open_hook);",
     ("calls network_set_provisioning_gate_hooks other than as the whole statement",)),
    ("A7: the take hook peeks", _FIX_MAIN,
     "  return canary::net::provisioning_gate::take(g_prov_gate",
     "  return canary::net::provisioning_gate::is_open(g_prov_gate",
     ("prov_gate_take_hook changed",)),
    ("K1: compare takes a session under Authorization", _FIX_AUTH,
     "  if (strncmp(auth_buf, \"Bearer \", 7) != 0)",
     "  if (session_table_has(auth_buf, strlen(auth_buf))) return Presented::RIGHT;\n"
     "  if (strncmp(auth_buf, \"Bearer \", 7) != 0)",
     ("AuthManager::compare changed",)),
    ("L1: grant compares a held source", _FIX_AUTH,
     "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
     "  return canary::net::auth_backoff::grant(",
     "  if (compare(req, expected_token) == Presented::RIGHT) return BearerResult::OK;\n"
     "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
     "  return canary::net::auth_backoff::grant(",
     ("AuthManager::grant changed",)),
    ("L2: check drops its lockout read", _FIX_AUTH,
     "  if (canary::net::auth_backoff::locked(m_table, source, now)) {\n"
     "    httpd_resp_set_status(req, \"429 Too Many Requests\");\n    return false;\n  }\n",
     "", ("AuthManager::check changed",)),
    ("L3: the page clears the backoff without a tap", _FIX_NET,
     "  } else if (tap_spent) {\n    auth_handed_token(request_peer_ipv4(req));\n  }\n",
     "  }\n  auth_handed_token(request_peer_ipv4(req));\n",
     ("page_token_inject changed",)),
    ("L4: the receipt clears the backoff on a refusal", _FIX_NET,
     "  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n",
     "  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n"
     "    auth_handed_token(request_peer_ipv4(req));\n",
     ("calls auth_handed_token outside a SERVE_* test",)),
    ("L5: a handler reaches the instance", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static esp_err_t handle_wifi_connect2(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n"
     "  auth_get_instance().handedToken(request_peer_ipv4(req));\n  return ESP_OK;\n}\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     ("auth_get_instance called from handle_wifi_connect2",)),
    ("L5: a raw compare outside the lib's two", _FIX_AUTH, "",
     "bool auth_peek(const char* t, size_t n) {\n"
     "  return AuthManager::constantTimeCompare(t, auth_get_token(), n);\n}\n",
     ("constantTimeCompare called from auth_peek", "auth_peek() reads the bearer token and is no")),
    ("L6: the lockout keyed on X-Forwarded-For", _FIX_NET,
     "static uint32_t request_peer_ipv4(httpd_req_t* req) {\n  return peer_of(req);\n}\n",
     "static uint32_t request_peer_ipv4(httpd_req_t* req) {\n  char xff[48];\n"
     "  if (httpd_req_get_hdr_value_str(req, \"X-Forwarded-For\", xff, sizeof(xff)) == ESP_OK)\n"
     "    return parse_ip(xff);\n  return peer_of(req);\n}\n",
     ("a header not in REQUEST_HEADERS",)),
    ("S1: the page sets a session cookie", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, \"Set-Cookie\", \"cv_session=1\");\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("sets the response header '\"Set-Cookie\"'", "ANY_SESSION_GATES", "F328")),
    ("S1: the page sets one, lower case", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, \"set-cookie\", \"cv_session=1\");\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("sets the response header '\"set-cookie\"'",)),
    ("S1: a header name that is no literal", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, kSession, \"cv_session=1\");\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("sets the response header 'kSession'",)),
    ("S1: a handler reads the Cookie header", _FIX_NET,
     "  ScvNetworkManager& net = network_get_instance();\n  JsonDocument doc;\n"
     "  doc[\"provisioning_gate\"]",
     "  char c[64];\n  httpd_req_get_hdr_value_str(req, \"Cookie\", c, sizeof(c));\n"
     "  ScvNetworkManager& net = network_get_instance();\n  JsonDocument doc;\n"
     "  doc[\"provisioning_gate\"]",
     ("reads the request header '\"Cookie\"'",)),
    ("S1: a handler reads a cookie", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  char v[40];\n  size_t n = sizeof(v);\n"
     "  httpd_req_get_cookie_val(req, \"cv_session\", v, &n);\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("reads a cookie (httpd_req_get_cookie_val)",)),
    ("P12: compare reads a query credential", _FIX_AUTH,
     "  char auth_buf[128];\n",
     "  char auth_buf[128];\n  char q[64];\n"
     "  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) return Presented::RIGHT;\n",
     ("AuthManager::compare reads the URL query",)),
    ("P12: /api/status reads the URL query", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (!auth_gate(req)) return ESP_OK;\n  (void)httpd_req_get_url_query_len(req);\n"
     "  ScvNetworkManager& net",
     ("handle_status reads the URL query",)),
    ("M13: a helper prints the SoftAP password", _FIX_NET, "",
     "static void dump_ap() {\n  Serial.println(network_get_instance().getApPassword());\n}\n",
     ("dump_ap() reads the SoftAP password and is no",)),
    ("a helper prints the token", _FIX_NET, "",
     "static void dump(void) {\n  Serial.println(auth_get_token());\n}\n",
     ("dump() reads the bearer token and is no",)),
    ("A10: setup hands the SoftAP password to the BLE label", _FIX_MAIN,
     "  network_set_provisioning_gate_hooks(",
     "  ble_status_set_label(g_ap_password);\n  network_set_provisioning_gate_hooks(",
     ("setup() reads the SoftAP password in a statement not in its SECRET_LOCAL_USES row",
      "Now: (")),
    ("A11: raiseAp publishes it over MQTT", _FIX_NET,
     "  announce_setup_network(m_ap_ssid, m_ap_password);\n",
     "  announce_setup_network(m_ap_ssid, m_ap_password);\n"
     "  mqtt_publish_state(\"ap\", m_ap_password);\n",
     ("ScvNetworkManager::raiseAp() reads the SoftAP password in a statement not in its "
      "SECRET_LOCAL_USES row",)),
    ("mint drift: the SoftAP key from the public key", _FIX_MAIN,
     "derive_ap_password(device.privkey,", "derive_ap_password(device.pubkey,",
     ("setup()'s SECRET_LOCAL_USES row holds", "in a statement not in its")),
    ("mint: the result discarded", _FIX_MAIN,
     "  if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
     "sizeof(g_ap_password))) {\n"
     "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
     "    while (true) { delay(1000); }\n  }\n",
     "  canary::net::derive_ap_password(device.privkey, g_ap_password, sizeof(g_ap_password));\n",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("mint: the halt dropped", _FIX_MAIN,
     "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
     "    while (true) { delay(1000); }\n",
     "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("mint: the call deleted", _FIX_MAIN,
     "  if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
     "sizeof(g_ap_password))) {\n"
     "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
     "    while (true) { delay(1000); }\n  }\n", "",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("mint: the token's refusal dropped", _FIX_AUTH,
     "      s_bearer[0] = '\\0';\n      return false;\n", "      s_bearer[0] = '\\0';\n",
     ("auth_load_or_derive()'s SECRET_LOCAL_USES row holds",)),
    # The roll-call calls a `begin(` (the tree's does, through Preferences);
    # ScvNetworkManager::begin's local-use row is keyed by its file and
    # class, so a `begin` elsewhere that reads a credential is not excused.
    ("keyed rows: another file's begin answers the SoftAP password", _FIX_OTHER, "",
     "const char* Other::begin(const char* ns, bool ro) {\n"
     "  return network_get_instance().getApPassword();\n}\n",
     ("Other::begin() reads the SoftAP password and is no", _PUBLIC_AP)),
    ("keyed rows: the same member in another file", _FIX_OTHER, "",
     "bool ScvNetworkManager::begin(const char* x) {\n  return m_ap_password[0] != x[0];\n}\n",
     ("ScvNetworkManager::begin() reads the SoftAP password and is no", _PUBLIC_AP)),
    ("a credential outside every function", _FIX_NET, "",
     "static const char* kLeak = auth_get_token();\n",
     ("a credential named outside every function ('static const char* kLeak",)),
    ("the page answers the token by itself", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, \"X-T\", auth_get_token());\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("handle_ui() reaches a credential through auth_get_token, not the page path",)),
    ("the status answers before its refusal", _FIX_NET,
     "  if (!rate_limit_check(req)) return ESP_OK;\n  if (!auth_gate(req)) return ESP_OK;\n",
     "  if (!rate_limit_check(req)) return ESP_OK;\n"
     "  if (network_get_instance().getTlsCertFp()[0]) return ESP_OK;\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("reaches a credential (getTlsCertFp) before its refusal",)),
    ("the receipt answers outside a served verdict", _FIX_NET,
     "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n",
     "  Serial.println(network_get_instance().getApPassword());\n"
     "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n",
     ("reaches a credential (getApPassword) outside a SERVE_* test",)),
    ("the receipt stops carrying the pin", _FIX_NET,
     "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n  return send_doc(req, doc);\n}\n"
     "static esp_err_t handle_provisioning_receipt",
     "  return send_doc(req, doc);\n}\nstatic esp_err_t handle_provisioning_receipt",
     ("emitter send_provisioning_receipt() carries the bearer token, the SoftAP password; its "
      "TOKEN_EMITTERS entry says", "handle_provisioning_receipt() carries the bearer token, "
      "the SoftAP password; its SECRET_ANSWERS entry says")),
    ("a comparer reads the SoftAP password", _FIX_NET,
     "  return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n",
     "  (void)network_get_instance().getApPassword();\n"
     "  return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n",
     ("comparer bearer_present_and_valid() reads the bearer token, the SoftAP password",)),
    ("an accessor defined twice", _FIX_OTHER, "",
     "const char* getApPassword() {\n  return \"\";\n}\n",
     ("SECRET_ACCESSORS: getApPassword() is defined in",)),
    ("a gate primitive called outside every function", _FIX_NET, "",
     "static bool g_ok = auth_check(nullptr, \"\", 0);\n",
     ("auth_check() called outside every function",)),
    ("the hooks wired twice", _FIX_MAIN,
     "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n",
     "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n"
     "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n",
     ("network_set_provisioning_gate_hooks is called 2 times",)),
    ("the take hook's variable set outside its setter", _FIX_NET,
     "static network_gate_fn_t s_gate_take = nullptr;\n",
     "static network_gate_fn_t s_gate_take = prov_gate_is_open_hook;\n",
     ("s_gate_take named outside every function other than in its declaration",)),
    # Round-1 review: where a mint sits. Each compiles the derivation out
    # (the key stays "", and the SoftAP rises open) with its text intact.
    ("mint: the SoftAP key's mint under #if 0", _FIX_MAIN, _FIX_MINT_SRC,
     "#if 0\n" + _FIX_MINT_SRC + "#endif\n",
     ("setup()'s SECRET_LOCAL_USES row holds", "[within #if 0] if (!canary::net::")),
    ("mint: the SoftAP key's mint under #ifndef", _FIX_MAIN, _FIX_MINT_SRC,
     "#ifndef CANARY_DEV_OPEN_AP\n" + _FIX_MINT_SRC + "#endif\n",
     ("[within #ifndef CANARY_DEV_OPEN_AP]",)),
    ("mint: the SoftAP key's mint under if (false)", _FIX_MAIN, _FIX_MINT_SRC,
     "  if (false) {\n" + _FIX_MINT_SRC + "  }\n",
     ("setup()'s SECRET_LOCAL_USES row holds", "[within if (false) {]")),
    ("mint: a goto past the SoftAP key's mint", _FIX_MAIN, _FIX_MINT_SRC,
     "  if (dev_open) goto keyed;\n" + _FIX_MINT_SRC + "keyed:\n",
     ("setup()'s SECRET_LOCAL_USES row holds", "[within after a goto]")),
    ("mint: the token's mint under #if 0 inside its block", _FIX_AUTH,
     "    if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) {\n",
     "#if 0\n    if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) {\n",
     ("auth_load_or_derive()'s SECRET_LOCAL_USES row holds", "[within if (!loaded) { > #if 0]")),
    # The token's mint input, where the key enters and inside the deriver.
    ("the boot hands the token's mint the public key", _FIX_WITNESS,
     "auth_load_or_derive(g_device.privkey)", "auth_load_or_derive(g_device.pubkey)",
     ("witness_provision_device calls auth_load_or_derive other than as the whole statement",)),
    ("the boot drops the token mint's refusal", _FIX_WITNESS,
     "    return false;\n  }\n", "  }\n",
     ("witness_provision_device calls auth_load_or_derive other than as the whole statement",)),
    ("the token's deriver keyed on the public key", _FIX_CRYPTO,
     "hmac_into(privkey,", "hmac_into(witness_get_device().pubkey,",
     ("derive_api_token changed",)),
    # Every SoftAP raise, with its key.
    ("begin() raises the SoftAP open", _FIX_NET,
     "WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL", "WiFi.softAP(ap_ssid, nullptr, AP_CHANNEL",
     ("ScvNetworkManager::begin calls softAP other than as the whole statement",)),
    ("a third SoftAP raise", _FIX_NET, "void ScvNetworkManager::idle() {\n",
     "void ScvNetworkManager::idle() {\n  WiFi.softAP(m_ap_ssid, \"\");\n",
     ("softAP called from ScvNetworkManager::idle",)),
    ("the security request asks for an open SoftAP", _FIX_NET,
     "c.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;", "c.ap.authmode = WIFI_AUTH_OPEN;",
     ("apply_ap_security changed",)),
    # The refusal's place (W1/W2 nested, an early exit, a conditional).
    ("W1 nested: Wi-Fi connect's refusal only outside setup", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!setup_is_active()) {\n    if (!auth_gate(req)) return ESP_OK;\n  }\n",
     ("handle_wifi_connect calls auth_gate other than at its own top level "
      "(within if (!setup_is_active()) {)",)),
    ("W2 nested: Wi-Fi connect's refusal only off the SoftAP", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!from_ap_subnet(req)) {\n    if (!auth_gate(req)) return ESP_OK;\n  }\n",
     ("handle_wifi_connect calls auth_gate other than at its own top level",)),
    ("Wi-Fi connect's refusal under #if", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "#if !CANARY_DEV_OPEN\n  if (!auth_gate(req)) return ESP_OK;\n#endif\n",
     ("(within #if !CANARY_DEV_OPEN)",)),
    ("Wi-Fi connect applied before its refusal", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (from_ap_subnet(req)) return wifi_connect_apply(req);\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect can leave before its auth_gate call: 'if (from_ap_subnet(req)) "
      "return wifi_connect_apply(req);'",)),
    ("a goto past Wi-Fi connect's refusal", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (setup_is_active()) goto gated;\n  if (!auth_gate(req)) return ESP_OK;\ngated:\n",
     ("handle_wifi_connect can leave before its auth_gate call",)),
    ("a rate limit whose argument does the work", _FIX_NET,
     "  if (!rate_limit_check(req)) return ESP_OK;\n  if (!auth_gate(req)) return ESP_OK;\n",
     "  if (!rate_limit_check(req, wifi_connect_apply(req))) return ESP_OK;\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("handle_status can leave before its auth_gate call",)),
    ("the status refusal nested (the bearer grant's own top level)", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (!setup_is_active()) {\n    if (!auth_gate(req)) return ESP_OK;\n  }\n"
     "  ScvNetworkManager& net",
     ("handle_status() answers the TLS pin with no `if (!auth_gate(req)) return ESP_OK;` at "
      "its own top level", "handle_status calls auth_gate other than at its own top level")),
    # The two stores the reads missed.
    ("the roll-call answers the token's NVS record", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  char tb[40] = {0};\n  nvs.getBytes(NVS_KEY_TOKEN, tb, sizeof(tb) - 1);\n"
     "  httpd_resp_set_hdr(req, \"X-Diag\", tb);\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     ("public route handler handle_fleet() answers the bearer token",)),
    ("the roll-call answers the driver's SoftAP passphrase", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  wifi_config_t c;\n  esp_wifi_get_config(WIFI_IF_AP, &c);\n"
     "  httpd_resp_set_hdr(req, \"X-Diag\", (const char*)c.ap.password);\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     (_PUBLIC_AP,)),
    # A sink inside an accessor, an emitter or the setup-network line.
    ("the token's accessor prints it", _FIX_AUTH,
     "const char* auth_get_token() {\n  return s_bearer;\n}\n",
     "const char* auth_get_token() {\n  Serial.printf(\"[AUTH] bearer %s\\n\", s_bearer);\n"
     "  return s_bearer;\n}\n",
     ("auth_get_token changed",)),
    ("the SoftAP password's accessor publishes it", _FIX_NET_H,
     "const char* getApPassword() const { return m_ap_password; }",
     "const char* getApPassword() const { mqtt_publish_state(\"ap\", m_ap_password); "
     "return m_ap_password; }",
     ("getApPassword changed",)),
    ("the SoftAP password's accessor copies it out", _FIX_NET_H,
     "const char* getApPassword() const { return m_ap_password; }",
     "const char* getApPassword() const { strncpy(g_ap_copy, m_ap_password, 64); "
     "return m_ap_password; }",
     ("getApPassword changed",)),
    ("the receipt's builder prints the token", _FIX_NET,
     "  doc[\"token\"] = auth_get_token();\n",
     "  doc[\"token\"] = auth_get_token();\n"
     "  Serial.printf(\"[RCPT] token %s\\n\", auth_get_token());\n",
     ("send_provisioning_receipt() reads the bearer token in a statement not in its "
      "EMITTER_STATEMENTS row",)),
    ("the setup-network line publishes the SoftAP password", _FIX_NET,
     "  if (announce_line(line, sizeof(line), ssid, pass) > 0) Serial.println(line);\n",
     "  if (announce_line(line, sizeof(line), ssid, pass) > 0) Serial.println(line);\n"
     "  mqtt_publish_state(\"ap\", pass);\n",
     ("announce_setup_network changed",)),
    # A handler defined twice: the #else body is read like the first.
    ("the roll-call's #else body sends the receipt", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "#if !FEATURE_FLEET_RECEIPT\nstatic esp_err_t handle_fleet(httpd_req_t* req) {\n"
     "  return httpd_resp_sendstr(req, fleet_body());\n}\n#else\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n"
     "  if (host_is_foreign(req)) { send_host_refusal(req); return ESP_OK; }\n"
     "  return send_provisioning_receipt(req);\n}\n#endif\n"
     "static esp_err_t handle_fleet_old(httpd_req_t* req) {\n",
     ("public route handler handle_fleet() answers the bearer token",
      "send_provisioning_receipt called from handle_fleet")),
    ("a device-side use's second body, a stub that reads nothing", _FIX_MAIN, "",
     "#if !FEATURE_WIFI_AP_KEY\nvoid setup() {\n  delay(1);\n}\n#endif\n",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("/api/status's second body answers before its refusal", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n",
     "static esp_err_t handle_status(httpd_req_t* req) {\n"
     "  doc[\"tls_cert_fp\"] = network_get_instance().getTlsCertFp();\n"
     "  if (!auth_gate(req)) return ESP_OK;\n  return send_doc(req, doc);\n}\n"
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n",
     ("handle_status() reaches a credential (getTlsCertFp) before its refusal",)),
    ("an emitter defined twice", _FIX_OTHER, "",
     "static esp_err_t send_html_with_token(httpd_req_t* req, const char* html,\n"
     "                                      PageToken verdict) {\n"
     "  return send_page(req, html, \"\");\n}\n",
     ("EMITTER_STATEMENTS: send_html_with_token() is defined in fx/lib/securacv_network.cpp, "
      "fx/lib/securacv_other.cpp",)),
    # A primitive or pinned glue defined twice, or shadowed in another lib.
    ("host_is_foreign shadowed in another lib", _FIX_OTHER, "",
     "static bool host_is_foreign(httpd_req_t* req) {\n  (void)req;\n  return false;\n}\n",
     ("GLUE_DEFS pins host_is_foreign() in securacv_network.cpp, which ROOTS defines twice",)),
    ("auth_gate shadowed in another lib, gating a route there", _FIX_OTHER, "",
     "static bool auth_gate(httpd_req_t* req) {\n  (void)req;\n  return true;\n}\n"
     "esp_err_t handle_zdiag(httpd_req_t* req) {\n  if (!auth_gate(req)) return ESP_OK;\n"
     "  return httpd_resp_sendstr(req, \"{}\");\n}\n",
     ("GATE_CALL_SITES row auth_gate: ROOTS defines it twice",
      "GLUE_DEFS pins auth_gate() in securacv_network.cpp, which ROOTS defines twice")),
    ("pinned glue defined twice in its own file", _FIX_NET,
     "static bool bearer_present_and_valid(httpd_req_t* req) {\n",
     "static bool provisioning_gate_take() { return true; }\n"
     "static bool bearer_present_and_valid(httpd_req_t* req) {\n",
     ("GLUE_DEFS pins provisioning_gate_take() in securacv_network.cpp, which ROOTS defines "
      "twice",)),
    ("a shared decision shadowed under ROOTS", _FIX_NET, "",
     "static ReceiptVerdict receipt_decide(bool f, Fn b, Fn t) {\n"
     "  return ReceiptVerdict::SERVE_TAP;\n}\n",
     ("GATE_CALL_SITES row receipt_decide: ROOTS defines it once",)),
    # The AuthManager reached by a fourth function.
    ("a new auth-lib function reaches s_auth", _FIX_AUTH, "",
     "void auth_clear_source(uint32_t source) {\n  s_auth.handedToken(source);\n}\n",
     ("s_auth called from auth_clear_source",)),
    ("the lockout table reset from a new function", _FIX_AUTH, "",
     "void auth_forget_all() { s_auth.reset(); }\n",
     ("s_auth called from auth_forget_all",)),
    ("a namespace-scope lambda reaches the instance", _FIX_NET, "",
     "static auto clear_backoff = [](uint32_t s) { return "
     "auth_get_instance().handedToken(s); };\n",
     ("auth_get_instance() called outside every function",)),
    ("a namespace-scope lambda's else branch clears the backoff", _FIX_NET, "",
     "static auto clear_backoff = [](uint32_t s) { if (!s) return; else "
     "auth_handed_token(s); };\n",
     ("auth_handed_token() called outside every function",)),
    ("a noexcept helper reaches the instance", _FIX_NET, "",
     "static void clear_backoff(uint32_t s) noexcept { return "
     "auth_get_instance().handedToken(s); }\n",
     ("auth_get_instance called from clear_backoff",)),
    ("a noexcept helper clears the backoff", _FIX_NET, "",
     "static void clear_backoff(uint32_t s) noexcept { return auth_handed_token(s); }\n",
     ("auth_handed_token called from clear_backoff",)),
    ("the constant-time compare made a memcmp", _FIX_AUTH,
     "  volatile uint8_t diff = 0;\n  for (size_t i = 0; i < len; i++) diff |= pa[i] ^ pb[i];\n"
     "  return diff == 0;\n",
     "  (void)pa;\n  (void)pb;\n  return memcmp(a, b, len) == 0;\n",
     ("AuthManager::constantTimeCompare changed",)),
    # The private key, and the request's raw path.
    ("the roll-call answers the private key", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  char pk[65];\n  hex_to_str(pk, witness_get_device().privkey, 32);\n"
     "  httpd_resp_set_hdr(req, \"X-Diag\", pk);\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     ("route handler handle_fleet() names the private key",)),
    ("a query credential parsed from the raw path", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n"
     "  if (strstr(req->uri, \"cv_session=\")) return send_doc(req, doc);\n",
     ("handle_fleet reads a request's raw path (its uri, which keeps its query), outside "
      "URI_READERS",)),
    ("a request header named by a variable", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  char c[64];\n"
     "  httpd_req_get_hdr_value_str(req, kCookie, c, sizeof(c));\n",
     ("reads the request header 'kCookie'",)),
    # Round-2 review: begin()'s key, where its copy sits, and its callers.
    ("begin()'s key parameter reassigned", _FIX_NET, "  if (ap_password) {\n    strncpy(",
     "  ap_password = nullptr;\n  if (ap_password) {\n    strncpy(",
     ("ScvNetworkManager::begin() reads the SoftAP password in a statement not in its "
      "SECRET_LOCAL_USES row: 'ap_password = nullptr;'",)),
    ("begin()'s copy made dead", _FIX_NET, "  if (ap_password) {\n    strncpy(",
     "  if (ap_password && false) {\n    strncpy(", ("'if (ap_password && false) {'",)),
    ("begin()'s copy compiled out", _FIX_NET,
     "    strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);\n",
     "#if 0\n    strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);\n#endif\n",
     ("[within if (ap_password) { > #if 0] strncpy(m_ap_password",)),
    ("a second start of the network handed an empty key", _FIX_MAIN, _FIX_SETUP_PRINT,
     _FIX_SETUP_PRINT + "  network_get_instance().begin(device.ap_ssid, \"\");\n",
     ("setup calls ScvNetworkManager::begin other than as the whole statement",
      "ScvNetworkManager::begin is called 2 times by setup")),
    ("network_init called with no key", _FIX_MAIN, "",
     "static void restart_setup() {\n  network_init(witness_get_device().ap_ssid, nullptr, "
     "nullptr);\n}\n",
     ("network_init named in restart_setup: its GATE_CALL_SITES row allows it in no "
      "function",)),
    ("the network started through another name", _FIX_MAIN, "",
     "static void restart_setup() {\n  ScvNetworkManager& mgr = network_get_instance();\n"
     "  mgr.begin(\"x\", \"\");\n}\n",
     ("names the network manager 'mgr'",)),
    ("the network started through an auto reference", _FIX_MAIN, "",
     "static void restart_setup() {\n  auto& m = network_get_instance();\n"
     "  m.begin(\"x\", \"\");\n}\n",
     ("names the network manager 'm'",)),
    ("a member restarts the network with no key", _FIX_NET, "  if (!ap_ok) return;\n",
     "  if (!ap_ok) return;\n  begin(m_ap_ssid, \"\");\n",
     ("ScvNetworkManager::begin called from ScvNetworkManager::raiseAp",)),
    ("a member restarts the network through this", _FIX_NET, "  if (!ap_ok) return;\n",
     "  if (!ap_ok) return;\n  this->begin(m_ap_ssid, nullptr);\n",
     ("ScvNetworkManager::begin called from ScvNetworkManager::raiseAp",)),
    ("the driver's AP config opened beside the raise", _FIX_NET, "  if (!ap_ok) return;\n",
     "  if (!ap_ok) return;\n  { wifi_config_t o; esp_wifi_get_config(WIFI_IF_AP, &o); "
     "o.ap.authmode = WIFI_AUTH_OPEN; esp_wifi_set_config(WIFI_IF_AP, &o); }\n",
     ("esp_wifi_set_config called from ScvNetworkManager::raiseAp",)),
    ("a helper opens the driver's AP config", _FIX_NET, "",
     "static void ap_open() {\n  wifi_config_t c;\n  c.ap.authmode = WIFI_AUTH_OPEN;\n"
     "  esp_wifi_set_config(WIFI_IF_AP, &c);\n}\n",
     ("esp_wifi_set_config called from ap_open",)),
    ("the driver's AP config written in place", _FIX_NET,
     "    const bool accepted = (esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK);\n",
     "    c.ap.authmode = WIFI_AUTH_OPEN;\n"
     "    const bool accepted = (esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK);\n",
     ("apply_ap_security changed",)),
    ("core 3's AP class raises a SoftAP", _FIX_NET, "  if (!ap_ok) return;\n",
     "  if (!ap_ok) return;\n  WiFi.AP.create(m_ap_ssid, nullptr);\n",
     ("WiFi.AP named in ScvNetworkManager::raiseAp",)),
    # Taking an address is a mention: an accessor's, a census primitive's.
    ("the token's accessor through a function pointer", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static const char* (*const s_tok)() = &auth_get_token;\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n"
     "  httpd_resp_set_hdr(req, \"X-Diag\", s_tok());\n",
     ("a credential named outside every function ('static const char* (*const s_tok)() = "
      "&auth_get_token;')",)),
    ("the tap's clear through a function pointer", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static void (*const s_seen)(uint32_t) = auth_handed_token;\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n  s_seen(request_peer_ipv4(req));\n",
     ("auth_handed_token() called outside every function",)),
    ("the SoftAP password through a member pointer", _FIX_NET, "",
     "static const char* (ScvNetworkManager::*const s_ap)() const = "
     "&ScvNetworkManager::getApPassword;\n",
     ("a credential named outside every function ('static const char* "
      "(ScvNetworkManager::*const s_ap)",)),
    ("the instance's address at namespace scope", _FIX_AUTH, "",
     "static AuthManager* const s_view = &auth_get_instance();\n",
     ("auth_get_instance() called outside every function",)),
    ("a trailing-return helper reaches the instance", _FIX_AUTH, "",
     "auto auth_clear_source(uint32_t source) -> void {\n"
     "  (&auth_get_instance())->handedToken(source);\n}\n",
     ("auth_get_instance called from auth_clear_source",)),
    ("a trailing-return shadow of auth_gate in another lib", _FIX_OTHER, "",
     "static auto auth_gate(httpd_req_t* req) -> bool { (void)req; return true; }\n",
     ("GLUE_DEFS pins auth_gate() in securacv_network.cpp, which ROOTS defines twice",
      "GATE_CALL_SITES row auth_gate: ROOTS defines it twice")),
    ("a trailing-return shadow of host_is_foreign in another lib", _FIX_OTHER, "",
     "static auto host_is_foreign(httpd_req_t* req) -> bool { (void)req; return false; }\n",
     ("GLUE_DEFS pins host_is_foreign() in securacv_network.cpp, which ROOTS defines twice",)),
    # Macros: the preprocessor's shadow of a pinned body or a census name.
    ("a macro ungates every route", _FIX_NET, "static network_gate_fn_t s_gate_take = nullptr;\n",
     "#define auth_gate(r) (true)\nstatic network_gate_fn_t s_gate_take = nullptr;\n",
     ("a macro names 'auth_gate'",)),
    ("a macro on two lines makes the bearer grant true", _FIX_NET,
     "static void announce_setup_network(",
     "#define bearer_present_and_valid(r) \\\n  (true)\nstatic void announce_setup_network(",
     ("a macro names 'bearer_present_and_valid'",)),
    ("a macro makes every Host this device's", _FIX_NET, "",
     "#define host_is_foreign(r) (false)\n", ("a macro names 'host_is_foreign'",)),
    ("a macro reads a credential", _FIX_NET, "",
     "#define AP_KEY network_get_instance().getApPassword()\n",
     ("a macro names 'getApPassword'",)),
    # The page grant's setup input at its source.
    ("the first-boot unlock widened at its source", _FIX_SETUP,
     "  return setup_wiz::s_active;\n",
     "  return setup_wiz::s_active || millis() < 600000UL;\n",
     ("setup_is_active changed — the first-boot unlock's source",)),
    ("first boot read as always", _FIX_SETUP,
     "  s_first_boot = !prefs.getBool(NVS_KEY_SETUP_OK, false);\n",
     "  s_first_boot = true;\n", ("setup_init changed",)),
    ("a new writer of the setup state", _FIX_SETUP, "  return copy_name(out, len);\n",
     "  setup_wiz::s_active = true;\n  return copy_name(out, len);\n",
     ("s_active called from setup_get_device_name",)),
    ("the setup timeout's call dropped", _FIX_MAIN, "    setup_check_timeout();\n", "",
     ("GATE_CALL_SITES row setup_check_timeout names loop (src/main.cpp), which no longer "
      "calls it",)),
    # The token's helpers.
    ("the token's HMAC ignores its key", _FIX_CRYPTO,
     "  return mbed_hmac(key, key_len, data, data_len, out);\n",
     "  return mbed_hmac((const uint8_t*)\"k\", 1, data, data_len, out);\n",
     ("hmac_sha256 changed",)),
    ("the token's rendering skips its input", _FIX_CRYPTO,
     "    output[i] = UNAMBIGUOUS_ALPHABET[input[i] % UNAMBIGUOUS_LEN];\n",
     "    output[i] = UNAMBIGUOUS_ALPHABET[i % UNAMBIGUOUS_LEN];\n",
     ("format_api_token_string changed",)),
    ("the token's alphabet narrowed", _FIX_CRYPTO,
     "\"23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz\"", "\"22222222\"",
     ("UNAMBIGUOUS_ALPHABET named outside every function other than in its declaration",)),
    # The BOOT tap's open side.
    ("the BOOT tap opened for any SoftAP peer", _FIX_MAIN,
     "static void handle_boot_button() {\n",
     "static void open_for_ap_peer() {\n  if (WiFi.softAPgetStationNum() > 0) "
     "canary::net::provisioning_gate::open(g_prov_gate, millis());\n}\n"
     "static void handle_boot_button() {\n",
     ("g_prov_gate called from open_for_ap_peer",
      "provisioning_gate::open called from open_for_ap_peer")),
    ("the BOOT tap opened on every pass", _FIX_MAIN,
     "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n",
     "  canary::net::provisioning_gate::open(g_prov_gate, millis());\n"
     "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n",
     ("g_prov_gate: handle_boot_button's statements that name it are not its row",
      "provisioning_gate::open is called 2 times by handle_boot_button")),
    ("the BOOT tap's stamp written directly", _FIX_MAIN,
     "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n",
     "  g_prov_gate.opened_at_ms = millis();\n"
     "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n",
     ("g_prov_gate: handle_boot_button's statements that name it are not its row",)),
    ("the BOOT tap open from boot", _FIX_MAIN, "State g_prov_gate = {0};",
     "State g_prov_gate = {1};",
     ("g_prov_gate named outside every function other than in its declaration",)),
    # The refusal's place and the mint's: a split head, ESP-IDF's early-exit
    # macros, a throw, a nested rate limit; the mint's order.
    ("Wi-Fi connect's head split across #if / #else", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "#if CANARY_DEV_OPEN_SETUP\nstatic esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "#else\nstatic esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n#endif\n",
     ("handle_wifi_connect calls auth_gate other than at its own top level (within static "
      "esp_err_t handle_wifi_connect(httpd_req_t* req) {",)),
    ("an ESP_RETURN_ON_* before Wi-Fi connect's refusal", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  ESP_RETURN_ON_FALSE(!from_ap_subnet(req), wifi_connect_apply(req), \"net\", \"ap\");\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect can leave before its auth_gate call: 'ESP_RETURN_ON_FALSE(",)),
    ("a throw before Wi-Fi connect's refusal", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (from_ap_subnet(req)) throw wifi_connect_apply(req);\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect can leave before its auth_gate call: 'if (from_ap_subnet(req)) "
      "throw",)),
    ("a rate-limit refusal nested before the status refusal", _FIX_NET,
     "  if (!rate_limit_check(req)) return ESP_OK;\n  if (!auth_gate(req)) return ESP_OK;\n",
     "  if (from_ap_subnet(req)) {\n    if (!rate_limit_check(req)) return ESP_OK;\n  }\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("handle_status can leave before its auth_gate call: 'if (!rate_limit_check(req)) "
      "return ESP_OK;'",)),
    ("an ESP_GOTO_ON_* past the SoftAP key's mint", _FIX_MAIN, _FIX_MINT_SRC,
     "  ESP_GOTO_ON_FALSE(device.provisioned, ESP_OK, keyed, \"boot\", \"x\");\n"
     + _FIX_MINT_SRC + "keyed:\n",
     ("setup()'s SECRET_LOCAL_USES row holds", "[within after a goto] if (!canary::net::")),
    ("mint: the SoftAP key's mint after its use", _FIX_MAIN, _FIX_MINT_SRC + _FIX_SETUP_NET,
     _FIX_SETUP_NET + _FIX_MINT_SRC,
     ("setup()'s statements that read a credential are not its SECRET_LOCAL_USES row",)),
    # The stores and spellings the round-2 review added.
    ("the roll-call answers the TLS private key", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  uint8_t kb[1300];\n  size_t kl = nvs.getBytes(NVS_KEY_TLS_KEY, kb, sizeof(kb));\n"
     "  httpd_resp_send(req, (const char*)kb, kl);\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     ("public route handler handle_fleet() answers the TLS private key",)),
    ("the driver's passphrase through a copy of its union member", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  wifi_config_t c;\n  esp_wifi_get_config(WIFI_IF_AP, &c);\n"
     "  const wifi_ap_config_t& a = c.ap;\n"
     "  httpd_resp_set_hdr(req, \"X-Diag\", (const char*)a.password);\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     (_PUBLIC_AP,)),
    ("a query credential through (*req).uri", _FIX_NET, "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n"
     "  if (strstr((*req).uri, \"cv_session=\")) return send_doc(req, doc);\n",
     ("handle_fleet reads a request's raw path (its uri, which keeps its query)",)),
    ("the instance's accessor resets the table", _FIX_AUTH,
     "AuthManager& auth_get_instance() {\n  return s_auth;\n}\n",
     "AuthManager& auth_get_instance() {\n  s_auth.reset();\n  return s_auth;\n}\n",
     ("auth_get_instance changed",)),
    # Round-3 review: a macro of a name the pinned glue calls.
    ("a macro turns the take hook into a peek", _FIX_MAIN, "static char g_ap_password[16];\n",
     "#define take(s, n, t) is_open(s, n, t)\nstatic char g_ap_password[16];\n",
     ("a macro names 'take'",)),
    ("a macro makes every LAN peer a SoftAP one", _FIX_NET, "static ScvNetworkManager s_network;\n",
     "#define request_on_softap(p, l, ai, am, si, sm) request_on_softap(ai, ai, ai, am, 0, 0)\n"
     "static ScvNetworkManager s_network;\n", ("a macro names 'request_on_softap'",)),
    ("a macro decides every receipt served", _FIX_NET, "",
     "#define receipt_decide(f, b, t) (ReceiptVerdict::SERVE_TAP)\n",
     ("a macro names 'receipt_decide'",)),
    ("a macro makes every Host name the device", _FIX_NET, "",
     "#define host_names_this_device(h) host_names_this_device(((void)(h), \"canary.local\"))\n",
     ("a macro names 'host_names_this_device'",)),
    # The receipt's own statements, under its served verdicts.
    ("the receipt publishes the token under its served tap", _FIX_NET,
     "    auth_handed_token(request_peer_ipv4(req));\n    return send_provisioning_receipt(req);\n",
     "    auth_handed_token(request_peer_ipv4(req));\n    mqtt_publish_status(auth_get_token());\n"
     "    return send_provisioning_receipt(req);\n",
     ("handle_provisioning_receipt() reads the bearer token in a statement not in its "
      "ANSWER_STATEMENTS row",)),
    ("the receipt prints the token under its served tap", _FIX_NET,
     "    auth_handed_token(request_peer_ipv4(req));\n    return send_provisioning_receipt(req);\n",
     "    auth_handed_token(request_peer_ipv4(req));\n"
     "    Serial.printf(\"[RX] %s\\n\", auth_get_token());\n"
     "    return send_provisioning_receipt(req);\n",
     ("handle_provisioning_receipt() reads the bearer token in a statement not in its "
      "ANSWER_STATEMENTS row",)),
    ("the receipt prints the SoftAP password under its bearer", _FIX_NET,
     "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n"
     "    return send_provisioning_receipt(req);\n",
     "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n"
     "    Serial.printf(\"%s\", network_get_instance().getApPassword());\n"
     "    return send_provisioning_receipt(req);\n",
     ("handle_provisioning_receipt() reads the SoftAP password in a statement not in its "
      "ANSWER_STATEMENTS row",)),
    # The SoftAP raised by a mode switch on the open config dropAp leaves.
    ("dropAp raises the SoftAP back", _FIX_NET,
     "  WiFi.softAPdisconnect(true);\n  WiFi.mode(WIFI_STA);\n",
     "  WiFi.softAPdisconnect(true);\n  WiFi.mode(WIFI_AP_STA);\n",
     ("GATE_CALL_SITES row WiFi.mode: ScvNetworkManager::dropAp's statements that name it are "
      "not its row",)),
    ("a helper switches the mode to AP+STA", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n"
     "  if (!(WiFi.getMode() & WIFI_AP)) WiFi.mode(WIFI_AP_STA);\n}\n",
     ("WiFi.mode called from keep_setup_ap_up",)),
    ("a helper enables the AP", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n  WiFi.enableAP(true);\n}\n",
     ("WiFi.mode called from keep_setup_ap_up",)),
    ("a helper switches the mode through the class's static", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n  WiFiGenericClass::mode(WIFI_AP_STA);\n}\n",
     ("WiFi.mode called from keep_setup_ap_up",)),
    ("a helper sets the driver's mode", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n  esp_wifi_set_mode(WIFI_MODE_APSTA);\n}\n",
     ("WiFi.mode called from keep_setup_ap_up",)),
    ("a helper configures the AP's address (which enables it)", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n  WiFi.softAPConfig(ip, ip, mask);\n}\n",
     ("WiFi.mode called from keep_setup_ap_up",)),
    # softAPdisconnect()'s default argument is `false`: on core 2.0.17 it writes
    # the open, empty-SSID config and leaves a running SoftAP up on it, and on
    # core 3.x it raises the AP first (AP.clear() runs AP.begin()), even from
    # STA-only mode. softAPbandwidth() raises it on 3.x the same way.
    ("a new member quiets the SoftAP with softAPdisconnect's default", _FIX_NET, "",
     "void ScvNetworkManager::quietAp() {\n  WiFi.softAPdisconnect();\n"
     "  m_status.ap_active = false;\n}\n",
     ("WiFi.mode called from ScvNetworkManager::quietAp",)),
    ("the setup lib quiets the SoftAP with softAPdisconnect's default", _FIX_SETUP, "",
     "void setup_stop_captive_portal(void) {\n  WiFi.softAPdisconnect();\n}\n",
     ("WiFi.mode called from setup_stop_captive_portal",)),
    ("dropAp's softAPdisconnect loses its wifioff", _FIX_NET,
     "  WiFi.softAPdisconnect(true);\n  WiFi.mode(WIFI_STA);\n",
     "  WiFi.softAPdisconnect();\n  WiFi.mode(WIFI_STA);\n",
     ("GATE_CALL_SITES row WiFi.mode: ScvNetworkManager::dropAp's statements that name it are "
      "not its row",)),
    ("a helper sets the AP's bandwidth (which raises it on core 3.x)", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n  WiFi.softAPbandwidth(WIFI_BW_HT20);\n}\n",
     ("WiFi.mode called from keep_setup_ap_up",)),
    # A way out between a mode raise and the softAP() that keys it leaves the
    # SoftAP up on whatever config the driver loaded, never keyed (raiseAp()
    # returns while the AP is up): an STA-first boot, or an empty-SSID guard.
    ("begin() returns between its mode raise and its softAP()", _FIX_NET,
     "  WiFi.mode(WIFI_AP_STA);\n  bool ap_ok = WiFi.softAP(ap_ssid,",
     "  WiFi.mode(WIFI_AP_STA);\n  if (has_creds && m_creds.enabled) {\n    connectToHome();\n"
     "    return true;\n  }\n  bool ap_ok = WiFi.softAP(ap_ssid,",
     ("ScvNetworkManager::begin can leave between its SoftAP raise `WiFi.mode(WIFI_AP_STA);` "
      "and the softAP() that keys it: 'return true;'",)),
    ("raiseAp() returns between its mode raise and its softAP()", _FIX_NET,
     "  WiFi.mode(WIFI_AP_STA);\n  bool ap_ok = WiFi.softAP(m_ap_ssid,",
     "  WiFi.mode(WIFI_AP_STA);\n  if (m_ap_ssid[0] == '\\0') return;\n"
     "  bool ap_ok = WiFi.softAP(m_ap_ssid,",
     ("ScvNetworkManager::raiseAp can leave between its SoftAP raise",)),
    ("begin() keys the SoftAP before it raises the mode", _FIX_NET,
     "  WiFi.mode(WIFI_AP_STA);\n  bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, "
     "false, AP_MAX_CONNECTIONS);\n",
     "  bool ap_ok = WiFi.softAP(ap_ssid, ap_password, AP_CHANNEL, false, "
     "AP_MAX_CONNECTIONS);\n  WiFi.mode(WIFI_AP_STA);\n",
     ("ScvNetworkManager::begin's SoftAP raise `WiFi.mode(WIFI_AP_STA);` is not followed by the "
      "softAP() that keys it",)),
    # The BOOT sweep's every-pass call, and a server no route rule reads.
    ("loop() polls the BOOT button only on its edge", _FIX_MAIN,
     "void loop() {\n  handle_boot_button();\n",
     "void loop() {\n  if (g_boot_edge) {\n    g_boot_edge = false;\n"
     "    handle_boot_button();\n  }\n",
     ("loop calls handle_boot_button other than as the whole statement "
      "`handle_boot_button();`",)),
    ("a diagnostics WebServer beside the routes", _FIX_MAIN, "",
     "static WebServer g_diag(8080);\nstatic void diag_begin() {\n"
     "  g_diag.on(\"/diag/restart\", HTTP_POST, []() { ESP.restart(); });\n"
     "  g_diag.begin();\n}\n",
     ("another HTTP server named outside every function: 'static WebServer g_diag(8080);' "
      "— its GATE_CALL_SITES row allows it nowhere",)),
    ("an unauthenticated OTA push started from setup()", _FIX_MAIN,
     _FIX_SETUP_PRINT, _FIX_SETUP_PRINT + "  ArduinoOTA.begin();\n",
     ("another HTTP server named in setup",)),
    ("an ESPAsyncWebServer beside the routes", _FIX_MAIN, "",
     "#include <ESPAsyncWebServer.h>\nstatic AsyncWebServer g_diag(8080);\n"
     "static void diag_begin() {\n  g_diag.on(\"/diag/restart\", HTTP_POST, "
     "[](AsyncWebServerRequest* r) { r->send(200); ESP.restart(); });\n  g_diag.begin();\n}\n",
     ("another HTTP server named outside every function: 'static AsyncWebServer "
      "g_diag(8080);'",)),
    ("a raw WiFiServer log console", _FIX_MAIN, "",
     "static WiFiServer g_console(23);\nstatic void console_poll() {\n"
     "  WiFiClient c = g_console.available();\n  if (c) c.println(\"canary log\");\n}\n",
     ("another HTTP server named outside every function: 'static WiFiServer g_console(23);'",)),
    ("core 3.x's NetworkServer started in a helper", _FIX_MAIN, "",
     "static void console_begin() {\n  static NetworkServer s(23);\n  s.begin();\n}\n",
     ("another HTTP server named in console_begin",)),
    ("the core's WiFiProv raising its own SoftAP from setup()", _FIX_MAIN,
     _FIX_SETUP_PRINT, _FIX_SETUP_PRINT + "  WiFiProv.beginProvision(WIFI_PROV_SCHEME_SOFTAP, "
     "WIFI_PROV_SCHEME_HANDLER_NONE, WIFI_PROV_SECURITY_0, NULL, \"SecuraCV-Recovery\");\n",
     ("another HTTP server named in setup",)),
    ("a WiFiProvClass instance of its own raising the SoftAP", _FIX_MAIN, "",
     "static WiFiProvClass s_prov;\nstatic void recovery_begin() {\n"
     "  s_prov.beginProvision(WIFI_PROV_SCHEME_SOFTAP, WIFI_PROV_SCHEME_HANDLER_NONE, "
     "WIFI_PROV_SECURITY_0, NULL, \"SecuraCV-Recovery\");\n}\n",
     ("another HTTP server named outside every function: 'static WiFiProvClass s_prov;'",)),
    # ...and the ESP-IDF provisioning manager it wraps, called directly (IDF's
    # wifi_prov_mgr example, ported; handed no service key, its SoftAP is open).
    ("the ESP-IDF provisioning manager raising its own SoftAP from setup()", _FIX_MAIN,
     _FIX_SETUP_PRINT, _FIX_SETUP_PRINT + "  { wifi_prov_mgr_config_t cfg = { .scheme = "
     "wifi_prov_scheme_softap, .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE };\n"
     "    wifi_prov_mgr_init(cfg);\n    wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_0, "
     "NULL, \"SecuraCV-Recovery\", NULL); }\n",
     ("another HTTP server named in setup",)),
    ("core 3.x's network provisioning manager started in a helper", _FIX_MAIN, "",
     "static void recovery_begin() {\n  network_prov_mgr_config_t cfg = {};\n"
     "  network_prov_mgr_init(cfg);\n}\n",
     ("another HTTP server named in recovery_begin",)),
    # A way out of loop() above the BOOT sweep, or above the setup timeout,
    # skips it on every pass that takes it.
    ("loop() returns early above the BOOT sweep", _FIX_MAIN,
     "void loop() {\n  handle_boot_button();\n",
     "void loop() {\n  if (g_console_attached) return;\n  handle_boot_button();\n",
     ("loop can leave before its handle_boot_button call: 'if (g_console_attached) return;'",
      "loop can leave before its setup_check_timeout call: 'if (g_console_attached) "
      "return;'")),
    ("loop() returns early above the setup timeout", _FIX_MAIN,
     "  handle_boot_button();\n#if FEATURE_SETUP_WIZARD\n",
     "  handle_boot_button();\n  if (g_ota_writing) { delay(10); return; }\n"
     "#if FEATURE_SETUP_WIZARD\n",
     ("loop can leave before its setup_check_timeout call: 'return;'",)),
    # A second begin() through `network`, the binding setup() itself uses on the
    # tree (its banner block): handed "", it raises the SoftAP open.
    ("a second start through setup()'s own `network` binding", _FIX_MAIN, _FIX_SETUP_PRINT,
     _FIX_SETUP_PRINT + "  ScvNetworkManager& network = network_get_instance();\n"
     "  if (!network.isTlsEnabled()) network.begin(device.ap_ssid, \"\", device.device_id);\n",
     ("setup calls ScvNetworkManager::begin other than as the whole statement",
      "ScvNetworkManager::begin is called 2 times by setup; its GATE_CALL_SITES row says "
      "exactly once")),
    # A second begin() through a spelling the census did not read.
    ("begin through the accessor's address", _FIX_MAIN, "",
     "static void network_restart_setup() {\n  (&network_get_instance())->begin(\"x\", \"\");\n}\n",
     ("ScvNetworkManager::begin called from network_restart_setup",
      "names network_get_instance() other than")),
    ("begin through a function returning the manager", _FIX_MAIN, "",
     "static ScvNetworkManager& mgr() { return network_get_instance(); }\n"
     "static void network_restart_setup() {\n  mgr().begin(\"x\", \"\");\n}\n",
     ("names the network manager 'mgr'",)),
    ("begin through a cast", _FIX_MAIN, "",
     "static void network_restart_setup() {\n"
     "  static_cast<ScvNetworkManager&>(network_get_instance()).begin(\"x\", nullptr);\n}\n",
     ("ScvNetworkManager::begin called from network_restart_setup",)),
    ("begin on the instance itself", _FIX_NET, "",
     "static void network_restart_open(const char* s) { s_network.begin(s, \"\"); }\n",
     ("s_network called from network_restart_open",)),
    ("begin through a brace-initialized auto", _FIX_MAIN, "",
     "static void restart_setup() {\n  auto& m{network_get_instance()};\n"
     "  m.begin(\"x\", \"\");\n}\n",
     ("names the network manager 'm' (a deduced type)",)),
    ("begin through a direct-initialized reference", _FIX_MAIN, "",
     "static void restart_setup() {\n  ScvNetworkManager& m(network_get_instance());\n"
     "  m.begin(\"x\", \"\");\n}\n", ("names the network manager 'm'",)),
    ("begin through (*this)", _FIX_NET, "  if (!ap_ok) return;\n",
     "  if (!ap_ok) return;\n  (*this).begin(m_ap_ssid, \"\");\n",
     ("ScvNetworkManager::begin called from ScvNetworkManager::raiseAp",)),
    ("begin through an auto alias of net", _FIX_MAIN,
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     "    auto& mgr = net;\n    mgr.begin(ap_ssid, \"\");\n"
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     ("names the network manager 'mgr' (a deduced type)",)),
    ("begin through an auto pointer to net", _FIX_MAIN,
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     "    auto* mgr = &net;\n    mgr->begin(ap_ssid, \"\");\n"
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     ("names the network manager 'mgr' (a deduced type)",)),
    ("begin through a member returning *this", _FIX_NET_H, "  ScvNetworkManager();\n",
     "  ScvNetworkManager();\n  ScvNetworkManager& self() { return *this; }\n",
     ("names the network manager 'self'", "hands the network manager on as a value")),
    ("begin through a decltype alias", _FIX_MAIN, "",
     "static void restart_setup() {\n"
     "  decltype(network_get_instance()) m = network_get_instance();\n"
     "  m.begin(\"x\", \"\");\n}\n",
     ("names the network manager 'm' (a deduced type)", "names network_get_instance() other than")),
    ("the manager captured under another name", _FIX_MAIN,
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     "    auto later = [&m = net]() { m.begin(\"x\", \"\"); };\n    later();\n"
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     ("hands the network manager on as a value",)),
    ("the manager handed to a template", _FIX_MAIN,
     "    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     "    restart_any(net);\n    if (net.begin(ap_ssid, g_ap_password, device.device_id)) {\n",
     ("hands the network manager on as a value",)),
    ("core 3's AP class through a reference", _FIX_NET, "  if (!ap_ok) return;\n",
     "  if (!ap_ok) return;\n  { auto& apc = WiFi.AP; apc.create(m_ap_ssid, nullptr); }\n",
     ("WiFi.AP named in ScvNetworkManager::raiseAp",)),
    ("the radio under another name", _FIX_MAIN, "",
     "static void keep_setup_ap_up() {\n  auto& w = WiFi;\n  w.mode(WIFI_AP_STA);\n}\n",
     ("names WiFi other than with a member after it",)),
    # A mint moved ahead of the key it reads.
    ("mint: the SoftAP key minted before the key is established", _FIX_MAIN,
     _FIX_PROVISION + "  DeviceIdentity& device = witness_get_device();\n" + _FIX_MINT_SRC,
     "  DeviceIdentity& device = witness_get_device();\n" + _FIX_MINT_SRC + _FIX_PROVISION,
     ("setup() calls derive_ap_password() before",)),
    ("mint: the token minted before the key is established", _FIX_WITNESS,
     "  if (!establish_identity_key()) return false;\n"
     "  if (!auth_load_or_derive(g_device.privkey)) {\n"
     "    Serial.println(\"[!!] Bearer credential provisioning failed\");\n"
     "    return false;\n  }\n",
     "  if (!auth_load_or_derive(g_device.privkey)) {\n"
     "    Serial.println(\"[!!] Bearer credential provisioning failed\");\n"
     "    return false;\n  }\n"
     "  if (!establish_identity_key()) return false;\n",
     ("witness_provision_device() calls auth_load_or_derive() before",)),
    ("mint: the key's establishment compiled out", _FIX_WITNESS,
     "  if (!establish_identity_key()) return false;\n",
     "#if 0\n  if (!establish_identity_key()) return false;\n#endif\n",
     ("witness_provision_device() no longer holds `if (!establish_identity_key()) return "
      "false;`",)),
    ("mint: the key's establishment ignored", _FIX_WITNESS,
     "  if (!establish_identity_key()) return false;\n", "  (void)establish_identity_key();\n",
     ("witness_provision_device() no longer holds `if (!establish_identity_key()) return "
      "false;`",)),
    # A way out before the anchor skips the key's establishment and still
    # answers: a degraded boot's `return true;` hands setup() a success over an
    # all-zero private key, and setup() mints the SoftAP key from it.
    ("mint: a degraded boot returns before the key is established", _FIX_WITNESS,
     "  if (!establish_identity_key()) return false;\n",
     "  if (!nvs_identity_readable()) {\n"
     "    Serial.println(\"[!] NVS unreadable: booting degraded, no stored identity\");\n"
     "    return true;\n  }\n"
     "  if (!establish_identity_key()) return false;\n",
     ("witness_provision_device() can leave before `if (!establish_identity_key()) return "
      "false;` (MINT_AFTER): 'return true;'",)),
    ("mint: setup() returns before the key is established", _FIX_MAIN, _FIX_PROVISION,
     "  if (recovery_console_requested()) return;\n" + _FIX_PROVISION,
     ("setup() can leave before `if (!witness_provision_device())",)),
    # An NVS record by its literal key, and the private key's record.
    ("the roll-call answers the token's record by its literal key", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  char tb[40] = {0};\n"
     "  nvs.getBytes(\"api_token\", tb, sizeof(tb) - 1);\n"
     "  httpd_resp_set_hdr(req, \"X-Diag\", tb);\n",
     ("the string literal 'api_token' is NVS_KEY_TOKEN's value",)),
    ("the roll-call plants a bearer by its literal key", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  char tb[40] = {0};\n"
     "  const int n = httpd_req_recv(req, tb, sizeof(tb) - 1);\n"
     "  if (n >= 35) nvs.putBytes(\"api_token\", tb, (size_t)n);\n",
     ("the string literal 'api_token' is NVS_KEY_TOKEN's value",)),
    ("the roll-call answers the TLS key's record, its key joined from two literals", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  uint8_t kb[1300];\n"
     "  const size_t kl = prefs.getBytes(\"tls_\" \"key\", kb, sizeof(kb));\n"
     "  return httpd_resp_send(req, (const char*)kb, kl);\n",
     ("the string literal 'tls_key' is NVS_KEY_TLS_KEY's value",)),
    ("the first-boot flag cleared by an escaped literal", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  prefs.remove(\"setup\\x5fok\");\n",
     ("the string literal 'setup_ok' is NVS_KEY_SETUP_OK's value",)),
    ("the roll-call reads the private key's record", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  uint8_t pk[32];\n  nvs.getBytes(NVS_KEY_PRIV, pk, 32);\n",
     ("NVS_KEY_PRIV called from handle_fleet",)),
    ("the roll-call loads the private key", _FIX_NET, "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  uint8_t pk[32];\n  (void)nvs_load_key(pk);\n",
     ("nvs_load_key called from handle_fleet",)),
    # The first-boot flag and the reset that clears it.
    ("the roll-call clears the first-boot flag and restarts", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  prefs.remove(NVS_KEY_SETUP_OK);\n  ESP.restart();\n",
     ("NVS_KEY_SETUP_OK called from handle_fleet",)),
    ("a new caller of the credentials reset", _FIX_MAIN, "",
     "static void on_setup_rerun_requested() {\n"
     "  factory_reset(canary::io::ResetScope::CredentialsAndSettings);\n}\n",
     ("factory_reset called from on_setup_rerun_requested",)),
    ("a new caller of the hold's dispatch", _FIX_MAIN, "",
     "static void on_reset_requested() {\n"
     "  reset_hold_dispatch(canary::io::HoldEvent::Released);\n}\n",
     ("reset_hold_dispatch called from on_reset_requested",)),
    # from_ap_subnet's interface helper, and the lockout's source key.
    ("netif_ip4 reads the other interface", _FIX_NET,
     "  esp_netif_t* nif = esp_netif_get_handle_from_ifkey(ifkey);\n",
     "  esp_netif_t* nif = esp_netif_get_handle_from_ifkey(\n"
     "      strcmp(ifkey, \"WIFI_AP_DEF\") == 0 ? \"WIFI_STA_DEF\" : \"WIFI_AP_DEF\");\n",
     ("netif_ip4 changed",)),
    ("the lockout keyed on a value that changes per request", _FIX_NET,
     "  return peer_of(req);\n}\nstatic void send_host_refusal",
     "  return peer_of(req) ^ esp_random();\n}\nstatic void send_host_refusal",
     ("request_peer_ipv4 changed",)),
    # Code after a raw literal, which the walkers once read as a string to the end
    # of its file.
    ("a reader after a raw literal", _FIX_NET, "",
     "static const char kPage[] = R\"html(<p class=\"x\">\" // </p>)html\";\n"
     "static const char* page_value() { return auth_get_token(); }\n",
     ("page_value() reads the bearer token and is no",)),
    ("the tap opened between two raw literals", _FIX_MAIN, "",
     "static const char kA[] = R\"(\")\";\n"
     "static void tap_for_peers() {\n"
     "  canary::net::provisioning_gate::open(g_prov_gate, millis());\n}\n"
     "static const char kB[] = R\"(\")\";\n",
     ("g_prov_gate called from tap_for_peers",)),
    # The page's emitter and the TLS key's loader, reached another way.
    ("the page emitter through a function pointer", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static esp_err_t (*const s_page_send)(httpd_req_t*, const char*, PageToken) = "
     "send_html_with_token;\nstatic esp_err_t handle_fleet(httpd_req_t* req) {\n"
     "  return s_page_send(req, \"__CV_TOKEN__\", PageToken::INJECT);\n",
     ("send_html_with_token() called outside every function",)),
    ("the page emitter through a reference", _FIX_NET, "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  { auto& page = send_html_with_token; "
     "return page(req, \"__CV_TOKEN__\", PageToken::INJECT); }\n",
     ("send_html_with_token called from handle_fleet",)),
    ("a public route stores a TLS key of its own", _FIX_NET, "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n"
     "  { uint8_t k[8] = {0}; (void)tls_store_der_to_nvs(k, 8); }\n",
     ("tls_store_der_to_nvs called from handle_fleet",)),
    ("the TLS key's loader from a public route", _FIX_NET, "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  { uint8_t* k = nullptr; size_t kl = 0;\n"
     "    if (tls_load_der_from_nvs(&k, &kl)) return httpd_resp_send(req, (const char*)k, kl); }\n",
     ("tls_load_der_from_nvs called from handle_fleet",)),
    # The BOOT tap at its source.
    ("a press faked by the clock", _FIX_MAIN,
     "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);\n",
     "  bool pressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW) || (millis() % 4000) < 200;\n",
     ("handle_boot_button changed",)),
    ("the peek hook writes the tap's stamp", _FIX_MAIN, "static bool prov_gate_is_open_hook() {\n",
     "static bool prov_gate_is_open_hook() {\n  g_prov_gate.opened_at_ms = millis();\n",
     ("prov_gate_is_open_hook changed",)),
    ("a public route runs the peek hook", _FIX_NET, "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n  (void)(s_gate_is_open && s_gate_is_open());\n",
     ("s_gate_is_open called from handle_fleet",)),
    # The AuthManager's own `this`.
    ("the AuthManager's this saved by its constructor", _FIX_AUTH,
     "AuthManager::AuthManager() : m_table{} {}\n",
     "static AuthManager* s_self = nullptr;\n"
     "AuthManager::AuthManager() : m_table{} { s_self = this; }\n"
     "void auth_fleet_seen(uint32_t s) {\n  if (s_self) s_self->handedToken(s);\n}\n",
     ("this (the auth lib) named in AuthManager::AuthManager",)),
    # The first-boot unlock's length.
    ("the setup timeout widened", _FIX_SETUP_H, "(15UL * 60UL * 1000UL)", "(0xFFFFFFFFUL)",
     ("PINNED_DEFINES: SETUP_TIMEOUT_MS is defined once",)),
    ("the setup timeout redefined", _FIX_SETUP, "",
     "#undef SETUP_TIMEOUT_MS\n#define SETUP_TIMEOUT_MS (0xFFFFFFFFUL)\n",
     ("#undef SETUP_TIMEOUT_MS", "PINNED_DEFINES: SETUP_TIMEOUT_MS is defined twice")),
    # A request's raw path through a reference to it.
    ("a query credential through a reference to the request", _FIX_NET,
     "  prefs.begin(\"fleet\", true);\n",
     "  prefs.begin(\"fleet\", true);\n"
     "  { const httpd_req_t& r = *req; if (strstr(r.uri, \"cv_session=\")) return ESP_OK; }\n",
     ("handle_fleet reads a request's raw path",)),
    # Branches the round-2 repair added that no plant held.
    ("the instance's address, direct-initialized at namespace scope", _FIX_AUTH, "",
     "static AuthManager* const s_view(&auth_get_instance());\n",
     ("auth_get_instance() called outside every function",)),
    ("a macro spells the private key for a handler", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "#define DEV_PK privkey\nstatic esp_err_t handle_fleet(httpd_req_t* req) {\n"
     "  { char pk[65]; hex_to_str(pk, witness_get_device().DEV_PK, 32); "
     "httpd_resp_set_hdr(req, \"X-K\", pk); }\n",
     ("a macro names 'privkey'",)),
    ("an inline header member's bare begin", _FIX_NET_H, "  ScvNetworkManager();\n",
     "  ScvNetworkManager();\n  void restartOpen() { begin(m_ap_ssid, \"\"); }\n",
     ("ScvNetworkManager::begin called from restartOpen",)),
    ("a manager pointer named other than net or network", _FIX_MAIN, "",
     "static void restart_setup() {\n  ScvNetworkManager* p = &network_get_instance();\n"
     "  p->begin(\"x\", \"\");\n}\n", ("names the network manager 'p'",)),
    ("begin through a member pointer", _FIX_MAIN, "",
     "static void restart_open() {\n  auto b = &ScvNetworkManager::begin;\n"
     "  (network_get_instance().*b)(\"x\", \"\");\n}\n",
     ("ScvNetworkManager::begin called from restart_open",)),
    ("a namespace-scope lambda's do-statement clears the backoff", _FIX_NET, "",
     "static auto clear_backoff = [](uint32_t s) { do auth_handed_token(s); while (false); };\n",
     ("auth_handed_token() called outside every function",)),
    ("a co_return before Wi-Fi connect's refusal", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (from_ap_subnet(req)) co_return wifi_connect_apply(req);\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect can leave before its auth_gate call: 'if (from_ap_subnet(req)) "
      "co_return",)),
    ("the tap's sweep moved after its open (a census row's order)", _FIX_MAIN, _FIX_BOOT_BODY,
     _FIX_BOOT_BODY[len(_FIX_BOOT_SWEEP):] + _FIX_BOOT_SWEEP,
     ("g_prov_gate: handle_boot_button's statements that name it are not its row",)),
    # What the compiler reads, not what the text shows: a backslash-newline
    # splits a name, a digraph spells a `#` or a brace.
    ("a reader split by a backslash-newline", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  httpd_resp_set_hdr(req, \"X-Diag\", auth_get_\\\ntoken());\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     ("public route handler handle_fleet() answers the bearer token",)),
    ("a #define split after its #", _FIX_MAIN, "static char g_ap_password[16];\n",
     "#\\\ndefine take(s, n, t) is_open(s, n, t)\nstatic char g_ap_password[16];\n",
     ("a macro names 'take'",)),
    ("a #define spelled with a digraph", _FIX_NET, "",
     "%:define auth_gate(r) (true)\n", ("a macro names 'auth_gate'",)),
    ("a #define whose name the check cannot read", _FIX_NET, "",
     "#define (auth_gate) true\n", ("a #define whose name the check cannot read",)),
    # A macro of a keyword, or of a value a pinned body reads.
    ("a macro makes every `return false;` below it a `return true;`", _FIX_NET, "",
     "#define false true\n", ("a macro names 'false'",)),
    ("the tap's TTL undefined and widened under ROOTS", _FIX_MAIN,
     "static char g_ap_password[16];\n",
     "#undef PROVISIONING_GATE_TTL_MS\n#define PROVISIONING_GATE_TTL_MS 0xFFFFFFFFUL\n"
     "static char g_ap_password[16];\n",
     ("#undef PROVISIONING_GATE_TTL_MS", "a macro names 'PROVISIONING_GATE_TTL_MS'")),
    ("a BOOT press threshold of 0 under ROOTS", _FIX_MAIN, "static char g_ap_password[16];\n",
     "#define BOOT_SHORT_PRESS_MS 0\nstatic char g_ap_password[16];\n",
     ("a macro names 'BOOT_SHORT_PRESS_MS'",)),
    ("Wi-Fi connect's refusal nested in a digraph block", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!from_ap_subnet(req)) <% ;\n    if (!auth_gate(req)) return ESP_OK;\n  %>\n",
     ("handle_wifi_connect calls auth_gate other than at its own top level",)),
    ("mint: compiled out under a digraph block", _FIX_MAIN, _FIX_MINT_SRC,
     "  if (false) <% ;\n" + _FIX_MINT_SRC + "  %>;\n",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    # Round-4 review: a name built at its use site, which no rule reads by its
    # spelling. A token paste names nothing held in its #define, and its call
    # never spells the name it pastes.
    ("a token-paste macro answers the token", _FIX_NET, _FIX_FLEET,
     "#define CV_JOIN(a, b) a##b\n" + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", CV_JOIN(auth_get_, token)());\n",
     (_PASTE,)),
    ("a token paste with spaces answers the token", _FIX_NET, _FIX_FLEET,
     "#define TOK auth_get ## _token\n" + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", TOK());\n", (_PASTE,)),
    ("a token paste onto __VA_ARGS__ answers the token", _FIX_NET, _FIX_FLEET,
     "#define X(...) auth_get##__VA_ARGS__\n" + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", X(_token)());\n", (_PASTE,)),
    ("a token paste clears the roll-call's own lockout", _FIX_NET, _FIX_FLEET,
     "#define JOIN(a, b) a##b\n" + _FIX_FLEET
     + "  JOIN(auth_handed_, token)(request_peer_ipv4(req));\n", (_PASTE,)),
    ("a token paste spelled with a digraph", _FIX_NET, _FIX_FLEET,
     "#define JOIN(a, b) a %:%: b\n" + _FIX_FLEET
     + "  JOIN(auth_handed_, token)(request_peer_ipv4(req));\n", (_PASTE,)),
    ("a token paste spends the tap", _FIX_MAIN, "static char g_ap_password[16];\n",
     "#define GATE g_prov##_gate\nstatic char g_ap_password[16];\n", (_PASTE,)),
    # ...and assembly, which reaches a function by its symbol.
    ("an asm label answers the token", _FIX_NET, _FIX_FLEET,
     "extern const char* cv_t() __asm__(\"_Z14auth_get_tokenv\");\n" + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", cv_t());\n", (_ASM_WORDS,)),
    ("an asm statement clears the roll-call's own lockout", _FIX_NET, _FIX_FLEET,
     _FIX_FLEET + "  asm volatile(\"call8 _Z17auth_handed_tokenm\" ::: \"memory\");\n",
     (_ASM_WORDS,)),
    # ...and the asm siblings, which bind a name to a symbol the same way: a
    # weakref / alias attribute, a __REDIRECT macro, a redefine_extname pragma
    # (as #pragma or _Pragma), and a name spelled as a C++ symbol.
    ("a weakref attribute answers the token", _FIX_NET, _FIX_FLEET,
     'static const char* cv_t() __attribute__((weakref("_Z14auth_get_tokenv")));\n' + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", cv_t());\n", (_BIND_WORDS,)),
    ("a weakref attribute clears the roll-call's own lockout", _FIX_NET, _FIX_FLEET,
     'static void cv_seen(uint32_t) __attribute__((__weakref__("_Z17auth_handed_tokenm")));\n'
     + _FIX_FLEET + "  cv_seen(request_peer_ipv4(req));\n", (_BIND_WORDS,)),
    ("an alias attribute answers the token", _FIX_NET, _FIX_FLEET,
     'extern "C" const char* cv_t() __attribute__((alias("_Z14auth_get_tokenv")));\n' + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", cv_t());\n", (_BIND_WORDS,)),
    ("a __REDIRECT macro answers the token", _FIX_NET, _FIX_FLEET,
     'extern const char* __REDIRECT(cv_t, (), _Z14auth_get_tokenv);\n' + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", cv_t());\n", (_BIND_WORDS,)),
    ("a redefine_extname pragma answers the token", _FIX_NET, _FIX_FLEET,
     'extern "C" const char* cv_t();\n#pragma redefine_extname cv_t _Z14auth_get_tokenv\n'
     + _FIX_FLEET + "  httpd_resp_set_hdr(req, \"X-D\", cv_t());\n", (_BIND_WORDS,)),
    ("a _Pragma redefine_extname answers the token", _FIX_NET, _FIX_FLEET,
     'extern "C" const char* cv_t();\n_Pragma("redefine_extname cv_t _Z14auth_get_tokenv")\n'
     + _FIX_FLEET + "  httpd_resp_set_hdr(req, \"X-D\", cv_t());\n", (_BIND_WORDS,)),
    ("a name spelled as a C++ symbol answers the token", _FIX_NET, _FIX_FLEET,
     'extern "C" const char* _Z14auth_get_tokenv();\n' + _FIX_FLEET
     + "  httpd_resp_set_hdr(req, \"X-D\", _Z14auth_get_tokenv());\n", (_BIND_WORDS,)),
    # The NVS literal rule reads every file's literals, a file that holds no
    # copy of a key's value included.
    ("the first-boot flag's key joined, in a file that names no key", _FIX_SETUP, "",
     'static const char* const kFlagName = "setup_" "ok";\n',
     ("the string literal 'setup_ok' is NVS_KEY_SETUP_OK's value",)),
    ("the token's key escaped, in a file that names no key", _FIX_SETUP, "",
     'static const char* const kTokName = "api\\x5ftoken";\n',
     ("the string literal 'api_token' is NVS_KEY_TOKEN's value",)),
    # The port-80 hint's two members and the TLS state they read.
    ("the hint's TLS server handle answers the port-80 server's", _FIX_NET_H,
     "httpd_handle_t getHttpsServer() const { return m_https_server; }",
     "httpd_handle_t getHttpsServer() const { return m_http_server; }",
     ("getHttpsServer changed",)),
    ("the hint's TLS flag answers false", _FIX_NET_H,
     "bool isTlsEnabled() const { return m_tls_enabled; }",
     "bool isTlsEnabled() const { return false; }", ("isTlsEnabled changed",)),
    ("the TLS flag cleared once HTTPS is up", _FIX_NET,
     "      registerHttpHandlers(m_https_server);\n",
     "      registerHttpHandlers(m_https_server);\n      m_tls_enabled = false;\n",
     ("GATE_CALL_SITES row m_tls_enabled: ScvNetworkManager::startHttpServer's statements",)),
    ("the TLS server's handle pointed at the port-80 server", _FIX_NET, "",
     "bool ScvNetworkManager::startRedirectServer() {\n"
     "  if (httpd_start(&m_http_server, nullptr) != ESP_OK) return false;\n"
     "  m_https_server = m_http_server;\n  return true;\n}\n",
     ("m_https_server called from ScvNetworkManager::startRedirectServer, which its "
      "GATE_CALL_SITES row does not name",)),
)


def _with(key: str, change):
    """A table plant: `change` applied to a copy of the fixture table `key`."""
    def plant(tables):
        tables = dict(tables)
        tables[key] = change(tables.get(key))
        return tables
    return plant


_FIX_TABLE_PLANTS = (
    ("a ROOTS_DEFINES row the tree does not hold",
     _with("roots_defines", lambda r: dict(r or {}, PROVISIONING_GATE_TTL_MS=(_FIX_MAIN, 1))),
     ("ROOTS_DEFINES: PROVISIONING_GATE_TTL_MS is defined 0 times under ROOTS",)),
    ("a stale marker left in AUTH_MARKERS (F340 deleted auth_check_optional)",
     _with("markers", lambda m: tuple(m or AUTH_MARKERS) + ("auth_check_optional(",)),
     ("AUTH_MARKERS entry 'auth_check_optional(' names no GATE_CALL_SITES row",)),
    ("a PRE_HOST_STEPS entry naming nothing", _with("steps", lambda s: tuple(s or PRE_HOST_STEPS)
                                                    + ("auth_check_optional(",)),
     ("PRE_HOST_STEPS entry 'auth_check_optional(' names no GATE_CALL_SITES row",)),
    ("a SECRET_ANSWERS row naming no handler",
     _with("answers", lambda a: dict(a, handle_gone=("token", "page"))),
     ("SECRET_ANSWERS names handle_gone(), which is no registered route handler",)),
    ("a grant that is no grant",
     _with("answers", lambda a: dict(a, handle_ui=("token", "session"))),
     ("gives handle_ui() the grant 'session'",)),
    ("a public route with the bearer grant",
     _with("public", lambda p: dict(p) | {("GET", "/api/status"): "x"}),
     ("handle_status() is a PUBLIC_ALLOWLIST route handler with the 'bearer' grant",)),
    ("a local-use row for a function that is gone",
     _with("local", lambda u: dict(u) | {(_NET, "ScvNetworkManager::gone"): ("x", ())}),
     ("SECRET_LOCAL_USES names ScvNetworkManager::gone() in securacv_network.cpp, which does "
      "not define it",)),
    ("a local-use row reading nothing",
     _with("local", lambda u: dict(u) | {(_NET, "ScvNetworkManager::idle"): ("x", ())}),
     ("SECRET_LOCAL_USES names ScvNetworkManager::idle() in securacv_network.cpp, which reads "
      "no credential",)),
    ("a local-use row naming a route handler",
     _with("local", lambda u: dict(u) | {(_NET, "handle_status"): (
         "x", ("doc[\"tls_cert_fp\"] = net.getTlsCertFp();",))}),
     ("handle_status() is a route handler, so it answers",)),
    ("a GLUE_DEFS row whose function is gone",
     _with("glue", lambda g: dict(g) | {(_NET, "gone_glue"): ("x", "y")}),
     ("GLUE_DEFS pins gone_glue() in securacv_network.cpp, which ROOTS defines 0 times",)),
    ("a census row with no call",
     _with("census", lambda c: dict(c or GATE_CALL_SITES) | {"gone_gate": {"callers": None}}),
     ("GATE_CALL_SITES row gone_gate has no call",)),
    ("a census caller that no longer calls",
     _with("census", lambda c: dict(c or GATE_CALL_SITES) | {"auth_check": {
         "callers": {(_NET, "auth_gate"), (_NET, "handle_status")}}}),
     ("GATE_CALL_SITES row auth_check names handle_status (securacv_network.cpp), which no "
      "longer calls it",)),
    ("an OUTSIDE_FUNCTION_LINES row naming nothing",
     _with("outside", lambda o: tuple(o) + ((_NET, "char m_gone[1];"),)),
     ("OUTSIDE_FUNCTION_LINES holds 'char m_gone[1];'",)),
    ("an emitter row reading nothing",
     _with("emitters", lambda e: dict(e or TOKEN_EMITTERS) | {"send_gone": ("token", "x")}),
     ("TOKEN_EMITTERS names send_gone(), which reads no credential",
      "TOKEN_EMITTERS names send_gone(), which has 0 EMITTER_STATEMENTS rows")),
    ("an EMITTER_STATEMENTS row naming no emitter",
     _with("emitter_stmts", lambda e: dict(e) | {(_NET, "send_gone"): ()}),
     ("EMITTER_STATEMENTS names send_gone() (securacv_network.cpp), which is no TOKEN_EMITTERS "
      "key",)),
    ("a census row with no home",
     _with("census", lambda c: dict(c or GATE_CALL_SITES) | {"auth_check": {
         "callers": {(_NET, "auth_gate")}}}),
     ("GATE_CALL_SITES row auth_check names no home",)),
    ("a URI_READERS row naming no reader",
     _with("uri", lambda u: dict(u) | {(_NET, "handle_ui"): "x"}),
     ("URI_READERS names handle_ui (securacv_network.cpp), which no longer reads it",)),
    # Round-2 review: a row's file is part of it (a glue body, a census home).
    ("a GLUE_DEFS row keyed to another file than its body's",
     _with("glue", lambda g: {(("securacv_other.cpp" if q == "provisioning_gate_take" else f), q):
                              v for (f, q), v in g.items()}),
     ("GLUE_DEFS pins provisioning_gate_take() in securacv_other.cpp, which ROOTS defines once "
      "(fx/lib/securacv_network.cpp)",)),
    ("a census home in another file than the definition",
     _with("census", lambda c: dict(c or GATE_CALL_SITES) | {"auth_check": dict(
         (c or GATE_CALL_SITES)["auth_check"], home=(_NET, "auth_check"))}),
     ("GATE_CALL_SITES row auth_check: ROOTS defines it once (auth_check "
      "(fx/lib/securacv_auth.cpp))",)),
    ("a URL_QUERY_READERS row naming no reader",
     _with("queries", lambda q: set(q or URL_QUERY_READERS) | {(_NET, "handle_ui")}),
     ("URL_QUERY_READERS names handle_ui (securacv_network.cpp), which no longer reads it",)),
    # Round-4 review: the table-hygiene branches, each a plant.
    ("two ANSWER_STATEMENTS rows for one handler",
     _with("answer_stmts", lambda a: dict(a) | {(_AUTH, "handle_ui"): ()}),
     ("SECRET_ANSWERS names handle_ui(), which has 2 ANSWER_STATEMENTS rows",)),
    ("an ANSWER_STATEMENTS row naming no answer",
     _with("answer_stmts", lambda a: dict(a) | {(_NET, "handle_gone"): ()}),
     ("ANSWER_STATEMENTS names handle_gone() (securacv_network.cpp), which is no SECRET_ANSWERS "
      "key",)),
    ("an ANSWER_STATEMENTS row keyed to another file than its handler's",
     _with("answer_stmts", lambda a: {(_AUTH if q == "handle_ui" else f, q): v
                                      for (f, q), v in a.items()}),
     ("ANSWER_STATEMENTS: handle_ui() is defined in fx/lib/securacv_network.cpp; its row says "
      "securacv_auth.cpp",)),
    ("a MINT_AFTER row naming no function",
     _with("mint_after", lambda m: tuple(m or MINT_AFTER) + (
         (_MAIN, "gone_setup", "derive_ap_password", MINT_AFTER[0][3]),)),
     ("MINT_AFTER names gone_setup() in src/main.cpp, which does not define it",)),
    ("a MINT_AFTER row whose function no longer calls its mint",
     _with("mint_after", lambda m: tuple(m or MINT_AFTER) + (
         (_MAIN, "setup", "derive_api_token", MINT_AFTER[0][3]),)),
     ("MINT_AFTER: setup() (fx/src/main.cpp) no longer calls derive_api_token()",)),
)


# The flagship's configuration in small, as the library rule's self-test writes
# it into a temp dir (relative paths under it): platformio.ini with [env]'s
# library list, envs that extend [env:dev], and extra_configs files, one with
# the platform section [env]'s `platform` interpolates and one with two envs of
# its own (as firmware/envs/platformio/platforms.ini and
# firmware/provisioning/platformio_secure.ini are today), and a library under
# lib/ with the tree's manifest form.
_PIO_TODAY = {
    "canary/platformio.ini": (
        "; the flagship's configuration, in small\n[platformio]\ndefault_envs = dev\n"
        "extra_configs =\n    ../envs/platforms.ini\n    ../provisioning/secure.ini\n\n"
        "[env]\nplatform = ${platform_s3c3.platform}\nframework = arduino\n"
        "; Core libraries (always included)\nlib_deps =\n    rweather/Crypto @ ^0.4.0\n"
        "    bblanchon/ArduinoJson @ ^7.0.0\nbuild_flags =\n    -DBOARD_HAS_PSRAM\n"
        "    -I${PROJECT_DIR}/include\n\n[env:dev]\nbuild_flags =\n    ${env.build_flags}\n"
        "    -DFEATURE_WIFI_AP=1\n\n[env:dev_ha]\nextends = env:dev\nlib_deps =\n"
        "    ${env.lib_deps}\n    knolleary/PubSubClient @ ^2.8\n\n[env:full]\n"
        "extends = env:dev\nlib_deps =\n    ${env.lib_deps}\n    ; Must be 2.x\n"
        "    h2zero/NimBLE-Arduino@^2.3.8\nlib_ignore =\n"),
    "envs/platforms.ini": "[platform_s3c3]\nplatform = espressif32@6.9.0\n",
    "provisioning/secure.ini": (
        "[env:secure]\nplatform = ${platform_s3c3.platform}\nlib_deps =\n"
        "    rweather/Crypto @ ^0.4.0\n    bblanchon/ArduinoJson @ ^7.0.0\n\n"
        "[env:secure_ha]\nextends = env:secure\nlib_deps =\n    ${env:secure.lib_deps}\n"
        "    knolleary/PubSubClient @ ^2.8\n"),
    "canary/lib/securacv_mesh/library.json": (
        '{\n    "name": "securacv_mesh",\n    "version": "0.1.0",\n'
        '    "repository": {"type": "git", "url": "https://github.com/kmay89/securaCV.git"},\n'
        '    "license": "Apache-2.0",\n    "frameworks": "arduino",\n'
        '    "platforms": "espressif32"\n}\n'),
}


def _set_environ(values: dict) -> None:
    """Set each environment variable to its value, or unset it for None."""
    for key, value in values.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value


def _assert_answer_gate_rule() -> None:
    """The bearer-only answer rule: the head parser names what it reads, the
    clean fixture passes, and every plant fails for its named reason. Runs
    every time, before the tree."""
    # The head parser: a constructor is named for its class, its initializer
    # list included (the old parser named this body `b_`); a lambda names no
    # function, so grant's compare( is grant's; a brace-initialized member
    # and a destructor are named too.
    heads = ("Foo::Foo() : a_(0), b_(0) { x = auth_get_token(); }\n"
             "R A::grant(int s) { return g([&]() { return compare(s); }); }\n"
             "B::B() : m_t{}, n_(f(1)) { y(); }\nC::~C() { z(); }\n"
             "ns::Type D::get() const { return 1; }\nint free_fn(int a) { return a ? f(a) : 0; }\n"
             "static void h(uint32_t s) noexcept { return k(s); }\n"
             "int E::f() const override { return 2; }\n"
             "auto T::g(int s) -> void { k(s); }\n"
             "static auto t2(int s) noexcept -> std::pair<int, int> { return {s, s}; }\n")
    spans = function_spans(heads)
    assert [n for n, _s, _e in spans] == ["Foo::Foo", "A::grant", "B::B", "C::~C", "D::get",
                                          "free_fn", "h", "E::f", "T::g", "t2"], spans
    # (a `noexcept` helper is a function, not code outside every function, and
    # definition_spans finds a member's `const` definition and a trailing
    # return type's.)
    assert definition_span(heads, "h") and definition_span(heads, "E::f")
    assert definition_span(heads, "T::g") and definition_span(heads, "t2")
    assert definition_span("int r = f(x) -> y;\nauto q = f(x);\n", "f") is None
    assert definition_span("const char* g() const { return m_x; }\n", "g") == (12, 37)
    for needle, owner in (("auth_get_token", "Foo::Foo"), ("compare(", "A::grant"),
                          ("y()", "B::B"), ("f(1)", "B::B")):
        assert enclosing_function(spans, heads.find(needle))[0] == owner, (needle, spans)
    assert definition_span("AuthManager::Presented AuthManager::compare(int a) { return a; }\n"
                           "bool constantTimeCompare(int b) { return b; }\n",
                           "AuthManager::compare")[0] == 23
    assert definition_span("bool constantTimeCompare(int b) { return b; }\n",
                           "AuthManager::compare") is None
    clean = _fixture_sources()
    tables = _fixture_tables(clean)
    got = check_bearer_only_answers(clean, tables)
    assert got == [], "the bearer-only answer rule's clean fixture fails: " + "; ".join(got)
    for what, fname, old, new, words in _FIX_PLANTS:
        planted = _fixture_sources()
        text = planted.get(fname, "")
        assert old in text, f"fixture plant {what!r}: its text is not in {fname}"
        # Read back as the tree's files are (strip_comments: its splices and
        # digraphs), which leaves an already-read file as it is.
        planted[fname] = strip_comments(text.replace(old, new, 1) if old else text + new)
        got = check_bearer_only_answers(planted, tables)
        missing = [w for w in words if not any(w in p for p in got)]
        assert not missing, (f"the bearer-only answer rule passed a plant ({what}): expected "
                             f"problems naming {missing!r}, got {got!r}")
    for what, plant, words in _FIX_TABLE_PLANTS:
        got = check_bearer_only_answers(clean, plant(tables))
        missing = [w for w in words if not any(w in p for p in got)]
        assert not missing, (f"the bearer-only answer rule passed a table plant ({what}): "
                             f"expected problems naming {missing!r}, got {got!r}")
    # A lambda's `return` leaves the lambda, not the function: a callback
    # registered between begin()'s mode raise and its softAP() is no way out.
    calm = _fixture_sources()
    raise_then_key = "  WiFi.mode(WIFI_AP_STA);\n  bool ap_ok = WiFi.softAP(ap_ssid,"
    assert raise_then_key in calm[_FIX_NET]
    calm[_FIX_NET] = calm[_FIX_NET].replace(
        raise_then_key,
        "  WiFi.mode(WIFI_AP_STA);\n  WiFi.onEvent([](arduino_event_id_t event) {\n"
        "    if (event != ARDUINO_EVENT_WIFI_STA_GOT_IP) return;\n    start_mdns();\n  });\n"
        "  bool ap_ok = WiFi.softAP(ap_ssid,", 1)
    got = check_bearer_only_answers(calm, tables)
    assert got == [], "a callback's return between begin()'s raise and softAP() fails: " + (
        "; ".join(got))
    # ...nor in loop(), above the BOOT sweep and the setup timeout ("lead").
    calm = _fixture_sources()
    sweep = "void loop() {\n  handle_boot_button();\n"
    assert sweep in calm[_FIX_MAIN]
    calm[_FIX_MAIN] = calm[_FIX_MAIN].replace(
        sweep, "void loop() {\n  auto due = [](uint32_t now) { if (now == 0) return false; "
               "return true; };\n  (void)due(millis());\n  handle_boot_button();\n", 1)
    got = check_bearer_only_answers(calm, tables)
    assert got == [], "a lambda's return above loop()'s BOOT sweep fails: " + "; ".join(got)
    # A -D build flag is a #define too; the flags the build sets name none.
    ini = "build_flags =\n    -DFEATURE_WIFI_AP=1\n    -DCORE_DEBUG_LEVEL=0\n"
    assert check_bearer_only_answers(clean, tables, flags={"fx/platformio.ini": ini}) == []
    for flag in ("-D'auth_gate(r)=true'", '-D"host_is_foreign(r)=false"',
                 "-Dbearer_present_and_valid(r)=1", "-D'take(s,n,t)=is_open(s,n,t)'",
                 "-DSETUP_TIMEOUT_MS=0xFFFFFFFFUL", "-Dfalse=true",
                 # a held name in the flag's value only, never its name:
                 "-DDIAG_VALUE=auth_get_token()", "-D'DIAG_GATE(r)=auth_gate(r)'"):
        got = check_bearer_only_answers(clean, tables,
                                        flags={"fx/platformio.ini": ini + "    " + flag + "\n"})
        assert any("the build flag" in p and "names '" in p for p in got), (flag, got)
    # ...a flag that pastes tokens, and a build input the check does not read (a
    # linker option, a forced include, a build script, a local library, a moved
    # folder), fail; a registry library REGISTRY_LIBS names does not.
    got = check_bearer_only_answers(clean, tables, flags={
        "fx/platformio.ini": ini + "    -D'CV_JOIN(a,b)=a##b'\n"})
    assert any("the build flag" in p and "pastes tokens with ##" in p for p in got), got
    for line in ("    -Wl,--wrap=_Z10auth_checkP9httpd_reqPKcm", "    -Xlinker --defsym=x=y",
                 "    -include ../common/diag.h", "    -imacros ../common/diag.h",
                 "    -T custom.ld", "extra_scripts = pre:add_flags.py",
                 "lib_extra_dirs = ../common", "lib_deps =\n    symlink://../common/diag",
                 "lib_deps = file://../common/diag", "src_dir = other",
                 "board_build.ldscript = custom.ld",
                 # the `=` / `--` / glued long forms the first version missed
                 "    --include=../common/diag.h", "    --imacros=../common/diag.h",
                 "    -include../common/diag.h", "    -specs=nano.specs",
                 "    --specs=nano.specs", "    -fplugin=./p.so", "    -wrapper gdb,--args",
                 "    -B${PROJECT_DIR}/tools", "    -B/opt/x", "    -Xpreprocessor -include "
                 "../common/diag.h", "    -Wp,-include,../common/diag.h",
                 "    -Xassembler --defsym=x=y", "    -Wa,--defsym,x=y",
                 # an include / sysroot path the check does not read as a reach root
                 "    -isystem ../common/diag", "    -iquote ../common/diag",
                 "    -idirafter ../common/diag", "    -isystem../common/diag",
                 "    --sysroot=/opt/x", "    -iprefix ../common/ -iwithprefix diag",
                 # a response file and PlatformIO's `!command` dynamic value
                 "    @${PROJECT_DIR}/flags.rsp", "    !python3 ${PROJECT_DIR}/gen.py",
                 "build_src_flags = !python3 gen.py", "build_unflags =!echo -DX",
                 "extra_script = pre:add_flags.py",
                 # a key in any case (configparser lowercases it) and a value the
                 # build-time environment sets (the eleventh round)
                 "Extra_Scripts = pre:add_flags.py", "  LIB_EXTRA_DIRS = ../common",
                 "    ${sysenv.EXTRA_FLAGS}", "lib_deps = ${sysenv.LIB_DEPS}"):
        got = check_bearer_only_answers(clean, tables,
                                        flags={"fx/platformio.ini": ini + line + "\n"})
        assert any("is a build input the check does not read" in p for p in got), (line, got)
    registry = ini + "lib_deps =\n    rweather/Crypto @ ^0.4.0\n    ${env.lib_deps}\n"
    assert check_bearer_only_answers(clean, tables, flags={"fx/platformio.ini": registry}) == []
    # Every library an env fetches, its lib_deps as PlatformIO's own reader
    # resolves it, or a manifest names is a REGISTRY_LIBS row, or a library
    # under ROOTS by its name. Each case writes a project into a temp dir
    # (_PIO_TODAY, the flagship's configuration in small, with the case's
    # edit) and runs it as main() runs the tree: flag_sources, then
    # platformio_lib_deps (ProjectConfig, from the project's folder) and
    # library_manifests over its lib/, all fed to check_bearer_only_answers.
    # No case reads the ini's text for its libraries.

    def project(edit="", swap=None, files=None, env=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            texts = dict(_PIO_TODAY, **(files or {}))
            if swap:
                fname, old, new = swap
                assert texts[fname].count(old) == 1, (fname, old)
                texts[fname] = texts[fname].replace(old, new)
            texts["canary/platformio.ini"] += edit
            for rel, text in texts.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(text)
            ini = root / "canary" / "platformio.ini"
            flags, flag_problems = flag_sources((ini,))
            saved = {k: os.environ.get(k) for k in env or {}}
            try:
                _set_environ(env or {})
                lib_deps, pio_problems = platformio_lib_deps(flags, ini, ini.parent)
            finally:
                _set_environ(saved)
            return lib_deps, flag_problems + pio_problems + check_bearer_only_answers(
                clean, tables, flags=flags, lib_deps=lib_deps,
                manifests=library_manifests((root / "canary" / "lib",)))

    # The configuration as the flagship's is today passes, each env holding
    # what PlatformIO resolves for it ([env]'s list, an env extending another,
    # the envs an extra_configs file adds)...
    core = ["rweather/Crypto @ ^0.4.0", "bblanchon/ArduinoJson @ ^7.0.0"]
    deps, got = project()
    assert got == [] and deps == {
        "dev": core, "dev_ha": core + ["knolleary/PubSubClient @ ^2.8"],
        "full": core + ["h2zero/NimBLE-Arduino@^2.3.8"], "secure": core,
        "secure_ha": core + ["knolleary/PubSubClient @ ^2.8"]}, (deps, got)
    # ...as does a row or a library under ROOTS in each spelling PlatformIO
    # takes: a version range (one entry: PlatformIO splits a one-line value
    # on ", " only), a row in another case, a one-line list, an interpolated
    # list with an entry after it, `${this.option}`, an indented env, an
    # inline comment and an older key name.
    this_libs = ("canary/platformio.ini", "lib_deps =\n" + "".join(
        f"    {e}\n" for e in core) + "build_flags", "custom_extra_libs =\nlib_deps =\n" + "".join(
        f"    {e}\n" for e in core) + "    ${this.custom_extra_libs}\nbuild_flags")
    for edit, swap in (
            ("[env:range]\nlib_deps = bblanchon/ArduinoJson @ >=7.0.0,<8.0.0\n", None),
            ("[env:range]\nlib_deps =\n    bblanchon/ArduinoJson @ >=7.0.0,<8.0.0\n", None),
            ("[env:ha]\nlib_deps = Rweather/crypto @ ^0.4.0, knolleary/PubSubClient @ ^2.8\n",
             None),
            ("[env:mesh]\nlib_deps =\n    ${env.lib_deps}\n    securacv_mesh\n", None),
            ("[common]\nlibs = bblanchon/ArduinoJson @ ^7.0.0\n[env:x]\nmore = rweather/Crypto\n"
             "lib_deps =\n    ${common.libs}\n    ${this.more}\n", None),
            ("[common]\nlibs = bblanchon/ArduinoJson @ ^7.0.0, knolleary/PubSubClient @ ^2.8\n"
             "[env:x]\nlib_deps = ${common.libs}, rweather/Crypto\n", None),
            ("[env:recovery]\n\textends = env:dev\n\tlib_deps =\n\t\t${env.lib_deps}\n"
             "\t\trweather/Crypto @ ^0.4.0 # pinned, not tzapu/WiFiManager\n", None),
            ("[env:old]\nlib_install = knolleary/PubSubClient @ ^2.8\n", None),
            ("[env:dev_ha2]\nextends = env:dev\ncustom_extra_libs = knolleary/PubSubClient @ ^2.8\n",
             this_libs)):
        _deps, got = project("\n" + edit, swap=swap)
        assert got == [], (edit, got)
    # ...and any other library fails, naming each env that fetches it:
    # WiFiManager's autoConnect() raises an open SoftAP and serves a firmware
    # upload no rule reads (the round-7 review), however the configuration
    # tells PlatformIO to fetch it. Every spelling the seventh to tenth
    # reviews found the old text reader missing is here.
    recovery = "[env:recovery]\nextends = env:dev\nlib_deps =\n    ${env.lib_deps}\n    "
    for edit, spec, swap in (
            (recovery + "tzapu/WiFiManager @ ^2.0.17\n", "tzapu/WiFiManager @ ^2.0.17", None),
            ("[env:recovery]\nlib_deps = bblanchon/ArduinoJson @ ^7.0.0, tzapu/WiFiManager\n",
             "tzapu/WiFiManager", None),
            (recovery + "me-no-dev/ESP Async WebServer @ ^1.2.3\n",
             "me-no-dev/ESP Async WebServer @ ^1.2.3", None),
            ("[common]\nlibs_external =\n    tzapu/WiFiManager\n[env:recovery]\n"
             "lib_deps =\n    ${env.lib_deps}\n    ${common.libs_external}\n",
             "tzapu/WiFiManager", None),
            ("[env:a]\nlib_ignore = rweather/Crypto @ ^0.4.0, WiFiManager\n\n[env:recovery]\n"
             "lib_deps =\n    ${env:a.lib_ignore}\n    knolleary/PubSubClient\n", "WiFiManager", None),
            ("[env:recovery]\nlib_deps =\n    rweather/Crypto\n\n    tzapu/WiFiManager\n",
             "tzapu/WiFiManager", None),
            # the round-8 review: an indented env (a tab, two spaces), the key
            # in another case, and the older names PlatformIO reads as lib_deps
            ("[env:recovery]\n\textends = env:dev\n\tbuild_flags =\n\t\t${env:dev.build_flags}\n"
             "\t\t-DSECURACV_RECOVERY_PORTAL=1\n\tlib_deps =\n\t\t${env.lib_deps}\n"
             "\t\ttzapu/WiFiManager @ ^2.0.17\n", "tzapu/WiFiManager @ ^2.0.17", None),
            ("[env:recovery]\n  extends = env:dev\n  lib_deps =\n    ${env.lib_deps}\n"
             "    tzapu/WiFiManager @ ^2.0.17\n", "tzapu/WiFiManager @ ^2.0.17", None),
            ("[env:recovery]\nLib_Deps = tzapu/WiFiManager\n", "tzapu/WiFiManager", None),
            ("[env:recovery]\nlib_install =\n    tzapu/WiFiManager @ ^2.0.17\n",
             "tzapu/WiFiManager @ ^2.0.17", None),
            ("[env:recovery]\nlib_use = tzapu/WiFiManager\n", "tzapu/WiFiManager", None),
            ("[env:recovery]\nlib_force: tzapu/WiFiManager\n", "tzapu/WiFiManager", None),
            # the round-9 review: a one-line ", " list after an interpolated
            # list, which PlatformIO splits whole
            ("[common]\nlibs = bblanchon/ArduinoJson @ ^7.0.0, knolleary/PubSubClient @ ^2.8\n"
             "[env:recovery]\nlib_deps = ${common.libs}, WiFiManager\n", "WiFiManager", None),
            ("[common]\nlibs_core = bblanchon/ArduinoJson @ ^7.0.0\n"
             "libs_portal = WiFiManager, knolleary/PubSubClient @ ^2.8\n[env:recovery]\n"
             "lib_deps = ${common.libs_core}, ${common.libs_portal}\n", "WiFiManager", None),
            # the round-10 review: [env]'s `${this.option}`, each env's own
            ("[env:recovery]\nextends = env:dev\ncustom_extra_libs = tzapu/WiFiManager @ ^2.0.17\n",
             "tzapu/WiFiManager @ ^2.0.17", this_libs),
            # a bare name, though a row's name is the same, and a registry id
            ("[env:recovery]\nlib_deps =\n    WiFiManager\n", "WiFiManager", None),
            ("[env:recovery]\nlib_deps =\n    ArduinoJson\n", "ArduinoJson", None),
            ("[env:recovery]\nlib_deps = 1234\n", "1234", None),
            # a row is matched by its owner as well as its name (a fork)...
            (recovery + "thingsboard/PubSubClient @ ^2.8\n", "thingsboard/PubSubClient @ ^2.8",
             None),
            # ...and a URL, a git URL in the version slot (under a row's
            # owner/name) or a path is what PlatformIO fetches or copies,
            # whatever the rows say, as is a line holding two entries, which
            # PlatformIO reads as one URL
            (recovery + "rweather/Crypto @ git@github.com:tzapu/WiFiManager.git\n",
             "rweather/Crypto @ git@github.com:tzapu/WiFiManager.git", None),
            (recovery + "rweather/Crypto @ https://github.com/rweather/arduinolibs.git\n",
             "rweather/Crypto @ https://github.com/rweather/arduinolibs.git", None),
            (recovery + "https://github.com/tzapu/WiFiManager.git#v2.0.17\n",
             "https://github.com/tzapu/WiFiManager.git#v2.0.17", None),
            (recovery + "WiFiManager=https://github.com/tzapu/WiFiManager.git\n",
             "WiFiManager=https://github.com/tzapu/WiFiManager.git", None),
            (recovery + "../../outside/diag\n", "../../outside/diag", None),
            ("[env:recovery]\nlib_deps = rweather/Crypto @ ^0.4.0,tzapu/WiFiManager\n",
             "rweather/Crypto @ ^0.4.0,tzapu/WiFiManager", None),
            (recovery + "rweather/Crypto @ ^0.4.0, tzapu/WiFiManager\n",
             "rweather/Crypto @ ^0.4.0, tzapu/WiFiManager", None)):
        _deps, got = project("\n" + edit, swap=swap)
        assert any(p.startswith("env:recovery: lib_deps, as PlatformIO resolves it, fetches "
                                f"{spec!r}, a library no REGISTRY_LIBS row names") for p in got), (
            edit, got)
    # An entry [env] lists is fetched by every env that inherits it, and one an
    # extra_configs file's env lists is held as one in platformio.ini is.
    _deps, got = project(swap=("canary/platformio.ini", "    bblanchon/ArduinoJson @ ^7.0.0\nbuild",
                               "    bblanchon/ArduinoJson @ ^7.0.0\n    tzapu/WiFiManager\nbuild"))
    assert any(p.startswith("env:dev, env:dev_ha, env:full: lib_deps, as PlatformIO resolves it, "
                            "fetches 'tzapu/WiFiManager'") for p in got), got
    _deps, got = project(swap=("provisioning/secure.ini", "[env:secure_ha]",
                               "[env:recovery]\nextends = env:secure\nlib_deps =\n"
                               "    ${env:secure.lib_deps}\n    tzapu/WiFiManager @ ^2.0.17\n\n"
                               "[env:secure_ha]"))
    assert any(p.startswith("env:recovery: lib_deps, as PlatformIO resolves it, fetches "
                            "'tzapu/WiFiManager @ ^2.0.17'") for p in got), got
    # A dependency a library under the tree declares in its library.json
    # (read by PlatformIO's manifest parser, found by library_manifests).
    _deps, got = project(files={"canary/lib/recovery/library.json":
                                '{"name": "recovery", "dependencies": '
                                '{"tzapu/WiFiManager": "^2.0.17"}}'})
    assert any("recovery/library.json:1: its dependency list fetches 'tzapu/WiFiManager', a "
               "library no REGISTRY_LIBS row names" in p for p in got), got
    # A `${sysenv.*}` reference fails wherever it is, read in the file's text,
    # so the environment at check time (unset, a row, or WiFiManager) changes
    # nothing (the round-9 review's `${sysenv.LIB_DEPS}`).
    for value in (None, "knolleary/PubSubClient @ ^2.8", "tzapu/WiFiManager"):
        _deps, got = project("\n" + recovery + "${sysenv.LIB_DEPS}\n", env={"LIB_DEPS": value})
        assert any("'${sysenv.LIB_DEPS}' is a build input the check does not read" in p
                   for p in got), (value, got)
    _deps, got = project("\n[env:recovery]\nextends = env:dev\nbuild_flags =\n"
                         "    ${env:dev.build_flags}\n    ${sysenv.EXTRA_FLAGS}\n",
                         env={"EXTRA_FLAGS": None})
    assert any("'${sysenv.EXTRA_FLAGS}' is a build input the check does not read" in p
               for p in got), got
    # A configuration PlatformIO refuses fails, and so does a file it reads that
    # flag_sources does not (`Extra_Configs`, which configparser lowercases),
    # whose -D flags and build inputs no rule would read.
    _deps, got = project("\n[env:recovery]\nlib_deps = rweather/Crypto\nlib_deps = WiFiManager\n")
    assert any(f"PlatformIO {PLATFORMIO_PIN}'s reader refuses the configuration" in p
               for p in got), got
    _deps, got = project("\n" + recovery + "${common.portal_libs}\n")
    assert any(f"PlatformIO {PLATFORMIO_PIN}'s reader cannot resolve env:recovery's lib_deps"
               in p for p in got), got
    _deps, got = project(swap=("canary/platformio.ini", "extra_configs =", "Extra_Configs ="))
    assert sorted(p.split(":")[0].rsplit("/", 2)[-2] for p in got
                  if "PlatformIO reads it as part of" in p and "(flag_sources)" in p) == [
        "envs", "provisioning"], got
    with tempfile.TemporaryDirectory() as tmp:
        _deps, got = platformio_lib_deps({}, Path(tmp) / "platformio.ini", Path(tmp))
        assert any("the configuration PlatformIO reads is missing" in p for p in got), got
    # A row is an owner/name: a bare-name row admits nothing and is named; and
    # an entry PlatformIO cannot read as a library spec fails.
    got = check_bearer_only_answers(
        clean, dict(tables, registry=dict(REGISTRY_LIBS, WiFiManager="the recovery portal")),
        lib_deps={"recovery": ["WiFiManager"]})
    assert any("REGISTRY_LIBS: 'WiFiManager' is no owner/name row" in p for p in got) and any(
        "fetches 'WiFiManager'" in p for p in got), got
    got = check_bearer_only_answers(clean, tables,
                                    lib_deps={"recovery": ["rweather/Crypto @ ^0.4.0 junk"]})
    assert any("fetches 'rweather/Crypto @ ^0.4.0 junk'" in p
               and "PlatformIO cannot read it as a library spec" in p for p in got), got
    # A library manifest's dependencies are held the same way, each read by
    # PlatformIO's manifest parser in every form it takes: a registry row or a
    # library under ROOTS passes, anything else fails.
    mesh = {"fx/lib/securacv_mesh/library.json": '{"name": "securacv_mesh"}'}
    manifests = dict(mesh, **{
        "fx/lib/securacv_network/library.json":
            '{"name": "securacv_network", "dependencies": {"bblanchon/ArduinoJson": "^7.0.0"}}',
        "fx/lib/securacv_ble_scan/library.json":
            '{"name": "securacv_ble_scan", "dependencies": {"securacv_mesh": "*"}}',
        # (a list of names; a library under ROOTS by the name its manifest
        # gives, its folder named otherwise, or by its folder's name, its
        # manifest giving none)
        "fx/lib/scan/library.json":
            '{"name": "securacv_scan", "dependencies": ["securacv_mesh", "securacv_diag"]}',
        "fx/lib/securacv_diag/library.json": '{"dependencies": ["securacv_scan"]}'})
    assert check_bearer_only_answers(clean, tables, manifests=manifests) == []
    for path, text, word in (
            ("fx/lib/securacv_network/library.json",
             '{"name": "securacv_network", "dependencies": {"tzapu/WiFiManager": "^2.0.17"}}',
             "its dependency list fetches 'tzapu/WiFiManager'"),
            ("fx/lib/securacv_network/library.json",
             '{"dependencies": [{"owner": "tzapu", "name": "WiFiManager"}]}',
             "its dependency list fetches 'tzapu/WiFiManager'"),
            ("fx/lib/securacv_network/library.json",
             '{"dependencies": {"WiFiManager": "https://github.com/tzapu/WiFiManager.git"}}',
             "'WiFiManager=https://github.com/tzapu/WiFiManager.git'"),
            # (a version PlatformIO cannot read as one is a spec of its own: an
            # scp-style git URL under a row's owner/name fetches what it names)
            ("fx/lib/securacv_network/library.json",
             '{"dependencies": [{"owner": "rweather", "name": "Crypto", '
             '"version": "git@github.com:tzapu/WiFiManager.git"}]}',
             "'rweather/Crypto=git@github.com:tzapu/WiFiManager.git'"),
            # (and an owner/name as the version is the library PlatformIO fetches)
            ("fx/lib/securacv_network/library.json",
             '{"dependencies": [{"owner": "rweather", "name": "Crypto", '
             '"version": "tzapu/WiFiManager"}]}',
             "'rweather/Crypto=tzapu/WiFiManager'"),
            ("fx/lib/securacv_network/library.json",
             '{"dependencies": {"diag": "symlink://../../../common/diag"}}',
             "'diag=symlink://../../../common/diag'"),
            ("fx/lib/diag/library.properties", "name=diag\ndepends=WiFiManager\n",
             "its dependency list fetches 'WiFiManager'"),
            ("fx/lib/securacv_network/library.json", '{"name": "x", "dependencies": {',
             f"a library manifest PlatformIO {PLATFORMIO_PIN} cannot read")):
        got = check_bearer_only_answers(clean, tables, manifests=dict(manifests, **{path: text}))
        assert any(word in p for p in got), (text, got)
    # ...and library_manifests() finds each manifest under a root by either
    # name, however deep, and its output fed as `manifests=` (as main() feeds
    # it) is held: a library that fetches WiFiManager fails. (main()'s own
    # `manifests=library_manifests()` is not run by a self-test; see the limits
    # in the docstring.)
    with tempfile.TemporaryDirectory() as tmp:
        lib = Path(tmp) / "lib"
        (lib / "diag").mkdir(parents=True)
        (lib / "recovery" / "extras").mkdir(parents=True)
        (lib / "diag" / "library.json").write_text(
            '{"name": "diag", "dependencies": {"tzapu/WiFiManager": "^2.0.17"}}')
        (lib / "recovery" / "extras" / "library.properties").write_text(
            "name=recovery\ndepends=WiFiManager\n")
        found = library_manifests((lib,))
        assert sorted(found) == [(lib / "diag" / "library.json").as_posix(),
                                 (lib / "recovery" / "extras" / "library.properties").as_posix()
                                 ], found
        got = check_bearer_only_answers(clean, tables, manifests=found)
        assert any("diag/library.json:1: its dependency list fetches 'tzapu/WiFiManager'" in p
                   for p in got) and any(
            "extras/library.properties:2: its dependency list fetches 'WiFiManager'" in p
            for p in got), got
    # ...and the ` @ ` of a lib version spec, and a bare `-DBOARD` / `-B` inside
    # another flag's name, are not a response file or a prefix flag.
    benign_flags = (ini + "    -DBOARD_HAS_PSRAM\n    -mfix-esp32-psram-cache-issue\n"
                    "lib_deps =\n    bblanchon/ArduinoJson @ ^7.0.0\n")
    assert check_bearer_only_answers(clean, tables, flags={"fx/platformio.ini": benign_flags}) == []
    # ...and the files extra_configs names are read on the key's own line or
    # indented under it, comma-separated, a missing one named, and a flag's
    # line number is its file's (comment lines blanked, never dropped).
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "extra").mkdir()
        (root / "platformio.ini").write_text(
            "[platformio]\nextra_configs = extra/one.ini, extra/two.ini\n"
            "    extra/gone.ini\n[env]\nbuild_flags =\n    -DFEATURE_WIFI_AP=1\n")
        (root / "extra" / "one.ini").write_text(
            "; a comment\n; another\n[env:secure]\nbuild_flags =\n    -D'auth_gate(r)=true'\n")
        (root / "extra" / "two.ini").write_text(
            "[env:y]\nbuild_flags =\n    -D'take(s,n,t)=is_open(s,n,t)'\n")
        texts, missing = flag_sources((root / "platformio.ini",))
        assert len(texts) == 3 and any("gone.ini" in m and "missing" in m for m in missing), (
            texts, missing)
        got = check_bearer_only_answers(clean, tables, flags=texts)
        assert any("one.ini:5: the build flag" in p and "'auth_gate'" in p for p in got), got
        assert any("two.ini:3: the build flag" in p and "'take'" in p for p in got), got
    # A #define in a header outside ROOTS (#included after the function it
    # names) is held as one under ROOTS is, but the NVS records' names in
    # canary_config.h.
    for name, header in (("bearer_present_and_valid",
                          "#define bearer_present_and_valid(r) (true)\n"),
                         ("take", "#define take(s, n, t) is_open(s, n, t)\n"),
                         ("false", "#define false true\n"),
                         ("getApPassword",
                          "#define AP_KEY network_get_instance().getApPassword()\n"),
                         ("NVS_KEY_TOKEN", '#define NVS_KEY_TOKEN "api_token"\n')):
        got = check_bearer_only_answers(clean, tables,
                                        includes={"fx/common/network/diag.h": header})
        assert any(f"a macro names {name!r}" in p and "outside ROOTS" in p for p in got), (
            name, got)
    config = {"fx/include/canary_config.h": "".join(
        f'#define {k} "{v}"\n' for k, v in _FIX_NVS_KEYS.items())}
    assert check_bearer_only_answers(clean, tables, includes=config) == []
    # ...and a header outside ROOTS names nothing else the check holds there: its
    # code is not read, so a reader or a tap opened in it would be followed by
    # no rule.
    for name, header in (("auth_get_token", "inline const char* diag_t() { return "
                                            "auth_get_token(); }\n"),
                         ("g_prov_gate", "inline void tap_now() { canary::net::provisioning_gate::"
                                         "open(g_prov_gate, millis()); }\n"),
                         ("network_get_instance", "inline void restart() { "
                                                  "network_get_instance().begin(\"x\", \"\"); }\n"),
                         ("NVS_KEY_TOKEN", "inline void wipe(Preferences& p) { "
                                           "p.remove(NVS_KEY_TOKEN); }\n")):
        got = check_bearer_only_answers(clean, tables,
                                        includes={"fx/common/network/helper.h": header})
        assert any(f"the flagship compiles or #includes names {name!r}" in p for p in got), (
            name, got)
    # ...a census row's spelling for a primitive defined outside ROOTS included
    # (a header helper, called from loop(), raising the SoftAP open), but the two
    # Host-first decisions where provisioning_gate.h defines them.
    for name, header in (("softAP", 'inline void ap_keepalive() { if (!(WiFi.getMode() & '
                                    'WIFI_AP)) WiFi.softAP("Canary-Setup", nullptr); }\n'),
                         ("WiFi.mode", "inline void ap_keepalive() { WiFi.mode(WIFI_AP_STA); }\n"),
                         ("WiFi.softAPdisconnect", "inline void ap_quiet() { "
                                                   "WiFi.softAPdisconnect(); }\n"),
                         ("esp_wifi_set_mode", "inline void ap_up() { "
                                               "esp_wifi_set_mode(WIFI_MODE_APSTA); }\n"),
                         ("esp_wifi_set_config", "inline void ap_open() { wifi_config_t c = {}; "
                                                 "c.ap.authmode = WIFI_AUTH_OPEN; "
                                                 "esp_wifi_set_config(WIFI_IF_AP, &c); }\n"),
                         ("WiFi.AP", "inline void ap2() { WiFi.AP.create(\"x\"); }\n"),
                         ("provisioning_gate::open", "inline void tap(State& s) { "
                                                     "provisioning_gate::open(s, 0); }\n"),
                         ("receipt_decide", "inline int r() { return receipt_decide(true); }\n")):
        got = check_bearer_only_answers(clean, tables,
                                        includes={"fx/include/ap_keepalive.h": header})
        assert any(f"the flagship compiles or #includes names {name!r}" in p for p in got), (
            name, got)
    gate_h = ("inline Verdict receipt_decide(bool foreign) { return foreign ? A : B; }\n"
              "inline Verdict page_token_decide(bool foreign) { return foreign ? A : B; }\n")
    assert check_bearer_only_answers(clean, tables, includes={
        "fx/common/network/provisioning_gate.h": gate_h}) == []
    # ...and a token paste or assembly in a file outside ROOTS the flagship
    # reaches is held as one under ROOTS is; GNU's comma elision and the empty
    # compiler barrier are not pastes or calls, there or under ROOTS.
    for words, text in ((_PASTE, "#define CV_JOIN(a, b) a##b\n"),
                        (_PASTE, "#define LOGF(fmt, ...) printf(fmt, a##__VA_ARGS__)\n"),
                        (_ASM_WORDS, 'extern const char* t() __asm__("_Z14auth_get_tokenv");\n'),
                        (_ASM_WORDS, 'inline void f() { asm("call8 x"); }\n'),
                        # the asm siblings, in a file outside ROOTS the flagship reaches
                        (_BIND_WORDS,
                         'static const char* t() __attribute__((weakref("_Z14auth_get_tokenv")));\n'),
                        (_BIND_WORDS,
                         'extern "C" const char* a() __attribute__((alias("_Z14auth_get_tokenv")));\n'),
                        (_BIND_WORDS,
                         'extern "C" const char* t();\n#pragma redefine_extname t _Z14auth_get_tokenv\n'),
                        (_BIND_WORDS, 'extern const char* __REDIRECT(t, (), _Z14auth_get_tokenv);\n'),
                        (_BIND_WORDS, 'extern "C" const char* _Z14auth_get_tokenv();\n')):
        got = check_bearer_only_answers(clean, tables, includes={"fx/common/csi/x.cpp": text})
        assert (any(words in p for p in got) if words in (_ASM_WORDS, _BIND_WORDS)
                else any(words in p and "outside ROOTS" in p for p in got)), (text, got)
    benign = ('#define LOGF(fmt, ...) do { Serial.printf("[x] " fmt "\\n", ##__VA_ARGS__); } '
              'while (0)\n#define LOGW(fmt, ...) printf(fmt , ## __VA_ARGS__)\n'
              'inline void fence() { asm volatile("" ::: "memory"); __asm__ __volatile__("");'
              ' }\n')
    assert check_bearer_only_answers(clean, tables, includes={"fx/common/csi/x.cpp": benign}) == []
    planted = dict(clean)
    planted[_FIX_OTHER] = strip_comments(benign)
    assert check_bearer_only_answers(planted, tables) == []
    # Every file under ROOTS is read or named: an .inc or .hpp of code is code,
    # a library manifest (by its name) is not, a file git ignores is excused, and
    # anything else is a problem (a .json that is no manifest among them).
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "lib" / "x" / "src"
        src.mkdir(parents=True)
        (src / "a.cpp").write_text('#include "b.inc"\n')
        (src / "b.inc").write_text("static const char* dbg() { return auth_get_token(); }\n")
        (src / "c.hpp").write_text("inline int k() { return 1; }\n")
        (src.parent / "library.json").write_text("{}\n")
        (src.parent / "library.properties").write_text("name=x\n")
        (src / "d.xyz").write_text("int z;\n")
        (src / "notes.json").write_text("{}\n")
        (src / ".DS_Store").write_text("x")
        texts, unread = tree_sources((Path(tmp) / "lib",), ignored=lambda paths: set())
        assert sorted(Path(f).name for f in texts) == ["a.cpp", "b.inc", "c.hpp"], texts
        assert sorted(Path(u.split(":")[0]).name for u in unread) == [
            ".DS_Store", "d.xyz", "notes.json"], unread
        texts, unread = tree_sources((Path(tmp) / "lib",), ignored=lambda paths: {
            p for p in paths if p.endswith(".DS_Store")})
        assert sorted(Path(u.split(":")[0]).name for u in unread) == ["d.xyz", "notes.json"], unread
        # ...and git's answer: an ignored, untracked file is excused; a tracked
        # one of the same pattern never is (when git can answer at all).
        repo = Path(tmp) / "repo"
        repo.mkdir()
        try:
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True, capture_output=True,
                           timeout=60)
            (repo / ".gitignore").write_text("*~\n.DS_Store\n")
            for name in ("a.cpp~", ".DS_Store", "b.h~", "c.xyz"):
                (repo / name).write_text("x")
            subprocess.run(["git", "add", "-f", "b.h~"], cwd=repo, check=True,
                           capture_output=True, timeout=60)
            have_git = True
        except (OSError, subprocess.SubprocessError):
            have_git = False
        if have_git:
            got = git_ignored(["a.cpp~", ".DS_Store", "b.h~", "c.xyz"], cwd=repo)
            assert got == {"a.cpp~", ".DS_Store"}, got
        assert git_ignored(["x~"], cwd=tmp) == set()  # no work tree: nothing excused
    # What the flagship reaches outside ROOTS (build_reach): the files
    # build_src_filter compiles and each file an #include reaches, from ROOTS or
    # from those, beside the including file or in include/ and the -I folders;
    # not a file only another product or a host test builds. An #include the
    # check cannot follow, a filter that matches nothing, a compiled file that is
    # not C or C++ and a file of this tree outside the read folders are problems.
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        proj, common = base / "fw" / "canary", base / "fw" / "common"
        for d in (proj / "src", proj / "include", proj / "lib" / "a" / "src", common / "net",
                  common / "csi" / "src", common / "power", base / "fw" / "other"):
            d.mkdir(parents=True)
        (proj / "src" / "main.cpp").write_text(
            '#include "conf.h"\n#include <net/gate.h>\n#include "csi_hal.h"\n'
            "#include <WiFi.h>\n")
        (proj / "include" / "conf.h").write_text('#include "net/keys.h"\n')
        (common / "net" / "gate.h").write_text('#include "gate_impl.h"\n')
        (common / "net" / "gate_impl.h").write_text("int g;\n")
        (common / "net" / "keys.h").write_text("int k;\n")
        (common / "net" / "portal.cpp").write_text('#include "keys.h"\n')  # another product's
        (common / "csi" / "src" / "csi_hal.h").write_text("int h;\n")
        (common / "csi" / "src" / "csi_hal.cpp").write_text('#include "csi_util.h"\n')
        (common / "csi" / "src" / "csi_util.h").write_text("int u;\n")
        (common / "csi" / "src" / "csi_fast.S").write_text("call8 x\n")
        (common / "power" / "test_power.cpp").write_text("#define millis() 0\n")  # a host test
        (base / "fw" / "other" / "x.h").write_text("int x;\n")
        ini = ("build_flags =\n    -I${PROJECT_DIR}/../common\n"
               "    -I${PROJECT_DIR}/../common/csi/src\n    -I${platformio.packages_dir}/core\n"
               "build_src_filter =\n    +<*>\n    +<../../common/csi/src/csi_hal.cpp>\n")
        roots = (proj / "include", common)
        lib = {(proj / "src" / "main.cpp").as_posix():
               strip_comments((proj / "src" / "main.cpp").read_text())}
        tree = (proj / "lib", proj / "src")
        got, problems = build_reach(lib, {"ini": ini}, project=proj, roots=roots, tree=tree,
                                    repo=base)
        names = sorted(Path(f).relative_to(base).as_posix() for f in got)
        assert names == ["fw/canary/include/conf.h", "fw/common/csi/src/csi_hal.cpp",
                         "fw/common/csi/src/csi_hal.h", "fw/common/csi/src/csi_util.h",
                         "fw/common/net/gate.h", "fw/common/net/gate_impl.h",
                         "fw/common/net/keys.h"], names
        assert problems == [], problems
        lib[(proj / "src" / "main.cpp").as_posix()] += (
            "#include CSI_HEADER\n#include \"../../other/x.h\"\n")
        bad = ini + "    +<../../common/csi/src/csi_fast.S>\n    +<../../common/gone/*.cpp>\n"
        _got, problems = build_reach(lib, {"ini": bad}, project=proj, roots=roots, tree=tree,
                                     repo=base)
        for words in ("names its file through a macro", "x.h, outside ROOTS and the include roots",
                      "csi_fast.S, which is not C or C++", "+<../../common/gone/*.cpp> matches no"):
            assert any(words in p for p in problems), (words, problems)
    # ...and build_reach's output, fed as `includes=` to check_bearer_only_answers
    # the way main() feeds it, is scanned: a header reached only through build_reach
    # that names a credential is caught, so passing `includes={}` (the round-5 nit's
    # one-token main() mutation) would show here. (main()'s own `includes=includes`
    # and its `+ reach_failures` extend are not themselves run by a self-test — see
    # the limits in the docstring.)
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        proj, common = base / "fw" / "canary", base / "fw" / "common"
        (proj / "src").mkdir(parents=True)
        (proj / "include").mkdir()
        (common / "diag").mkdir(parents=True)
        (proj / "src" / "app.cpp").write_text('#include "dnote.h"\n')
        (common / "diag" / "dnote.h").write_text(
            "static const char* dbg() { return auth_get_token(); }\n")
        ini2 = ("build_flags =\n    -I${PROJECT_DIR}/../common/diag\n"
                "build_src_filter =\n    +<*>\n")
        appsrc = {(proj / "src" / "app.cpp").as_posix():
                  strip_comments((proj / "src" / "app.cpp").read_text())}
        includes, reach_failures = build_reach(
            appsrc, {"ini": ini2}, project=proj, roots=(proj / "include", common),
            tree=(proj / "lib", proj / "src"), repo=base)
        assert any(p.endswith("diag/dnote.h") for p in includes) and reach_failures == [], (
            includes, reach_failures)
        got = check_bearer_only_answers(clean, tables, includes=includes)
        assert any("names 'auth_get_token'" in p and "dnote.h" in p for p in got), got
        assert check_bearer_only_answers(clean, tables, includes={}) == []  # the mutation hides it
    # The NVS keys' values are read from canary_config.h as `#define NAME "..."`;
    # a key spelled any other way is named, so its literal rule never goes quiet.
    with tempfile.TemporaryDirectory() as tmp:
        config = Path(tmp) / "canary_config.h"
        config.write_text("".join(f'#define {k} "{v}"\n' for k, v in _FIX_NVS_KEYS.items()))
        assert nvs_key_values(config) == (_FIX_NVS_KEYS, [])
        config.write_text(config.read_text().replace(
            '#define NVS_KEY_TOKEN "api_token"', '#define NVS_KEY_TOKEN NVS_PREFIX "token"'))
        values, problems = nvs_key_values(config)
        assert "NVS_KEY_TOKEN" not in values and len(problems) == 1 and (
            "NVS_KEY_TOKEN is not a `#define NVS_KEY_TOKEN" in problems[0]), problems
    # A ROOTS_DEFINES name defined as often as its row says, in another file.
    planted = dict(clean)
    planted[_FIX_AUTH] += "#define CANARY_SOFTAP_SAE_IN_BUILD 1\n"
    row = dict(tables["roots_defines"], CANARY_SOFTAP_SAE_IN_BUILD=(_NET, 1))
    got = check_bearer_only_answers(planted, dict(tables, roots_defines=row))
    assert any("ROOTS_DEFINES: CANARY_SOFTAP_SAE_IN_BUILD is defined once under ROOTS "
               "(fx/lib/securacv_auth.cpp); its row says once, in securacv_network.cpp" in p
               for p in got), got
    row = dict(tables["roots_defines"], CANARY_SOFTAP_SAE_IN_BUILD=(_AUTH, 1))
    assert check_bearer_only_answers(planted, dict(tables, roots_defines=row)) == []


def check_route_gates(sources: dict, routes: list) -> tuple:
    """(failures, matched allowlist keys): every registered route is on the
    PUBLIC allowlist or its handler, every definition of it (an `#if` body
    and its `#else` one alike), reaches a credential gate whose answer
    decides, before any unconditional return."""
    failures, matched_allowlist = [], set()
    for uri, method, handler, fname in routes:
        if method is None or handler is None:
            failures.append(f"{uri or '<a route>'} ({fname}): the parser cannot read this "
                            f"registration's {'.method' if method is None else '.handler'} "
                            f"— write `.method = HTTP_<NAME>` and `.handler = <function>` "
                            f"(a cast method or a lambda handler is a route no rule "
                            f"would read)")
            continue
        if uri is None:
            failures.append(f"{method} <non-literal uri> → {handler} ({fname}): "
                            f"the parser cannot resolve this .uri; use a string "
                            f"literal or the probe-array loop form")
            continue
        key = (method, uri)
        if key in PUBLIC_ALLOWLIST:
            matched_allowlist.add(key)
            continue
        base = handler.split("::")[-1]
        if base in SELF_GATING_HANDLERS:
            continue
        bodies = handler_bodies(sources, base)
        if not bodies:
            failures.append(f"{method} {uri}: handler {handler} not found "
                            f"(registered in {fname})")
            continue
        present = all(any(marker_at(body, marker) >= 0 for marker in AUTH_MARKERS)
                      for body in bodies)
        decides = all(any(marker_decides(body, marker) for marker in AUTH_MARKERS)
                      for body in bodies)
        guarded = all(gate_guards(body) for body in bodies)
        twice = f" (one of its {len(bodies)} definitions)" if len(bodies) > 1 else ""
        if not present:
            failures.append(f"{method} {uri}: handler {handler} ({fname}){twice} has NO "
                            f"credential gate and is not on the public allowlist")
        elif not decides:
            failures.append(f"{method} {uri}: handler {handler} ({fname}){twice} calls a "
                            f"credential gate but ignores its result — use it in an "
                            f"if (...) condition or a return expression")
        elif not guarded:
            failures.append(f"{method} {uri}: handler {handler} ({fname}){twice} returns "
                            f"unconditionally before its first credential gate — the "
                            f"gate is unreachable")
    return failures, matched_allowlist


def git_ignored(paths, cwd=None) -> set:
    """The paths among `paths` (relative to `cwd`, the working directory by
    default) that git ignores: untracked and matched by a .gitignore, so never
    committed (an editor's backup, an OS's .DS_Store). A tracked file is never
    one. None when git cannot say (no git, no work tree), so nothing is
    excused."""
    paths = sorted(paths)
    if not paths:
        return set()
    try:
        done = subprocess.run(["git", "check-ignore", "-z", "--stdin"], cwd=cwd,
                              input="\0".join(paths) + "\0", capture_output=True, text=True,
                              timeout=60, check=False)
    except (OSError, subprocess.SubprocessError):
        return set()
    if done.returncode not in (0, 1):  # 1: none ignored; anything else: no answer
        return set()
    return {p for p in done.stdout.split("\0") if p}


def tree_sources(roots=ROOTS, ignored=git_ignored) -> tuple:
    """({path: comment-stripped text}, problems) for every file under
    `roots`: a SOURCE_SUFFIXES file is read (whatever git says of it), a
    NON_SOURCE_NAMES manifest is not, and any other is a problem (a file of
    code the check would not read) unless git ignores it."""
    sources, unread = {}, []
    for root in roots:
        for path in sorted(p for p in root.rglob("*") if p.is_file()):
            if path.suffix.lower() in SOURCE_SUFFIXES:
                sources[path.as_posix()] = strip_comments(path.read_text(errors="replace"))
            elif path.name not in NON_SOURCE_NAMES:
                unread.append(path)
    skipped = ignored(p.as_posix() for p in unread)
    problems = [f"{p.as_posix()}: a file under ROOTS the check does not read "
                f"({p.suffix.lower() or 'no suffix'}) — give C or C++ code one of "
                f"{', '.join(SOURCE_SUFFIXES)}, or teach the check the new kind"
                for p in unread if p.as_posix() not in skipped]
    return sources, problems


def nvs_key_values(path=NVS_KEY_HOME) -> tuple:
    """({name: value}, problems): each NVS_LITERAL_KEYS define's string in
    include/canary_config.h."""
    text = path.read_text(errors="replace") if path.is_file() else ""
    values, problems = {}, []
    for name in NVS_LITERAL_KEYS:
        m = re.search(r'^[ \t]*#[ \t]*define[ \t]+' + name + r'[ \t]+"([^"\\]+)"', text, re.M)
        if m:
            values[name] = m.group(1)
        else:
            problems.append(f"{path.as_posix()}: {name} is not a `#define {name} \"...\"` the "
                            f"check can read, so a literal of its NVS record would pass — fix "
                            f"the path or the define")
    return values, problems


def main() -> int:
    if platformio is None or platformio.__version__ != PLATFORMIO_PIN:
        found = ("no platformio module imports" if platformio is None
                 else f"platformio {platformio.__version__} is installed")
        print(f"FAIL: the library rule reads firmware/canary/platformio.ini with PlatformIO "
              f"{PLATFORMIO_PIN}'s own reader and {found} — run: {PLATFORMIO_INSTALL}")
        return 1
    _assert_parser_robust()
    _assert_answer_gate_rule()
    sources, failures = tree_sources()

    routes = []
    for fname, text in sources.items():
        routes.extend(parse_routes(fname, text))

    if len(routes) < MIN_ROUTES:
        print(f"FAIL: parsed only {len(routes)} routes (floor {MIN_ROUTES}) — "
              f"parser regression?")
        return 1

    gate_failures, matched_allowlist = check_route_gates(sources, routes)
    failures.extend(gate_failures)

    failures.extend(check_page_token_wiring(sources))
    failures.extend(check_host_first(sources))
    flags, flag_failures = flag_sources()
    keys, key_failures = nvs_key_values()
    includes, reach_failures = build_reach(sources, flags)
    lib_deps, pio_failures = platformio_lib_deps(flags)
    failures.extend(flag_failures + key_failures + reach_failures + pio_failures)
    failures.extend(check_bearer_only_answers(sources, tables={"nvs_keys": keys}, flags=flags,
                                              includes=includes,
                                              manifests=library_manifests(),
                                              lib_deps=lib_deps))
    failures.extend(check_registration_sites(sources))
    failures.extend(check_registration_count(sources))
    failures.extend(check_route_budgets(sources))

    for key in sorted(set(PUBLIC_ALLOWLIST) - matched_allowlist):
        failures.append(f"allowlist entry {key[0]} {key[1]} matches no registered "
                        f"route — remove or fix it")

    if failures:
        print(f"ROUTE SECURITY CHECK FAILED ({len(failures)} problem(s)):")
        for f in failures:
            print("  -", f)
        return 1

    print(f"canary route security OK: {len(routes)} registrations checked, "
          f"{len(matched_allowlist)} documented-public, rest gated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
