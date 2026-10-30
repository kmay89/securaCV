# Submitting the iPhone app to the App Store

The checklist and the copy for taking a TestFlight build of the SecuraCV
companion (iPhone, iPad and the embedded Apple Watch app) through App Review.
The build itself comes from the normal release path: **Actions → "Update
everything (only what needs it)"** with `only: ios` and `publish` ticked, which
uploads to App Store Connect and tags `ios-v<version>`
([`docs/RELEASE_BUTTONS.md`](../docs/RELEASE_BUTTONS.md)). Everything below is
what App Store Connect asks for on top of a build.

## Before you submit

| Check | Where it stands |
|---|---|
| The build passes App Store Connect's own validation | Watch for an "Action needed" email after each upload: it arrives after CI is green. ITMS-90626 (an Apple product name in an App Intent string) rejected 0.8.0; `scripts/tests/test_ios_intent_strings.py` now fails the build on it first. |
| Export compliance | `ITSAppUsesNonExemptEncryption` is `false` in the iPhone and watch Info.plists. The app uses only the encryption built into iOS (HTTPS, CryptoKit signature checks), which is exempt. |
| Privacy manifest | `Support/PrivacyInfo.xcprivacy`: no tracking, no collected data types, one required-reason API (UserDefaults, `CA92.1`). It rides in the app, the watch app and both widget bundles (`scripts/lint_apple_plists.py`). |
| CloudKit schema | Run `ios/scripts/cloudkit_schema.sh check` before submitting. A production schema missing a field silently stops fleet sync for reviewers and users ([`docs/RELEASE_BUTTONS.md`](../docs/RELEASE_BUTTONS.md), "Things no button can do"). |
| Critical Alerts | Release builds sign without it until Apple grants the entitlement (`critical_alerts` input on `ios-release.yml`). The app falls back to time-sensitive alerts. Request it separately at developer.apple.com; it is not needed for review. |
| Privacy policy URL | `https://securacv.com/privacy` must describe the app, not only the website (guideline 5.1.1). The app's section is on that page. |
| Apple agreements | An unaccepted Program License Agreement stops every signing step ([`.github/RELEASE_LESSONS.md`](../.github/RELEASE_LESSONS.md), 2026-10-29). App Store Connect → Business shows any agreement waiting. |

## App Store Connect: the app record

| Field | Value |
|---|---|
| Name | SecuraCV |
| Subtitle (30 characters) | Your home's honest witnesses |
| Primary category | Utilities |
| Secondary category | Lifestyle |
| Age rating | 4+ (no objectionable content, no web browsing, no user-generated content shared with strangers) |
| Support URL | `https://securacv.com/help` |
| Marketing URL | `https://securacv.com` |
| Privacy policy URL | `https://securacv.com/privacy` |
| Copyright | Errer Labs |

**Keywords** (100 characters, comma-separated, no spaces after commas):

```
canary,home,security,privacy,witness,sensor,alerts,presence,local,fleet,offline,camera-free
```

**Promotional text** (can change without a new build):

```
See every Canary in your home at a glance, add a new one in a few taps, and keep their software up to date from your phone.
```

**Description:**

```
SecuraCV is the companion app for your Canaries: small privacy-first sensors that witness what happens at home without recording video or sending it anywhere.

At a glance
• See the whole fleet on one screen, with the one sentence that matters: is everything okay?
• Get time-sensitive alerts for what you choose, quiet hours that respect your night, and a test that sends a real alert to check it still reaches you.
• Check a signature yourself: a Canary's event chain is verified on your phone against the key it was paired with.

Set up in a few taps
• A new Canary near your phone shows a card. Tap it, pick your Wi-Fi, and it joins.
• A display shows a code on its own screen: scan it and your phone hands it your Wi-Fi.
• The app tells you which Canary is the new one as it appears on your network.

Keep them current
• Check for and install a Canary's software update from its own screen. The Canary checks the update's signature itself before installing, and goes back to its previous software if the new version fails its health check.

Yours, and only yours
• No account and no SecuraCV server. Your fleet syncs between your own devices through your own iCloud.
• Nothing is collected about you. The app talks only to devices on your home network and to your own iCloud.

Works with Home Assistant, Apple Home, Siri and Shortcuts, widgets, Live Activities and Apple Watch.

SecuraCV needs Canary hardware to show a real fleet. Try the demo fleet from the Fleet tab to see everything first.
```

## App Privacy ("nutrition label")

Answer **Data Not Collected**. Apple counts data as collected when it leaves
the device in a way the developer or a third party can access. SecuraCV has no
server and no analytics or crash SDK, and what syncs goes to the user's own
iCloud private database, which the developer cannot read:

- the paired-device list (name, type, local address, pairing date, pinned
  certificate fingerprint), so a second iPhone or iPad has the same fleet
  (`Cloud/CloudSync.swift`);
- content-free "away" wake records, a severity class only, so an alert can
  reach the user's other devices through Apple's push service
  (`Native/AwayPush.swift`);
- a household share, only when the user invites someone through iCloud
  sharing (`Cloud/HouseholdShare.swift`).

Secrets (device tokens, pinned keys, a remembered Wi-Fi password) stay in the
device Keychain and never sync.

## App Review notes

Paste into **App Review Information → Notes**:

```
SecuraCV is the companion app for Canary home sensors, which are separate hardware. The reviewer does not need hardware: on the Fleet tab, tap "Try the demo fleet" (or Fleet tab → ⋯ menu → Demo fleet). The demo fleet is clearly labeled "Demo fleet — sample data" on every screen and exercises the same screens a real fleet uses: Today, Fleet, Alerts, each device's detail screen, Siri and Shortcuts ("Check SecuraCV"), widgets and the Apple Watch app.

No account or sign-in exists. The app talks only to devices on the user's own home network and to the user's own iCloud.

Permissions, each asked at the moment it is needed:
- Bluetooth (including background "bluetooth-central"): hears nearby Canaries' beacons for presence and the "Find this Canary" guide, and hands a brand-new Canary the home Wi-Fi over its Bluetooth setup connection.
- Local network: finds Canaries and the user's Home Assistant hub on the home Wi-Fi.
- Camera: reads the setup QR code a display Canary shows on its screen. Nothing is recorded.
- Hotspot configuration: joins a new Canary's own setup Wi-Fi network to hand it the home Wi-Fi.
- HomeKit (optional): adds Canaries to the Home app as sensors.
- Face ID: unlocks the key that opens a sealed snapshot.
- Notifications (time-sensitive) and background "remote-notification": alerts, and wakes delivered through the user's own iCloud when away from home.
```

## Screenshots

App Store Connect needs screenshots for each device family the app runs on:
the largest iPhone size it asks for, the 13-inch iPad size (the app supports
iPad), and the Apple Watch size (the watch app is included). Take them from
the Simulator with the demo fleet on (it is on by default there,
[`DEMO.md`](DEMO.md)): Today, Fleet, a device's detail screen with **Software
update**, the **Add a Canary** walkthrough, Alerts, and the watch's **Check
the Fleet** answer. The "Demo fleet — sample data" chip may stay visible;
Apple accepts real UI showing sample data.

## After approval

Release manually or automatically from App Store Connect. The next version
starts with a `MARKETING_VERSION` bump in `ios/project.yml`; the release
workflow refuses to publish a version that is already tagged.
