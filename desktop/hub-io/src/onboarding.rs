//! onboarding — finish Home Assistant's first-run setup over its own API.
//!
//! The account pre-seed ([`crate::account`]) writes a `.storage` snapshot onto
//! the card as a head start, but whether HAOS imports it was never a promise —
//! and a user left on Home Assistant's sign-in or setup-wizard page after being
//! told their account exists is the worst outcome the hub flow can produce. So
//! the moment the freshly-booted hub answers, the app calls **HA's supported
//! onboarding REST API** and drives first-run setup to done:
//!
//!   * `GET  /api/onboarding` — which steps are still pending (no auth needed).
//!   * `POST /api/onboarding/users` — create the owner account (only valid
//!     while the `user` step is pending; returns an auth code).
//!   * `POST /auth/token` — exchange the code for an ephemeral access token.
//!   * `POST /api/onboarding/core_config` / `analytics` / `integration` —
//!     finish the remaining wizard pages with their defaults.
//!
//! The design rule is **converge, don't assume**: every run starts by asking
//! HA what state it is actually in and only does what is still missing, so the
//! same call self-heals every path — the `.storage` seed worked (verify the
//! login and stop), it half-worked, the user already clicked through the
//! wizard in a browser, a previous run died partway, or nothing happened at
//! all (do the whole thing). Running it twice is always safe.
//!
//! Privacy posture: everything here talks only to the hub on the local
//! network. The password comes from the operator's own typing, is sent only to
//! their own hub, and is never logged; the access token lives in memory for
//! the seconds this takes and the refresh token is revoked on the way out.

use serde_json::Value;

/// The onboarding steps this module knows how to finish, in the order HA's
/// own frontend walks them. A future HA may add steps we don't know — those
/// are reported as remaining, never guessed at.
pub const STEP_USER: &str = "user";
pub const STEP_CORE_CONFIG: &str = "core_config";
pub const STEP_ANALYTICS: &str = "analytics";
pub const STEP_INTEGRATION: &str = "integration";

/// The owner account to create — same shape the flash-time seed uses.
pub struct OwnerLogin<'a> {
    pub name: &'a str,
    pub username: &'a str,
    pub password: &'a str,
}

/// What a run found and did. `ok()` is the UI's one question; the fields are
/// the honest breakdown behind it.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct OnboardOutcome {
    /// Steps HA reported already finished when we arrived.
    pub already_done: Vec<String>,
    /// Steps this run completed.
    pub completed: Vec<String>,
    /// Steps still pending after the run (a step we don't know, or one that
    /// refused) — empty on a clean converge.
    pub remaining: Vec<String>,
    /// True when this run created the owner account itself.
    pub created_user: bool,
    /// True when the typed credentials are proven to open this hub — either
    /// because this run just created them, or because a login check passed.
    pub login_verified: bool,
    /// Calm, user-facing advice when a human still has something to do.
    pub note: Option<String>,
}

impl OnboardOutcome {
    /// Fully converged: nothing pending and the typed login provably works.
    pub fn ok(&self) -> bool {
        self.remaining.is_empty() && self.login_verified
    }
}

/// Turn the UI's host string into the base URL every call builds on. Same
/// character rules the probe applies (a bare `hostname[:port]`, never a URL —
/// so this can't be steered at an arbitrary address), plus HA's default port
/// when none was given.
pub fn base_url_for_host(host: &str) -> Result<String, String> {
    let host = host.trim();
    if host.is_empty()
        || host.contains('/')
        || host.contains(' ')
        || !host
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-' || c == ':')
    {
        return Err("that doesn't look like a hostname".to_string());
    }
    if host.contains(':') {
        Ok(format!("http://{host}"))
    } else {
        Ok(format!("http://{host}:8123"))
    }
}

/// The OAuth client identity for this app's calls. Home Assistant's auth is
/// IndieAuth-shaped: the client_id must be a URL, and its own frontend uses
/// the instance's base URL — so do we, which also keeps the session HA
/// records recognizable rather than branded with a third-party address.
pub fn client_id(base: &str) -> String {
    format!("{base}/")
}

/// The redirect_uri paired with [`client_id`] — HA requires it to share the
/// client_id's origin; the value mirrors its own frontend's.
pub fn redirect_uri(base: &str) -> String {
    format!("{base}/?auth_callback=1")
}

/// Parse `GET /api/onboarding`'s `[{"step": …, "done": …}, …]`.
pub fn parse_steps(v: &Value) -> Result<Vec<(String, bool)>, String> {
    let arr = v
        .as_array()
        .ok_or("Home Assistant's onboarding state wasn't a list")?;
    let mut out = Vec::with_capacity(arr.len());
    for item in arr {
        let step = item
            .get("step")
            .and_then(Value::as_str)
            .ok_or("an onboarding step had no name")?;
        let done = item
            .get("done")
            .and_then(Value::as_bool)
            .ok_or("an onboarding step had no done flag")?;
        out.push((step.to_string(), done));
    }
    Ok(out)
}

/// The request body for `POST /api/onboarding/users`.
pub fn user_request(base: &str, login: &OwnerLogin<'_>) -> Value {
    serde_json::json!({
        "client_id": client_id(base),
        "name": login.name,
        "username": login.username,
        "password": login.password,
        "language": "en",
    })
}

/// The request body for `POST /api/onboarding/integration`.
pub fn integration_request(base: &str) -> Value {
    serde_json::json!({
        "client_id": client_id(base),
        "redirect_uri": redirect_uri(base),
    })
}

/// Pull the `auth_code` out of an onboarding/login-flow success response.
pub fn auth_code_from(v: &Value) -> Option<String> {
    v.get("auth_code")
        .or_else(|| v.get("result"))
        .and_then(Value::as_str)
        .map(str::to_string)
}

fn agent() -> ureq::Agent {
    ureq::Agent::config_builder()
        .user_agent("SecuraCV-Flasher")
        .timeout_connect(Some(std::time::Duration::from_secs(5)))
        // Creating the owner also creates its person entry and first refresh
        // token — give a Pi mid-first-boot room to be slow without hanging
        // the UI forever.
        .timeout_recv_response(Some(std::time::Duration::from_secs(30)))
        .timeout_recv_body(Some(std::time::Duration::from_secs(30)))
        // HA answers meaningful JSON on non-2xx (a step already finished is a
        // 403 with a message) — deliver those as responses whose body callers
        // can read, not as a bodyless `Error::StatusCode`. The status checks
        // this obliges live in `fetch_steps` / `post_json` / `exchange_code`.
        .http_status_as_error(false)
        .build()
        .into()
}

/// GET the onboarding step list. `Err` here means "not reachable / not ready
/// yet" — the caller's retry loop owns that, not this module.
fn fetch_steps(agent: &ureq::Agent, base: &str) -> Result<Vec<(String, bool)>, String> {
    let mut resp = agent
        .get(&format!("{base}/api/onboarding"))
        .call()
        .map_err(|e| format!("couldn't read the hub's setup state: {e}"))?;
    if !resp.status().is_success() {
        return Err(format!(
            "couldn't read the hub's setup state: {}",
            resp.status()
        ));
    }
    let v: Value = resp
        .body_mut()
        .read_json()
        .map_err(|e| format!("the hub's setup state wasn't readable: {e}"))?;
    parse_steps(&v)
}

/// POST a JSON body, giving back (status, parsed body) so callers can treat
/// HA's "already done" 403s as convergence rather than failure.
fn post_json(
    agent: &ureq::Agent,
    url: &str,
    token: Option<&str>,
    body: &Value,
) -> Result<(u16, Value), String> {
    let mut req = agent.post(url);
    if let Some(t) = token {
        req = req.header("Authorization", format!("Bearer {t}"));
    }
    // The agent is configured with `http_status_as_error(false)`, so a non-2xx
    // arrives here as a response too — its status and body flow to the caller
    // the same way a 200's do.
    match req.send_json(body) {
        Ok(mut resp) => {
            let status = resp.status().as_u16();
            let v = resp.body_mut().read_json().unwrap_or(Value::Null);
            Ok((status, v))
        }
        Err(e) => Err(format!("couldn't reach the hub: {e}")),
    }
}

/// Exchange an auth code for tokens. Returns (access_token, refresh_token).
fn exchange_code(
    agent: &ureq::Agent,
    base: &str,
    code: &str,
) -> Result<(String, Option<String>), String> {
    let cid = client_id(base);
    let mut resp = agent
        .post(&format!("{base}/auth/token"))
        .send_form([
            ("grant_type", "authorization_code"),
            ("code", code),
            ("client_id", cid.as_str()),
        ])
        .map_err(|e| format!("the hub wouldn't exchange its sign-in code: {e}"))?;
    if !resp.status().is_success() {
        return Err(format!(
            "the hub wouldn't exchange its sign-in code: {}",
            resp.status()
        ));
    }
    let v: Value = resp
        .body_mut()
        .read_json()
        .map_err(|e| format!("the hub's token reply wasn't readable: {e}"))?;
    let access = v
        .get("access_token")
        .and_then(Value::as_str)
        .ok_or("the hub's token reply had no access token")?
        .to_string();
    let refresh = v
        .get("refresh_token")
        .and_then(Value::as_str)
        .map(str::to_string);
    Ok((access, refresh))
}

/// Best-effort: revoke the refresh token this run minted, so finishing setup
/// leaves no standing session behind — the app holds the maintenance SSH key
/// for its long-lived access; it doesn't need an HA login too. A failure is
/// harmless (the session shows in the owner's profile, deletable there).
fn revoke_refresh_token(agent: &ureq::Agent, base: &str, refresh: &str) {
    let _ = agent
        .post(&format!("{base}/auth/token"))
        .send_form([("token", refresh), ("action", "revoke")]);
}

/// Prove the typed credentials open this hub, via HA's login flow. Returns
/// `Ok(true)`/`Ok(false)` for a definitive answer, `Err` when the hub
/// couldn't be asked.
fn login_works(agent: &ureq::Agent, base: &str, login: &OwnerLogin<'_>) -> Result<bool, String> {
    let (status, v) = post_json(
        agent,
        &format!("{base}/auth/login_flow"),
        None,
        &serde_json::json!({
            "client_id": client_id(base),
            "redirect_uri": redirect_uri(base),
            "handler": ["homeassistant", null],
        }),
    )?;
    if status != 200 {
        return Err(format!(
            "the hub refused to start a sign-in check ({status})"
        ));
    }
    let flow_id = v
        .get("flow_id")
        .and_then(Value::as_str)
        .ok_or("the hub's sign-in check had no flow id")?
        .to_string();
    let (status, v) = post_json(
        agent,
        &format!("{base}/auth/login_flow/{flow_id}"),
        None,
        &serde_json::json!({
            "client_id": client_id(base),
            "username": login.username,
            "password": login.password,
        }),
    )?;
    if status != 200 {
        return Err(format!("the hub's sign-in check failed ({status})"));
    }
    // A successful login ends the flow with an auth code; wrong credentials
    // re-serve the form. The code is single-use and expires on its own — we
    // asked "does this password work", not "log me in", so it goes unused.
    Ok(v.get("type").and_then(Value::as_str) == Some("create_entry"))
}

/// The whole converge: read HA's actual state, finish what's missing, verify
/// the login, and say plainly what (if anything) is left for a human.
///
/// `log` narrates user-facing progress lines; secrets never appear in them.
pub fn complete_onboarding(
    host: &str,
    login: &OwnerLogin<'_>,
    log: &dyn Fn(String),
) -> Result<OnboardOutcome, String> {
    let base = base_url_for_host(host)?;
    let agent = agent();
    let steps = fetch_steps(&agent, &base)?;

    let mut out = OnboardOutcome::default();
    for (step, done) in &steps {
        if *done {
            out.already_done.push(step.clone());
        }
    }
    let pending: Vec<String> = steps
        .iter()
        .filter(|(_, done)| !done)
        .map(|(s, _)| s.clone())
        .collect();

    // Already fully set up — the .storage seed applied, a previous run
    // finished, or someone clicked through the wizard. Verify the typed login
    // so "you're done, just sign in" is a checked fact, not a hope.
    if pending.is_empty() {
        log("→ Home Assistant is already set up — checking that your login opens it…".into());
        match login_works(&agent, &base, login) {
            Ok(true) => {
                out.login_verified = true;
                log("→ it does. Sign in any time.".into());
            }
            Ok(false) => {
                out.note = Some(
                    "Home Assistant is set up, but the account you typed when flashing doesn't \
                     open it — it was probably created by hand with different details. Sign in \
                     with the account that was actually created."
                        .to_string(),
                );
            }
            Err(e) => {
                out.note = Some(format!(
                    "Home Assistant is set up, but the login check didn't get an answer ({e}). \
                     Try signing in normally."
                ));
            }
        }
        return Ok(out);
    }

    // The owner account: create it if the slot is open, otherwise sign in
    // with the typed credentials to finish the remaining steps.
    let mut access: Option<String> = None;
    let mut refresh: Option<String> = None;
    if pending.iter().any(|s| s == STEP_USER) {
        log("→ creating your Home Assistant account on the hub…".into());
        let (status, v) = post_json(
            &agent,
            &format!("{base}/api/onboarding/users"),
            None,
            &user_request(&base, login),
        )?;
        match auth_code_from(&v) {
            Some(code) if status == 200 => {
                let (a, r) = exchange_code(&agent, &base, &code)?;
                access = Some(a);
                refresh = r;
                out.created_user = true;
                out.login_verified = true;
                out.completed.push(STEP_USER.to_string());
                log("→ account created — Home Assistant accepted the login.".into());
            }
            _ => {
                // Most likely raced: someone finished the user step in a
                // browser between our GET and this POST. Re-read the truth
                // and fall through to the sign-in path.
                let msg = v
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("no reason given");
                log(format!(
                    "→ the hub declined to create the account ({status}: {msg}) — checking \
                     whether it already exists…"
                ));
            }
        }
    }
    if access.is_none() {
        match login_works(&agent, &base, login) {
            Ok(true) => {
                out.login_verified = true;
                // A verified login still needs a token to finish the other
                // steps — run the flow again, this time keeping the code.
                if let Some(code) = login_code(&agent, &base, login)? {
                    let (a, r) = exchange_code(&agent, &base, &code)?;
                    access = Some(a);
                    refresh = r;
                }
            }
            Ok(false) => {
                out.remaining = pending;
                out.note = Some(
                    "An owner account already exists on this hub and it isn't the one you \
                     typed when flashing — finish the remaining setup by signing in with the \
                     account that was actually created."
                        .to_string(),
                );
                return Ok(out);
            }
            Err(e) => {
                out.remaining = pending;
                out.note = Some(format!(
                    "Couldn't create or verify your account ({e}). The hub may still be \
                     starting — this is safe to retry."
                ));
                return Ok(out);
            }
        }
    }
    let Some(token) = access else {
        out.remaining = pending
            .iter()
            .filter(|s| *s != STEP_USER)
            .cloned()
            .collect();
        out.note = Some(
            "Your login works, but the hub wouldn't issue a token to finish the remaining \
             setup pages — open Home Assistant and click through them (a minute at most)."
                .to_string(),
        );
        return Ok(out);
    };

    // The rest of the wizard, with its defaults. Only steps HA itself reports
    // pending are attempted; a step this app doesn't know is left for the
    // browser rather than guessed at.
    for step in pending.iter().filter(|s| *s != STEP_USER) {
        let done = match step.as_str() {
            STEP_CORE_CONFIG => finish_step(
                &agent,
                &format!("{base}/api/onboarding/core_config"),
                &token,
                &serde_json::json!({}),
                log,
                "→ finishing initial settings…",
            ),
            STEP_ANALYTICS => finish_step(
                &agent,
                &format!("{base}/api/onboarding/analytics"),
                &token,
                &serde_json::json!({}),
                log,
                "→ leaving analytics at Home Assistant's default (off)…",
            ),
            STEP_INTEGRATION => finish_step(
                &agent,
                &format!("{base}/api/onboarding/integration"),
                &token,
                &integration_request(&base),
                log,
                "→ finishing the last setup page…",
            ),
            unknown => {
                log(format!(
                    "→ this Home Assistant has a setup page this app doesn't know ({unknown}) — \
                     leaving it for the browser."
                ));
                false
            }
        };
        if done {
            out.completed.push(step.clone());
        } else {
            out.remaining.push(step.clone());
        }
    }

    if let Some(r) = refresh.as_deref() {
        revoke_refresh_token(&agent, &base, r);
    }

    if !out.remaining.is_empty() && out.note.is_none() {
        out.note = Some(format!(
            "Almost everything is set up — Home Assistant still wants a click on: {}. Open it \
             in the browser and it will ask.",
            out.remaining.join(", ")
        ));
    }
    Ok(out)
}

/// Run the login flow keeping the auth code (for finishing steps that need a
/// token when this run didn't create the user itself).
fn login_code(
    agent: &ureq::Agent,
    base: &str,
    login: &OwnerLogin<'_>,
) -> Result<Option<String>, String> {
    let (status, v) = post_json(
        agent,
        &format!("{base}/auth/login_flow"),
        None,
        &serde_json::json!({
            "client_id": client_id(base),
            "redirect_uri": redirect_uri(base),
            "handler": ["homeassistant", null],
        }),
    )?;
    if status != 200 {
        return Ok(None);
    }
    let Some(flow_id) = v.get("flow_id").and_then(Value::as_str) else {
        return Ok(None);
    };
    let (status, v) = post_json(
        agent,
        &format!("{base}/auth/login_flow/{flow_id}"),
        None,
        &serde_json::json!({
            "client_id": client_id(base),
            "username": login.username,
            "password": login.password,
        }),
    )?;
    if status == 200 && v.get("type").and_then(Value::as_str) == Some("create_entry") {
        Ok(auth_code_from(&v))
    } else {
        Ok(None)
    }
}

/// POST one wizard step. HA answers 403 for a step that is already done —
/// someone clicked it in a browser mid-run — which is convergence, not error.
fn finish_step(
    agent: &ureq::Agent,
    url: &str,
    token: &str,
    body: &Value,
    log: &dyn Fn(String),
    narration: &str,
) -> bool {
    log(narration.to_string());
    match post_json(agent, url, Some(token), body) {
        Ok((200, _)) | Ok((403, _)) => true,
        Ok((status, v)) => {
            let msg = v
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("no reason given");
            log(format!("→ that page didn't finish ({status}: {msg})."));
            false
        }
        Err(e) => {
            log(format!("→ that page didn't finish ({e})."));
            false
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read as _, Write as _};
    use std::net::TcpListener;
    use std::sync::{Arc, Mutex};

    // ── pure decisions ──────────────────────────────────────────────────

    #[test]
    fn base_url_appends_has_default_port_only_when_missing() {
        assert_eq!(
            base_url_for_host("homeassistant.local:8123").unwrap(),
            "http://homeassistant.local:8123"
        );
        assert_eq!(
            base_url_for_host("10.0.0.5").unwrap(),
            "http://10.0.0.5:8123"
        );
        assert_eq!(
            base_url_for_host(" hub-2.local ").unwrap(),
            "http://hub-2.local:8123"
        );
    }

    #[test]
    fn base_url_refuses_anything_that_is_not_a_bare_host() {
        // Same posture as the probe: this must never be steerable into an
        // arbitrary URL.
        for bad in ["", "http://evil", "host/path", "host name", "a\nb"] {
            assert!(base_url_for_host(bad).is_err(), "{bad:?} must be refused");
        }
    }

    #[test]
    fn client_identity_is_the_instance_itself() {
        // IndieAuth: client_id must be a URL, and redirect_uri must share its
        // origin — the exact pair HA's own frontend uses.
        let base = "http://hub.local:8123";
        assert_eq!(client_id(base), "http://hub.local:8123/");
        assert_eq!(redirect_uri(base), "http://hub.local:8123/?auth_callback=1");
    }

    #[test]
    fn parse_steps_reads_has_shape_and_refuses_others() {
        let v = serde_json::json!([
            {"step": "user", "done": false},
            {"step": "core_config", "done": true},
        ]);
        assert_eq!(
            parse_steps(&v).unwrap(),
            vec![("user".into(), false), ("core_config".into(), true)]
        );
        assert!(parse_steps(&serde_json::json!({"nope": 1})).is_err());
        assert!(parse_steps(&serde_json::json!([{"step": "user"}])).is_err());
    }

    #[test]
    fn user_request_carries_the_typed_login_and_a_url_client_id() {
        let login = OwnerLogin {
            name: "Kay",
            username: "kay",
            password: "hunter22",
        };
        let v = user_request("http://h:8123", &login);
        assert_eq!(v["client_id"], "http://h:8123/");
        assert_eq!(v["username"], "kay");
        assert_eq!(v["password"], "hunter22");
        assert_eq!(v["language"], "en");
    }

    #[test]
    fn auth_code_is_read_from_both_spellings() {
        // The onboarding endpoints answer {"auth_code": …}; the login flow's
        // create_entry carries it as {"result": …}.
        assert_eq!(
            auth_code_from(&serde_json::json!({"auth_code": "abc"})).as_deref(),
            Some("abc")
        );
        assert_eq!(
            auth_code_from(&serde_json::json!({"type": "create_entry", "result": "xyz"}))
                .as_deref(),
            Some("xyz")
        );
        assert_eq!(auth_code_from(&serde_json::json!({})), None);
    }

    // ── the converge, against a scripted loopback Home Assistant ────────

    /// A tiny scripted HTTP server: each accepted connection consumes the
    /// next (path-substring, status, json-body) entry. Records every request
    /// line + body so tests can assert what was sent.
    fn serve_script(script: Vec<(&'static str, u16, Value)>) -> (String, Arc<Mutex<Vec<String>>>) {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("addr");
        let seen = Arc::new(Mutex::new(Vec::new()));
        let seen2 = seen.clone();
        std::thread::spawn(move || {
            for (expect_path, status, body) in script {
                let Ok((mut sock, _)) = listener.accept() else {
                    return;
                };
                let mut buf = Vec::new();
                let mut byte = [0u8; 1];
                while !buf.ends_with(b"\r\n\r\n") {
                    if sock.read(&mut byte).unwrap_or(0) == 0 {
                        break;
                    }
                    buf.push(byte[0]);
                }
                let head = String::from_utf8_lossy(&buf).to_string();
                let len: usize = head
                    .lines()
                    .find_map(|l| {
                        l.to_ascii_lowercase()
                            .strip_prefix("content-length:")
                            .map(|v| v.trim().parse().unwrap_or(0))
                    })
                    .unwrap_or(0);
                let mut req_body = vec![0u8; len];
                if len > 0 {
                    let _ = sock.read_exact(&mut req_body);
                }
                let line = head.lines().next().unwrap_or("").to_string();
                assert!(
                    line.contains(expect_path),
                    "expected a request to {expect_path}, got {line}"
                );
                seen2
                    .lock()
                    .unwrap()
                    .push(format!("{line}\n{}", String::from_utf8_lossy(&req_body)));
                let payload = body.to_string();
                let resp = format!(
                    "HTTP/1.1 {status} X\r\nContent-Type: application/json\r\n\
                     Content-Length: {}\r\nConnection: close\r\n\r\n{payload}",
                    payload.len()
                );
                let _ = sock.write_all(resp.as_bytes());
            }
        });
        // Strip the scheme: complete_onboarding wants a bare host.
        (format!("{addr}"), seen)
    }

    fn quiet() -> impl Fn(String) {
        |_line| {}
    }

    #[test]
    fn fresh_hub_gets_the_whole_wizard_finished() {
        let (host, seen) = serve_script(vec![
            (
                "/api/onboarding",
                200,
                serde_json::json!([
                    {"step": "user", "done": false},
                    {"step": "core_config", "done": false},
                    {"step": "analytics", "done": false},
                    {"step": "integration", "done": false},
                ]),
            ),
            (
                "/api/onboarding/users",
                200,
                serde_json::json!({"auth_code": "code-1"}),
            ),
            (
                "/auth/token",
                200,
                serde_json::json!({
                    "access_token": "at-1", "refresh_token": "rt-1", "token_type": "Bearer"
                }),
            ),
            ("/api/onboarding/core_config", 200, serde_json::json!({})),
            ("/api/onboarding/analytics", 200, serde_json::json!({})),
            (
                "/api/onboarding/integration",
                200,
                serde_json::json!({"auth_code": "code-2"}),
            ),
            ("/auth/token", 200, serde_json::json!({})), // the revoke
        ]);
        let login = OwnerLogin {
            name: "Kay",
            username: "kay",
            password: "pw-pw-pw",
        };
        let out = complete_onboarding(&host, &login, &quiet()).expect("converges");
        assert!(out.ok(), "fresh hub must converge: {out:?}");
        assert!(out.created_user);
        assert!(out.login_verified);
        assert_eq!(out.remaining, Vec::<String>::new());
        let seen = seen.lock().unwrap();
        // The wizard steps after user carry the Bearer token…
        assert!(seen[3].contains("core_config"));
        // …and the last call revoked the refresh token this run minted.
        assert!(seen[6].contains("token=rt-1") && seen[6].contains("action=revoke"));
    }

    #[test]
    fn an_already_set_up_hub_only_verifies_the_login() {
        let (host, _) = serve_script(vec![
            (
                "/api/onboarding",
                200,
                serde_json::json!([
                    {"step": "user", "done": true},
                    {"step": "core_config", "done": true},
                    {"step": "analytics", "done": true},
                    {"step": "integration", "done": true},
                ]),
            ),
            (
                "/auth/login_flow",
                200,
                serde_json::json!({"flow_id": "f1", "type": "form"}),
            ),
            (
                "/auth/login_flow/f1",
                200,
                serde_json::json!({
                    "type": "create_entry", "result": "code-3"
                }),
            ),
        ]);
        let login = OwnerLogin {
            name: "Kay",
            username: "kay",
            password: "pw-pw-pw",
        };
        let out = complete_onboarding(&host, &login, &quiet()).expect("reachable");
        assert!(out.ok());
        assert!(!out.created_user);
        assert!(out.login_verified);
    }

    #[test]
    fn a_foreign_owner_account_is_reported_not_guessed_at() {
        // Set up by hand with different credentials: the run must say so
        // plainly, never claim success, and never mutate anything.
        let (host, _) = serve_script(vec![
            (
                "/api/onboarding",
                200,
                serde_json::json!([
                    {"step": "user", "done": true},
                    {"step": "core_config", "done": true},
                    {"step": "analytics", "done": true},
                    {"step": "integration", "done": true},
                ]),
            ),
            (
                "/auth/login_flow",
                200,
                serde_json::json!({"flow_id": "f1", "type": "form"}),
            ),
            (
                "/auth/login_flow/f1",
                200,
                serde_json::json!({
                    "type": "form", "errors": {"base": "invalid_auth"}
                }),
            ),
        ]);
        let login = OwnerLogin {
            name: "Kay",
            username: "kay",
            password: "wrong",
        };
        let out = complete_onboarding(&host, &login, &quiet()).expect("reachable");
        assert!(!out.ok());
        assert!(!out.login_verified);
        assert!(out.note.unwrap().contains("doesn't open it"));
    }

    #[test]
    fn an_unknown_future_step_is_left_for_the_browser() {
        let (host, _) = serve_script(vec![
            (
                "/api/onboarding",
                200,
                serde_json::json!([
                    {"step": "user", "done": false},
                    {"step": "quantum_align", "done": false},
                ]),
            ),
            (
                "/api/onboarding/users",
                200,
                serde_json::json!({"auth_code": "code-1"}),
            ),
            (
                "/auth/token",
                200,
                serde_json::json!({
                    "access_token": "at-1", "refresh_token": "rt-1"
                }),
            ),
            ("/auth/token", 200, serde_json::json!({})), // the revoke
        ]);
        let login = OwnerLogin {
            name: "Kay",
            username: "kay",
            password: "pw-pw-pw",
        };
        let out = complete_onboarding(&host, &login, &quiet()).expect("reachable");
        assert!(!out.ok(), "an unknown step must not be claimed done");
        assert!(out.created_user, "the known part still converges");
        assert_eq!(out.remaining, vec!["quantum_align".to_string()]);
        assert!(out.note.unwrap().contains("quantum_align"));
    }

    #[test]
    fn an_unreachable_hub_is_an_err_for_the_retry_loop_not_a_verdict() {
        // Nothing listening: the caller's retry loop owns "not yet" — a
        // refused connection must never masquerade as an onboarding outcome.
        let login = OwnerLogin {
            name: "K",
            username: "k",
            password: "p",
        };
        assert!(complete_onboarding("127.0.0.1:1", &login, &quiet()).is_err());
    }
}
