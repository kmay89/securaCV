// HubAPI.swift
//
// The pair of hands for HubOnboarding and HubProvisionPlan: every call the
// phone makes to a Home Assistant hub while finishing it. Three surfaces,
// one client:
//
//   * Home Assistant's onboarding REST API (`/api/onboarding/*`, `/auth/*`)
//     — the supported way to create the owner account and finish the
//     first-run wizard; what the desktop Flasher's companion also uses.
//   * The Supervisor, through Home Assistant's own admin proxy
//     (`/api/hassio/<path>`) — add-on repositories, installs, options,
//     starts. Exactly the calls the Settings → Apps page makes.
//   * Config-entry flows (`/api/config/config_entries/*`) — connecting
//     Home Assistant to the broker and to the SecuraCV integration.
//
// Trust posture: the base URL has already passed the "only on this
// network" gate (HubOnboarding.baseURL), and this client refuses to be
// pointed anywhere else. Plain http on the LAN is what Home Assistant's
// own frontend speaks to a fresh hub; there is no certificate to pin
// until the owner adds one. The password rides in the one request that
// creates the account (and the one that checks it), is never logged, and
// is dropped the moment a token exists.

import Foundation

struct HubAPIError: Error, LocalizedError, Equatable {
    var message: String
    var errorDescription: String? { message }
}

/// The token pair a converge run ends with. Kept only as long as the
/// provisioning plan needs it; revoked on the way out.
struct HubSession: Equatable, Sendable {
    var accessToken: String
    var refreshToken: String?
}

final class HubAPI {
    let base: URL
    private let session: URLSession

    init(base: URL) {
        precondition(DeviceAPI.isPrivate(base), "a hub is only ever dialed on this network")
        self.base = base
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 900   // an add-on install pulls an image
        config.waitsForConnectivity = false
        config.httpAdditionalHeaders = ["User-Agent": "SecuraCV-iPhone"]
        self.session = URLSession(configuration: config)
    }

    // MARK: - the probe

    /// Where the hub is in its first boot. Short timeouts: this runs every
    /// few seconds from the watch. Any transport failure is `.offline`.
    static func probe(_ base: URL) async -> HubProbe {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 5
        config.timeoutIntervalForResource = 5
        config.waitsForConnectivity = false
        let quick = URLSession(configuration: config)
        var request = URLRequest(url: base.appendingPathComponent("api/onboarding"))
        request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        do {
            let (data, resp) = try await quick.data(for: request)
            return HubProbe.classify(status: (resp as? HTTPURLResponse)?.statusCode, body: data)
        } catch {
            return .offline
        }
    }

    // MARK: - one request shape

    enum Body { case none, json([String: Any]), form(String) }

    /// One call. Non-2xx answers come back as (status, body) — Home
    /// Assistant says meaningful things on a 403 ("already done") and the
    /// Supervisor on a 400 — only a transport failure throws.
    func call(_ method: String, _ path: String, body: Body = .none, token: String? = nil,
              timeout: TimeInterval = 30) async throws -> (status: Int, data: Data) {
        var request = URLRequest(url: base.appendingPathComponent(path))
        request.httpMethod = method
        request.timeoutInterval = timeout
        request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        if let token { request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        switch body {
        case .none: break
        case .json(let obj):
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONSerialization.data(withJSONObject: obj)
        case .form(let encoded):
            request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
            request.httpBody = Data(encoded.utf8)
        }
        do {
            let (data, resp) = try await session.data(for: request)
            return ((resp as? HTTPURLResponse)?.statusCode ?? 0, data)
        } catch {
            throw HubAPIError(message: "Couldn't reach the hub: \(error.localizedDescription)")
        }
    }

    private static func json(_ data: Data) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: data) as? [String: Any]) ?? [:]
    }

    // MARK: - onboarding

    func onboardingSteps() async throws -> [OnboardingStep] {
        let (status, data) = try await call("GET", "api/onboarding")
        guard status == 200, let steps = HubOnboarding.parseSteps(data) else {
            throw HubAPIError(message: "Couldn't read the hub's setup state (\(status)).")
        }
        return steps
    }

    /// `POST /api/onboarding/users`. Returns the status and the auth code
    /// when the hub created the account (200); a 403 means the slot was
    /// taken, and the caller falls through to the sign-in check.
    func createOwner(_ login: HubOwnerLogin) async throws -> (status: Int, code: String?, message: String?) {
        let (status, data) = try await call("POST", "api/onboarding/users",
                                            body: .json(HubOnboarding.userRequest(base: base, login: login)))
        return (status, status == 200 ? HubOnboarding.authCode(from: data) : nil,
                Self.json(data)["message"] as? String)
    }

    /// Exchange an auth code for tokens.
    func exchange(code: String) async throws -> HubSession {
        let (status, data) = try await call("POST", "auth/token",
                                            body: .form(HubOnboarding.tokenExchangeForm(base: base, code: code)))
        let v = Self.json(data)
        guard status == 200, let access = v["access_token"] as? String else {
            throw HubAPIError(message: "The hub wouldn't exchange its sign-in code (\(status)).")
        }
        return HubSession(accessToken: access, refreshToken: v["refresh_token"] as? String)
    }

    /// Best effort: leave no standing session behind.
    func revoke(_ session: HubSession) async {
        guard let refresh = session.refreshToken else { return }
        _ = try? await call("POST", "auth/token", body: .form(HubOnboarding.revokeForm(refreshToken: refresh)))
    }

    /// Finish one wizard page with its defaults. 200 or a 403 "already
    /// done" both converge.
    func finish(step: String, session: HubSession) async throws -> Bool {
        let body: Body = step == HubOnboarding.stepIntegration
            ? .json(HubOnboarding.integrationRequest(base: base)) : .json([:])
        let (status, _) = try await call("POST", "api/onboarding/\(step)", body: body, token: session.accessToken)
        return status == 200 || status == 403
    }

    /// Prove the typed credentials open this hub through the login flow.
    /// Returns the auth code on success, nil for wrong credentials; throws
    /// when the hub couldn't be asked.
    func loginCode(_ login: HubOwnerLogin) async throws -> String? {
        let l = login.normalized
        let (s1, d1) = try await call("POST", "auth/login_flow", body: .json([
            "client_id": HubOnboarding.clientID(base: base),
            "redirect_uri": HubOnboarding.redirectURI(base: base),
            "handler": ["homeassistant", NSNull()],
        ]))
        guard s1 == 200, let flowID = Self.json(d1)["flow_id"] as? String else {
            throw HubAPIError(message: "The hub refused to start a sign-in check (\(s1)).")
        }
        let (s2, d2) = try await call("POST", "auth/login_flow/\(flowID)", body: .json([
            "client_id": HubOnboarding.clientID(base: base),
            "username": l.username, "password": l.password,
        ]))
        guard s2 == 200 else { throw HubAPIError(message: "The hub's sign-in check failed (\(s2)).") }
        let v = Self.json(d2)
        guard v["type"] as? String == "create_entry" else { return nil }
        return v["result"] as? String
    }

    // MARK: - the Supervisor, through Home Assistant's proxy

    /// `GET /api/hassio/<path>` → the raw body (nil unless 200), for the
    /// snapshot the plan reads.
    func supervisorGET(_ path: String, session: HubSession) async -> Data? {
        guard let reply = try? await call("GET", "api/hassio/\(path)", token: session.accessToken),
              reply.status == 200 else { return nil }
        return reply.data
    }

    /// `POST /api/hassio/<path>`; the Supervisor answers `{"result":"ok"}`
    /// or `{"result":"error","message":…}`.
    func supervisorPOST(_ path: String, body: [String: Any]? = nil, session: HubSession,
                        timeout: TimeInterval = 120) async throws {
        let (status, data) = try await call("POST", "api/hassio/\(path)",
                                            body: body.map { .json($0) } ?? .json([:]),
                                            token: session.accessToken, timeout: timeout)
        let v = Self.json(data)
        if status == 200, (v["result"] as? String) != "error" { return }
        let message = (v["message"] as? String) ?? "the Supervisor refused (\(status))"
        throw HubAPIError(message: message)
    }

    /// `GET /api/config/config_entries/<path>` → body if 200.
    func coreGET(_ path: String, session: HubSession) async -> Data? {
        guard let reply = try? await call("GET", "api/\(path)", token: session.accessToken),
              reply.status == 200 else { return nil }
        return reply.data
    }

    /// `POST /api/config/config_entries/flow[/<id>]`.
    func flow(_ path: String, body: [String: Any], session: HubSession) async throws -> HubProvisionPlan.FlowOutcome {
        let (status, data) = try await call("POST", "api/config/config_entries/flow\(path)",
                                            body: .json(body), token: session.accessToken, timeout: 60)
        return HubProvisionPlan.flowOutcome(data, status: status)
    }
}

// MARK: - the converge, end to end

extension HubAPI {
    /// Read the hub's real state, finish what is missing, verify the login,
    /// and say plainly what (if anything) is left for a human. The session
    /// comes back so the provisioning plan can use it; the caller revokes
    /// it when done. Narration lines never carry a secret.
    func completeOnboarding(login: HubOwnerLogin, log: @escaping (String) -> Void) async throws
        -> (outcome: HubOnboardOutcome, session: HubSession?) {
        let steps = try await onboardingSteps()
        var out = HubOnboardOutcome()
        let pending = HubOnboarding.pending(steps)
        var session: HubSession?

        // Fully set up already — the Flasher got there first, or someone
        // clicked through the wizard. Verify the typed login so "you're
        // done, just sign in" is a checked fact, not a hope.
        if pending.isEmpty {
            log("Home Assistant is already set up — checking that your login opens it…")
            do {
                if let code = try await loginCode(login) {
                    out.loginVerified = true
                    session = try await exchange(code: code)
                    log("It does. Sign in any time.")
                } else {
                    out.note = "Home Assistant is set up, but the account you typed doesn't open it — it was probably created with different details. Sign in with the account that was actually created."
                }
            } catch {
                out.note = "Home Assistant is set up, but the login check didn't get an answer (\(error.localizedDescription)). Try signing in normally."
            }
            return (out, session)
        }

        if pending.contains(HubOnboarding.stepUser) {
            log("Creating your Home Assistant account on the hub…")
            let reply = try await createOwner(login)
            if reply.status == 200, let code = reply.code {
                session = try await exchange(code: code)
                out.createdUser = true
                out.loginVerified = true
                out.completed.append(HubOnboarding.stepUser)
                log("Account created — Home Assistant accepted the login.")
            } else {
                log("The hub declined to create the account (\(reply.status): \(reply.message ?? "no reason given")) — checking whether it already exists…")
            }
        }
        if session == nil {
            do {
                if let code = try await loginCode(login) {
                    out.loginVerified = true
                    session = try await exchange(code: code)
                } else {
                    out.remaining = pending
                    out.note = "Home Assistant's owner account already exists and isn't the one you typed, so this phone can't finish the setup pages for you. Sign in with the account that was created and the wizard walks you through the rest."
                    return (out, nil)
                }
            } catch {
                out.remaining = pending
                out.note = "Couldn't sign in to finish setup (\(error.localizedDescription)). Open the hub and its wizard walks you through the same steps."
                return (out, nil)
            }
        }
        guard let token = session else { return (out, nil) }

        let (finishable, unknown) = HubOnboarding.split(pending: pending)
        for step in finishable {
            log("Finishing the \(step.replacingOccurrences(of: "_", with: " ")) page…")
            if try await finish(step: step, session: token) {
                out.completed.append(step)
            } else {
                out.remaining.append(step)
            }
        }
        out.remaining.append(contentsOf: unknown)
        if !out.remaining.isEmpty {
            out.note = "Home Assistant still has a setup page waiting in the browser: " + out.remaining.joined(separator: ", ") + "."
        }
        return (out, token)
    }
}
