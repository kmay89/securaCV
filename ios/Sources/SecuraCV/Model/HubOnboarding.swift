// HubOnboarding.swift
//
// Finishing a Raspberry Pi hub from the phone — the decisions, with no
// network in them.
//
// A freshly flashed hub (the desktop Flasher writes the card; see
// docs/full_stack_setup.md) boots into Home Assistant's first-run wizard,
// and until now the only thing that could spare the owner that wizard was
// the Flasher itself, kept open on the same network through a 10–20 minute
// first boot. The phone is the device that is ALWAYS on the home Wi-Fi and
// always in the hand, so it is the right companion: it finds the hub, waits
// for Home Assistant to actually exist, creates the owner account over Home
// Assistant's own onboarding API, finishes the wizard's remaining pages, and
// proves the login works.
//
// This file is the Swift twin of the Flasher's `hub_io::onboarding` (Rust,
// desktop/hub-io/src/onboarding.rs): same endpoints, same converge rule, same
// client identity. Everything here is pure — values in, values out — so the
// judgment calls are host-tested; HubAPI is the pair of hands.
//
// The design rule is CONVERGE, DON'T ASSUME: every run starts by asking the
// hub what state it is actually in and does only what is still missing, so
// the same run self-heals every path — the Flasher got there first (verify
// the login and stop), someone clicked through the wizard in a browser, a
// previous run died partway, or nothing happened at all. Running it twice is
// always safe.

import Foundation

/// What a probe of the hub's web port found. Three states, not two, because
/// Home Assistant OS answers HTTP long before Home Assistant exists: on a
/// first boot it serves a "Preparing Home Assistant" page on :8123 for the
/// minutes Core takes to download. Acting on that page is how a companion
/// creates an account against nothing and gives up.
enum HubProbe: Equatable, Sendable {
    /// Nothing answered — still booting, or not on this network.
    case offline
    /// Something answered on the port, but not Home Assistant's own API.
    case preparing
    /// Home Assistant Core answers its onboarding API: setup can begin.
    case ready

    /// Classify one answer from `GET /api/onboarding`: nil for a transport
    /// failure, else the status and body. Only a 200 whose body is the
    /// onboarding step list counts as ready — the landing page is HTML, a
    /// bare 404 is the port being open with nothing behind it yet.
    static func classify(status: Int?, body: Data) -> HubProbe {
        guard let status else { return .offline }
        if status == 200, HubOnboarding.parseSteps(body) != nil { return .ready }
        return .preparing
    }
}

/// One of Home Assistant's first-run wizard pages, with whether it is done.
struct OnboardingStep: Equatable, Sendable {
    var step: String
    var done: Bool
}

/// The owner account to create — what the person types on the phone.
struct HubOwnerLogin: Equatable, Sendable {
    var name: String
    var username: String
    var password: String

    /// Home Assistant's own rules, applied before anything is sent: a name,
    /// a username with no whitespace inside it, a password of at least 8.
    /// Returns the first problem as a sentence for the form, or nil.
    func problem() -> String? {
        if name.trimmingCharacters(in: .whitespaces).isEmpty { return "Your name is empty." }
        let user = username.trimmingCharacters(in: .whitespaces)
        if user.isEmpty { return "Pick a username." }
        if user.contains(where: { $0.isWhitespace }) { return "The username can't contain spaces." }
        if password.count < 8 { return "The password needs at least 8 characters." }
        return nil
    }

    /// The canonical form Home Assistant stores: trimmed, username lowercased
    /// (it compares usernames case-insensitively).
    var normalized: HubOwnerLogin {
        HubOwnerLogin(name: name.trimmingCharacters(in: .whitespaces),
                      username: username.trimmingCharacters(in: .whitespaces).lowercased(),
                      password: password)
    }
}

/// What a converge run found and did — the honest breakdown behind the
/// one question the screen asks (`ok`).
struct HubOnboardOutcome: Equatable, Sendable {
    /// Steps the hub reported already finished when we arrived.
    var alreadyDone: [String] = []
    /// Steps this run completed.
    var completed: [String] = []
    /// Steps still pending after the run — a step this build doesn't know,
    /// or one that refused. Empty on a clean converge.
    var remaining: [String] = []
    /// This run created the owner account itself.
    var createdUser = false
    /// The typed login is proven to open this hub — created by this run, or
    /// checked through the login flow.
    var loginVerified = false
    /// Calm advice when a human still has something to do.
    var note: String?

    /// Fully converged: nothing pending and the typed login provably works.
    var ok: Bool { remaining.isEmpty && loginVerified }
}

enum HubOnboarding {
    /// The wizard pages this build knows how to finish, in the order Home
    /// Assistant's own frontend walks them. A future page is reported as
    /// remaining, never guessed at.
    static let stepUser = "user"
    static let stepCoreConfig = "core_config"
    static let stepAnalytics = "analytics"
    static let stepIntegration = "integration"
    static let knownSteps = [stepUser, stepCoreConfig, stepAnalytics, stepIntegration]

    /// Home Assistant's default web port — appended when a host carries none.
    static let defaultPort = 8123
    /// The name every Home Assistant OS hub answers to on the LAN.
    static let defaultHost = "homeassistant.local"

    /// Turn a typed or discovered host into the base URL every call builds
    /// on. A bare `hostname[:port]` (never a URL, so this can't be steered
    /// at an arbitrary address), and the host must be one that can only be
    /// on this network — the owner's typed password goes to it.
    static func baseURL(forHost raw: String) -> URL? {
        let host = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "http://", with: "")
            .replacingOccurrences(of: "https://", with: "")
            .split(separator: "/", maxSplits: 1, omittingEmptySubsequences: false)
            .first.map(String.init) ?? ""
        guard !host.isEmpty, !host.contains(" "),
              host.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "." || $0 == "-" || $0 == ":" })
        else { return nil }
        let withPort: String
        if let colon = host.lastIndex(of: ":"), host.filter({ $0 == ":" }).count == 1,
           host[host.index(after: colon)...].allSatisfy(\.isNumber),
           !host[host.index(after: colon)...].isEmpty {
            withPort = host
        } else if host.contains(":") {
            return nil   // an IPv6 literal is not a hub address this build dials
        } else {
            withPort = "\(host):\(defaultPort)"
        }
        guard let url = URL(string: "http://\(withPort)"), DeviceAPI.isPrivate(url) else { return nil }
        return url
    }

    /// The OAuth client identity for the phone's calls. Home Assistant's auth
    /// is IndieAuth-shaped: the client_id must be a URL, and its own frontend
    /// uses the instance's base URL — so does the phone, which also keeps
    /// the session the hub records recognizable rather than branded with a
    /// third-party address.
    static func clientID(base: URL) -> String { base.absoluteString + "/" }
    /// The redirect_uri paired with `clientID`; Home Assistant requires it to
    /// share the client_id's origin.
    static func redirectURI(base: URL) -> String { base.absoluteString + "/?auth_callback=1" }

    /// Parse `GET /api/onboarding`'s `[{"step": …, "done": …}, …]`. Nil for
    /// anything that is not that list — which is how the probe tells the
    /// landing page from Core.
    static func parseSteps(_ data: Data) -> [OnboardingStep]? {
        guard let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return nil }
        var out: [OnboardingStep] = []
        for item in arr {
            guard let step = item["step"] as? String, let done = item["done"] as? Bool else { return nil }
            out.append(OnboardingStep(step: step, done: done))
        }
        return out
    }

    /// The request body for `POST /api/onboarding/users`.
    static func userRequest(base: URL, login: HubOwnerLogin) -> [String: Any] {
        let l = login.normalized
        return ["client_id": clientID(base: base), "name": l.name, "username": l.username,
                "password": l.password, "language": "en"]
    }

    /// The request body for `POST /api/onboarding/integration`.
    static func integrationRequest(base: URL) -> [String: Any] {
        ["client_id": clientID(base: base), "redirect_uri": redirectURI(base: base)]
    }

    /// The form body for `POST /auth/token` exchanging an auth code.
    static func tokenExchangeForm(base: URL, code: String) -> String {
        formEncode([("grant_type", "authorization_code"), ("code", code),
                    ("client_id", clientID(base: base))])
    }

    /// The form body for `POST /auth/token` revoking a refresh token.
    static func revokeForm(refreshToken: String) -> String {
        formEncode([("token", refreshToken), ("action", "revoke")])
    }

    /// Pull the `auth_code` out of an onboarding or login-flow success body.
    static func authCode(from data: Data) -> String? {
        guard let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return (v["auth_code"] as? String) ?? (v["result"] as? String)
    }

    /// The steps still pending, in Home Assistant's order.
    static func pending(_ steps: [OnboardingStep]) -> [String] {
        steps.filter { !$0.done }.map(\.step)
    }

    /// Which pending steps this build can finish on its own once it holds a
    /// token, and which it must report as remaining. The user step is
    /// handled separately (it is where the token comes from).
    static func split(pending: [String]) -> (finishable: [String], unknown: [String]) {
        let after = pending.filter { $0 != stepUser }
        return (after.filter { knownSteps.contains($0) }, after.filter { !knownSteps.contains($0) })
    }

    /// `application/x-www-form-urlencoded`, the strict way: everything but
    /// the unreserved set is percent-encoded, spaces included, so a password
    /// with `&`, `+` or `=` survives the trip exactly.
    static func formEncode(_ pairs: [(String, String)]) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return pairs.map { key, value in
            let k = key.addingPercentEncoding(withAllowedCharacters: allowed) ?? key
            let v = value.addingPercentEncoding(withAllowedCharacters: allowed) ?? value
            return "\(k)=\(v)"
        }.joined(separator: "&")
    }
}
