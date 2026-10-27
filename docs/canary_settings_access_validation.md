# Canary Settings Access Validation

Manual test pass for the `canary.local/settings` flow on Canary WAP firmware.

## Goal

Confirm that Settings is easy to reach without weakening the threat model:

- A browser with no session for the device gets no settings data or actions
  until it signs in. That is not a boundary against a host on the WiFi: the
  landing page at `/` mints a session for any host on the network with one
  tap ("Open dashboard"), and that session reaches the settings API (ledger
  F346), all but the actions the security checks below name, which take the
  API token (F289, F334, F335, F345, F346, F361).
- A valid dashboard session opens Settings without another BOOT tap.
- API token and BOOT-tap fallback remain clear and usable.

## Preconditions

- Canary WAP firmware includes PR #526 or later.
- Test phone or laptop is connected to the Canary AP or same reachable network.
- Browser cache is not pinned to an older firmware page. Use a private window if unsure.
- Device is reachable at one of:
  - `http://canary.local`
  - `http://192.168.4.1`

## Test Matrix

| Case | Steps | Expected Result |
| --- | --- | --- |
| Direct Settings route | Open `http://canary.local/settings`. | Legacy dashboard loads with the Settings panel selected. If not signed in, auth modal is shown. |
| Dashboard Settings button | Open `http://canary.local`, complete normal dashboard pairing if prompted, then click Settings. | Browser navigates to `/settings` and opens Settings directly. |
| Existing session reuse | With a valid dashboard session, refresh `/settings`. | Settings API calls work without entering the API token or pressing BOOT. |
| WiFi-only access blocked | In a browser with no session for the device (close every private window first: Chrome and Firefox share one private session, and its cookies, across the private windows that are open; or delete the site's `cv_session` in the developer tools), open `/settings` and do not enter a token or press BOOT. | UI shell may load, but protected data/actions stay behind auth. |
| API token fallback | In a browser with no session for the device (as in the WiFi-only row), open `/settings`, paste the `cv_...` API token into the auth modal and click Connect. | Modal closes and Settings data loads. Invalid token shows a clear error. (On a page a session already signed in, the box checks a pasted token by itself, without the cookie, since the owner-authority PR: a wrong token is refused there too.) |
| BOOT-tap fallback | In a browser with no session for the device (as in the WiFi-only row), open `/settings`; in the auth modal click "Use BOOT Tap Once", then short-tap BOOT when the page says to (within 60 seconds). | Device returns provisioning receipt, sets the browser session, and Settings loads. |
| BOOT tap not repeated | After BOOT-tap fallback succeeds, refresh `/settings`. | Settings loads from the saved `cv_session`; no second BOOT tap needed. |
| Reboot behavior | Reboot the Canary, then open `/settings` in the same browser before the 24-hour session expires. | Existing session continues to authorize browser API calls if firmware session state survives as expected; otherwise auth modal offers token/BOOT fallback. |
| API token not persisted | Reload the page after connecting with token. | Token is not stored in localStorage; user may need token/BOOT unless `cv_session` was set by pairing or BOOT fallback. |

## Security Checks

- Confirm no full API token appears in page source for `/` or `/settings`.
- Confirm `Set-Cookie` includes `HttpOnly`, `SameSite=Strict`, `Path=/`, and `Max-Age=86400`, and `Secure` when the page is `https://` (the first-boot setup at `http://192.168.4.1` sets it without `Secure`, which the browser keeps).
- Confirm a session alone does not fetch the recovery kit: **Settings**, **Device**, **Recovery kit** on a page signed in only by the cookie brings the sign-in back, which reads "Your session does not cover this, or has ended", and BOOT Tap Once then saves the kit.
- Confirm a session alone does not show the pairing QR, whose payload carries the API token: **Settings**, **Device**, **Pair with the Canary Vision app**, **Show QR** on a cookie-only page brings the sign-in up with no alert, and a curl to `/api/pairing-qr` with the cookie alone answers 401 (F289).
- Confirm a session alone does not register a vault key: Register on a cookie-only page brings the sign-in back with no alert; Not now returns; a mistyped token is refused at the sign-in; pasting the right token then registers it.
- Confirm a cookie-only page cannot confirm a Bluetooth or opera pairing: Start Pairing and Create/Join bring the sign-in up first (Not now goes back), a mistyped token is refused there, and a curl confirm with the cookie alone answers 401.
- Confirm a cookie-only page cannot change the Wi-Fi, or reboot the unit with Reboot Device or Retry full boot, after setup: Connect, Remove credentials, Disconnect, Reboot Device and Retry full boot bring the sign-in up with no "Failed" alert and the typed network kept, and a curl to `/api/wifi/connect`, `/api/wifi/forget` or `/api/reboot` with the cookie alone answers 401.
- Confirm unauthenticated calls to protected endpoints such as `/api/status` return `401` or `403`.
- Confirm repeated bad token attempts trigger auth backoff instead of unlimited retries.

## Notes

- `canary.local/settings` intentionally serves the UI shell before auth. The authorization boundary is the API layer, not the static HTML route.
- If `.local` resolution fails on Android or some routers, use `192.168.4.1` as shown in the boot banner.
