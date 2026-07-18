# The Nightstand Wave — a bedside display worth sleeping next to

Research across the bedside category (Echo Show 5, Nest Hub 2, Hatch
Restore, Loftie, LaMetric, Tidbyt, Braun/Lexon, Mui) converges on a short
list of truths:

1. **The #1 complaint about every glowing bedside product is "still too
   bright at night."** The loved ones (Braun, Loftie blackout, Mui) emit
   *zero* light until asked.
2. **The #1 trust-killer is an alarm or feature that depends on the
   cloud** (Nest sleep-sensing paywall, Hatch+ subscription, LaMetric's
   server-rendered apps, Loftie's alarm-missed bugs).
3. The loved rituals are simple: weather before you rise, room comfort at
   a glance, a gentle two-phase wake, one-tap goodnight.

The Canary Display's constraints — no cloud, no camera, no mic, local hub
only — are this category's *wishlist*. The nightstand wave closes the gap.

## What shipped

| Feature | Flag | Watch | Dash |
|---|---|---|---|
| True night blackout + dim red tap-to-peek | `FEATURE_NIGHT_BLACKOUT` | on | off (wall panel: dark theme + scheduled backlight-off) |
| Bedroom comfort words (temp/humidity) | `FEATURE_COMFORT_WORDS` | on | on |
| Hub weather line (morning) | `FEATURE_HUB_WEATHER` | on | on |
| On-device sunrise/sunset | (with `CD_LAT`/`CD_LON` in secrets.h) | on | on |
| Two-phase gentle wake alarm | `FEATURE_WAKE_ALARM` | on | off |

### Night: darkness by default, honesty holds the veto

During quiet hours with everything quiet and links up, the backlight goes
to **0** — the room is dark, full stop. A tap peeks the red-shifted clock
plus the bedroom comfort line at `CD_BRIGHT_PEEK` (28/255), nowhere near
the daytime blast. Anything Warn-or-worse, or a dead WiFi/hub link, keeps
the familiar night glow — **silence is never rendered as safety**, so a
dark screen always genuinely means "all is well."

### Comfort words, not numbers

Bands follow sleep-science consensus (Sleep Foundation / Cleveland Clinic):
15.5–19.5 °C ideal for sleep; 40–60 % RH ideal, under 30 % dries airways,
over 60 % blocks the body's evaporative cooling. With hysteresis so the
word never flickers at a band edge:

> `bedroom 18.5° just right` · `bedroom 22.4° too warm · dry air`

The bedside witness is whichever canary's room name contains
`CD_BEDSIDE_ROOM` (default `"bed"`), else the first one publishing
temperature. Ideal humidity earns silence — only a problem gets words.

### Weather before you rise (the hub fetches; the glass never does)

The display subscribes to ONE retained topic, `securacv/env/weather`.
Home Assistant republishes its own forecast there — Met.no (HA's key-free
default) or Open-Meteo both work. The glass shows it in the morning
window (until 10:00): `24°/13° · rain 20% · some clouds · sun up 5:37`.
A forecast older than 3 hours simply doesn't render.

```yaml
# HA automation: republish the forecast for the displays
alias: Weather for SecuraCV displays
triggers:
  - trigger: time_pattern
    minutes: "/30"
  - trigger: homeassistant
    event: start
actions:
  - action: weather.get_forecasts
    target: { entity_id: weather.forecast_home }
    data: { type: daily }
    response_variable: fc
  - variables:
      today: "{{ fc['weather.forecast_home'].forecast[0] }}"
  - action: mqtt.publish
    data:
      topic: securacv/env/weather
      retain: true
      qos: 1
      payload: >-
        {{ {"cond": states('weather.forecast_home'),
            "hi": today.temperature, "lo": today.templow,
            "rain": today.precipitation_probability | default(0) | int,
            "ts": now().timestamp() | int} | to_json }}
```

### Sun times, computed on the glass

The simplified NOAA sunrise equation runs on-device once a day (±2 min
verified against NOAA for New York, London, and Sydney; polar day/night
handled). Set `CD_LAT` / `CD_LON` in `secrets.h` — the coordinates never
leave the device, which is the point. Evening badge: `sun down 20:27`.

### The gentle wake (`securacv/<display-id>/alarm/set`, retained)

```json
{"at": 1752906600, "ramp_min": 25, "phase2_after_s": 420}
```

Timeline for alarm time **T** — the Hatch/Loftie shape on our hardware:

- **T−ramp → T**: backlight sunrise, gamma-corrected (a linear duty ramp
  looks like a light switch; γ=2.2 feels like dawn).
- **T → T+60 s**: soft ascending chirps (C6→E6, off-resonance = quiet by
  physics) every ~25 s.
- **T+60 s → T+7 min**: silence — the built-in snooze.
- **then**: insistent pulses (the Tier-2 cadence, never the intruder
  alarm's grammar) for at most 10 minutes — it never beeps forever into an
  empty room.
- **Tap anywhere, any phase**: dismissed.

The Loftie lesson is enforced structurally: the hub only *configures* the
alarm. The schedule persists to NVS and fires from the device's own clock
— WiFi down, hub down, it still wakes you. An empty retained payload (or
`{"at": 0}`) clears it. Note `FEATURE_CHIME` gates the *audible* part on
the piezo actually being populated; the sunrise ramp works regardless.

## Copy discipline

Every new line went through the glass vocabulary rules (microcopy lint
check 5): no dBm, no topic names, no jargon on the wall.
