# Magic pairing — the Bluetooth setup door (design)

**Status (2026-10):** built on Canary Sense, Canary Vision and the Canary
WAP's FULL profile; the pure halves are host-tested on both ends; **nothing
has been bench-tested on hardware yet.** The radio, the iOS pairing sheet's
timing on NimBLE 1.4.x and the BOOT button on each board are the bench items
([the checklist](#9-bench-checklist-what-to-verify-before-calling-it-shipped)).
The flagship `firmware/canary` build is a **follow-up**, not part of this
change (tracked as F251 in the
[sweep backlog](../audit/repo_todo_sweep_2026-09.md)). The displays keep their glass QR and open no door.

**Canonical statements this page restates, never overrides:** the wire and
the door rules are the header comment of
[`firmware/common/network/improv_core.h`](../../firmware/common/network/improv_core.h);
the claim ticket is
[`firmware/common/network/claim_ticket.h`](../../firmware/common/network/claim_ticket.h);
the hub hand-off's document is
[`firmware/common/network/hub_handoff.h`](../../firmware/common/network/hub_handoff.h);
the receipt's grant order is
[`firmware/common/network/provisioning_gate.h`](../../firmware/common/network/provisioning_gate.h).
If this page and a header disagree, the header wins and this page is the bug.

Vocabulary: [the glossary](../GLOSSARY.md) ("Bluetooth setup door", "Claim
ticket", "Magic pairing"). The user-facing answers:
[the FAQ](../FAQ.md#how-do-i-give-a-new-canary-my-wi-fi-do-i-have-to-type-a-setup-key).

---

## 1. Purpose

A brand-new Canary has no Wi-Fi and no screen. Until now the person had to
find its `SecuraCV-XXXX` setup network, type a key only its serial console
had printed (or a display showed), join it, and fill in a portal. Magic pairing is the one-tap
version: the Canary says "I am new and I am listening" over Bluetooth, the
SecuraCV iPhone app shows a card ("A new Canary is nearby"), the person taps
it, picks their Wi-Fi from the networks *the Canary itself can see*, types the
password once, and the Canary answers with its own verdict. For a WAP the
same tap ends with the phone **paired** — holding the device's bearer token —
without that token ever crossing Bluetooth.

The protocol is not ours. It is **Improv Wi-Fi over BLE**
([improv-wifi.com](https://www.improv-wifi.com/)), the open standard Home
Assistant, ESPHome and the improv-wifi web SDK speak, byte for byte. We add
the *rules* (when the door is open, how long, how often) and, on the WAP, a
separate companion service for the claim ticket. A standard client gets Wi-Fi
provisioning and nothing else.

What "magic" does **not** mean: it is not a pairing that happens without the
person, and it is not open to whoever walks past. The whole design is the
sentence from `improv_core.h`:

> the door is open for a device NOBODY OWNS YET, and for an owner's own tap —
> never for a device that has an owner and a bad day.

---

## 2. Threat model

### 2.1 Who can open the door

| Situation | Door | Why |
|---|---|---|
| No credentials stored (first boot, after a factory reset), within **30 minutes** of power-on | **Open** (`Door::NoCredentials`) | A device nobody owns yet. The window is `IMPROV_FIRST_BOOT_WINDOW_MS` (`Timing::first_boot_window_ms`, half an hour by default); a power cycle re-arms it, a factory reset re-arms it from the moment the credentials vanish; compile-time `0` makes the device tap-only. A **software restart does not** re-arm it: the spent time rides in RTC memory across `ESP.restart()` and watchdog resets and is cleared only by a power-on, a brownout, a power glitch or a reset a host asserted from the USB / JTAG port — the Flasher's, so a re-flashed unit never inherits the old image's spent window (the WAP's first-boot wizard restarts the unit every 15 idle minutes, which would otherwise have re-opened the door forever). |
| No credentials stored, the window has run out | Shut | "A unit forgotten in a drawer is not claimable from the street for the rest of its life." A power cycle or a tap reopens it; a software restart does not. |
| Credentials stored, the network is fine | Shut | A device that has an owner offers nothing. |
| Credentials stored, the saved network is **failing** (the SoftAP recovery portal is up) | **Shut** | The recovery portal has a key printed on the unit; this door does not. See §2.3. |
| A short **BOOT tap** on a board whose firmware reads one (Sense, Vision), with or without credentials | **Open for 60 s** (`Door::Tap`, `Timing::tap_ttl_ms`) | The owner's own act, the same trust the BOOT-tap receipt already expresses. No effect while the no-credentials door is already open (that one has its own window). On a witness that already has a network the tap also raises the SoftAP setup portal underneath the door for that minute — the door's join path is the portal's — with no quiet retry of the saved network under it, and lowers it again when the door shuts without a join (`wifi_open_tap_portal` / `wifi_close_tap_portal`). |
| The 11th `WIFI_SETTINGS` attempt on one open door | Shut, until a tap or a power cycle | `Timing::wifi_settings_cap` = 10. A no-credentials door shut this way marks its window spent. |

The WAP has **no tap door in this version**: its BOOT button keeps the
sketch's own gestures (a short press opens the 30 s receipt gate; a 2 s hold
prints the identity on serial), and **no button gesture factory-resets a WAP
today**. A WAP's door is the first-boot window alone; the owner's way to
re-arm it is **Forget Wi-Fi** on the dashboard (`POST /api/wifi/forget`),
which clears the credentials and the carried window record together.

### 2.2 What a stranger in radio range can and cannot do

While the door is **shut** (the steady state of every owned Canary):

- They see the fleet presence beacon — the same bytes the device has always
  put on air. On a Sense or a Vision the device is **not connectable**; on a
  WAP the pairing channel answers as before (its Opera scan response, its
  bonded services, which refuse an unauthenticated key).
- A standard Improv client that reaches the service reads **awaiting
  authorization** (`0x01`) on the state characteristic. A `WIFI_SETTINGS`
  write earns `NotAuthorized` (`0x04`) and starts nothing.

While the door is **open** (a new unit, or the owner tapped it):

- They can connect and, after the Just Works pairing, write credentials for
  *their* network. That is the inherent exposure of any "no owner yet"
  door, and it is bounded: the window is 30 minutes from power-on, not
  forever; a join that succeeds stores credentials and shuts the door; an
  owner who finds the unit on the wrong network holds it, factory-resets it,
  and starts again.
- They **cannot** hold the door open or hold the single link against the
  owner: a client that sends nothing for **3 minutes** is dropped
  (`Timing::idle_disconnect_ms`); accepted credential writes are at least
  **3 s** apart (`Timing::wifi_settings_cooldown_ms`); **10 attempts** per
  open door, accepted or malformed, and the door shuts. A flood of garbage
  is an attempt too (`session_on_malformed_wifi_settings`).
- They **cannot** learn which byte of a bad packet was wrong — every parse
  failure is `InvalidRpc` (`0x01`), because "a device that names which byte
  was wrong is a device helping someone probe it."
- They **cannot** get the WAP's bearer token, its console, its OTA channel,
  its witness export or its bonded provisioning: the key a Just Works
  pairing yields is *unauthenticated*, and every `*_AUTHEN` characteristic
  refuses it. The claim ticket they could read opens nothing from the street
  (§2.4).
- A **passive listener** learns nothing: the command and result
  characteristics require an encrypted link (LE Secure Connections), checked
  by the characteristic's properties *and again in the write handler*. The
  state, error and capability bytes stay plain, which is what lets a
  standard client show "awaiting authorization" without pairing first.

### 2.3 Why a recovery portal never opens it

The first committed version of this feature opened the Bluetooth door
whenever the SoftAP setup portal was up — and the portal is up not only on a
first boot but also when a *saved* network keeps failing (a changed
password, a router that rebooted, an SSID that went away). That turned "my
router is down" into "anyone in radio range can re-point my Canary at their
network." The two cases look alike from the portal's side and are opposite
from the owner's: the first is a device nobody owns, the second is a device
that has an owner and a bad day.

So the door takes the **stored-credentials fact**, never the portal's. The
glue passes `no_credentials` as what NVS holds; `session_set_no_credentials`
documents that "a recovery portal is NOT 'no credentials'." The recovery
portal remains exactly what it was: a SoftAP with a device-unique key printed
on the unit — a door with a key. The lesson is written down in
[`firmware/LESSONS_LEARNED.md`](../../firmware/LESSONS_LEARNED.md) ("a jammed
router is not a claim window") so it is not re-learned.

### 2.4 Why the token never rides Bluetooth

A Just Works pairing is the only kind a phone with no prior bond can open in
one tap, and it yields an encrypted but **unauthenticated** link: it resists
a passive listener, not an active one who was there first. Nothing that
*lasts* belongs on such a link. The WAP's bearer token lasts for the life of
the device. The first version put the whole pairing receipt — token
included — on that link, and its `base_url` pointed at a SoftAP address that
is dead the moment the AP drops after a join.

The claim ticket (§3.5) replaces it: 16 random bytes, minted the moment the
join the phone asked for succeeds, readable by the link that provisioned
during **one read** (a ~300-byte value is one Read Response plus its Read
Blob continuations, so the value stays for a 3 s grace after the first
read, then is withdrawn), good for **180 s**, spent by **one** HTTP request *on the home
LAN* that answers the same receipt the BOOT-tap route serves. Two factors,
then: the encrypted link that provisioned, **and** presence on the Wi-Fi the
device just joined. A phone that was only near the device gets a string that
opens nothing from the street. A wrong guess burns the claim; an expired one
is burned too; a second taker on any task reads nothing.

---

## 3. The wire

### 3.1 UUIDs

The Improv standard's, verbatim (`improv_core.h`, `ImprovWire.swift`):

| UUID | Role | Properties | Payload |
|---|---|---|---|
| `00467768-6228-2272-4663-277478268000` | Improv Wi-Fi service | — | — |
| `...8001` | Current state | READ, NOTIFY (plain) | one byte, §4.1 |
| `...8002` | Error state | READ, NOTIFY (plain) | one byte, §4.1 |
| `...8003` | RPC command | WRITE, **encrypted** | the phone's request, §3.3 |
| `...8004` | RPC result | READ, NOTIFY, **encrypted** | the device's answer, §3.3 |
| `...8005` | Capabilities | READ (plain) | one byte, `CAP_*` bits |
| `0x4677` (16-bit) | Service data in the scan response | — | 6 bytes, §3.2 |

SecuraCV's own — the WAP registers CLAIM on it (`claim_ticket.h`, the
sketch's `ble_improv`), Sense and Vision register HUB (`hub_handoff.h`,
`improv_ble.cpp`); no device registers both yet:

| UUID | Role | Properties | Payload |
|---|---|---|---|
| `8fc1cf00-b162-4401-9607-c8ac21383e90` | SecuraCV companion service | — | — |
| `8fc1cf01-b162-4401-9607-c8ac21383e90` | CLAIM (WAP) | READ, **encrypted** | the claim JSON, §3.5, one read (with its blob continuations) by the provisioning link |
| `8fc1cf02-b162-4401-9607-c8ac21383e90` | HUB (Sense, Vision) | WRITE, READ, **encrypted** | write: the broker JSON, §3.6, ≤ 240 bytes; read: one status byte |

The companion service is deliberately *outside* the Improv service, so a
standard client sees a standard Improv device and never meets it. The whole
BLE UUID registry is in [`ble_protocol.md`](../ble_protocol.md).

### 3.2 What goes on air (the advert set)

The **fleet presence beacon stays the primary advert at all times**, so a
display's passive roster scan keeps hearing a device whether or not its
door is open, and a provisioned device puts exactly the bytes on air it
always did.

| | Door shut | Door open |
|---|---|---|
| Primary advert | the fleet beacon (manufacturer data) | the fleet beacon with **`FLEET_BEACON_FLAG_SETUP_OPEN`** (bit 5, `0x20`) set, the flags AD, and a short local name `Sense-AB12` / `Vision-AB12` / `WAP-AB12` |
| Connectable | **No** on Sense and Vision (nothing to dial); the WAP's pairing channel as before | Yes |
| Scan response | none on Sense and Vision; the WAP keeps its Opera scan response (name + its own service UUID) | the 128-bit Improv service UUID + 6 bytes of service data under `0x4677` |

On a Sense or Vision the four hex characters of the name are the same
four as the device's `SecuraCV-XXXX` setup network (both come from the
device pseudonym), so a phone can tell the two doors are one device. A
WAP's name ends in the last four hex characters of its key fingerprint —
the same four as its Opera `SCV-XXXX` name and the fingerprint its
dashboard shows — while its setup network's suffix is spelled in the
no-confusion alphabet of its device id, so the two differ there; the
card's "Blink it" button is the sure way to tell two WAPs apart. The service data is `[state][capabilities][0 0 0 0]`
(`build_service_data`). iOS, Android and Chrome each merge the scan response
into the advertisement a service filter matches, so a scan filtered on the
Improv UUID still finds it; the beacon bit alone is a hint, the UUID is the
promise (`NearbyCanary.swift`).

### 3.3 Packets

```
RPC command   [cmd][data_len][data ...][checksum]
WIFI_SETTINGS data: [ssid_len][ssid bytes][pass_len][pass bytes]
RPC result    [cmd][data_len][len_1][str_1] ... [len_n][str_n][checksum]
checksum = the low 8 bits of the sum of every byte before it
```

Commands: `WIFI_SETTINGS` 0x01, `IDENTIFY` 0x02, `GET_DEVICE_INFO` 0x03,
`GET_WIFI_NETWORKS` 0x04, `HOSTNAME` 0x05, `DEVICE_NAME` 0x06,
`GET_NETWORK_STATE` 0x07. Capability bits: identify `0x01`, device info
`0x02`, scan Wi-Fi `0x04`, hostname `0x08`.

- A `WIFI_SETTINGS` result carries the URLs the device can now be reached
  at (the first is where a client should go; an empty string when it serves
  no page — a Sense or Vision answers `""`).
- A `GET_WIFI_NETWORKS` answer is one result per network — three strings:
  ssid, rssi as decimal text, auth name — closed by one empty result. This
  is why the phone never needs location permission to know the SSID: the
  Canary lists what *it* sees.
- A `GET_DEVICE_INFO` result is four strings: firmware name, firmware
  version, hardware, device name.
- The parser is **strict** on purpose: `data_len` must account for every
  byte between the header and the checksum, and a `WIFI_SETTINGS` body must
  consume its data exactly. SSID ≤ 32 bytes, password ≤ 64 (WPA2's own
  bounds); longer is **refused, never truncated** — "a truncated password
  persisted as the one the person typed is the worst outcome." An empty
  SSID or a NUL inside a credential is `Malformed`. Every refusal is
  `InvalidRpc` on the wire.

The byte vectors are pinned on both ends by
`firmware/tests_host/test_improv_core.cpp` and `ios/.../ImprovWireTests`
(the spec's worked example, byte for byte).

### 3.4 Security on the link

- **LE Secure Connections, Just Works, no bond kept.** The next setup pairs
  afresh, which is the point: a bond is a lasting relationship, and this
  door makes none.
- The command (`...8003`) and result (`...8004`) characteristics require
  encryption by their GATT properties, **and the write handler checks the
  link's encryption again** before it parses a byte — a stack that answered
  the property check wrong would still be refused.
- The state, error and capabilities characteristics stay plain, so a
  standard client sees "awaiting authorization" without pairing.
- On the WAP, the pairing channel's security profile is swapped to Just
  Works **only while the door is open** (`bluetooth_channel::set_setup_door`),
  **never while a Numeric Comparison is pending or an authenticated / bonded
  link is up**, and restored when the door shuts. The key such a pairing
  yields is unauthenticated, and the console, OTA, witness-export and bonded
  provisioning characteristics (`READ_AUTHEN` / `WRITE_AUTHEN`) refuse it.

### 3.5 The claim ticket (WAP)

Once the join the phone asked for succeeds, the WAP mints 16 random bytes
(`esp_fill_random` on the device; a fixed pattern in the tests), spelled as
32 lowercase hex characters, and arms the CLAIM characteristic for **the link
that provisioned** — for one read: the value stays through that read's blob
continuations and a 3 s grace, then the loop task withdraws it (and the
moment that link is gone). Anyone else, and any later read, gets `{}`. The
claim is armed BEFORE the Provisioned state and the WIFI_SETTINGS result are
notified, so a phone that reads it the instant it hears the verdict finds
it. Its JSON:

```json
{
  "device_id":   "<the device id>",
  "claim":       "<32 hex characters>",
  "claim_url":   "<where to spend it: the receipt route at the device's .local name>",
  "tls_cert_fp": "<SHA-256 of the HTTPS certificate the WAP will serve once setup completes — the pin the receipt's https base_url is held to; empty on a build with no HTTPS>",
  "sta_ip":      "<the address it got on your Wi-Fi, for a phone whose .local resolve is slow>",
  "mdns_host":   "<its .local host label>",
  "expires_in_s": 180
}
```

The phone spends it **on the home LAN**:

```
GET /api/provisioning-receipt?claim=<hex>
```

which answers the same receipt the BOOT-tap route serves —
`{device_id, base_url, token}` plus `tls_cert_fp` on a TLS-enabled device —
with `base_url` at the `.local` name (never the SoftAP address, which is dead
once the AP drops). The rules, from `claim_ticket.h`:

- `take()` claims the ticket with one atomic exchange **before** it compares,
  so exactly one caller on any task can get `true`; a second reads 0.
- The compare is constant time over all 32 characters; a presented string
  that is not exactly 32 characters is refused before the compare. The
  phone may upper-case it.
- A match serves the receipt. A **mismatch burns the claim** (one guess per
  join, not thousands). An **expired claim is burned too**. The TTL is
  `claim_ticket::TTL_MS` = 180 000 ms: "the phone spends it within a second
  or two of reading it; three minutes covers a slow `.local` resolve and one
  retry, and is short enough that a claim read by a phone that then walked
  away dies on its own."
- Minting touches the loop task; taking touches the httpd task; the
  minted-at word goes through the `__atomic` builtins exactly as
  `provisioning_gate.h`'s `State` does.

### 3.6 The hub hand-off (Sense, Vision)

A Sense or Vision that takes its Wi-Fi over this door has, at that moment,
no hub: its broker address and login come from NVS the flashers seeded or
from the compiled secrets, and neither family browses for a broker. Until
this hand-off, the "All set" card was followed by a Canary that never
published to Home Assistant, and the only remedy was a USB re-flash with the
Flasher's MQTT fields and a broker password dug out of Mosquitto's
configuration. The phone already holds both halves — it found Home
Assistant over Bonjour, and it minted the broker login it keeps in its hub
secret store — so right after the join it hands them over on the same
encrypted link.

**What crosses.** One WRITE to the companion service's HUB characteristic
(`8fc1cf02`), UTF-8 JSON, at most **240 bytes**:

```json
{"host":"homeassistant.local","port":1883,"user":"canary","pass":"<secret>","tls":false}
```

| Key | | Rule (`hub_handoff.h`) |
|---|---|---|
| `host` | required | 1..63 bytes: a DNS name — letters, digits, dots and hyphens in labels, no leading, trailing or doubled dot, no label starting or ending in a hyphen — or an IPv4 literal (four octets 0..255, no leading zeros). A name of digits and dots alone must be an address. |
| `port` | optional, 1883 | an integer 1..65535 as a plain JSON number (no sign, fraction, exponent or leading zero) |
| `user`, `pass` | optional, together | strings of at most 64 bytes each after unescaping; both present or both absent; the user is never empty, the pass may be |
| `tls` | optional, `false` | a JSON boolean |

The reader is small and strict, as the Improv packet parser is: exactly
these five keys, each at most once (**a duplicate key is refused**, not
"last wins"), the three escapes a password can need (`\"`, `\\`, `\/`) and
no other (`\n`, `\t`, `\uXXXX` are refused), no control byte inside a
string, no value of another type, no unknown key, nothing after the closing
brace but whitespace. A document that fails any of that is refused whole; a
field that fails its rule is refused with its own verdict (host, then port,
then credentials) once the document itself parsed. Every refusal wipes the
record. Host-tested in `firmware/tests_host/test_hub_handoff.cpp`.

**When it is accepted.** Only on an **encrypted link** — the characteristic's
`WRITE_ENC` property, **and** the write handler checks the link's
encryption again, as the RPC write does — **and** only while the Improv
session is **Provisioned** (the device has the Wi-Fi this phone gave it) or
**Provisioning** (a phone that writes a beat before the verdict lands). Any
other time — the door open but no join, the door shut, a stranger who
paired — the write stores nothing, logs one health line, and reads back as
rejected. The write callback runs on the NimBLE host task and only copies
the document into a mailbox (release/acquire, exactly like the RPC
command's); the loop task parses and applies it in `tick()`.

**What "stored" means.** The glue hands the parsed record to the board
(`Identity::store_hub`), which persists it through `runtime_config`'s
`set_mqtt_credentials()` — the same NVS keys the flashers seed
(`mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass`), with the same
readback-durability check the Wi-Fi credentials get — and then points the
MQTT manager at it: `mqtt_init()` is re-run, which re-reads the cached
config (PubSubClient keeps the host *pointer* into that cache and copies the
port; the broker transport re-reads its socket mode), and the connect
supervisor's backoff is reset so its next attempt dials the new broker at
once. The one case that path cannot absorb is a live session to a
*previous* broker (an installed witness re-pointed through the tap door on
the same network — nothing in `mqtt_mgr` hangs up); that case restarts the
unit ~25 s later, after the Improv link has lingered its 20 s and the phone
has read its status byte. The `tls` flag: `false` writes the broker socket
mode back to plain, so a plaintext hub named now never meets a TLS mode
flashed earlier; `true` leaves the stored mode alone — the hand-off carries
no certificate, so a unit with a provisioned CA or pin keeps verifying and a
unit without one stays plain and still reports stored (the contract's
"a device that cannot do MQTT-TLS stores false").

**The status byte.** A READ of HUB answers one byte:

| Byte | Meaning |
|---|---|
| `0` | nothing stored through the door this join (reset when a new `WIFI_SETTINGS` is accepted) |
| `1` | stored — durably, by readback — and the MQTT manager re-pointed |
| `2` | rejected: malformed, too long, a bad field, an unencrypted link, no join this session, or a store that would not hold it |
| `3` | stored, but the broker refused the login on the first attempt — **never emitted by Sense or Vision today**: nothing in their MQTT managers reports the CONNACK reason cheaply, so the phone reads `1` and the device's health log carries the refusal |

**Why the secret never reads back.** The document is the characteristic's
written value for the length of one callback: the host task copies it into
the mailbox and immediately sets the value back to the status byte over a
zeroed buffer, so neither a READ nor the attribute's own memory keeps it;
the loop task parses from the mailbox, wipes the mailbox, the parse buffer
and the record, and republishes the status byte. The broker password lives
in NVS and in `runtime_config`'s cache after that — the same two places the
flashers put it — and on the air only once, encrypted, in the direction the
phone chose.

**The WAP.** Its companion service carries CLAIM (§3.5) and not HUB: the
WAP's own setup page still carries its Home Assistant step for now, so the
phone pairs it through the receipt and the owner names the broker there. A
HUB on the WAP is a follow-up, not a claim this page makes.

### 3.7 The HTTP grant order

`GET /api/provisioning-receipt` is decided by the four-grant
`receipt_decide` in `provisioning_gate.h`, in this order and no other:

| # | Question | Answer | Effect on the others |
|---|---|---|---|
| 1 | Is the `Host` foreign (cannot name this device, and the request did not arrive over the SoftAP)? | `403 {"error":"host"}` | The bearer is unread, the claim untouched, the BOOT tap left unspent. |
| 2 | A valid **bearer**? | serve | The claim and the tap are left alone. |
| 3 | A **claim** presented as `?claim=<hex>`? | `take_claim()` — serve on a match | Called at most once; **spends the claim whatever it answers** (a wrong guess burns it). A claim that serves **leaves the BOOT tap unspent**, so the owner's own page load still finds it. |
| 4 | An unspent **BOOT tap**? | `take_gate()` — serve | One atomic exchange; the tap is one consumer across every path. |
| — | none of the above | `403 physical_confirmation_required` | The same body the three-grant route always answered (`build_gate_refusal_json`). |

Every caller the three-grant overload admitted is admitted here too, in the
same order; the claim is inserted between the bearer and the tap and
changes nothing for either. Host-tested in
`firmware/tests_host/test_provisioning_gate.cpp`.

Who asks row 1: the flagship's `auth_gate` answers the Host question ahead
of every handler, so its receipt route runs all four rows. **The WAP sketch
asks no Host question on this route today** (it has no `auth_gate`; its
`/` page checks the Host header only to pick the first-boot wizard), so its
`handle_provisioning_receipt` runs rows 2, 3 and 4 — bearer, claim, tap — in
that order, with the same single-use claim and the same unspent tap. Adding
the Host row to the WAP is a separate change, not a claim this document
makes. On the WAP, row 2 is the bearer in the `Authorization` header or a
dashboard session minted beside a receipt this route served (a BOOT tap's or
a claim's, whose answer already carried the token); a session the landing
page at `/` minted, which any caller that reaches the WAP can get, is not row
2 and falls to rows 3 and 4 like a request with none (sweep F289:
`cv_session_store.h` stores each session's origin when it is minted,
`api_auth.h`'s `api_auth_check_credential_optional()` reads it;
`tests_host/test_receipt_session.cpp`).

---

## 4. The session

### 4.1 States, errors and the advert

| State byte | Name | Meaning | On air |
|---|---|---|---|
| `0x00` | Stopped | the service is registered but not on offer | beacon |
| `0x01` | AwaitingAuthorization | the door is shut; a standard client reads this | beacon only; Sense / Vision not connectable |
| `0x02` | Authorized | the door is open; credentials are accepted | beacon + setup bit + name, connectable; Improv in the scan response |
| `0x03` | Provisioning | credentials received, the join is in flight | same as Authorized |
| `0x04` | Provisioned | the join worked | the link lingers ~20 s for the phone to read the result (and a WAP's claim), then back to the beacon |

| Error byte | Name | When |
|---|---|---|
| `0x00` | None | — |
| `0x01` | InvalidRpc | a packet that is not a packet; a second write mid-join; a write inside the cooldown |
| `0x02` | UnknownRpc | a command this device does not speak |
| `0x03` | UnableToConnect | the join failed, or no verdict within 30 s (`Timing::provisioning_timeout_ms`, the portal wizard's own timeout, so both doors agree) |
| `0x04` | NotAuthorized | credentials sent while the door was shut, or the attempt past the cap |
| `0x05` | BadHostname | — |
| `0xFF` | Unknown | — |

### 4.2 Transitions

| Event | Result |
|---|---|
| `begin`, no credentials, window > 0 | door `NoCredentials`, state Authorized, the window starts |
| `begin`, credentials stored | door Shut, state AwaitingAuthorization |
| `tap` (Sense, Vision) | door `Tap` for 60 s, state Authorized unless Provisioning; the per-door counters reset. **No effect while the no-credentials door is already open.** |
| the window elapses (30 min) | door Shut, `window_spent`; no reopening without a tap or a power cycle (a software restart carries the spent time in RTC memory) |
| the tap TTL elapses (60 s) | door Shut |
| `WIFI_SETTINGS`, door shut | `NotAuthorized`, nothing started |
| `WIFI_SETTINGS`, mid-join | `InvalidRpc`; the join in flight is not disturbed |
| `WIFI_SETTINGS` inside 3 s of the last accepted one | `InvalidRpc`, **not counted**, nothing started |
| `WIFI_SETTINGS`, the 11th attempt on this door | door Shut (a no-credentials door marks its window spent), `NotAuthorized` |
| `WIFI_SETTINGS` accepted | state Provisioning; the glue starts the board's own join path |
| a `WIFI_SETTINGS` that did not parse | `InvalidRpc`, and it **counts against the cap** |
| the join succeeded | state Provisioned; credentials persist; the no-credentials door shuts (the state stays Provisioned for the phone to read) |
| the join failed | back to Authorized if the door is still open (the person can retry with the right password), else AwaitingAuthorization; `UnableToConnect` |
| credentials vanish at runtime (factory reset) | the window is re-armed from now — the owner's own act |
| a connected client silent for 3 min | dropped (never mid-join: the verdict is owed to it) |
| the join is done and ~20 s have passed | the link is dropped; the device goes back to its beacon |

All time math is wrap-safe unsigned delta (`now - then`), the firmware's
`millis()` idiom; a clock reading of exactly 0 is stored as 1 so 0 can stay
the "never" sentinel.

### 4.3 The join itself

Credentials from the door go into the **same join path** the SoftAP wizard's
`POST /join` uses — on Sense and Vision `setup_portal_submit_join()` (one
Testing pass, persist only on success, the same linger, the same teardown);
on the WAP the sketch's own connect path, through `ble_improv_submit_join`
and `ble_improv_join_verdict`. The WAP's other provisioning paths (its
wizard, the QR scan, the bonded rescue) persist credentials first and
connect second; the door cannot, because one wrong password would mark the
unit "configured", shut the door on the next pass and refuse the retry. So
a door join on the WAP lives in RAM until WiFi reports it worked, the door
counts the unit as credential-less while it is pending, a proven join is
persisted then, and a failed or timed-out one is forgotten — the door stays
open for the person to try again. The door reimplements no join logic; it
only reports the verdict back over the result characteristic
(`setup_portal_join_state()` / `setup_portal_note_acked()` on the shared
portal).

---

## 5. Per family

| | Canary Sense | Canary Vision | Canary WAP | `firmware/canary` (flagship) | Canary Display |
|---|---|---|---|---|---|
| Door | ✅ | ✅ | ✅ FULL profile | ❌ follow-up | ➖ glass QR instead |
| Stack | NimBLE-Arduino 2.x (core 3.x) | NimBLE-Arduino 1.4.x (core 2.x) | the pairing channel's NimBLE server | — | — |
| Flag | `FEATURE_IMPROV` in `include/canary/config.h` (default 1; `-DFEATURE_IMPROV=0` compiles it out) | same | `FEATURE_IMPROV` = `FEATURE_BLUETOOTH && FEATURE_BLE` in `build_config.h` (FULL; 0 in DEV and MINIMAL) | — | — |
| First-boot window | 30 min (`IMPROV_FIRST_BOOT_WINDOW_MS`) | 30 min | 30 min | — | — |
| Tap door | ✅ 60 s, `BOOT_BUTTON_PIN` from the board's `pins.h` (XIAO ESP32-C6: GPIO9); on an installed witness the tap raises the setup portal under the door for the minute | ✅ 60 s (XIAO ESP32-C3 hosts: GPIO9; XIAO ESP32-S3: GPIO0); same | ❌ none in this version (a short BOOT press stays the receipt gate; Forget Wi-Fi re-arms the window) | — | — |
| Window across a software restart | the glue's RTC record (`improv_ble.cpp`) | same | the sketch's RTC record (`provisioning_logic::door_window_record_valid`); door activity restarts the first-boot wizard's 15-minute idle timer | — | — |
| Network list (`GET_WIFI_NETWORKS`) | ✅ the glue's own scan | ✅ same | ✅ the sketch's scan cache — the wizard's picker's list, under one lock | — | — |
| Connectable with the door shut | no | no | yes (the channel's bonded services) | — | — |
| After the join | `""` in the result (no page to open); the device appears on the LAN by mDNS and MQTT | same | the claim ticket → the receipt over Wi-Fi → **paired**; the post-join reboot into steady state waits while the claim is outstanding | — | — |
| Identify | blinks the LED | blinks the LED | the sketch's identify | — | — |
| Hub hand-off | ✅ | ✅ | ➖ not yet (its own page) | — | — |

**Sense.** `improv_ble::begin` after the beacon, `tick` beside it every loop
pass; the beacon carrier hands its bytes to `improv_ble::advertise()` instead
of touching NimBLE itself. Landing at ~90% of the OTA slot on the C6 is the
cost of being connectable (the controller's connection code).

**Vision.** The same module on the 1.4.x stack (the `ESP_ARDUINO_VERSION_MAJOR`
split mirrors `fleet_beacon_adv.cpp`). The C3 envs moved to `min_spiffs.csv`
(0x1E0000 app slots) for it — a partition-table change cannot ship over OTA,
so a unit flashed on `default.csv` needs one USB reflash.

**WAP.** `ble_improv` on the pairing channel's server, with a staged
byte-identical copy of the core beside the Arduino sketch
(`firmware/scripts/check_improv_sync.sh`). Opera composes the advert set:
the setup bit, the `WAP-XXXX` name and the Improv scan response while the
door is open, its own scan response otherwise. A fresh unit with nobody on
its SoftAP brings the pairing channel up **5 s** after boot instead of the
five-minute hold (`provisioning_logic::ble_fresh_unit_start_due`,
host-tested), and holds the BLE scanners (Nearby, Scout) on that path — a
scanning radio would fight the one link the door needs — until the SoftAP
drops after a join or the five-minute max hold
(`provisioning_logic::ap_only_for_discovery`: a fresh unit's AP-only *boot
state* is not the standalone AP-only *choice*, so the 45 s settle that
releases a disconnected unit's scanners never fires under a wizard phone's
handshake); the heap guard keeps the last word. The first-boot window rides
across the wizard's 15-minute idle restart in RTC-noinit memory
(`g_improv_window_*`, honored only when `esp_reset_reason()` is not a
power-on, brownout, power-glitch, USB / JTAG-asserted or unknown reset —
`door_window_record_valid`, host-tested; zeroed by Forget Wi-Fi on a unit
that had credentials), and every command, join and claim at
the door is a sign of life for that wizard timer (`setup_wizard::touch`), so
the restart cannot land mid-provisioning. `GET_WIFI_NETWORKS` streams the
sketch's scan cache — the same list the wizard's `GET /api/wifi/scan` serves,
behind one mutex, so the single radio is swept once for both and a sweep
already running is shared; a request during a join answers the empty list at
once (a sweep under `WiFi.begin()` can fail it). A door join persists only
once proven (§4), and a path that persisted credentials meanwhile (the
wizard, a QR scan, the bonded rescue) owns them: the door's later verdict
changes nothing, and the door's link gets no claim and no URL for a join it
did not make — it is told NotAuthorized (the unit has an owner now). The
join path drops a leftover sweep under the scan-cache lock, so the door's
harvest never reads a freed result set. The post-join reboot into steady state is held while a
claim is outstanding (the claim promised 180 s; the reboot came at 120 s)
and gets a 10 s runway once it is spent or expires. `set_setup_door` asks
the stack about **every** link it holds before applying Just Works, not the
connection card alone (a bonded phone connected first and a second peer
after it left the card on the newcomer). The profile swap and the
`*_AUTHEN` refusals are §3.4; the claim is §3.5.

**The flagship `firmware/canary` build.** Not in this change. Its
`securacv_network` keeps the SoftAP wizard and the BOOT-tap receipt; the door
and the claim are a follow-up (`claim_ticket.h` says so in its header;
tracked as F251 in the [sweep backlog](../audit/repo_todo_sweep_2026-09.md)). The
iOS name grammar already accepts a `Canary-XXXX` name and treats it as a
WAP-class device, so the app needs no change when it lands.

**The displays.** No door. A screen beats a radio: the glass draws a `WIFI:`
QR for its own setup network, and the phone joins it. The display's roster
scan does read `FLEET_BEACON_FLAG_SETUP_OPEN` from its siblings' beacons
(a parser that predates the bit ignores it like any other flag).

---

## 6. The iOS flow

The pure policy is `ios/Shared/` and `ios/Sources/SecuraCV/Model/`
(Foundation only, tested); the glue is the app's `Transport/` and
`Security/`. The bar is AirPods: bring a powered-on Canary near the phone →
one card slides up → one tap → Wi-Fi asked for at most once → a success
card that names this device.

0. **No prompt before its reason.** `BLEConsole` creates no
   `CBCentralManager` until the first scan or setup tap — creating one is
   what raises iOS's Bluetooth alert — and the scan starts only after the
   discovery consent. On a fresh install Today itself asks ("Find my
   Canary", `FirstRunCard`), and that tap raises only iOS's Bluetooth
   prompt: the mDNS browse, and the Local Network prompt with it, waits for
   its first real consumer — the walkthrough's "Watch it appear" row, the
   Fleet tab with a real fleet, or a Canary heard announcing it just joined
   (`FleetStore.startLocalNetworkDiscoveryIfConsented`).
1. **Hear it.** `BLEConsole` collects, per peripheral, the beacon (with its
   setup bit), the local name and the scan response's Improv UUID + service
   data. `NearbyCanaries.candidates` decides what qualifies: heard within
   15 s, **and** the Improv service with a state that accepts credentials
   (Authorized or Provisioning) — or, for a sighting whose service data was
   not caught this time, the beacon's setup bit. The name grammar is strict
   (`Sense-K7MZ`, `Vision-K7MZ`, `WAP-AB12`: four upper-case letters or
   digits — a Sense or Vision upper-cases its pseudonym, which is drawn from
   the no-confusion alphabet, not hex; a WAP's are hex). A device that is
   unambiguously one already paired is not "new"; a dismissed suffix stays
   quiet.
2. **The card comes to you.** When exactly one candidate is "close by"
   (−70 dBm or stronger) and has not been offered this session
   (`NearbyCanaries.autoOffer`), one card slides up over whatever screen is
   showing (`NearbyOfferOverlay`): the device's figure, its name,
   **Continue**, **Not now**. Two on the table stay a choice in the inline
   card — on Today, Fleet (comb and list), the "+" sheet and the first
   section of the Set up walkthrough, which listens before it asks which
   family it is (the advert already says). Strongest signal first. While
   the "+" sheet is up the card stands down — it would only show dimmed
   behind the sheet's half-height detent, offering what the sheet already
   lists — and nothing is counted as offered behind it; a card already up
   comes back when the sheet closes (`NearbyOfferLatch`).
3. **Tap → connect → the pairing sheet → pick.** `ImprovClient` connects,
   discovers the service, subscribes to state / error / result, reads the
   state, and asks `GET_WIFI_NETWORKS`. That first write to the command
   characteristic needs an encrypted link, so iOS shows its one-tap pairing
   sheet *now* — the screen says "Tap Pair when iOS asks" at this step, and
   a declined pairing is named (`ImprovClient.pairingDeclined`) instead of
   showing as an empty list. CoreBluetooth pairs and retries the write on
   its own. Nothing is bonded, so the next Canary shows the sheet again —
   that is the design. The person picks from the list the Canary sent; the
   remembered household Wi-Fi is preselected when it is in that list.
   Before sending, the likely failure is named: a network not in the
   Canary's list (2.4 GHz only), a password under 8 characters.
4. **The Canary's own verdict.** The app waits on the state and error
   characteristics: Provisioned, or `UnableToConnect` with both real causes
   (the password, or a network the board cannot see). `NotAuthorized`
   reads "This Canary isn't accepting a new network right now…" with the
   way back in (a BOOT tap on a Sense or Vision, a power cycle on a new
   one).
5. **Remember the Wi-Fi — only after it worked.** `HouseholdWiFi` is written
   to the **device-only Keychain** (`ThisDeviceOnly`; never iCloud Keychain,
   never CloudKit) only with the toggle on and only after the Canary itself
   said it joined (`shouldRemember(toggleOn:joined:ssid:)`). Every Wi-Fi
   form opens with it (the card and the setup network), "Update fleet
   Wi-Fi" replaces it once a Canary proved the new password, "Forget" on
   the Set up screen clears it. The second Canary is two taps.
6. **A WAP: the claim and the receipt.** `MagicPairPlan` sequences the rest:
   read the CLAIM characteristic over the same link, then, on the home LAN,
   `GET` the receipt at `claim_url` (the `.local` name; `sta_ip` as the
   fallback), pinning `tls_cert_fp` when present. The receipt lands in
   `DeviceStore` and the WAP is **paired**.
7. **The success card names this unit.** "<title> is on <network>", then
   the network watch ticks only the device whose mDNS host's last label
   starts with the same characters as its advert name or its setup network
   (`NearbyCanaries.isSameDevice`) — never any device of the same family —
   and ends in words after 90 s rather than spinning.

Fallbacks, each one named on screen: no card → the setup network (the key
the Flasher shows on its card after the flash, read from the Canary's own
first-boot `[SETUP]` line — the phone asks the portal's `GET /scan` for the
Canary's own network list), a display's glass QR (the camera is asked for
at the tap; a good scan starts the join), or a WAP's own setup page;
"not accepting" → tap BOOT, or power-cycle; a radio that is off or not
allowed → the reason and **Open Settings**, never a spinner. On a WAP's
claim: nothing answered (the phone off Wi-Fi, or the `.local` name not yet
settled) → the claim is unspent, so **Try again** while it lives (180 s);
a `403` or any other answer → the device's row under **Ready to pair** on
the Fleet tab, which takes its recovery kit or a fresh setup. The kit is
the receipt the WAP's own pages save: the setup wizard's close-out after a
BOOT tap, and since #1802 its dashboard (F257), so a WAP set up through
this door, which never runs the wizard's close-out, has a kit to save. On
the dashboard (`https://canary-<suffix>.local/settings` once set up; port
80 redirects there) a browser that is signed out chooses **Use BOOT Tap
Once**, and the tap that signs it in offers **Save my recovery kit** in the
wizard's words; one signed in by a tap or a claim (their session cookie) or
by a token saves it from **Settings**, **Device**, **Recovery kit** without a
tap. One that only opened the landing page at `/` runs the dashboard on the
landing's session, which the receipt does not take (F289): its Settings save
brings the sign-in back, and **Use BOOT Tap Once** offers the kit. A kit
saved on the setup network names the
SoftAP address; one saved over the home network names the `.local` name
(F255: the receipt, and port 80's redirect, name the SoftAP address only
for a request that arrived over the SoftAP, or while the STA holds no
address). Neither lasts: the SoftAP is gone once the WAP is on your Wi-Fi,
and the `.local` name lasts only as long as the WAP's name — naming it
later (the wizard, or `POST /api/device-name`) changes the host. So the
app moves a kit whose `device_id` is the row's onto the address the WAP
answers at now, whatever host the kit names, keeping its scheme, port and
pin (`ProvisioningReceipt.rebased`, A88); a kit with no `device_id` moves
only off the SoftAP address, and another device's kit keeps the host it
names. A kit saved before a rename is dialed at the new name. "The row's
id" is a 16-bit suffix of the WAP's key fingerprint, so two WAPs in one
home can share one, and then one's kit moves onto the other (the rebase's
comment says what follows). The app has no BOOT-tap
pairing over the LAN: an installed WAP speaks https with no pin the phone
could hold it to before the first dial (F256).

Tests: `ImprovWireTests` (the codec, pinned to the firmware's vectors),
`MagicPairPlanTests` (the sequence, its fallbacks and the retry rule),
`NearbyCanaryTests` (which sightings qualify, which one is offered, which
unit joined), `SetupGuideTests` (the words the screen shows, twenty words
up front), `SetupPortalTests` (the scan list and the next step for every
verdict), `AddCanaryFlowTests` (no prompt at launch, the first-run card,
the radio and camera words), `DeviceAPITests` (which recovery kit moves
onto the WAP it was opened for, and which stays as it is).

---

## 7. Home Assistant and other clients

Because the door is the standard, **the Home Assistant companion app and the
improv-wifi web SDK can provision Wi-Fi through it** — they get exactly what
a standard device offers: the network list, the credentials write, the
verdict and the URLs. They do **not** get a SecuraCV pairing token: the
companion service is ours, and a client that does not know it never sees it.
A Sense or Vision set up this way appears in Home Assistant over MQTT like
any other.

Home Assistant's own **Improv via BLE** integration needs a Bluetooth
radio on the Home Assistant host to reach the device. Through an **ESPHome
Bluetooth proxy it cannot complete the Just Works pairing** — the proxy
forwards GATT, not a pairing — so a device seen that way shows and then
fails at the first write. Expected, and **untested**. The note in
[`homeassistant_setup.md`](../homeassistant_setup.md) says the same thing to
a user.

---

## 8. Residual risks (stated, not waved away)

- **The first-boot window is a window.** For 30 minutes after a new unit's
  power-on, anyone in radio range with a standard client can hand it a
  network. The bounds (§2.2) keep that from being a denial-of-service on the
  owner, and a unit joined to the wrong network is a unit its owner holds,
  not one that has been taken; the fix is a factory reset. A deployment
  that wants no window at all compiles `IMPROV_FIRST_BOOT_WINDOW_MS` to 0
  (tap-only) — on a WAP, which has no tap door yet, that means no door.
- **Just Works resists a passive listener, not an active one who paired
  first.** The claim's second factor (LAN presence) and the `*_AUTHEN`
  refusals are the mitigations; the bearer never rides the link.
- **The tap is a GPIO read with a debounce** (`short_tap.h`: at least 40 ms
  down, up before 700 ms). A board whose BOOT pin is also a strapping pin
  (`PIN_BUDGET.md` marks them) could, in principle, see a tap during a
  reset; the tap opens a 60 s door, not a credential, and the bounds still
  apply.
- **A standard client cannot tell our door from an ordinary Improv device**
  — by design. It also means the standard's own weaknesses (no
  authentication beyond the device's rule) are ours; the rule is the
  mitigation.
- **Radio coexistence** on the C3 / C6 of one BLE link beside a SoftAP and
  a candidate join is unproven on a bench (§9).
- **The display never opens a door**, which also means a display can only
  ever be set up by its glass; that is the accepted trade.

---

## 9. Bench checklist (what to verify before calling it shipped)

Nothing below has happened yet. Each line is a hardware fact the host tests
cannot speak to.

**Every family**

- [ ] A fresh unit shows the card on an iPhone within seconds of power-on;
      the beacon's bit 5 is set and the short name reads `<Family>-XXXX`
      with the setup network's four characters.
- [ ] A standard client (nRF Connect, the improv-wifi web SDK) sees the
      Improv service in the scan response and reads `0x02` while the door
      is open, `0x01` after it shuts.
- [ ] The first write to `...8003` raises the iOS pairing sheet; one tap
      pairs; the write lands. **On NimBLE 1.4.x (Vision) the sheet's timing
      is the open question** — confirm the retried write is not refused.
- [ ] Decline (or ignore) the pairing sheet at the network scan: the card
      says pairing was declined and offers a typed name — never a silently
      empty list — and the next Pair tap lets the join through.
- [ ] A fresh install launches with no Bluetooth alert; the alert appears
      only after "Find my Canary" on Today, and the card slides up by itself
      within seconds of a Canary powering on on the table (and not for one
      in the next room — the −70 dBm band).
- [ ] A write without encryption (a client that declines to pair) is
      refused by the stack **and** by the handler.
- [ ] The join succeeds with the right password; the state reads
      Provisioned; the link drops after ~20 s; the beacon is back to its
      steady bytes and (Sense, Vision) the device is no longer connectable.
- [ ] The wrong password answers `UnableToConnect` and the door stays open
      for a retry.
- [ ] **A saved network failing (power the router off) raises the SoftAP
      portal and does NOT reopen the door.**
- [ ] The window: 30 minutes after power-on with no credentials, the door
      shuts on its own; a power cycle reopens it; a factory reset reopens it;
      a software restart (the WAP's 15-minute wizard restart, `ESP.restart()`
      from a route) does NOT — the door stays shut or keeps its remaining
      time.
- [ ] Sense / Vision with a network: a BOOT tap raises `SecuraCV-XXXX` beside
      the door, the phone's credentials land through it, and with no join
      the portal is lowered when the minute ends and the saved link is back.
- [ ] The cap: the 11th `WIFI_SETTINGS` on one door shuts it; the cooldown:
      two writes 1 s apart earn `InvalidRpc` for the second and start nothing.
- [ ] The idle bound: a connected client that sends nothing is dropped at
      3 minutes, and not mid-join.
- [ ] The Home Assistant companion app provisions Wi-Fi through the door;
      through an ESPHome Bluetooth proxy the pairing fails as expected.

**Sense (XIAO ESP32-C6, NimBLE 2.x)**

- [ ] `BOOT_BUTTON_PIN` = GPIO9 reads a short tap (40–700 ms) and opens the
      door for 60 s on a unit that already has Wi-Fi; a 2 s hold does not.
- [ ] One BLE link, the SoftAP and a candidate join coexist on the C6
      radio for the length of a provisioning.
- [ ] The OTA slot headroom at ~90% is real on the shipped partition table.

**Vision (XIAO ESP32-C3 hosts GPIO9; XIAO ESP32-S3 GPIO0; NimBLE 1.4.x)**

- [ ] The tap on each host board; the S3's GPIO0 is a strapping pin —
      confirm a tap at runtime does not reset.
- [ ] A unit flashed on `default.csv` takes the one USB reflash to
      `min_spiffs.csv` and then OTAs normally.

**WAP (FULL profile)**

- [ ] The pairing channel is up 5 s after a fresh boot with nobody on the
      SoftAP; the scanners are held on that path — still held at 60 s with
      the SoftAP up, released once the AP drops after a join; the heap
      guard's verdict.
- [ ] A fresh WAP left plugged in: the door shuts 30 minutes after power-on
      and stays shut through the wizard's 15-minute restarts (the serial
      banner says how much of the window the reset carried); a phone at the
      door during minute 14 does not get restarted under; pulling the plug
      reopens the door; Forget Wi-Fi reopens it.
- [ ] The picker: `GET_WIFI_NETWORKS` lists the networks the wizard's own
      picker lists; asked during a join it answers the empty list at once.
- [ ] The security profile swaps to Just Works only with the door open; it
      is refused while a Numeric Comparison is pending or a bonded link is
      up; it is restored when the door shuts.
- [ ] With the Just Works key, the console, OTA, witness export and bonded
      provisioning characteristics refuse.
- [ ] After the join: the CLAIM read works on the provisioning link (the
      whole ~300-byte value, blob continuations included); a read more than
      3 s after the first is empty; a read from another link is refused.
- [ ] `GET /api/provisioning-receipt?claim=<hex>` at the `.local` name
      serves the receipt with `base_url` at that name; the same claim a
      second time is `403`; a wrong claim is `403` **and** burns the real
      one; a claim after 180 s is `403`; the BOOT tap is still unspent after
      a claim served.
- [ ] With the WAP joined to the home Wi-Fi, a BOOT-tap receipt fetched
      from a laptop on the home network (the dashboard's sign-in at
      `https://canary-<suffix>.local/settings`) names `base_url` at
      `https://canary-<suffix>.local`; the same tap from a phone still on
      the setup network inside the AP grace window names `192.168.4.1`; a
      bearer re-sync over the home network names the `.local` name; and
      `http://canary-<suffix>.local/settings` answers `301` to
      `https://canary-<suffix>.local/settings`, before and after the AP
      drops.
- [ ] The dashboard's BOOT-tap sign-in offers **Save my recovery kit**,
      and a dashboard signed in by that tap saves it from **Settings**,
      **Device**, **Recovery kit**; the saved `canary-recovery-kit.json` is
      the receipt, and it pairs the WAP from its row under **Ready to pair**
      in the iPhone app; a sign-in whose tap never came offers nothing.
- [ ] From a second host on the home network that only opened the landing
      page at `/` (its "Open dashboard" link), `GET
      /api/provisioning-receipt` answers the BOOT-tap refusal (`403`) and
      `GET /api/pairing-qr` answers `401`, neither carrying the token or
      the AP password; after a BOOT tap the same browser's receipt fetch
      serves, and the session that tap set serves both again with no tap
      (F289).
- [ ] The iPhone ends up paired from one tap; the `tls_cert_fp` pin holds
      on a TLS-enabled unit; a phone that takes three minutes to spend the
      claim still finds the WAP up (the reboot is held while the claim is
      outstanding) and the reboot follows ~10 s after the receipt.
- [ ] Opera's scan response is back after the door shuts.

**iOS**

- [ ] The card on Today, Fleet and the walkthrough; dismissal; the
      paired-and-unambiguous exclusion with two devices sharing a suffix.
- [ ] With the card up, the Fleet tab's "+" sheet hides it (nothing dimmed
      behind the half-height sheet, on an iPhone and on an iPad) and lists
      the Canary inline; closing the sheet brings the card back; a Canary
      first heard while the sheet is up is offered once it closes. On an
      iPad with two windows the stand-down is app-wide (one flag in
      `FleetStore`): a "+" sheet in one window hides the card in both, and
      with a "+" sheet up in each, closing one brings the card back
      behind the other; note what the bench sees.
- [ ] The Keychain item is written only after a proven join, never on a
      failure; Forget clears it; it is absent from iCloud Keychain.
- [ ] The `403`-on-claim path lands on the WAP's row under Ready to pair,
      and its recovery kit (saved before it joined) pairs it there.
- [ ] A kit saved from the dashboard over the home network, then the WAP
      named (`POST /api/device-name`, or the wizard's name), pairs it from
      its row at the new `.local` name, the pin held on a TLS-enabled
      unit; another WAP's kit opened from that row is not moved onto it.
- [ ] With the phone on cellular at the claim, the card names it and Try
      again pairs once the phone is back on the home Wi-Fi (inside 180 s).
- [ ] With an older Vision already on the network, the success card does
      not tick it for a new Vision; only the new unit's host is ticked.
- [ ] Bluetooth off / Bluetooth denied / Local Network denied: each setup
      screen names the cause (and offers Settings where the fix is there)
      instead of "Listening…".
- [ ] On the Sense / Vision walkthrough, the hub login copied on the
      iPhone pastes into the Flasher's MQTT fields on a Mac over Universal
      Clipboard, and is gone from the clipboard after its two minutes.

---

## 10. Where the code is

| What | Where | Tested by |
|---|---|---|
| The wire, the session, the rules (pure) | [`firmware/common/network/improv_core.h`](../../firmware/common/network/improv_core.h) | `firmware/tests_host/test_improv_core.cpp` |
| The NimBLE glue, both majors (Sense, Vision) | [`firmware/common/network/improv_ble.h`](../../firmware/common/network/improv_ble.h), `improv_ble.cpp` | CI-compiled; bench |
| The short BOOT tap (pure) | [`firmware/common/io/short_tap.h`](../../firmware/common/io/short_tap.h) | `firmware/tests_host/test_short_tap.cpp` |
| The claim ticket (pure) | [`firmware/common/network/claim_ticket.h`](../../firmware/common/network/claim_ticket.h) | `firmware/tests_host/test_claim_ticket.cpp` |
| The hub hand-off's document and status byte (pure) | [`firmware/common/network/hub_handoff.h`](../../firmware/common/network/hub_handoff.h) | `firmware/tests_host/test_hub_handoff.cpp` |
| The hub hand-off's store (Sense, Vision) | `include/canary/runtime_config.h` (`set_mqtt_credentials`), the `store_hub` hook in each `main.cpp` | CI-compiled; bench |
| The receipt's four-grant order | [`firmware/common/network/provisioning_gate.h`](../../firmware/common/network/provisioning_gate.h) (`receipt_decide`) | `firmware/tests_host/test_provisioning_gate.cpp` |
| Which address the WAP's receipt and port-80 redirect name (pure) | [`firmware/projects/canary-wap/arduino/canary_wap/receipt_authority.h`](../../firmware/projects/canary-wap/arduino/canary_wap/receipt_authority.h) | `firmware/projects/canary-wap/tests_host/test_receipt_authority.cpp` |
| Which sessions stand in for the WAP's bearer on the receipt and the pairing QR: a BOOT tap's or a claim's, never the landing page's (pure store; F289) | [`firmware/projects/canary-wap/arduino/canary_wap/cv_session_store.h`](../../firmware/projects/canary-wap/arduino/canary_wap/cv_session_store.h), `api_auth.h` (`api_auth_check_credential*`) | `firmware/projects/canary-wap/tests_host/test_receipt_session.cpp`; `check_route_security.py` |
| The WAP's recovery kit, one builder for the wizard and the dashboard (BOOT-tap offer and Settings) | `RECOVERY_KIT` in `companion_pwa.h` and `web_ui.h` (same lines) | `firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js` |
| The beacon's setup bit | [`firmware/common/fleet_link/fleet_beacon.h`](../../firmware/common/fleet_link/fleet_beacon.h) (`FLEET_BEACON_FLAG_SETUP_OPEN`) | the beacon tests |
| Sense / Vision flag and window | `include/canary/config.h` (`FEATURE_IMPROV`, `IMPROV_FIRST_BOOT_WINDOW_MS`) | the per-board `-DFEATURE_IMPROV=0` CI veto |
| The WAP's glue, advert set, claim service | [`firmware/projects/canary-wap/arduino/canary_wap/ble_improv.h`](../../firmware/projects/canary-wap/arduino/canary_wap/ble_improv.h), `build_config.h` (`FEATURE_IMPROV`), `bluetooth_channel::set_setup_door` | CI-compiled; `check_improv_sync.sh` holds the staged core |
| The phone's codec (pure) | [`ios/Shared/ImprovWire.swift`](../../ios/Shared/ImprovWire.swift) | `ImprovWireTests` |
| Which sightings become the card (pure) | [`ios/Shared/NearbyCanary.swift`](../../ios/Shared/NearbyCanary.swift) | `NearbyCanaryTests` |
| The walkthrough's words (pure) | [`ios/Shared/SetupGuide.swift`](../../ios/Shared/SetupGuide.swift) | `SetupGuideTests` |
| The CoreBluetooth glue | `ios/Sources/SecuraCV/Transport/ImprovClient.swift` | bench |
| The WAP sequence: claim → receipt → paired (pure) | [`ios/Shared/MagicPairPlan.swift`](../../ios/Shared/MagicPairPlan.swift) | `MagicPairPlanTests` |
| Which recovery kit moves onto the WAP it was opened for (pure) | `ProvisioningReceipt.rebased` in `ios/Sources/SecuraCV/Transport/DeviceAPI.swift` | `DeviceAPITests`; `PairView`'s call, as source: [`scripts/tests/test_ios_setup_glue.py`](../../scripts/tests/test_ios_setup_glue.py) |
| The remembered Wi-Fi (device-only Keychain) | `ios/Sources/SecuraCV/Security/HouseholdWiFi.swift` | — |
| The card | `ios/Sources/SecuraCV/Views/Components/NearbyCanaryCard.swift` | its latch (`NearbyOfferLatch`, pure): `NearbyCanaryTests`; the "+" sheet's stand-down wiring, as source: `test_ios_setup_glue.py` |
| The lessons | [`firmware/LESSONS_LEARNED.md`](../../firmware/LESSONS_LEARNED.md) (three entries dated 2026-10-07) | — |
| The parity row | [`firmware/FEATURES.md`](../../firmware/FEATURES.md) | the dashboard guard |

Related design: [one onboarding, every board](onboarding_shared_module.md)
(the SoftAP portal the door sits beside); [the iPhone companion app](iphone_companion_app.md);
[device trust](../device_trust.md) (what a Just Works link may touch on a
WAP); [the BLE protocol](../ble_protocol.md) (the UUID registry).
