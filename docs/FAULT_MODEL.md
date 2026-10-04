# Fault model and recovery — what survives what

**Status:** descriptive, not aspirational. Every row below names the code that
does the thing and the test that pins it, or says **not built**. Where a
guarantee is narrower than a reader would assume, the narrowing is stated in
the row, not in a footnote. Companion pages:
[`failure_semantics.md`](failure_semantics.md) says what is *recorded* when
something fails; [`FLIGHT_RULES.md`](FLIGHT_RULES.md) is the rule book and its
enforcement ledger; [`strategy/12`](strategy/12-engineering-foundations-flight-rules.md)
§4 is the FMEA that found most of the gaps named here. This page is the
*behavior*: for each fault class, what the system keeps, what it loses, what it
detects, and what it does next.

The threat model ([`spec/threat_model.md`](../spec/threat_model.md)) lists
adversaries and says everything else is out of scope. Faults are not
adversaries: a power cut, a clock step, a lost packet or a silent sensor has
no intent, and the system is designed around them all the same. This page is
where that design is written down.

## 1. Fault classes

| Fault | In scope | Where it is handled | What is claimed |
|---|---|---|---|
| Hub process crash (panic, `kill -9`, OOM) | yes | §2.1 | no committed event is lost; an unclean stop is sealed on the next boot |
| Hub power loss (torn write) | yes | §2.1 | at most the uncommitted append is lost; the chain resumes from the last committed row |
| Canary power loss mid-record | yes | §2.2 | at most the in-flight line is lost; the chain resumes from the last complete record (card) or the last cached head (no card) |
| Canary brownout / crash loop | yes | §2.6 | bounded: a crash-loop counter and a safe mode on the WAP and on the PlatformIO `canary`; a task watchdog on every product that has one; A/B rollback of an unconfirmed OTA image on every product (compile-checked and source-verified, not yet bench-verified) |
| Host clock step (forward or back) | yes | §2.4 | the chain is not ordered by the wall clock; a step is sealed as `ClockSkew`; retention is bounded against both directions |
| Device clock absent | yes | §2.4 | device buckets are uptime-based and say so; wall time comes from GPS (WAP), SNTP (display) or nothing (Sense, Vision) |
| Message loss, duplication, reordering (MQTT) | yes | §2.3 | at-most-once from a device; duplicates and losses are bounded and described, not prevented |
| Broker outage, network partition | yes | §2.3 | devices keep sealing locally; the hub keeps sealing its own sources; nothing replays across a partition except the PlatformIO tree's bounded RAM queue and, when that product owns a card, its SD event backfill |
| Sensor silent, corrupt, stuck | yes | §2.7 | silent and corrupt are detected and reported; stuck is not |
| Storage exhaustion (disk, NVS, card) | yes | §2.8 | detected and sealed on the hub; counted and retried on a Canary |
| A device that lies (Byzantine) | **no**, beyond signatures | [`spec/threat_model.md`](../spec/threat_model.md), [`device_trust.md`](device_trust.md) | a signature proves *who* asserted a claim, never that it is true; there is no cross-device consensus |
| A compromised host | **no** | [`root_paradox.md`](root_paradox.md) | none |

## 2. Component by component

### 2.1 The hub's sealed log (`witnessd`)

**Persistence.** SQLite in WAL mode with `synchronous=FULL` by default
(`src/lib.rs`, `set_sqlite_synchronous`; `Normal` is opt-in and documented as
able to lose the newest committed rows on a power cut). Every append is a
read-modify-write of the chain head inside one `BEGIN IMMEDIATE` transaction
(`src/storage.rs`, `in_immediate_write_tx`), so two writers sharing the file
cannot fork the chain (`concurrent_writers_cannot_fork_the_chain`). There is
no application write-ahead log and no torn-write test of our own: the
durability claim is SQLite's, on a disk that honors `fsync`. An append that
waits out the 5 s busy timeout is a failed seal, recorded as
`StorageWriteFailed`; it is not retried.

**Crash and power loss.** A committed row survives; an uncommitted one is
rolled back by the WAL. On the next start `witnessd` finds the last lifecycle
record is `start` with no `shutdown_clean` and seals a `PowerLoss` failure
record whose details say `unclean_shutdown` — a proxy that cannot tell power
loss from a crash or a kill (`src/bin/witnessd.rs`; `failure_semantics.md`).

**Boot.** Before the first append, `witnessd` verifies the chain from the
latest checkpoint through the tail. A structural failure (a bad link, a bad
signature, an untrusted checkpoint signer) puts the daemon in a report-only
**safe mode**: the read and verify API stays up, nothing is appended
(`verify_sealed_log`, `run_safe_mode`). **Narrowing:** this holds only when
the store *opens*. A database the kernel cannot open — corrupt, or keyed by a
seed that no longer matches — makes `Kernel::open` fail and the process exit;
under the example `systemd` unit (`Restart=on-failure`) that is a restart
loop, not a reporting state. FR-8 says the kernel "boots and reports even
with its store full or corrupt" and its own status column marks the
corrupt-store boot case open. It is open.

**What the chain cannot see.** The interior hash chain proves that no event
was edited, reordered or removed *between* two rows it still holds. It
cannot bind its own **length**: lopping off the newest rows, restoring an
older whole-database snapshot, or deleting the file and starting again all
leave an internally consistent chain that verifies. Two things close that:
the signed **high-water mark** (`src/log/high_water_mark.rs`) — a
device-signed `(seq, head)` kept outside the database, against which the
verifier and the boot check fail closed — and **anchoring** in time
(`timestamping.md`). **Narrowing:** the mark is **opt-in**
(`SECURACV_HWM_PATH`), no shipped configuration sets it, it is written
best-effort after the commit, and a mark kept in the same directory as the
database rolls back with it. So on a default installation a wiped log
restarts at genesis, boot verification passes (an empty chain is valid), and
the only trace is a fresh `start` record in the new chain. An installation
that wants "a wipe is detected" sets the mark path to append-only or external
media; the module's own doc comment lists the residuals.

**Retention.** Rows older than the configured retention are pruned behind a
signed checkpoint, in one transaction with the delete. **As of this writing
two clock faults turn that into loss:** the pass picks the newest row whose
stamp is expired and deletes every row at or below its id, so a clock
regression between two appends (a later row with an older stamp) sweeps
in-retention rows out with it, and a forward step (an RTC-less hub taking
its first NTP sync after boot) makes every row sealed minutes ago "older
than retention". #1738 (open) changes the prune to a **prefix** and ages the
rows this process sealed by the monotonic clock once the wall clock has
stepped; its residuals are stated in `failure_semantics.md` on that branch.
Until it lands, the host clock is trusted for retention, full stop.

**Liveness.** A watchdog thread aborts the process when the main loop makes
no progress for 60 s (`witnessd.rs`; unit-tested). Whether anything restarts
it is the operator's `systemd` unit; the `WatchdogSec` integration FR-9 asks
for is open.

### 2.2 A Canary's witness chain

A Canary signs every record into a per-device hash chain and persists it in
two tiers: the SD card's append-only `/WITNESS/records.jsonl` (one line per
record, the log of record) and an NVS cache of `{seq, head}` written every
`SD_PERSIST_INTERVAL` (10) records. The order is load-bearing: **SD first,
NVS second**, so a cut between the two leaves the card ahead, which is the
state boot recovery repairs.

**Power loss mid-record, card present.** The in-flight line is torn.
Recovery reads the tail, skips the torn line, adopts the last complete record
as head when it is strictly ahead of NVS *and* re-hashes and verifies under
this device's key (`witness_store.h`, `sd_wins`, `tail_parse`;
`test_witness_store_logic.cpp`). The record that tore is gone; its sequence
number is reused by the next record, chained from the last complete one.
**As of this writing the next append is written straight after the torn
fragment**, so the fragment and that record become one malformed line
mid-file, and `tools/verify_witness_log.py` reports it as an integrity
failure: an ordinary power cut reads as tampering on the next verification,
and the first post-reboot record is lost to the verifier with it. #1739
(open) seals the fragment onto its own line before the first append of a
mount and teaches the verifier to read a fragment the chain continues across
as a power-cut scar.

**Power loss, no card.** NVS holds a head up to nine records old. The device
resumes from it and **re-signs those sequence numbers with new content**:
two different records now exist for one `seq`, one of them only ever in RAM
and in whatever a broker retained. Nothing on the device or in Home
Assistant detects that regression today; the `chain_persist.h` comment says
so ("a power cut resumes from it"). The PlatformIO tree writes the
`{seq, head}` pair as one CRC-sealed blob so the pair itself cannot tear
(`chain_state.h`; `test_chain_state.cpp`) and counts and retries a persist
that NVS refused (`chain_persist.h`; `test_chain_persist.cpp`). **The
canary-wap sketch still writes the two values as two NVS puts and advances
its persisted-sequence marker whether or not they landed** — the defect the
common headers fixed, not yet ported.

**Genesis restart.** A wiped or replaced NVS with no card resumes from
genesis. No product seals a "chain reset" record; the hub's roll-call sees a
chain length that went backwards only if it happens to hold the old length.

**Card absent for a stretch.** The chain keeps advancing in RAM/NVS; when
the card returns, a fork guard refuses to append behind a tail that is at or
past the next sequence (`securacv_witness.cpp`, `sd_tail_forks_chain`), and
the offline verifier reports the missing stretch as a gap segment, verified
independently on each side.

### 2.3 Transport: MQTT between a Canary and the hub

**From a device.** Every product publishes at **QoS 0**: PubSubClient
publishes only at QoS 0 (`securacv_mqtt.cpp`), and the WAP's esp-mqtt path
passes `qos=0`. Delivery is at-most-once; "published" means the bytes reached
the TCP stack. The PlatformIO `canary` tree keeps a bounded RAM queue while
the broker is away (12 slots of 512 bytes, tamper alerts ranked first,
oversize refused, drops counted; `mqtt_offline_queue.h`,
`test_mqtt_offline_queue.cpp`) and replays it in order on reconnect; a
replay whose publish "succeeded" and was lost is not retried, and a replay
whose publish failed is retried, so the queue can produce **either** a loss
or a duplicate, and it is lost on reboot. **With a card, the same tree
recovers further:** its CSI event log's backfill (`csi_event_backfill.h`,
driven by `csi_event_egress_pump()`; host-tested against a model of Home
Assistant's replay gate) replays committed rows from `/EVENTS/today.ndjson`
once the RAM queue has drained, across a reboot, never below the id
watermark Home Assistant last verified (a ceiling persisted in NVS before
an id is handed over, so a reboot never republishes one and skips at most a
stride of undelivered ones), in id order so a newer row never overtakes an
older one (a row committed while the card is briefly out, or whose append
failed, waits in RAM behind the card's rows, for at most 45 s while the
card is out; sweeps F103, F104), and never for tamper alerts, which go at
commit whatever the backlog. An outage longer than the RAM queue is therefore recovered from
the card on that product; what it can still lose is a row that never
reached the card, a row the ceiling skipped, and a row waiting in RAM when
the device reboots or its broker changes. That last includes every row
committed in the first 45 s after a boot on a canary with an SD slot but
no usable card, so such a canary in a boot loop shorter than 45 s delivers
no events-topic row at all (tamper alerts, the boot's power verdict among
them, do not wait). Sense, Vision and Sentinel
have neither queue nor backfill: an event sealed while offline is not
published at all, and the gap shows as a jump in the retained chain length
(`canary-sense/src/main.cpp`, `record_event_now`). The record itself is
never at risk on any product: it is sealed into the device chain before any
publish is attempted.

**Into the hub.** `adapter_host` subscribes at QoS 1 but connects with
`clean_start`, so anything published while the hub was down is gone
(`src/bin/adapter_host.rs`). The client acknowledges on receipt, before the
kernel seals, so a crash between the two loses the claim. Reconnect is a
fixed 3 s sleep, unjittered. Claims are de-duplicated per **hub** 10-minute
bucket by `(adapter, kind, zone, hint)` (`src/adapter/host.rs`): a device
event replayed into a later bucket seals twice; two distinct events from one
device, one zone and one bucket seal once. The hub stamps every claim with
its **own** receive-time bucket; the device's bucket is not read. The kernel
does not verify a device's signature on ingest — a claim from the MQTT
adapter is kernel-signed at ingest, attestation `adapter`; Home Assistant is
where a device signature is checked against a pinned key.

**What this adds up to.** There is no end-to-end delivery guarantee, and
none is claimed: not at-least-once, not exactly-once. The guarantee that
holds is local: every event exists in the chain of the device that witnessed
it, and every event the hub accepted exists in the hub's chain. Whether a
given device event reached the hub is answerable after the fact by comparing
chains, not guaranteed in advance.

### 2.4 Time

**Hub.** Events are stamped with a 10-minute bucket of the host's wall clock
at sealing. The chain's order is the append order (`AUTOINCREMENT` id) and
its hash is `SHA-256(prev ‖ payload)`: **a wall clock that goes backwards
does not break the chain.** A regression shows in the timeline audit as a
warning, softened when a `ClockSkew` record covers it; `log_verify --strict`
turns warnings into failures. `ClockMonitor` seals one `ClockSkew` per
excursion — a drift between wall and monotonic clocks beyond
`clock.skew_tolerance_s`, or a bucket going backwards — then re-baselines.
Retention is the one place the wall clock had teeth; see §2.1.

**Devices.** A Canary's chain `time_bucket` is **uptime**, not wall time:
`millis()`-derived, reset every boot, wrapping at 49.7 days, inside the
hash as data (`witness_chain.h`). The WAP and the PlatformIO tree set the
wall clock from GPS when a fix exists and attest the source per record;
the display uses SNTP; Sense and Vision have no wall-clock source and their
`ts_ms` is uptime. Cross-device time comparison is therefore not something
the chains support, and the threat model lists "no cross-device
comparability" as a goal, not a gap.

### 2.5 Wi-Fi join: a self-stabilization argument, with its holes

Every networked Canary runs the same retry policy
(`firmware/common/network/wifi_join_policy.h`; the emulator runs the same
header, `lint_wifi_join_policy.py` proves it). Its state is
`{online, ever_online, attempts, lost_since_ms, last_attempt_ms}`; its
inputs are the radio's result and `millis()`.

**Claim.** From any state, the device converges to one of two legitimate
states within bounded time: *associated*, or *retrying at a bounded, jittered
interval*. It never enters an unbounded reboot loop.

**Argument.**
- Backoff is `base << (attempts−1)`, shift capped at 5, ceiling 30 s,
  overflow-guarded; jitter adds up to a quarter of the step, so the retry
  interval is bounded above by 37.5 s and below by 2 s
  (`wifi_backoff_ms`, `wifi_backoff_with_jitter_ms`; pinned in
  `test_wifi_join_policy.cpp`).
- The only reboot is `ever_online && outage ≥ 5 min`
  (`wifi_next_action`). `ever_online` lives in RAM, so a reboot clears it:
  after the reboot the link has "never worked" and the policy will not reboot
  again until a real association has happened. Hence **at most one reboot
  per association**, and a link that never associates retries forever
  without rebooting — the setup loop this header was written to end.
- Deadlines use wrap-safe signed deltas, so the 49.7-day `millis()` wrap
  does not make every deadline fire at once.

**What the argument does not give you**, stated so nobody has to discover
it on a bench:
- Convergence to *associated* depends on the environment (an access point
  that is up and reachable). The policy converges to *retrying*; it cannot
  make a router exist.
- The 5-minute outage reboot is **not jittered**. Every Canary that saw the
  same access point drop reboots in lockstep at +300 s.
- Because `ever_online` is RAM-only, a device that rebooted during a long
  outage comes back as "never online". If the access point is still down its
  join fails with `NotFound`, and after three attempts the policy opens the
  **setup portal** (`wifi_should_open_setup`) — the header's own rationale
  says a device that worked this morning should not start advertising an
  open setup network, and the test `a_device_that_worked_before_never_throws_away_its_config`
  assumes `ever_online` persists. The saved network keeps being retried
  underneath the portal, so the device does rejoin when the router returns;
  what it also does is expose the portal in the meantime.
- The host tests are single-step property tests. Nothing models the
  composed behavior across reboots; the argument above is prose, not a
  proof, and a reader who wants the model checked should treat it as an
  open item.

The MQTT supervisors on Sense, Vision and Sentinel use bounded backoff with
jitter (`canary-sense/src/main.cpp`); the PlatformIO tree's MQTT reconnect
has no jitter (`securacv_mqtt.cpp`; strategy doc 12, F4).

### 2.6 Crash loops, brownout, and the boot path

- **Canary WAP.** `safe_mode_check()` counts *consecutive crash resets*
  (panic, watchdog, brownout — never power-on, the reset button, or the
  device's own `ESP.restart()`) in NVS. Three in a row enter safe mode:
  optional peripherals off, dashboard up. After 60 s stable in safe mode the
  device tries a recovery reboot, at most three times, then stays in safe
  mode for a human; if NVS cannot be opened it stays in safe mode rather
  than risk an unbounded loop. Sixty seconds of stable full operation clears
  both counters (`hardware_state.h`). There is no time window on the
  *counting* — "three reboots in sixty seconds" is the wrong shorthand; it
  is three crash resets with no sixty-second stable run between them.
- **Canary (PlatformIO tree).** `bootguard::begin()`
  (`firmware/common/health/boot_guard.h`, the NVS glue around the pure
  `boot_policy.h`) counts every boot that does not reach *healthy* —
  `setup()` returned and `loop()` ran for 30 s, or the device chose to
  restart or deep-sleep (the loop's own restarts, and an authenticated
  `POST /api/reboot`, which the HTTP task hands to the loop rather than
  running itself) — and persists the count before any risky init.
  Four in a row on a confirmed image stop in a serial safe mode (radio,
  storage, sensors and the witness chain never start); an image still
  pending OTA confirmation never enters it, because the rollback below owns
  that case. The count starts over for a different image (OTA install,
  A/B rollback, USB flash of another build), on a healthy boot, or on the
  operator's confirmed "clear" (serial `c` then `y`, or BOOT held 2 s). If
  NVS cannot be opened, or the count reads but cannot be written back
  (including a real write probe on any boot bound for safe mode), it boots
  normally and says so. A power-on reset is **not** counted — the same
  rule as the WAP's: a switched outlet, a smart plug or a storm flicker must
  not put a home device into a no-radio safe mode. It neither adds to the
  count nor clears it. Every other reset reason counts (panic, watchdogs,
  brownout, software, external, deep-sleep wake). The cost: a hang that no
  watchdog catches, ended by someone pulling the plug, is not counted.
  Compile-checked; the
  decisions and the glue are host-tested (`test_boot_policy.cpp`,
  `test_boot_guard.cpp`); not bench-verified.
- **Every other product** with a task watchdog (`esp_task_wdt` on Sense,
  Vision, Display and Sentinel) resets on a stuck task and logs the reset
  reason on the next boot. Nothing counts those resets there.
- **OTA rollback.** A/B slots exist on every product, and the revert net —
  a new image stays `PENDING_VERIFY` until the boot self-test confirms it,
  and any reset before that boots the previous image — is compiled into
  every product. An earlier version of this section said the shipping
  Arduino and PlatformIO builds lacked the bootloader config and
  auto-confirmed; the pinned cores' own precompiled `sdkconfig` enables it
  (checked against arduino-esp32 2.0.17 and the 3.x lib-builder on
  2026-09-29), and the OTA engine now fails the build on a core that does
  not. Not yet bench-verified: no deliberately bad image has been pushed to
  a shipping build and watched to revert (`V1_BENCH_TEST_RUNBOOK.md`,
  Track E). The confirmation point differs per product
  (`firmware_ota.md`, "Where each property holds today"). The anti-rollback
  version floor is in NVS, which physical access can erase; it is not an
  eFuse.
- **Brownout.** Logged by reset reason (an `ERROR` entry on the WAP that
  names the supply), not sealed. `failure_semantics.md`'s `PowerLoss` is a
  hub-side proxy; device resets are telemetry.

### 2.7 Sensors

- **Radar (Canary Sense).** A silent UART drives the presence FSM to
  `Unknown` after the stall deadline, before any data is trusted
  (`mr60_presence.cpp`, deadline-before-data); `Unknown` is radar-link
  health, not a witness event, and reaches Home Assistant as the firmware's
  own "radar link problem" binary sensor. Home Assistant's separate
  radar-link diagnostic sensor reads a `radar` object from the health
  payload that **no firmware publishes as of this writing**, so that entity
  stays "unknown"; #1740 (open) adds the object. The same PR fixes a
  stall-recovery defect on the evidence path: today the first target frame
  after a stall moves the FSM through `Clear`, and canary-sense signs a
  `presence_cleared` record while a body is in view. Corrupt frames are
  dropped by checksum and counted; there is no alarm threshold on the
  count. A radar that reports the same value forever is **not** detected —
  the FSMs read a steady value as a held lock. There is no radar self-test,
  no warm-up gate, and the decoder has not yet parsed a frame from a real
  module.
- **Camera / RTSP ingest (hub).** An ingest supervisor latches one
  `GapMissingData` per outage after `ingest.failure_threshold_s` and
  reconnects with capped, unjittered backoff (`witnessd.rs`,
  `IngestSupervisor`).
- **Sensor disagreement.** `FailureType::SensorDisagreement` exists in the
  vocabulary and is **never emitted**: adapters are independent and there is
  no multi-sensor consensus layer. Sentinel fuses channels *within one
  device* (`sentinel_fusion.h`, host-tested); nothing fuses across devices.

### 2.8 Storage exhaustion

- **Hub.** `DiskMonitor` checks free space on the database filesystem every
  `storage.check_interval_s` and seals one latched `StorageFull` per drop
  below `storage.min_free_mb`; a failed append seals `StorageWriteFailed`
  with an alarm-table → stderr degradation chain. Storage health (wear,
  write rate, thermal) is sampled and reported (`storage_health.rs`).
- **Canary NVS.** The PlatformIO tree counts a refused chain persist, retries
  it once promptly and then once per interval, and reports the streak's
  start and end (`chain_persist.h`). What the Arduino core itself does when
  the NVS partition has no free pages has not been verified here; our code
  does not handle that case.
- **Canary SD.** A missing or full card is one latched warning per outage;
  records keep chaining in RAM/NVS and the gap is reported by the verifier
  (§2.2). Card errors past a threshold mark the card lost and trigger a
  remount (`securacv_storage.cpp`).

## 3. What is not claimed

- No end-to-end delivery guarantee (§2.3). No exactly-once, no
  at-least-once.
- No cross-device ordering or clock (§2.4): no Lamport, vector or hybrid
  logical clocks, and the event contract forbids sequence numbers that imply
  continuity across buckets.
- No cross-device consensus and no Byzantine tolerance: signatures bound
  *authorship*; they do not make a lying device's claim false. Gossip
  replication and co-signing are draft specs with no code
  (`spec/README.md`).
- No crash-injection, torn-write, partition or chaos test. FR-12's
  day-in-the-life campaign is open; issue #923 tracks property tests,
  fuzzing and crash injection. The fuzz targets that exist cover parsers,
  not storage or recovery.
- No formal model of any of the above. §2.5 is an argument in prose.

## 4. Where each row is pinned

| Claim | Test or gate |
|---|---|
| Two writers cannot fork the hub chain | `concurrent_writers_cannot_fork_the_chain` (`src/lib.rs`) |
| Seal-then-verify round-trips any event sequence | proptest in `src/lib.rs` |
| Unclean stop is sealed on reopen | `src/lib.rs` lifecycle tests |
| Tail truncation / rollback fail closed with the mark | `src/verify_runner.rs` high-water-mark tests |
| Retention prunes a prefix; a step cannot expire fresh rows | **not on main** — `retention_prunes_a_prefix_so_a_clock_regression_cannot_take_live_rows` and `storage::age_floor_tests` arrive with #1738 |
| One `ClockSkew` per excursion | `clock_monitor_seals_one_record_per_bucket_regression` (`witnessd.rs`) |
| Device chain pair cannot tear; refused persists are retried | `test_chain_state.cpp`, `test_chain_persist.cpp` |
| Torn tail is skipped on recovery | `test_witness_store_logic.cpp` (`tail_parse`) |
| Torn tail is sealed before the next append and read as a scar | **not on main** — `tail_is_torn` and the scar cases in `tools/test_verify_witness_log.py` arrive with #1739 |
| Wi-Fi backoff bounded and jittered; never-online never reboots | `test_wifi_join_policy.cpp`, `scripts/lint_wifi_join_policy.py` |
| Offline queue bounded, ordered, tamper-first | `test_mqtt_offline_queue.cpp` |
| Silent radar fails safe | `test_mr60_uart.cpp` (stall cases) |
| A returning radar never signs "cleared" over a body | **not on main** — the stall-recovery cases arrive with #1740 |
| Boot-time tail verification enters safe mode | `witnessd` boot path (#990); corrupt-store open is **not** covered |
| Watchdog aborts a stalled loop | `witnessd.rs` watchdog unit test |
| A Canary crash loop degrades to safe mode; a pending image never does; every escape works | `test_boot_policy.cpp`, `test_boot_guard.cpp` (decisions and NVS glue; the boot-path wiring is bench Track E) |
| A bad OTA image reverts | **not tested** — compile-time `#error` in `securacv_ota.cpp` proves the core enables the config; the revert itself is bench Track E |
