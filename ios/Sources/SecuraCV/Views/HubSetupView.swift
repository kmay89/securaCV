// HubSetupView.swift
//
// The hub walkthrough, rendered from SetupGuide.hub with the three live
// parts under their steps: the first-boot watch (found / preparing /
// ready), the owner-account form, and the provisioning plan narrated per
// action. The engine is HubSetupRunner; this view only shows its state and
// hands it the two things a person supplies — a hub address if discovery
// is blocked, and the account.

import SwiftUI

struct HubSetupView: View {
    @EnvironmentObject var store: FleetStore

    /// The runner is the store's, not this screen's: closing the
    /// walkthrough mid-boot keeps the watch, its clock, and the typed
    /// account (HubSetupRunner's drafts).
    var body: some View {
        HubSetupContent(runner: store.hubRunner)
    }
}

private struct HubSetupContent: View {
    @ObservedObject var runner: HubSetupRunner
    @State private var formProblem: String?

    var body: some View {
        List {
            ForEach(Array(SetupGuide.hub.enumerated()), id: \.element.id) { i, step in
                SetupStepSection(index: i + 1, step: step) {
                    switch step.action {
                    case .watchHub: watchControl
                    case .ownerAccount: accountControl
                    case .provisionHub: provisionControl
                    default: EmptyView()
                    }
                }
            }
            if !runner.log.isEmpty {
                Section("What this phone did") {
                    ForEach(Array(runner.log.enumerated()), id: \.offset) { _, line in
                        Text(line).font(.caption.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }
        }
        .navigationTitle("A hub")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            // Opening the walkthrough starts nothing: step 3's "Start
            // watching" does, so iOS's Local Network alert never comes
            // before the person reaches the step that explains it. A watch
            // already in progress is resumed if its loop is not running.
            // Leaving the screen does not stop it — the runner outlives the
            // screen, and the "Your hub is ready" notice and the finish
            // happen with the screen closed (HubSetupRunner.hubAnswered).
            // A rotation asked in Set up after the hub was finished in this
            // app session returns the runner to the start, so the re-mint
            // can run (a finished runner offers no Start or Finish).
            runner.resetForRotationIfAsked()
            let host = runner.draftTypedHost
            runner.resumeWatching(typedHost: host.isEmpty ? nil : host)
        }
    }

    // MARK: - the watch

    private var ready: Bool {
        if case .watching(.ready) = runner.phase { return true }
        return runner.phase == .onboarding || runner.phase == .provisioning || runner.phase == .done
    }

    @ViewBuilder private var watchControl: some View {
        switch runner.phase {
        case .idle:
            Button("Start watching") {
                let host = runner.draftTypedHost
                runner.startWatching(typedHost: host.isEmpty ? nil : host)
            }
            .buttonStyle(.borderedProminent)
        case .watching(let probe):
            HStack(spacing: Theme.s) {
                if probe == .ready {
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.color(.calm))
                } else {
                    ProgressView()
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(HubSetupRunner.line(for: probe)).font(.subheadline)
                    if let url = runner.hubURL {
                        Text(url.host ?? url.absoluteString).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if probe != .ready, let advice = SetupRadioAdvice.localNetwork(blocked: runner.discovery.localNetworkBlocked) {
                // The browse can't see anything at all — say so now, not
                // after 25 minutes of a spinner.
                RadioAdviceRow(advice: advice)
            }
            if runner.escalated {
                Text("Nothing has answered in 25 minutes. Check the Pi's light is blinking, that it is on the same Wi-Fi as this phone (or on ethernet), and look for \"homeassistant\" in your router's client list — then type its address here.")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
            }
            if runner.needsHubChoice {
                // Several Home Assistants answer on this network. Nothing
                // is dialed — not the account, not an install — until the
                // person says which one is theirs.
                Text("More than one Home Assistant is on this network. Which one is the hub you just flashed?")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                Picker("Hub", selection: $runner.selectedHubID) {
                    Text("Choose…").tag(String?.none)
                    ForEach(runner.discovery.found.filter { $0.baseURL != nil }) { hub in
                        Text("\(hub.name) · \(hub.baseURL?.host ?? "") · \(hub.installationType.isEmpty ? "Home Assistant" : hub.installationType) \(hub.version)")
                            .tag(String?.some(hub.id))
                    }
                }
                .pickerStyle(.inline)
                .labelsHidden()
            } else if !runner.discovery.found.isEmpty && probe != .ready {
                ForEach(runner.discovery.found) { hub in
                    Label("\(hub.name) · \(hub.installationType.isEmpty ? "Home Assistant" : hub.installationType) \(hub.version)",
                          systemImage: "house")
                        .font(.caption)
                }
            }
            if probe != .ready {
                HStack {
                    TextField("Hub address, if it isn't found (an IP works)", text: $runner.draftTypedHost)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    Button("Use") { runner.startWatching(typedHost: runner.draftTypedHost) }
                        .disabled(runner.draftTypedHost.trimmingCharacters(in: .whitespaces).isEmpty)
                }
                Button("Stop watching", role: .destructive) { runner.cancelWatching() }
                    .font(.footnote)
            }
        default:
            Label("Home Assistant answered at \(runner.hubURL?.host ?? "the hub").", systemImage: "checkmark.circle")
                .font(.subheadline)
        }
    }

    // MARK: - the account

    @ViewBuilder private var accountControl: some View {
        if let outcome = runner.onboardOutcome, outcome.loginVerified {
            Label(outcome.createdUser ? "Account created and the login checked." : "Your login opens the hub.",
                  systemImage: "checkmark.circle")
                .foregroundStyle(Theme.color(.calm))
        } else {
            TextField("Your name", text: $runner.draftName)
            TextField("Username", text: $runner.draftUsername)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            HStack {
                // This one IS a new credential — the Home Assistant owner
                // account being created on the hub — so it says so, and
                // `.newPassword` lets iOS offer a strong one. It is the only
                // password a person creates anywhere in setup; every Wi-Fi
                // field is an existing network's password and is annotated
                // `.password` instead.
                if runner.draftShowPassword {
                    TextField("New hub account password", text: $runner.draftPassword)
                        .textContentType(.newPassword)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                } else {
                    SecureField("New hub account password", text: $runner.draftPassword)
                        .textContentType(.newPassword)
                }
                Button { runner.draftShowPassword.toggle() } label: {
                    Image(systemName: runner.draftShowPassword ? "eye.slash" : "eye").foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }
            if !runner.draftShowPassword {
                SecureField("Confirm hub account password", text: $runner.draftConfirm)
                    .textContentType(.newPassword)
            }
            if let formProblem {
                Label(formProblem, systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
            }
        }
    }

    private var login: HubOwnerLogin { runner.draftLogin }

    /// The confirm field exists only while the password is hidden.
    private var passwordsMatch: Bool { runner.draftPasswordsMatch }

    // MARK: - finishing

    @ViewBuilder private var provisionControl: some View {
        switch runner.phase {
        case .idle, .watching:
            Button {
                startFinish()
            } label: {
                Label("Finish the hub from this phone", systemImage: "wand.and.stars")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!ready)
            if !ready {
                Text("Fill in the account above and this phone finishes the hub the moment it answers.")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        case .onboarding:
            HStack { ProgressView(); Text("Finishing Home Assistant's setup…") }
        case .provisioning, .done, .failed:
            ForEach(runner.steps) { step in
                VStack(alignment: .leading, spacing: 2) {
                    Text(step.title).font(.subheadline.weight(.medium))
                    ForEach(step.actions) { action in
                        stateRow(label: HubProvisionPlan.label(for: action.action),
                                 state: runner.stepState[action.id] ?? .planned)
                    }
                }
                .padding(.vertical, 2)
            }
            if case .failed(let why) = runner.phase {
                Label(why, systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                Button("Try again") { startFinish() }.buttonStyle(.bordered)
            }
            if runner.phase == .done {
                Label("Done — your hub is finished.", systemImage: "checkmark.seal")
                    .foregroundStyle(Theme.color(.calm))
            }
            if !runner.leftovers.isEmpty {
                VStack(alignment: .leading, spacing: Theme.xs) {
                    Text("Still yours").font(.subheadline.weight(.medium))
                    ForEach(Array(runner.leftovers.enumerated()), id: \.offset) { _, line in
                        Label(line, systemImage: "hand.point.right")
                            .font(.footnote).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
    }

    private func stateRow(label: String, state: HubSetupRunner.StepState) -> some View {
        HStack(alignment: .top, spacing: Theme.inline) {
            Group {
                switch state {
                case .planned: Image(systemName: "circle").foregroundStyle(.secondary)
                case .running: ProgressView().controlSize(.mini)
                case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.color(.calm))
                case .skipped: Image(systemName: "checkmark.circle").foregroundStyle(.secondary)
                case .failed: Image(systemName: "xmark.circle").foregroundStyle(Theme.color(.warn))
                case .yours: Image(systemName: "hand.point.right").foregroundStyle(Theme.color(.info))
                }
            }
            .frame(width: 18)
            VStack(alignment: .leading, spacing: 1) {
                Text(label).font(.caption)
                switch state {
                case .skipped(let why), .failed(let why), .yours(let why):
                    Text(why).font(.caption2).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                default: EmptyView()
                }
            }
        }
    }

    private func startFinish() {
        if let problem = login.problem() {
            formProblem = problem
            return
        }
        if !passwordsMatch {
            formProblem = "The two passwords don't match."
            return
        }
        formProblem = nil
        let l = login
        Task { await runner.finish(login: l) }
    }
}

#if DEBUG
#Preview("Hub walkthrough") {
    NavigationStack { HubSetupView() }.environmentObject(DemoFleet.previewStore())
}
#endif
