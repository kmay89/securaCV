# Canary Settings Access Validation

Manual test pass for the `canary.local/settings` flow on Canary WAP firmware.

## Goal

Confirm that Settings is easy to reach without weakening the threat model:

- WiFi access alone does not authorize settings API calls.
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
| WiFi-only access blocked | In a fresh/private browser with no session, open `/settings` and do not enter a token or press BOOT. | UI shell may load, but protected data/actions stay behind auth. |
| API token fallback | Paste the `cv_...` API token into the auth modal and click Connect. | Modal closes and Settings data loads. Invalid token shows a clear error. |
| BOOT-tap fallback | Click "Use BOOT Tap Once", then short-tap BOOT within 60 seconds. | Device returns provisioning receipt, sets the browser session, and Settings loads. |
| BOOT tap not repeated | After BOOT-tap fallback succeeds, refresh `/settings`. | Settings loads from the saved `cv_session`; no second BOOT tap needed. |
| Reboot behavior | Reboot the Canary, then open `/settings` in the same browser before the 24-hour session expires. | Existing session continues to authorize browser API calls if firmware session state survives as expected; otherwise auth modal offers token/BOOT fallback. |
| API token not persisted | Reload the page after connecting with token. | Token is not stored in localStorage; user may need token/BOOT unless `cv_session` was set by pairing or BOOT fallback. |

## Security Checks

- Confirm no full API token appears in page source for `/` or `/settings`.
- Confirm `Set-Cookie` includes `HttpOnly`, `SameSite=Strict`, `Path=/`, and `Max-Age=86400`.
- Confirm unauthenticated calls to protected endpoints such as `/api/status` return `401` or `403`.
- Confirm repeated bad token attempts trigger auth backoff instead of unlimited retries.

## Notes

- `canary.local/settings` intentionally serves the UI shell before auth. The authorization boundary is the API layer, not the static HTML route.
- If `.local` resolution fails on Android or some routers, use `192.168.4.1` as shown in the boot banner.
