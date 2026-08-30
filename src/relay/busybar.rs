//! Busy Bar sink for the alert relay — the LAN half of "poke the owner."
//!
//! A [Busy Bar](https://busy.app) is a desk status light with a 72x16 LED
//! matrix and an open local HTTP API. This module maps a [`Poke`] to the JSON
//! body of one `POST /api/display/draw` call, so a SecuraCV alert lights the
//! bar on the owner's desk: color by severity class, the poke's fixed title
//! as scrolling text, auto-expiring so a stale alert never shouts forever.
//!
//! Everything here is decision, not delivery (same split as the ntfy lane):
//! no socket, no clock, so the mapping and its privacy posture are
//! unit-tested. The `alert_relay` bin owns the HTTP call.
//!
//! The privacy posture is *stricter* than ntfy's, not looser, even though the
//! bar sits on the owner's own LAN:
//! - **Same poke, no widening.** The payload is built only from [`Poke`]
//!   fields — the class word, the fixed title, the topic's device word. No
//!   timestamp, no event content, no fingerprint, exactly like every other
//!   sink (Invariant III).
//! - **LAN only, never the vendor cloud.** The bar's API is also reachable
//!   through busy.app's cloud proxy with an account-linked token. Routing
//!   pokes there would hand a third party a per-event timing feed — the
//!   exact oracle the relay exists to avoid — so [`validate_url`] refuses
//!   any `busy.app` host outright. This is not configurable.

use super::{Poke, PokeClass};

/// The `application_name` the relay draws under. The bar scopes content and
/// clearing per application, so one stable name means a newer alert replaces
/// an older one instead of stacking, and `DELETE /api/display/draw` can
/// remove only ours. Lowercase per the naming rule for the code layer.
pub const APPLICATION_NAME: &str = "securacv";

/// The draw endpoint, relative to the owner-configured base URL.
pub const DRAW_PATH: &str = "/api/display/draw";

impl PokeClass {
    /// The bar color for this class, `#RRGGBB`. Red is reserved for the top
    /// of the ladder (tamper, a heard alarm); integrity and offline are
    /// amber nudges — the same split the Hue alert-light blueprint ships.
    /// The drill is green: a test that lights up red teaches people that
    /// red is usually a drill.
    pub fn busybar_color(self) -> &'static str {
        match self {
            PokeClass::Tamper | PokeClass::Pattern => "#FF0000",
            PokeClass::Integrity | PokeClass::Offline => "#FF8C00",
            PokeClass::Drill => "#00BE50",
        }
    }

    /// Draw priority (1..=100) on the bar — higher wins when another app is
    /// holding the display. A heard alarm outranks everything; the drill
    /// deliberately outranks nothing important.
    pub fn busybar_priority(self) -> u8 {
        match self {
            PokeClass::Tamper | PokeClass::Pattern => 100,
            PokeClass::Integrity => 80,
            PokeClass::Offline => 60,
            PokeClass::Drill => 30,
        }
    }

    /// How long the alert stays on the bar, in milliseconds. The class's
    /// debounce gap, clamped to [60 s, 15 min]: long enough to be seen by
    /// someone walking back to the desk, short enough that a stale alert
    /// does not shout all afternoon. A condition that persists re-lights
    /// the bar on its next allowed poke; the relay has no all-clear lane,
    /// so expiry-by-timeout IS the all-clear.
    pub fn busybar_hold_ms(self) -> u64 {
        self.default_min_gap_secs().clamp(60, 900) * 1000
    }
}

/// The scrolling line on the 72x16 front matrix: the fixed title, plus the
/// topic's device word when the poke has one. Never the body sentence and
/// never payload content — a desk display is readable by anyone in the room,
/// so it gets the tersest form of the already-coarse vocabulary.
pub fn display_text(poke: &Poke) -> String {
    if poke.device.is_empty() {
        poke.title.to_string()
    } else {
        format!("{}: {}", poke.title, poke.device)
    }
}

/// The complete `POST /api/display/draw` body for one poke. The fields ARE
/// the privacy contract, same as [`Poke`] itself: class-derived color,
/// priority and hold time, the fixed title text — nothing else.
pub fn draw_payload(poke: &Poke) -> serde_json::Value {
    let color = poke.class.busybar_color();
    serde_json::json!({
        "application_name": APPLICATION_NAME,
        "priority": poke.class.busybar_priority(),
        "led_notification_color": color,
        "elements": [{
            "id": "securacv-alert",
            "type": "text",
            "text": display_text(poke),
            "font": "small",
            "color": color,
            "x": 0,
            "y": 0,
            "width": 72,
            "scroll_rate": 20,
            "display": "front",
            "timeout": poke.class.busybar_hold_ms(),
        }],
    })
}

/// One drawing currently owned by the relay on the bar, for severity
/// arbitration. The bar replaces content drawn under the same application
/// name, so without this a later low-severity draw (an offline nudge, the
/// drill) would wipe an active red alarm off the display — the bar's own
/// `priority` field arbitrates against *other* apps, not within ours.
/// Pure and clock-free like the rest of the core: callers pass monotonic
/// seconds, so the rule is unit-tested.
#[derive(Default)]
pub struct DisplayArbiter {
    /// What the bar is showing for us: (draw priority, expiry in caller
    /// seconds). None once nothing of ours can still be on the display.
    holding: Option<(u8, u64)>,
}

impl DisplayArbiter {
    /// May this poke be drawn now? Equal severity redraws (latest wins —
    /// a new tamper refreshes the word and the hold); strictly lower
    /// severity waits until the held drawing has expired on its own.
    pub fn may_draw(&self, poke: &Poke, now_secs: u64) -> bool {
        match self.holding {
            None => true,
            Some((priority, until)) => {
                now_secs >= until || poke.class.busybar_priority() >= priority
            }
        }
    }

    /// Mark this poke as on the display. Call only after a successful draw.
    pub fn record_draw(&mut self, poke: &Poke, now_secs: u64) {
        self.holding = Some((
            poke.class.busybar_priority(),
            now_secs.saturating_add(poke.class.busybar_hold_ms() / 1000),
        ));
    }
}

/// Refuse the vendor cloud; accept anything else the owner points at.
///
/// `http://` is fine here — the bar's local API is plain HTTP (USB-Ethernet
/// ships at `http://10.0.4.20`). What is refused, unconditionally, is any
/// `busy.app` host: that is the vendor's cloud proxy, and a poke routed
/// through it becomes a per-event timing feed to a third party under an
/// account-linked token. This check is a guardrail against configuring the
/// one documented cloud path, not a reachability proof — the relay cannot
/// know which of the owner's hostnames resolve on their LAN, so keeping the
/// sink actually LAN-local is the owner's half of the contract (use the
/// bar's local IP or LAN name).
pub fn validate_url(url: &str) -> Result<(), String> {
    let rest = url
        .strip_prefix("http://")
        .or_else(|| url.strip_prefix("https://"))
        .ok_or_else(|| "Busy Bar URL must start with http:// or https://".to_string())?;
    let host = rest
        .split(['/', '?', '#'])
        .next()
        .unwrap_or_default()
        .rsplit('@')
        .next()
        .unwrap_or_default()
        .split(':')
        .next()
        .unwrap_or_default()
        // A trailing dot is the DNS root — `busy.app.` IS `busy.app`.
        .trim_end_matches('.')
        .to_ascii_lowercase();
    if host.is_empty() {
        return Err("Busy Bar URL has no host".to_string());
    }
    if host == "busy.app" || host.ends_with(".busy.app") {
        return Err(
            "Busy Bar URL points at the vendor cloud (busy.app). This sink is LAN-only: \
             use the bar's local address (e.g. http://10.0.4.20 over USB, or its Wi-Fi IP) \
             so pokes never leave your network."
                .to_string(),
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::relay::evaluate;

    #[test]
    fn the_payload_carries_no_timestamp_and_no_event_content() {
        let poke = evaluate(
            "securacv/porch/tamper",
            br#"{"state":"on","confidence":0.93,"kind":"enclosure_tamper"}"#,
        )
        .expect("tamper poke");
        let body = draw_payload(&poke).to_string();
        // Only class-derived constants and the poke's own coarse words —
        // never the MQTT payload's fields, never a clock reading.
        assert!(!body.contains("enclosure"));
        assert!(!body.contains("0.93"));
        assert!(!body.contains("confidence"));
        assert!(!body.contains("\"ts\""));
        assert!(!body.contains("timestamp"));
        assert!(body.contains("\"application_name\":\"securacv\""));
    }

    #[test]
    fn the_desk_line_is_the_title_plus_the_device_word_at_most() {
        let poke = evaluate("securacv/porch/availability", b"offline").expect("offline poke");
        assert_eq!(display_text(&poke), "Canary went dark: porch");
        // Hub-level lanes have no device; the title stands alone with no
        // dangling separator.
        let chain = evaluate("witness/chain_problem", b"ON").expect("integrity poke");
        assert_eq!(display_text(&chain), "Chain verification failed");
    }

    #[test]
    fn red_is_reserved_for_the_top_of_the_ladder() {
        assert_eq!(PokeClass::Tamper.busybar_color(), "#FF0000");
        assert_eq!(PokeClass::Pattern.busybar_color(), "#FF0000");
        assert_ne!(PokeClass::Offline.busybar_color(), "#FF0000");
        assert_ne!(PokeClass::Integrity.busybar_color(), "#FF0000");
        // The drill must never wear an alarm's clothes — same rule as its
        // ntfy priority, enforced in color here.
        assert_ne!(PokeClass::Drill.busybar_color(), "#FF0000");
        assert!(PokeClass::Drill.busybar_priority() < PokeClass::Offline.busybar_priority());
    }

    #[test]
    fn severity_order_survives_the_priority_mapping() {
        // The bar's contention rule (higher priority wins the display) must
        // agree with the ntfy ladder, or a nudge could cover an alarm.
        let ladder = [
            PokeClass::Drill,
            PokeClass::Offline,
            PokeClass::Integrity,
            PokeClass::Tamper,
        ];
        for pair in ladder.windows(2) {
            assert!(pair[0].busybar_priority() < pair[1].busybar_priority());
        }
        assert_eq!(
            PokeClass::Pattern.busybar_priority(),
            PokeClass::Tamper.busybar_priority()
        );
    }

    #[test]
    fn every_alert_expires_on_its_own() {
        // The relay has no all-clear lane, so an element that never times
        // out would hold a stale alert on the desk forever. Every class
        // gets a bounded hold, and the payload actually carries it.
        for class in [
            PokeClass::Tamper,
            PokeClass::Integrity,
            PokeClass::Offline,
            PokeClass::Pattern,
            PokeClass::Drill,
        ] {
            let ms = class.busybar_hold_ms();
            assert!((60_000..=900_000).contains(&ms), "{class:?} hold {ms}");
        }
        let poke = Poke::drill();
        let value = draw_payload(&poke);
        assert_eq!(
            value["elements"][0]["timeout"].as_u64(),
            Some(PokeClass::Drill.busybar_hold_ms())
        );
    }

    #[test]
    fn the_vendor_cloud_is_refused_and_the_lan_is_not() {
        assert!(validate_url("http://10.0.4.20").is_ok());
        assert!(validate_url("http://192.168.1.44:80").is_ok());
        assert!(validate_url("https://busybar.local").is_ok());
        // The cloud proxy in every spelling: bare, subdomain, port,
        // trailing path, userinfo trick.
        assert!(validate_url("https://busy.app").is_err());
        assert!(validate_url("https://api.busy.app").is_err());
        assert!(validate_url("https://cloud.busy.app:443/x").is_err());
        assert!(validate_url("https://user@api.busy.app/api").is_err());
        // The DNS-root spelling: `busy.app.` IS `busy.app`.
        assert!(validate_url("https://busy.app.").is_err());
        assert!(validate_url("https://api.busy.app./x").is_err());
        // But a LAN hostname that merely contains the word is fine.
        assert!(validate_url("http://mybusy.appliance.lan").is_ok());
        // No scheme, no dice — a bare host would default somewhere silently.
        assert!(validate_url("10.0.4.20").is_err());
    }

    #[test]
    fn a_lower_severity_draw_never_wipes_an_active_alarm() {
        // The bar replaces same-application content, so the arbiter is what
        // keeps a drill or an offline nudge from erasing a red tamper word
        // mid-hold. Lower severity waits out the hold; it does not queue.
        let tamper = evaluate(
            "securacv/porch/tamper",
            br#"{"state":"on","kind":"enclosure_tamper"}"#,
        )
        .unwrap();
        let offline = evaluate("securacv/porch/availability", b"offline").unwrap();
        let mut arb = DisplayArbiter::default();
        assert!(arb.may_draw(&tamper, 1000));
        arb.record_draw(&tamper, 1000);
        assert!(!arb.may_draw(&offline, 1001), "offline must not wipe red");
        assert!(!arb.may_draw(&Poke::drill(), 1001), "nor may the drill");
        // Equal-or-higher severity redraws — latest wins, the hold refreshes.
        let smoke = evaluate(
            "securacv/hall/sensing",
            br#"{"acoustic_event":"smoke_alarm_t3","t3_detected":1}"#,
        )
        .unwrap();
        assert!(arb.may_draw(&smoke, 1001), "an alarm may replace an alarm");
        // Once the held drawing has expired off the bar, anything may draw.
        let after = 1000 + PokeClass::Tamper.busybar_hold_ms() / 1000;
        assert!(arb.may_draw(&offline, after));
    }
}
