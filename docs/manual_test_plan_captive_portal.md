# Manual Test Plan: Canary WAP Captive Portal / `canary.local` Onboarding

## Purpose
Validate that a phone joining the Canary's `SecuraCV-XXXX` AP **stays
connected** (never flagged "no internet") and can reach the dashboard via
`canary.local` or the `192.168.4.1` fallback — across iOS/macOS, Android, and
Windows, during first-boot setup, in steady state after provisioning, and over
home Wi-Fi (STA) once joined.

This is the on-device sign-off for the captive-portal work landed in #611,
#622, and #626. The pure response logic is covered by host unit tests — the
DNS redirector by `tests_host/test_captive_dns.cpp` and the per-platform probe
policy (Apple page / Android 204 / Windows NCSI body) by
`tests_host/test_captive_probe.cpp`, both run by the firmware host-tests CI
job; this plan covers the parts only real hardware and real OS supplicants can
exercise.

## How the firmware behaves (reference)
- **AP SSID** is `SecuraCV-XXXX` (last 4 hex of the MAC) in both setup and
  steady state. The softAP is always on (AP+STA) and does **not** NAT.
- **Transport**: the dashboard / setup wizard / provisioning flow is served
  over **HTTPS on port 443** (TLS, self-signed cert generated on-device).
  Plain **HTTP port 80 returns a 301 redirect to HTTPS** for everything
  *except* the OS connectivity-probe paths below, which are answered in the
  clear so the supplicant's probe succeeds. So a browser pointed at
  `canary.local` / `192.168.4.1` lands on `http://` → 301 → `https://`, and
  the first visit shows a self-signed-cert warning that the tester accepts.
  (`wap_server.h`: `HTTPS_PORT = 443`, `HTTP_PORT = 80` → "redirect to HTTPS".)
- **Per-platform connectivity probes** (always on plain HTTP, port 80, **not**
  redirected):
  - Apple `/hotspot-detect.html`, `/library/test/success.html` → instruction
    HTML (pops the Captive Network Assistant sheet; iOS/macOS keep the
    association while it's open).
  - Android `/generate_204`, `/gen_204` → **HTTP 204 No Content** (no sheet,
    stays connected, no cellular fallback).
  - Windows `/connecttest.txt`, `/ncsi.txt` → `Microsoft Connect Test` /
    `Microsoft NCSI`.
- **Captive DNS redirector** runs for the whole life of the AP. It answers
  `A` queries with `192.168.4.1` and returns NODATA (NOERROR, ANCOUNT=0) for
  `AAAA`/`HTTPS`/other QTYPEs. Truncated/malformed queries are dropped.
- **mDNS**: a unique `canary-<name>.local` (or `canary-<mac>.local`) plus a
  first-wins delegated `canary.local` catch-all (AP always; home LAN when a
  single Canary is present).

## Preconditions
- A flashed Canary WAP (XIAO ESP32-S3) on current `main`.
- The AP password from the device's setup card.
- One device of each platform under test: an iPhone/iPad (or Mac), an Android
  phone (Chrome), and a Windows laptop.
- For the home-Wi-Fi steps: a 2.4 GHz home network the Canary can join, with
  only **one** Canary on it (so the `canary.local` catch-all is unambiguous).

---

## Test Steps

### A) First-boot setup — phone stays connected

#### A1) Apple (iOS/macOS)
**Action**
- Factory-fresh device (or after NVS `setup_ok` cleared). Join `SecuraCV-XXXX`.

**Expected**
- A "Sign in to network" sheet appears showing the instruction page.
- Wi-Fi does **not** drop and is **not** marked "No Internet Connection"
  while the sheet is open.
- Opening Safari → `canary.local` redirects to HTTPS (`https://canary.local`,
  301 from port 80) and, after accepting the self-signed-cert warning, loads
  the setup/dashboard. `https://192.168.4.1` also loads.

#### A2) Android (Chrome)
**Action**
- Join `SecuraCV-XXXX`.

**Expected**
- **No** captive sign-in sheet (this is correct — the `204` is what keeps
  Android connected).
- Status bar shows connected; **no** "Wi-Fi has no internet" warning, and the
  phone does **not** silently switch back to cellular.
- In Chrome, `canary.local` resolves and loads **promptly** (no multi-second
  stall from AAAA/HTTPS probes), redirecting to HTTPS (accept the self-signed
  cert). If `.local` fails on the device's Android version,
  `https://192.168.4.1` loads immediately.

#### A3) Windows
**Action**
- Join `SecuraCV-XXXX`.

**Expected**
- Network shows connected (may show limited connectivity, but not flapping).
- `https://192.168.4.1` loads (port 80 → 301 → HTTPS; accept the self-signed
  cert); `canary.local` loads if the host supports mDNS.

### B) Complete provisioning
**Action**
- From the dashboard, enter home Wi-Fi credentials and submit.

**Expected**
- Device reports success and reboots. On reboot it joins home Wi-Fi (STA)
  while keeping the AP up.

### C) Steady-state AP rejoin — still no disconnect
**Action**
- After provisioning (NVS `setup_ok` set), forget/rejoin `SecuraCV-XXXX` from
  each platform. (Optionally simulate the real trigger by disabling the home
  Wi-Fi network so the device's STA link drops.)

**Expected**
- Same connected behavior as Section A on every platform — the AP is **not**
  flagged "no internet", and `canary.local` / `192.168.4.1` reach the
  dashboard (not the first-boot setup page).

### D) `canary.local` over home Wi-Fi (STA / mDNS)
**Action**
- With the Canary joined to home Wi-Fi and your phone/laptop on that **same**
  home network (not the AP), open `https://canary.local`.

**Expected**
- Resolves to the Canary (via the delegated `canary.local` mDNS catch-all) and
  loads over HTTPS (port 80 → 301 → HTTPS; accept the self-signed cert).
- `https://canary-<name>.local` (the unique hostname, e.g. `canary-kitchen.local`)
  also resolves.
- With two Canaries on the LAN, `canary.local` resolves to whichever claimed
  it first; each still has its unique `canary-<name>.local`.

### E) DNS probe spot-check (optional, with a laptop on the AP)
**Action**
- On the AP, query the device directly:
  - `nslookup -type=A canary.local 192.168.4.1`
  - `nslookup -type=AAAA canary.local 192.168.4.1`
  - `curl -so /dev/null -w "%{http_code}\n" http://192.168.4.1/generate_204`

**Expected**
- `A` query returns `192.168.4.1`.
- `AAAA` query returns NOERROR with **no** address (NODATA), not a malformed
  answer or NXDOMAIN.
- `/generate_204` returns `204`.

---

## Pass/Fail Summary
Record per platform (✅ / ❌ / N/A):

| Scenario | iOS/macOS | Android | Windows |
|---|---|---|---|
| A. First-boot: stays connected | | | |
| A. First-boot: `canary.local` loads | | | |
| A. First-boot: `192.168.4.1` loads | | | |
| C. Steady-state rejoin: stays connected | | | |
| C. Steady-state: reaches dashboard | | | |
| D. `canary.local` over home Wi-Fi | | | |
| E. DNS probe spot-check (A / AAAA / 204) | | | |

## Notes
- Android `.local` resolution depends on the browser/OS version; the
  `https://192.168.4.1` fallback is the guaranteed path and is documented for
  users in `docs/getting_started_canary.md` §2.
- If a phone *does* disconnect on current firmware, capture the OS/version and
  the probe URL it hit — the per-platform handlers live on the port-80 server
  and must never sit behind the HTTP→HTTPS redirect (see
  `firmware/LESSONS_LEARNED.md`, "Networking & Captive Portal").
