// HubProvisionPlan.swift
//
// What turns a freshly onboarded Home Assistant into a SecuraCV hub, as a
// plan the phone can carry out — and the decisions about what is ALREADY
// done, with no network in them.
//
// The Flasher's self-setup bundle does this over the hub's service console
// (canary-local/devices/hub_seed.json, executed by hub_seed_apply.py). The
// phone has no console and no file system on the hub; what it has is the
// owner's Home Assistant session, and Home Assistant proxies the Supervisor
// to an administrator at `/api/hassio/…` — the same calls its own Settings →
// Apps page makes. So the phone's plan is the bundle's plan, minus the one
// thing only a file write can do (Frigate's curated camera config), stated
// honestly as the step that is yours.
//
// Same posture as the executor: every action is idempotent, a snapshot of
// the hub decides what is skipped, and the broker login the Canaries will
// use is minted ONCE and kept where only this phone can read it — so the
// Canary walkthrough can hand it to a device later instead of sending the
// owner to read it out of an add-on's configuration page.

import Foundation
import Security

/// A snapshot of the hub, read through the Supervisor proxy, that decides
/// what the plan still has to do.
struct HubObserved: Equatable, Sendable {
    /// Add-on store repositories, as normalized source URLs.
    var repositories: Set<String> = []
    /// Installed add-ons: Supervisor slug → state ("started", "stopped", …).
    var addons: [String: String] = [:]
    /// Integrations Home Assistant already has a config entry for.
    var configEntryDomains: Set<String> = []
    /// Usernames in the Mosquitto add-on's `logins` option.
    var brokerLogins: Set<String> = []
    /// The kernel add-on's current `mode` option, when installed.
    var kernelMode: String?
}

/// One thing the phone does to the hub. The values are the exact calls
/// HubAPI makes, so a plan rendered on screen doubles as the audit trail.
enum HubProvisionAction: Equatable, Sendable {
    case registerRepository(url: String)
    case installAddon(slug: String, friendly: String)
    /// Append a broker login to Mosquitto's `logins` (never replacing the
    /// list) and restart it so it reads the new account.
    case addBrokerLogin(username: String)
    case setKernelMode(mode: String)
    case startAddon(slug: String, friendly: String)
    /// Accept the MQTT card the Mosquitto add-on announces, or start the
    /// flow by hand. Best effort: Home Assistant may answer with a form a
    /// phone can't fill, and that is reported, never fatal.
    case connectMQTT
    /// Create the SecuraCV config entry (`setup_mode: auto`). Only possible
    /// once the integration's files are on the hub, which the phone cannot
    /// place — a missing handler is reported as the one step that is yours.
    case connectSecuraCV
}

/// A planned action with whether the snapshot says it is already done.
struct HubPlannedAction: Equatable, Identifiable, Sendable {
    var action: HubProvisionAction
    var already: Bool
    var reason: String
    var id: String { HubProvisionPlan.label(for: action) }
}

/// One narrated step: a title, the why, and its actions.
struct HubProvisionStep: Equatable, Identifiable, Sendable {
    var id: String
    var title: String
    var why: String
    var actions: [HubPlannedAction]
    /// What is honestly left to the owner after this step.
    var yours: String?
}

enum HubProvisionPlan {
    // The same slugs and repositories the bundle's plan carries
    // (canary-local/devices/hub_seed.json): Supervisor add-on slugs are
    // "<repo-hash>_<addon-slug>", where repo-hash is sha1(lowercased repo
    // URL)[:8] — ccab4aaf for blakeblackshear's repository, d0491a67 for
    // SecuraCV's. A slug that is wrong here fails the same way it would in
    // the bundle: the install answers 404, and the step says so.
    static let frigateRepository = "https://github.com/blakeblackshear/frigate-hass-addons"
    static let securacvRepository = "https://github.com/kmay89/securaCV"
    static let mosquittoSlug = "core_mosquitto"
    static let frigateSlug = "ccab4aaf_frigate"
    static let kernelSlug = "d0491a67_privacy_witness_kernel"
    /// The broker account the Canaries sign in as. Mosquitto's add-on
    /// refuses anonymous clients and Home Assistant's own reserved users
    /// don't apply to a device, so without this a hub finishes setup and
    /// rejects the first Canary that ever publishes.
    static let canaryUsername = "canary"
    static let kernelMode = "frigate"

    /// Repository URLs compare after the same normalization the executor
    /// applies: lowercase, no trailing slash, no `.git`.
    static func normalizeRepository(_ url: String) -> String {
        var s = url.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        while s.hasSuffix("/") { s.removeLast() }
        if s.hasSuffix(".git") { s.removeLast(4) }
        return s
    }

    /// Mint a broker password on the phone: 18 random bytes, written in the
    /// URL-safe alphabet so it pastes into any MQTT field without escaping.
    /// Nil only if the system's random source refuses, which the caller
    /// treats as "do not create a login", never as an empty password.
    static func mintPassword(bytes: Int = 18) -> String? {
        var buf = [UInt8](repeating: 0, count: bytes)
        guard SecRandomCopyBytes(kSecRandomDefault, bytes, &buf) == errSecSuccess else { return nil }
        return Data(buf).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    /// Expand the plan against what the hub already has.
    static func plan(observed o: HubObserved) -> [HubProvisionStep] {
        func repo(_ url: String) -> HubPlannedAction {
            let have = o.repositories.contains(normalizeRepository(url))
            return HubPlannedAction(action: .registerRepository(url: url), already: have,
                                    reason: have ? "already registered" : "")
        }
        func install(_ slug: String, _ friendly: String) -> HubPlannedAction {
            let have = o.addons[slug] != nil
            return HubPlannedAction(action: .installAddon(slug: slug, friendly: friendly), already: have,
                                    reason: have ? "already installed" : "")
        }
        func start(_ slug: String, _ friendly: String) -> HubPlannedAction {
            let running = o.addons[slug] == "started"
            return HubPlannedAction(action: .startAddon(slug: slug, friendly: friendly), already: running,
                                    reason: running ? "already running" : "")
        }
        let loginPresent = o.brokerLogins.contains(canaryUsername)
        let mqttConnected = o.configEntryDomains.contains("mqtt")
        let securacvConnected = o.configEntryDomains.contains("securacv")
        let modeSet = o.kernelMode == kernelMode

        return [
            HubProvisionStep(
                id: "add-repositories",
                title: "Tell the hub where the apps live",
                why: "Home Assistant installs add-ons only from repositories it has been told about. Two are registered: Frigate's, and SecuraCV's own.",
                actions: [repo(frigateRepository), repo(securacvRepository)]),
            HubProvisionStep(
                id: "install-broker",
                title: "Install the broker",
                why: "Mosquitto is the meeting point: every Canary publishes what it witnessed to it, and Home Assistant and the witness kernel listen there. Without it the devices have nowhere to report.",
                actions: [install(mosquittoSlug, "Mosquitto broker"), start(mosquittoSlug, "Mosquitto broker")]),
            HubProvisionStep(
                id: "broker-login",
                title: "Make the login your Canaries will use",
                why: "The broker refuses anonymous clients. A \"\(canaryUsername)\" account is created with a password minted on this phone and kept in its Keychain — the Canary walkthrough hands it to each device for you.",
                actions: [HubPlannedAction(action: .addBrokerLogin(username: canaryUsername), already: loginPresent,
                                           reason: loginPresent ? "\(canaryUsername) can already sign in" : "")]),
            HubProvisionStep(
                id: "connect-mqtt",
                title: "Connect Home Assistant to the broker",
                why: "A broker Home Assistant never subscribes to is a hub that looks empty: Canaries publish, nothing appears. On a hub with a keyboard you'd accept a discovered-MQTT card; this does the same click.",
                actions: [HubPlannedAction(action: .connectMQTT, already: mqttConnected,
                                           reason: mqttConnected ? "already connected" : "")]),
            HubProvisionStep(
                id: "install-frigate",
                title: "Install Frigate (the camera eyes)",
                why: "Frigate looks at camera video and decides \"that's a person\". SecuraCV doesn't re-implement detection — it turns Frigate's events into signed, identity-stripped claims. Installed, not started: it needs a camera config first.",
                actions: [install(frigateSlug, "Frigate")],
                yours: "Add your cameras from the SecuraCV panel in Home Assistant (it writes Frigate's config), then start Frigate. The hub works for Canaries without any camera."),
            HubProvisionStep(
                id: "install-securacv",
                title: "Install the SecuraCV witness kernel",
                why: "The part that makes this a witness rather than a camera system: it strips identity from every detection and writes a signed, hash-chained log you can verify without trusting anyone. Set to Frigate mode before it starts, so it consumes detections instead of waiting in its setup wizard.",
                actions: [install(kernelSlug, "Privacy Witness Kernel"),
                          HubPlannedAction(action: .setKernelMode(mode: kernelMode), already: modeSet,
                                           reason: modeSet ? "already in \(kernelMode) mode" : ""),
                          start(kernelSlug, "Privacy Witness Kernel")]),
            HubProvisionStep(
                id: "connect-securacv",
                title: "Connect the SecuraCV integration",
                why: "The integration is what turns witness events into Home Assistant entities and the Verified Timeline card. Its files install from the hub's own terminal in one command; once they are there, this creates its config entry.",
                actions: [HubPlannedAction(action: .connectSecuraCV, already: securacvConnected,
                                           reason: securacvConnected ? "already connected" : "")],
                yours: "If the integration isn't installed yet: Settings → Apps → Terminal & SSH, then run the one-line installer from the full-stack guide. The add-on is already running and announces itself the moment the integration exists."),
        ]
    }

    /// Every action across the plan that still needs doing.
    static func todo(_ steps: [HubProvisionStep]) -> [HubProvisionAction] {
        steps.flatMap { $0.actions }.filter { !$0.already }.map(\.action)
    }

    /// One line per action, naming the call — what the screen shows and
    /// what a tester reads.
    static func label(for action: HubProvisionAction) -> String {
        switch action {
        case .registerRepository(let url): return "register repository \(url)"
        case .installAddon(let slug, _): return "install add-on \(slug)"
        case .addBrokerLogin(let username): return "add broker login \(username)"
        case .setKernelMode(let mode): return "set \(kernelSlug) mode=\(mode)"
        case .startAddon(let slug, _): return "start add-on \(slug)"
        case .connectMQTT: return "connect Home Assistant to mqtt"
        case .connectSecuraCV: return "connect Home Assistant to securacv"
        }
    }

    /// The Supervisor path (under `/api/hassio/`) an action calls, or nil
    /// for the two that go through Home Assistant's config-entry flows.
    static func supervisorPath(for action: HubProvisionAction) -> String? {
        switch action {
        case .registerRepository: return "store/repositories"
        case .installAddon(let slug, _): return "store/addons/\(slug)/install"
        case .addBrokerLogin: return "addons/\(mosquittoSlug)/options"
        case .setKernelMode: return "addons/\(kernelSlug)/options"
        case .startAddon(let slug, _): return "addons/\(slug)/start"
        case .connectMQTT, .connectSecuraCV: return nil
        }
    }

    /// Fold `GET /api/hassio/store/repositories`, `GET /api/hassio/addons`,
    /// the two add-ons' info and `GET /api/config/config_entries/entry`
    /// into one snapshot. Each argument is the raw JSON body (or nil when
    /// the call failed — an unreadable answer reads as "not there", which
    /// only ever errs toward doing a step again, and every step is safe to
    /// repeat).
    static func observe(repositories: Data?, addons: Data?, mosquittoInfo: Data?,
                        kernelInfo: Data?, configEntries: Data?) -> HubObserved {
        var o = HubObserved()
        if let d = repositories, let v = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
            let list = (v["data"] as? [[String: Any]])
                ?? ((v["data"] as? [String: Any])?["repositories"] as? [[String: Any]]) ?? []
            o.repositories = Set(list.compactMap { ($0["source"] as? String).map(normalizeRepository) })
        }
        if let d = addons, let v = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
           let data = v["data"] as? [String: Any], let list = data["addons"] as? [[String: Any]] {
            for a in list {
                guard let slug = a["slug"] as? String else { continue }
                o.addons[slug] = (a["state"] as? String) ?? "unknown"
            }
        }
        if let d = mosquittoInfo, let opts = options(in: d) {
            let logins = (opts["logins"] as? [[String: Any]]) ?? []
            o.brokerLogins = Set(logins.compactMap { $0["username"] as? String })
        }
        if let d = kernelInfo, let opts = options(in: d) {
            o.kernelMode = opts["mode"] as? String
        }
        if let d = configEntries, let list = try? JSONSerialization.jsonObject(with: d) as? [[String: Any]] {
            o.configEntryDomains = Set(list.compactMap { $0["domain"] as? String })
        }
        return o
    }

    /// The `options` dictionary inside a Supervisor `addons/<slug>/info` answer.
    static func options(in data: Data) -> [String: Any]? {
        guard let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let d = v["data"] as? [String: Any] else { return nil }
        return d["options"] as? [String: Any]
    }

    /// Mosquitto's `logins` with this account appended — read → append →
    /// write the whole list back, never replacing accounts the owner made
    /// by hand. A matching username already present is replaced in place.
    static func loginsAppending(username: String, password: String,
                                to current: [[String: Any]]) -> [[String: Any]] {
        var out = current.filter { ($0["username"] as? String) != username }
        out.append(["username": username, "password": password])
        return out
    }

    /// Find the MQTT card the Mosquitto add-on announced, in Home
    /// Assistant's list of flows in progress (`GET /api/config/config_entries/flow`).
    static func discoveredMQTTFlowID(in data: Data) -> String? {
        guard let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return nil }
        for f in list {
            guard f["handler"] as? String == "mqtt", let id = f["flow_id"] as? String else { continue }
            let source = ((f["context"] as? [String: Any])?["source"] as? String) ?? ""
            if source == "hassio" { return id }
        }
        return nil
    }

    /// What a config-entry flow answer means: finished, needs a human, or
    /// already there. `type` is Home Assistant's flow-result vocabulary.
    enum FlowOutcome: Equatable, Sendable { case created, alreadyConfigured, needsForm(step: String), failed(String) }

    static func flowOutcome(_ data: Data, status: Int) -> FlowOutcome {
        guard let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return .failed("Home Assistant's answer wasn't readable (\(status)).")
        }
        if status == 404 { return .failed("Home Assistant doesn't have this integration installed yet.") }
        guard status < 300 else {
            return .failed((v["message"] as? String) ?? "Home Assistant refused (\(status)).")
        }
        switch v["type"] as? String {
        case "create_entry": return .created
        case "abort":
            let reason = (v["reason"] as? String) ?? ""
            return ["already_configured", "single_instance_allowed"].contains(reason)
                ? .alreadyConfigured : .failed("Home Assistant stopped the flow: \(reason).")
        case "form", "menu": return .needsForm(step: (v["step_id"] as? String) ?? "?")
        default: return .failed("Home Assistant answered with \((v["type"] as? String) ?? "nothing").")
        }
    }
}
