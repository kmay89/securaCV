# Household presence and household posture — decision record

Status: **Proposed** — answers the July 2026 firmware audit's item 16 and
settles Apple Home open decisions #3 and #4 for the household-key path. No
firmware changes in this record; the firmware work is listed as follow-up
(§3.3, §9).
Intended Status: Informative (architecture decision record)
Last Updated: 2026-10-18

> One paragraph: the canary-wap **household IRK store is dropped**, because it
> is a stored identifier that changes what the Canary witnesses and household
> keys are the strengthening alternative. The **familiar recognizer stays,
> as notification-only novelty**: RAM-only, at most 48 hours, a fresh salt
> every boot, no federated sharing. It never changes an event, it may move a
> notification between two tiers but never drop one, and if a reviewer doubts
> that this is a strengthening change, that part goes to an RFC. The **BLE
> Scout is not decided here** and stays unwired. The **household posture**
> is `away`, `home` or `night`, and the only thing that may write it is a
> household key: intent credentials alone may lower sensing, `home` lapses on
> its own, statements are signed, and a witness that doubts one senses fully.
> No notification tier keys on household membership.

## 1. The questions

The July 2026 audit
([`esp32s3_firmware_full_audit_2026-07.md`](../audit/esp32s3_firmware_full_audit_2026-07.md),
item 16) asks:

> The BLE Scout, `household` IRK store, and `familiar` recognizer all keep
> persistent per-device identity and/or a known-vs-unknown distinction that
> the hard invariants forbid. These are deliberate design tensions, not
> accidents — resolve them in an ADR (carve-out for owner-paired devices, or
> drop the features) before enabling them.

[`apple_home_integration.md`](apple_home_integration.md) §9 leaves two
decisions open that the same work answers. Decision #3: "Good Night arms the
fleet" needs a posture vocabulary that does not exist yet, dictionary-first.
Decision #4: quiet-when-home is the only inbound control path; decide "who may
write it …, what it may touch …, and whether it demands the physical-confirm
idiom. Design as a trust surface, not a toggle."

The research behind the answers is
[`face_id_without_faces.md`](../research/face_id_without_faces.md), and the
wire contract is [`spec/household_keys_v0.md`](../../spec/household_keys_v0.md).

The governance rules that decide the kind of each change are in
[`governance_and_invariants.md`](../governance_and_invariants.md):
strengthening changes are acceptable (§2.2); weakening ones need a formal,
public RFC that shows a strengthening alternative is not viable (§3.2); and
"if in doubt, treat the change as a weakening change" (§4).

## 2. Decisions at a glance

| # | Subject | Decision | Kind of change |
|---|---|---|---|
| D1 | canary-wap `household` IRK store, and the owner-sighting auto-context in `presence_context` | **Drop** | Strengthening (§2.2); no RFC |
| D2 | canary-wap `familiar` recognizer | **Keep as notification-only novelty**, under §4's conditions | D2a strengthening; D2b argued in §4.3, and an RFC if a reviewer is in doubt |
| D3 | canary-wap BLE Scout | **Unchanged, pending**; stays unwired | None |
| D4 | The household posture vocabulary (Apple Home #3) | `away` / `home` / `night` | Dictionary vocabulary |
| D5 | The inbound trust surface (Apple Home #4) | Household keys only, under five rules | A Canary acting on it needs the RFC in §8 |
| D6 | Notification tiers | No tier keys on household membership | Strengthening; no RFC |

## 3. D1 — drop the household IRK store

### 3.1 What exists

All paths below are under `firmware/projects/canary-wap/arduino/canary_wap/`.

- `household.h` stores up to eight bonded phones' identity resolving keys
  (IRKs) with a user label, in the plaintext NVS keys `hh_slots` and
  `hh_count`. Its comment said the IRKs are "flash-encrypted at rest on ESP32
  with secure boot". That was false, and the comment now says so (2026-10-21):
  flash encryption does not cover NVS, NVS encryption cannot be built under
  `framework = arduino` (neither pinned core's precompiled `sdkconfig` has
  it), and secure boot works on no tier: the provisioning kit prepares it,
  and its Phase 2 sets `SECURE_BOOT_EN` with no key digest, so nothing is
  verified ([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).
- The resolve step sits in the hot path. `rf_presence.cpp`'s `feed_ble_scan`
  returns early on a household match, before the privacy token is derived, so
  a household phone would never count toward the device total, the presence
  state machine, `RfEvent` or the vault's motion capture. That changes event
  generation.
- Enrollment is unwired. `add_irk()` is called only by a test and by a wizard
  path nothing calls, and no bonding callback extracts an IRK. So no IRK is
  ever stored and the resolve never matches on a real device. The feature is
  live code that does nothing.
- `presence_context` flips HOME/AWAY from owner sightings; on a fresh device
  it reports "away" while `notify` gates at HOME.
- Live egress: the `/api/household` and `/api/presence` routes, and the BLE
  console's `owner_min`, `hh` and `ble_hh` fields, which the browser flasher
  renders as "Owner last seen" and "(N known)".
- When enrollment does run, it logs the user's label (for example
  "Alice's iPhone") at INFO, which reaches `/HEALTH` and `/api/logs`.

### 3.2 Why drop, not carve out

- **It is an identifier store.** An IRK is a long-lived identifier that a phone
  hands to every peer it bonds with. Any advertisement ever captured keeps
  resolving under it, because resolution takes no time input. That is the
  "persistent identifier / cross-day correlation" Free Signals Invariant A
  forbids, and the "store … global identifiers" Invariant II forbids.
- **It changes event generation.** Invariant F's enforcement line is "Device
  lists are internal calibration only; never affect event semantics." This
  list removes a household phone's arrival from the event stream itself.
- **The carve-out is a weakening change.** Keeping it would be the audit's
  "carve-out for owner-paired devices", which needs a public RFC that shows no
  strengthening alternative exists (governance §3.2). One does exist:
  household keys (D5) cover the legitimate need, "we're home", with a
  deliberate, signed act instead of a passive inference.
- **Dropping it is strengthening.** It removes a stored identifier, an egress
  surface and a false at-rest claim, with nothing added. Governance §2.2
  allows it without an RFC.

### 3.3 Removal plan (follow-up firmware work)

Two firmware PRs. Line numbers were read on 2026-10-13 and will move.

**PR-a1, unwire and purge** (behavior change, still compiles):

1. `rf_presence.cpp`: delete the short-circuit in `feed_ble_scan` (about
   :1141-1154), the `household::init()` and `deinit()` calls and the include.
   Keep the two lines `test_dp_budget.cpp` pins byte-identical after
   whitespace squeeze: `dp::refill_if_due(s_session_epoch,now);` in `update()`
   and `dp::restore_budget(s_session_epoch,s_session_start_ms);` in `init()`.
2. `ble_presence.cpp`: delete the second `resolve_rpa_detailed` call and
   `s_adverts_resolved`; drop `Stats.adverts_resolved_household` and its
   readers in `household_api.h` and `ble_console.cpp`.
3. `presence_context`: make it manual-only (the existing 24 h override TTL).
   Delete the household queries and the `owner_seen_recently` and
   `ms_since_any_owner` fields. In `init()`, set the last pushed and last
   automatic context to `notify::get_context()`, which fixes the fresh-device
   mismatch. Keep `GET /api/presence` and the override routes, which also keeps
   the `char buf[640];` anchor that `check_wap_json_answers.py` mutates.
4. `household_api.h`: delete the list, set-role and remove handlers and their
   registrations; set `household_handlers` from 6 to 3 in `canary_wap.ino`;
   re-anchor or remove the household-error mutations in
   `firmware/scripts/check_wap_json_answers.py`.
5. Egress: remove `owner_min`, `hh` and `ble_hh` from the BLE console snapshot
   and its example. In `canary-local/assets/flash-core.js`, stop rendering
   "Owner last seen", "Household devices" and "(N known)", and keep tolerating
   old snapshots that still carry the fields. Fix the matching assertions in
   `canary-local/tests/flash.test.js` (including the literal
   `'8421 (4 known)'`). Remove the household rows and card from
   `companion_pwa.h` and `web_ui.h`, then run `gen_web_assets_gz.py` and commit
   `web_assets_gz.h`. The desktop Flasher has no matching field.
6. **NVS purge.** Removing the code does not erase the keys: `hh_slots` and
   `hh_count`, labels included, survive OTA. `nvs_store` has no `remove()`
   for the `securacv` namespace yet. Add
   `nvs_store::remove(const char* key)`, call it once at boot for `hh_slots`
   and `hh_count`, and add its row to
   `tests_host/test_nvs_store_lock.cpp`.
7. `notify`: remove the `household.h` include, the
   `already_resolved_household` branch and field, and the self-test case that
   uses it. Keep `SUP_HOUSEHOLD` as a reserved value and
   `Stats.total_suppressed_household` at zero, so `get_stats_for_export` still
   draws exactly nine differential-privacy noises against the shared budget.
8. `wizard.cpp`: drop the household calls and status fields. Keep
   `WIZ_PAIRING` in the enum for NVS compatibility, and load a stored value of
   it as `TRAINING`.
9. `tests.cpp`: drop the household self-tests and `red_team_cloned_rpa`. That
   test flips a bit in an address rather than replaying one verbatim, so it
   never proved replay resistance.

**PR-a2:** delete `household.h` and `household.cpp` once none of the eight
files that include it does (`ble_console.cpp`, `ble_presence.cpp`,
`notify.cpp`, `presence_context.cpp`, `rf_presence.cpp`, `tests.cpp`,
`wizard.cpp`, `household_api.h`). Both the Arduino and PlatformIO builds
compile every `.cpp` in the sketch directory, so a leftover file still links.

Gates these edits touch: `gen_web_assets_gz.py --check` (firmware.yml), the
Regression Guards job (`check_wap_json_answers.py`), the canary.local page
logic tests (`flash.test.js`), the `test_dp_budget` source pins, and the
WAP host-test list. The emulator dist, the SBOM and the build matrix are not
touched unless `FIRMWARE_VERSION` or a `FEATURE_*` flag changes.

## 4. D2 — familiar as notification-only novelty

### 4.1 What it is

`familiar.h` keeps an 11-bit fingerprint of the **radio scene** at the moment
an arrival is declared, built from aggregates only: a coarse time-of-day
bucket, the mean signal strength, the advertisement density and the spread of
signal strengths. No MAC, token or address goes into it, and there are only
2,048 possible values. It is stored in two salted Bloom filters, "today" and
"yesterday", with 1% random bit flips. `notify` asks it whether a scene is
"ambient" (seen recently) to quiet routine arrivals.

What is wrong with it today:

- the "yesterday" filter, its salt and an always-ignore list persist in
  plaintext NVS (`fm_yest`, `fm_salt`, `fm_ignore`, plus the rotation anchor
  `fm_rot_up`). With 2,048 possible values, a flash dump enumerates the
  filter, so the header's claim that a dump reveals nothing is false;
- rotation needs 24 hours of continuous uptime, so a device that reboots more
  often keeps one "yesterday" filter indefinitely, which breaks the header's
  "~48 h at most";
- a federated path would ship the filter to peers and OR-merge theirs. Each
  device salts its own hashes, so a merge only sets unrelated bits, and an
  all-ones share would make every scene "ambient", which silences everything.
  Only its own self-test calls it;
- the time bucket comes from uptime, not the wall clock;
- `notify::evaluate`'s decision is thrown away (`(void)notify::evaluate(ni)`
  in `rf_presence.cpp`), and its dwell gate is unreachable with default
  settings, so none of this reaches anyone yet.

The fingerprint never leaves the device today: no MQTT, HTTP, BLE or event
output carries it.

### 4.2 The conditions

**D2a, strengthening on its own, done regardless of D2b:**

1. Memory is RAM-only. Stop persisting `fm_yest` and `fm_rot_up`, draw a
   fresh salt in RAM at every boot, and purge `fm_salt`, `fm_yest`,
   `fm_rot_up` and `fm_ignore` from NVS at init (with the
   `nvs_store::remove()` D1 adds). A reboot is then a cold start, which errs
   toward more notifications, never fewer, and the 48-hour horizon holds
   whatever the uptime.
2. Delete the federated familiar path: `build_familiar_share`,
   `handle_familiar_share`, `FamiliarShare`, `snapshot_yesterday`,
   `merge_remote_yesterday` and their self-test steps. Sharing it is
   meaningless (per-device salts), an attack (the all-ones share) and
   cross-property fusion (Free Signals §5). It is never revived.
3. Delete the always-ignore list and `fm_ignore`. An indefinitely persistent
   pattern list is not "ephemeral memory" (Invariant C) under any reading.
4. Take the time bucket from the wall clock (`csi_event_current_bucket()`,
   loop task only; with no synced clock it falls back to today's uptime
   bucket).
5. Fix the "45-minute" comments (the buckets are 40 minutes, and the evening
   folds onto the early morning) and the false flash-dump claim.
6. Make the dwell gate reachable (also evaluate at `rf_dwell_started`) or
   delete the unreachable severity branches.
7. Add `tests_host/test_notify_familiar.cpp`: every suppression branch,
   forgetting on rotation and on reboot, and a source pin that nothing
   outside `notify`, `familiar` and `wizard` names
   `AlertDecision::fingerprint` and that `RfEvent` has no fingerprint field.
   Register it on the Makefile's default goal.

**D2b, the part that is argued (§4.3):** once a notification path exists, the
ambient answer may move a radio-only arrival between two tiers, under these
rules:

- **It demotes, never drops.** An ambient arrival goes from `important` to
  `digest`, and is still in the digest. It never becomes `none`, and it never
  removes or changes an event.
- **It carries a bool, never the fingerprint.** `notify` sees "ambient or
  not". `AlertDecision.fingerprint` stays in RAM and is read by nothing
  outside `notify`, `familiar` and `wizard`.
- **It stays on the device.** The hint is not put in an event, MQTT, HTTP,
  BLE or Home Assistant. The hub attention engine's novelty input therefore
  stays `unknown`. A hub-side producer would need its own decision under the
  same conditions.
- **It touches radio-only arrivals and nothing else.** Never a camera, radar,
  contact or tamper event, and it never makes anything critical (the engine's
  rule 4, [`spec/household_keys_v0.md`](../../spec/household_keys_v0.md) §7.2).
- **It is never named.** No phrase or reason says "familiar", "known",
  "unknown" or "new". The "why did I get this" trace gives the same reason
  whatever the hint was.

### 4.3 Why this is not a weakening change

Everything D2 does to the code narrows it: it deletes persistence, a sharing
path and an indefinite list, and bounds what is left. The question is whether
D2b, which lets a scene statistic choose a notification tier, crosses an
invariant. Taken one by one:

- **Invariant A (No identity).** The fingerprint is not an identifier of a
  device or a person. It has no address input, and 2,048 values are shared by
  countless unrelated scenes. It compares today's coarse scene with
  yesterday's: a cross-day comparison of a radio statistic, not of an entity,
  held at most 48 hours in RAM and gone at the next reboot.
- **Invariant C (Ephemeral memory).** "Only aggregate, non-identifying
  summaries may persist." After D2a nothing persists at all.
- **Invariant F (Symmetry).** Its enforcement line is "Device lists are
  internal calibration only; never affect event semantics." This is internal
  calibration in exactly that sense: the event stream is byte-for-byte the same
  with or without it, which is the line D1's IRK store crossed. No device is
  classified, and event output carries no novelty, so there is no "known
  device vs unknown device distinction in event output".
- **Invariant E (Physics, not politics).** "Has this radio pattern been seen
  recently" is a physical statistic. It infers no intent, scores no threat,
  and cannot make anything critical.
- **Free Signals §5.** Nothing is "authorized": there is no roster, owner or
  enrollment in it.

The honest residue: a tier difference is still something the household
notices. An unfamiliar radio-only arrival may interrupt where a familiar one
waits for the morning digest. That is the point a reviewer has to weigh.

### 4.4 If a reviewer is in doubt

Governance §4 says "if in doubt, treat the change as a weakening change". If
any reviewer doubts D2b, D2b goes to a formal RFC under governance §3.2, and
until that RFC lands the ambient answer reaches no notification. D2a still
lands, because each of its steps is strengthening on its own.

## 5. D3 — the BLE Scout: unchanged, pending

The Scout is inert. Its pairing window has no caller outside `ble_scout*`,
`FEATURE_BLE_SCAN` is on only in the FULL build, and with an empty registry it
emits only `scout_initialized`. Wired as written, it would emit per-device
`arrived` and `departed` events carrying the user's label (the known-vs-unknown
distinction Invariant F forbids) and keep a persistent per-device key
(Invariant A).

This record does not decide it. It stays unwired. The audit's roadmap (wire
Scout pairing only after an ADR) therefore stays closed: this is not that ADR.
A future carve-out would be a weakening change and would need the §3.2 RFC;
"drop" stays available as a strengthening change at any time.

## 6. D4 and D5 — the household posture

### 6.1 D4: the vocabulary (Apple Home decision #3)

- The household posture is `away` (wire byte `0x00`), `home` (`0x01`) or
  `night` (`0x02`). It is always called the *household* posture in code,
  topics, entities and copy, never bare "posture", which already means body
  posture in Vision events, the Pocket Canary's bird and Apple Home's "sensing
  posture".
- `home`: witnesses whose zone kind is `indoor` sense tamper only. `away`:
  full sensing. `night`: full sensing, and `away`'s behavior in v0; it is
  reserved for an after-hours rule set, which is where the Business edition's
  open-hours need named in decision #3 can land later, through the dictionary.
- `unset` is only a consumer's value for "no valid statement". It is never
  signed and means today's behavior.
- It is dictionary vocabulary: the `household_posture` block of
  `spec/witness_dictionary.json`, landed with the hub attention engine.
- It is not canary-wap `notify`'s Context (`CTX_HOME`, `CTX_AWAY`,
  `CTX_QUIET_HOURS`, `CTX_TRAVELING`), which D1 stops deriving from owner
  sightings. Mapping one onto the other is for the RFC in §8. It is also not
  the iPhone app's `HomePresence`, the phone's own guess that it is away.
  Feeding the household posture into `HomePresence`'s dark-alert suppression
  would recreate the mute switch that file records.

### 6.2 D5: the inbound trust surface (Apple Home decision #4)

**Who may write it.** Only an enrolled household key answering a live hub
challenge, through a key satellite or the checker's LAN API. Not Apple Home,
not Home Assistant, not voice (`voice.py` already refuses posture-changing
intents), not a kernel API token, not an MQTT word. So Apple Home's "arrive
home" (scene 4 in that doc's §4.1) cannot write it: a geofence is passive
presence, which the research refuses. "Good Night" (scene 5) becomes a
household-key act. A LAN client's bearer token binds a challenge to its
session and reads the posture, but cannot change it; tokens are not linked to
members, so revoking a member does not unpair its device (`household_checker
pair --reset` unpairs every client). The LAN API answers only peers on the
home network, which is enforced by peer address, not by cryptography: a proxy
or VPN that re-originates traffic from a private or loopback address defeats it
([`spec/household_keys_v0.md`](../../spec/household_keys_v0.md) §6.1).

**What it may touch.** The household posture, and nothing else: never the
chain, Beacon, break-glass, retention or configuration, and never one specific
event.

**The five rules:**

1. **Intent lowers; possession only raises.** Only an intent credential (one
   that needed its holder's own user verification: a phone or watch key behind
   Face ID or a Class 3 biometric, a FIDO2 key with a fingerprint sensor) may
   set `home`. A possession credential (a card, a fob, a FIDO2 key with user
   presence only) may set `away` or `night`. A borrowed or relayed card can
   raise sensing and never lower it. A rate-limit cooldown (five rejected
   responses on one lane of one door in ten minutes cool that lane for
   60 s) or a roster on disk that does not verify refuses `home`, but
   `away` and `night` still go through (checked against the last roster that
   verified), so neither can hold a house in `home`. The one exception is a
   cooling card lane: it carries only `away` and `night` and stays closed
   while it cools, because each trial touches a real card's
   failed-authentication counter.
2. **Home lapses.** A `home` statement lives at most 24 hours (12 by default)
   and lapses on each witness's own clock. A witness also refuses a statement
   issued more than 5 minutes (`MAX_SKEW_SECS`) ahead of its clock, so a
   `home` counts on a witness for at most 24 hours 5 minutes of that
   witness's own clock, whatever the hub's clock said
   ([`spec/household_keys_v0.md`](../../spec/household_keys_v0.md) §3.3). A
   hub clock more than 5 minutes ahead (give or take the minute floor on
   `issued_at`) makes witnesses refuse its statements (they behave as
   `away`, the fail-safe); a hub clock behind makes `home` lapse early, and
   at once if it steps back more than 5 minutes behind the statement's
   `issued_at`, because the hub's own lapse check runs the same test on its
   clock. Plain NTP is unauthenticated, so the hub should take
   its time from an authenticated source (for example chrony with NTS). The
   hub replaces a lapsed `home` with `away` and starts in `away` after every
   restart. It also times each `home` on its monotonic clock from the
   command, so a wall clock held back (by a spoofed time source, say) cannot
   keep it past `home_ttl`. A roster change that drops any member, or a
   roster on disk that does not verify, ends a current `home` too: nothing
   records who set it, so a revoked member's `home` cannot be told from
   anyone else's, and ending it is the fail-safe; members still enrolled set
   it again (after a stale roster, once one verifies: rule 1). A forgotten
   tap, or a denial of service against the LAN API (which holds whatever
   posture is current), leaves an empty house watched less than today only
   until the lapse, so a short `home_ttl` limits both.
3. **Statements are signed.** Each is a 112-byte record signed with the hub's
   Ed25519 posture key, which witnesses pin. v0 offers it through a file seam
   and `GET /v1/posture`, never as an unsigned broker word. Whoever holds the
   posture key can sign `home` past rule 1, and a forged statement with a
   huge `seq` would lock out every genuine one, so a leaked key means a new
   posture key and every witness paired again. The checker seals it at rest,
   together with the hub roster key, under a key-encryption key (KEK) that it
   refuses to load from inside its data directory. That protects a copy of
   the directory, not a disk image or a backup that also holds the KEK;
   keeping the two apart is the operator's job, which v0 cannot enforce
   (spec §5.4). The KEK still needs a backup of its own, somewhere safe:
   losing it loses the household — a new household, a new posture key with
   every witness paired again, and every admin and member enrolled again.
4. **Witnesses fall to `away` on any doubt.** Missing, lapsed, wrong
   household, replayed, a bad signature, issued more than 5 minutes ahead of
   the witness's clock, or no trusted clock all read as `away`, which is full
   sensing. A statement refused only for being ahead is otherwise genuine
   and newer than anything accepted, so the witness still records its `seq`
   as the last accepted: refusing it only now would postpone a future-dated
   `home`, not void it.
5. **No who.** The roster holds random handles and public keys and no field
   that can hold a name. No output says which credential or member acted.
   v0 has no TLS of its own, though: on a LAN bind without a TLS-terminating
   proxy in front of it, each client's bearer token and, in a command, the
   member handle cross the network in cleartext, so a passive listener on the
   LAN can link acts to a device and to a member (spec §6.1).

**The physical-confirm idiom.** In the repo the idiom is a press on the device
itself (announce → press-BOOT; [`test_console.md`](test_console.md),
[`usb_onboard.md`](usb_onboard.md)). A tap at the door is not that: it
supplies possession and presence at the door. The idiom applies to
enrollment: creating the household, and adding a member when one admin
approval is enough (`threshold` 1), need a confirm at the hub. In v0 that
confirm is an operator command on the hub host; a button is follow-up work.
Changing the admin set or the threshold needs `threshold` approvals from the
current admins and a 24-hour pending period. `roster genesis` defaults to
`threshold` 1, under which one admin's approval is all any change needs,
including removing every other admin once the pending period has passed, and
nothing announces that pending change beyond `roster pending`. So "one stolen
admin key cannot take over" holds only with `threshold` 2 or more, which wants
at least three admins: with exactly two, losing one key leaves no way to
remove it ([`spec/household_keys_v0.md`](../../spec/household_keys_v0.md)
§5.3). One credential is never both an admin and a member, because a FIDO2
authenticator signs whatever its relaying device hands it (spec §5.2).

### 6.3 What is built, and what it does not authorize

The hub side of D4 and D5 is built and host-tested: the household-key checker
in [`household/`](../../household/) and the attention engine in
[`src/relay/attention.rs`](../../src/relay/attention.rs). Nothing calls the
engine, no key satellite or phone app exists, and no Canary reads a posture
statement. Building the hub side lets the contract be tested; it does not
decide that a Canary may act on it. That waits for §8.

## 7. D6 — notification tiers never key on household membership

[`apple_watch_and_notifications.md`](apple_watch_and_notifications.md) §1.1
put "household presence" in Digest and "non-household presence" in Important.
Those rows rested on the D1 module, and they are the known-vs-unknown split
Invariant F forbids. They are rewritten in the same change as this record: a
tier depends on the event's class, its corroboration across physically
independent channels and whether quiet hours are on, never on who someone is
or whether they belong to the household. This is a strengthening change.

## 8. What needs a follow-up RFC

1. **The posture/inbound RFC**, under governance §3.2, before any witness
   changes what it senses on a household posture, and before the hub delivers
   attention decisions with the household posture as an input (that lets a
   household key quiet indoor notifications). It must:
   - name the roster as a stored set of long-lived, per-household identifiers
     (governance §2.3 "adding new identifiers", §4 "creates new identifiers")
     and argue for it in those terms, including against Invariant II;
   - decide whether a household posture change is journaled at all, and if so
     as a new sealed-log record type (a ruleset change that starts in the
     dictionary) carrying only a coarse time bucket;
   - read a credential check that drives Canary sensing against Free Signals
     Invariant D ("no integration with access control");
   - say how a witness receives and pins the statement, and pace the traffic
     change when indoor witnesses go quiet (Invariant III);
   - decide whether Home Assistant may show the household posture, since its
     recorder keeps an exact-time diary of every change;
   - next to that, decide whether the change time the hub itself reveals is
     acceptable: nothing records which credential was used, but the current
     statement states the minute of the last posture change or `home` renewal
     (a command, a lapse, a roster change that ended `home` or a restart) or
     of a republish after a failed write or a hub clock set back, the
     modification times of the seam and `seq.json` state its second, and any
     live reader of the seam or `GET /v1/posture` learns each change's time
     to its own polling precision;
   - map the household posture onto canary-wap's `notify` Context; and
   - carry the key-satellite device-class contract
     ([`spec/household_keys_v0.md`](../../spec/household_keys_v0.md) §6.3) as
     its own page.
2. **D2b**, if any reviewer is in doubt (§4.4).
3. **Any BLE Scout carve-out** (§5).
4. **Any off-LAN delivery of an `important` tier.** No `important` decision
   has a cloud poke class today (`cloud_class` in
   [`src/relay/attention.rs`](../../src/relay/attention.rs)). The relay's
   vocabulary (`PokeClass`) is "the whole vocabulary that may leave the LAN",
   and widening it is "a privacy decision, not a tweak"
   ([`src/relay/mod.rs`](../../src/relay/mod.rs)); it and
   [`alert_relay.md`](alert_relay.md) §1 would be amended first.

## 9. Follow-up work that needs no RFC

- **Firmware:** PR-a1 and PR-a2 (§3.3); D2a as one PR.
- **Docs:** [`rf_sensing_phase12_http_routes.md`](../rf_sensing_phase12_http_routes.md)
  (the household routes); the audit's §3.8 and item 16 status lines, pointing
  here; [`apple_home_integration.md`](apple_home_integration.md) §4.1 scenes 4
  and 5 and §9 decisions #3 and #4, pointing here; and the same doc's §1.2,
  whose publishing-point sentence needs the hedge the research gives it.
- **Separate, and not decided here:** the token-map race in `rf_presence.cpp`
  (a lock modeled on `wifi_presence.h`).

**Review.** Maintainer sign-off on this record covers D1, D2a, D3, D4 and D6.
D2b is covered too unless a reviewer is in doubt, in which case it goes to an
RFC (§4.4). D5 is recorded here as the design. A Canary acting on it waits for
the RFC in §8.
