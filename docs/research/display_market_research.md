# Market research — glanceable security status displays (2026-07)

Source appendix for [`docs/hardware/display_ux_design.md`](../hardware/display_ux_design.md).
Five research tracks, agent-gathered 2026-07-06; prices are July-2026 USD
street/list unless noted. Condensed — each claim keeps one representative
source.

## 1. Smart displays as security monitors

| Product | Price | Alert path / latency | Persistent status? | Bedroom | Subscription |
|---|---|---|---|---|---|
| Echo Show 5/8/15 (Ring monitor) | $90/$150–180/$300 | cloud; announcements 15 s–3 min reported ([Ring community](https://community.ring.com/t/delayed-notification/7721)) | no — live view capped ~10 min server-side ([Ring community](https://community.ring.com/t/live-view-time-out-setting-for-echo-show-reduce/176760)); ambient mode now serves **full-screen ads** ([Tom's Guide](https://www.tomsguide.com/audio/smart-speakers/obnoxious-full-screen-ads-are-making-the-amazon-echo-show-useless-heres-how-to-fix-it)) | backlight glow; audible announcements | live view free; Ring Home $4.99–19.99/mo for history |
| Nest Hub / Hub Max (Nest monitor) | $99 / discont. 2025 | cloud; ~5–10 s stream lag ([Google support](https://support.google.com/googlenest/answer/9259827?hl=en)) | no (~30 min cap) | **best-in-class**: Ambient EQ near-black, Hub 2 camera-free | Nest Aware $50–300/yr |
| UniFi Protect Viewport | $199 | **local**, ~1–2 s | **yes** — 16-cam wall, no timeout | no controls of its own; silent | none (needs UniFi NVR estate) |
| Amazon Echo Hub | $180 | cloud for cams | closest Alexa gets (widgets), laggy chip ([Android Central](https://www.androidcentral.com/accessories/smart-home/amazon-echo-hub-review)) | wall panel, adaptive dim | none required |
| eufy Smart Display E10 (2025) | $200 | local-ish; pops feeds on motion | semi | battery, portable | none ([PCWorld](https://www.pcworld.com/article/2850610/eufy-smart-display-e10-review.html)) |

Ring sells **no display of its own** — its store bundles Echo Shows; even
ring-mqtt can't beat the cloud 10-min stream cap ([ring-mqtt](https://github.com/tsightler/ring-mqtt-ha-addon)).

## 2. Panels & keypads

| Product | Price | Glanceable info | Night behavior | Notable complaint |
|---|---|---|---|---|
| Ring Alarm Keypad 2nd gen | $49.99 | mode LED only | motion-lit numbers | one volume for chirps *and* countdown ([Ring community](https://community.ring.com/t/ring-alarm-volume-enhancements-for-chirps-and-entry-delay/47765)) |
| Ring Chime / Pro | $34.99/$59.99 | audio only (+nightlight on Pro) | per-schedule DND (good) | cloud delays/misses ([community](https://community.ring.com/t/delay-time-from-door-bell-to-the-chime-and-the-app/359)) |
| SimpliSafe keypad + base | $69.99 (+kit) | screen **sleeps** — armed state invisible ([forum request](https://support.simplisafe.com/conversations/product-requests-and-suggestions/status-light-on-the-keypad/6678eea8dde3fa78584778ad)) | base LED "lights up entire rooms," no dimmer — years-old top request ([forum](https://support.simplisafe.com/conversations/product-requests-and-suggestions/adjustable-setting-for-base-station-light-brightness/6190c67d8ea41ebb0623bb31)) | plans $9.99–79.99/mo |
| Aqara S1 Plus panel | ~€230 EU-only | 6.9" dashboards, proximity wake | screensaver/standby | not sold in US; no Thread |
| Abode Keypad 2 | $65.99 | color-coded logo LED | motion-wake backlight (battery) | cryptic color vocabulary |
| ADT Command 7" | pkg-only | richest panel glance | volume slider; **secondary screen won't turn off** ([ADT KB](https://help.adt.com/s/article/The-display-on-my-optional-secondary-7-touchscreen-WTS700-will-not-turn-off-Why)) | requires ~$46–62/mo + 36-mo contract |

Pattern: **local RF for the alarm loop is table stakes** (Z-Wave/433/Zigbee);
the phone/cloud is only for remote. The one pure-cloud device (Chime) is the
one with latency complaints.

## 3. DIY / ambient ecosystem

- **Guition ESP32-S3-4848S040** (~$30, 480×480 wall touchscreen) + ESPHome
  LVGL is the community default HA status panel — local push well under 1 s
  ([ESPHome devices](https://devices.esphome.io/devices/guition-esp32-s3-4848s040/), [HA thread](https://community.home-assistant.io/t/guition-4-480x480-esp32-s3-4848s040-smart-display-with-lvgl/729271)).
- **E-ink** (TRMNL $139 + $5/mo for <15-min refresh) is calm and zero-light
  but too slow for alerting ([trmnl.com](https://trmnl.com/)).
- **Tidbyt** acquired/paused (Modal, 2024); local rescue = Tronbyt
  ([Hackaday](https://hackaday.com/2025/03/29/open-source-framework-aims-to-keep-tidbyt-afloat/)). Cloud dependency = product end-of-life risk.
- **SenseCAP Watcher** ($99) — our watch form-factor reference; detection
  node, not a display; default cloud-LLM path ([Seeed](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html)).
- **LaMetric Time** ($199) — the polished local-push precedent: priority
  levels (info/warning/critical) in its notification API ([docs](https://lametric-documentation.readthedocs.io/en/latest/guides/first-steps/first-lametric-indicator-app.html)).

## 4. Baby monitors (the bedside-screen craft)

Local-FHSS parent units (Infant Optics DXR-8 Pro $199, eufy SpaceView ~$165,
VTech <$60, HelloBaby ~$65) beat WiFi/cloud rivals on **<0.2 s latency vs
10–20 s** ([safebabylab](https://safebabylab.com/why-do-baby-monitors-lag/)), privacy, and outage immunity. Patterns adopted into
our spec (§2/§3 of the UX doc): link-loss beeps within 5–20 s repeating
(Philips/Angelcare — [Philips](https://www.usa.philips.com/c-t/XC000004767/my-philips-avent-baby-monitor-beeps), [Angelcare](https://angelcarenorthamerica.zendesk.com/hc/en-us/articles/360061215234-AC527-Understanding-the-Alert-Notifications)); Owlet's green
positive-confirmation glow + yellow-fault/red-event two-tone split
([Owlet](https://support.owletcare.com/hc/en-us/articles/4411867692557-Dream-Sock-and-the-Base-Station-Status-Colors)); VOX screen-sleep with 3-level anchored sensitivity
([VTech](https://vtech.zendesk.com/hc/en-us/articles/4402484821145-Sound-Alert-VOX-Voice-Activated-Sensitivity)); physical eyes-closed controls. Cautionary tales: Miku's
brick-and-paywall ([The Register](https://www.theregister.com/2023/10/06/miku_baby_monitor/)), Nanit's subscription-gated activation
([Nanit community](https://community.nanit.com/discussion/5940/i-cant-activate-my-camera-without-an-insights-subscription-i-do-not-want-to-pay-for-one)), Cubo's ~100-notification nights
([review roundup](https://www.toolify.ai/ai-news/regretting-my-purchase-cubo-ai-baby-monitor-review-2023-20065)).

## 5. Phone-dependence pain (demand evidence)

- DND swallows security alerts — Ring, SimpliSafe, and Home Assistant all
  maintain workaround docs ([SimpliSafe](https://support.simplisafe.com/articles/alarm-event-monitoring/how-do-i-make-sure-i-always-receive-alarm-alerts-even-when-my-phone-is-silenced/634479594a42432bbd44f8b6), [HA](https://companion.home-assistant.io/docs/notifications/critical-notifications/)).
- Android battery optimization delays camera notifications 15–30 min
  ([Wyze forum](https://forums.wyze.com/t/android-notifications-15-20-minutes-late-cam-v3/268158)).
- ~70% of default motion alerts are noise (person-only filtering claim,
  [Roku support](https://support.roku.com/article/9194280478487)); NN/g documents disable-the-notifications fatigue
  ([NN/g](https://www.nngroup.com/articles/smart-home-notifications/)).
- Households are only as alerted as the primary phone — secondary users get
  nothing ([NN/g](https://www.nngroup.com/articles/smart-home-users/)).

## 6. UX standards drawn on

Calm technology (Weiser/Brown; Amber Case's 81-point Calm Tech Certified
standard: attention/periphery/light/sound — [calmtech.institute](https://www.calmtech.institute/calm-tech-principles));
ANSI Z535.1/ISO 3864 safety colors ([ANSI blog](https://blog.ansi.org/ansi/ansi-z535-1-2022-safety-colors-standard/)); WCAG 2.1 SC 1.4.1
color-independence ([W3C](https://www.w3.org/WAI/WCAG21/Understanding/use-of-color.html)); melatonin-band avoidance for night UI
(<10 lux bedroom guidance — [Sleep as Android](https://sleep.urbandroid.org/checking-your-light-conditions/)); Nest ack semantics
([Google support](https://support.google.com/googlehome/answer/9243193?hl=en)); IEC 60601-1-8 alarm-urgency grammar ([Same Sky](https://www.sameskydevices.com/blog/a-guide-to-iec-60601-1-8-and-medical-alarm-systems));
hospital alarm-fatigue statistics ([AHRQ](https://www.ncbi.nlm.nih.gov/books/NBK555522/)).

## 7. Regulatory

UL 985 (fire warning) / UL 2034 (CO) / UL 1023 (burglar units) scopes — a
passive information display stays outside all three **only if it never
claims alarm/detection function**; incumbent disclaimer template (Ring,
Wyze, SimpliSafe, Nest terms) adopted in the UX doc §6. FCC: pre-certified
module + Part 15B SDoC + "Contains FCC ID" label ([Espressif certs](https://www.espressif.com/en/support/documents/certificates), [SDoC](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-B)).
EU: RED self-declaration + 2025 RED-DA cybersecurity (EN 18031,
[Espressif guide](https://developer.espressif.com/blog/2025/04/esp32-red-da-en18031-compliance-guide/)). FTC/NAD: capability claims only (ADT 2014,
SimpliSafe 2020 precedents — [FTC](https://www.ftc.gov/news-events/news/press-releases/2014/03/home-security-company-adt-settles-ftc-charges-endorsements-deceived-consumers)).

## 8. Component prices used in the BOM (2026-07)

XIAO ESP32-S3 $7.49 list (3-pack ~$5.97/pc) ([Seeed](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html)); Seeed Round
Display $18.00 list, street $24–26 ([Seeed](https://www.seeedstudio.com/1-28-Round-Touch-Display-for-Seeed-Studio-XIAO-ESP32.html)); Waveshare
ESP32-S3-Touch-LCD-4.3 ~$28–33 (4.3B $36.99) ([Waveshare](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm), [CNX](https://www.cnx-software.com/2024/07/22/4-3-inch-esp32-s3-wireless-touchscreen-display-terminal-block-rs485-can-bus-i2c-dio/));
protected 302030/502530 LiPo $5.95–7.95 single (Adafruit [#1317](https://www.adafruit.com/product/1317)/[#1578](https://www.adafruit.com/product/1578)),
$1–2.20 at 10+; certified 5V/2A USB-C PSU $7–10 single, $0.60–1.50 at 100+;
M2 self-tappers ~$5–8/100. At 100 units the watch electronics land ~$30–32
and the dash ~$27–32 — both under a Ring Keypad's *retail* with no
subscription attached.
