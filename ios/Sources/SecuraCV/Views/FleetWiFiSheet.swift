// FleetWiFiSheet.swift
//
// "The Wi-Fi password changed" as one flow instead of N device chores.
//
// The screen is the plan made visible: who gets the update and over which
// path, who goes FIRST and why, who needs hands-on and is told so up front.
// The staging is the safety story (FleetWiFiRollout): one pilot proves the
// password before anything else is touched, so a typo strands one Canary —
// which keeps its Bluetooth rescue — never the fleet. Progress is honest
// per-device truth, and the closing line claims exactly what was proven.

import SwiftUI

struct FleetWiFiSheet: View {
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss

    @StateObject private var runner: FleetWiFiRunner
    @State private var plan: FleetWiFiRollout.Plan?
    @State private var ssid = ""
    @State private var password = ""
    @State private var showPassword = false
    @State private var problem: String?
    /// The one-time cleartext disclosure (FleetWiFiRollout), acknowledged
    /// for THIS run only — it is never remembered across sheets.
    @State private var cleartextApproved = false

    init(store: FleetStore) {
        _runner = StateObject(wrappedValue: FleetWiFiRunner(devices: store.devices,
                                                            ble: store.ble))
    }

    var body: some View {
        NavigationStack {
            List {
                if let plan {
                    if !runner.running && !runner.finished {
                        credentialForm
                        planSections(plan)
                    } else {
                        progressSections(plan)
                    }
                } else {
                    Section {
                        ProgressView("Looking at the fleet…")
                            .frame(maxWidth: .infinity)
                    }
                }
            }
            .navigationTitle("Fleet Wi-Fi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(runner.finished ? "Done" : "Cancel") { dismiss() }
                        .disabled(runner.running)
                }
            }
            .task { plan = FleetWiFiRollout.plan(store.wifiRolloutCandidates()) }
            .interactiveDismissDisabled(runner.running)
        }
    }

    // MARK: - the form

    private var credentialForm: some View {
        Section {
            TextField("Network name (SSID)", text: $ssid)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            HStack {
                if showPassword {
                    TextField("Password", text: $password)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } else {
                    SecureField("Password", text: $password)
                }
                Button {
                    showPassword.toggle()
                } label: {
                    Image(systemName: showPassword ? "eye.slash" : "eye")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }
            if let problem {
                Label(problem, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(Theme.color(.warn))
            }
            Button {
                start()
            } label: {
                Label("Update the fleet", systemImage: "wifi")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(ssid.trimmingCharacters(in: .whitespaces).isEmpty
                      || (plan?.pushTargets.isEmpty ?? true)
                      || ((plan?.needsCleartextDisclosure ?? false) && !cleartextApproved))
        } header: {
            Text("The new network")
        } footer: {
            Text("One Canary goes first and has to actually come back on the "
               + "new network before the rest are touched — a mistyped "
               + "password can never strand the whole fleet. The password "
               + "goes only to your Canaries — over bonded Bluetooth, over a "
               + "secure connection when the Canary has a certificate, or "
               + "over plain Wi-Fi only after you say so below; it is not "
               + "stored on this iPhone.")
        }
    }

    /// The cleartext disclosure: shown only when a push would ride plain
    /// http, and the run cannot start until its switch is on. The copy says
    /// exactly what crosses the network and the two ways to avoid it.
    private func cleartextSection(_ plan: FleetWiFiRollout.Plan) -> some View {
        Section {
            Toggle(isOn: $cleartextApproved) {
                Label(FleetWiFiRollout.cleartextAcknowledgment, systemImage: "lock.open")
            }
            ForEach(plan.cleartextTargets) { c in
                candidateRow(c, note: FleetWiFiRollout.cleartextNote)
            }
        } header: {
            Text("Unencrypted")
        } footer: {
            Text(FleetWiFiRollout.cleartextDisclosure)
        }
    }

    private func start() {
        guard let plan else { return }
        let trimmed = ssid.trimmingCharacters(in: .whitespacesAndNewlines)
        if let complaint = FleetWiFiRollout.credentialProblem(ssid: trimmed, password: password) {
            problem = complaint
            return
        }
        problem = nil
        let pass = password
        let approved = cleartextApproved
        Task {
            await runner.run(plan: plan, ssid: trimmed, password: pass,
                             cleartextApproved: approved)
            // The rollout may have moved devices across networks — let the
            // fleet fold catch up right away rather than at the next cycle.
            await store.refreshOnce()
        }
    }

    // MARK: - the plan, before it runs

    /// The per-row note for a lane, so the plan says which wire each
    /// password ride takes before anything is sent.
    private func laneNote(_ c: FleetWiFiRollout.Candidate) -> String? {
        switch c.path {
        case .ble: return "Bluetooth — bonded, encrypted"
        case .http: return "Secure connection — pinned to its certificate"
        case .httpCleartext: return FleetWiFiRollout.cleartextNote
        case .handsOn, .unreachable: return nil
        }
    }

    @ViewBuilder
    private func planSections(_ plan: FleetWiFiRollout.Plan) -> some View {
        if plan.needsCleartextDisclosure {
            cleartextSection(plan)
        }
        if let pilot = plan.pilot {
            Section {
                candidateRow(pilot, note: "Goes first — proves the password"
                             + (laneNote(pilot).map { " · " + $0 } ?? ""))
                ForEach(plan.followers) { c in
                    candidateRow(c, note: laneNote(c))
                }
            } header: {
                Text("Gets the update")
            } footer: {
                if plan.followers.contains(where: { $0.path == .ble }) {
                    Text("A Canary on the Bluetooth lane takes the new password "
                       + "over its bonded link — stand within a room or two of it.")
                }
            }
        } else if !plan.handsOn.isEmpty || !plan.unreachable.isEmpty {
            Section {
                Label("No Canary can take the update from here right now.",
                      systemImage: "wifi.exclamationmark")
                    .foregroundStyle(.secondary)
            }
        }
        if !plan.handsOn.isEmpty {
            Section {
                ForEach(plan.handsOn) { c in
                    candidateRow(c, note: nil)
                }
            } header: {
                Text("Needs hands-on")
            } footer: {
                Text("Displays store Wi-Fi only through their own setup "
                   + "screen: unplug the display, plug it back in, and it "
                   + "opens its setup network when it can't join — join that "
                   + "network from this iPhone and type the new password there.")
            }
        }
        if !plan.unreachable.isEmpty {
            Section("Not reachable right now") {
                ForEach(plan.unreachable) { c in
                    candidateRow(c, note: "Come back when it's in Bluetooth range")
                }
            }
        }
    }

    private func laneSymbol(_ c: FleetWiFiRollout.Candidate) -> String {
        switch c.path {
        case .ble: return "dot.radiowaves.up.forward"
        case .http: return "lock.wifi"
        case .httpCleartext: return "lock.open"
        case .handsOn, .unreachable: return "wifi"
        }
    }

    private func candidateRow(_ c: FleetWiFiRollout.Candidate, note: String?) -> some View {
        HStack(spacing: Theme.m) {
            Image(systemName: laneSymbol(c))
                .foregroundStyle(.secondary)
                .imageScale(.small)
            VStack(alignment: .leading, spacing: 2) {
                Text(c.name)
                if let note {
                    Text(note).font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
        }
    }

    // MARK: - progress + verdicts

    @ViewBuilder
    private func progressSections(_ plan: FleetWiFiRollout.Plan) -> some View {
        Section {
            ForEach(plan.pushTargets) { c in
                progressRow(c)
            }
        } header: {
            Text(runner.finished ? "How it went" : "Updating…")
        } footer: {
            if runner.finished {
                Text(closingLine(plan))
            }
        }
        if !plan.handsOn.isEmpty {
            Section("Still needs hands-on") {
                ForEach(plan.handsOn) { c in
                    progressRow(c)
                }
            }
        }
    }

    private func progressRow(_ c: FleetWiFiRollout.Candidate) -> some View {
        HStack(spacing: Theme.m) {
            stateIcon(runner.state(for: c.id))
            VStack(alignment: .leading, spacing: 2) {
                Text(c.name)
                Text(stateLine(runner.state(for: c.id)))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
    }

    @ViewBuilder
    private func stateIcon(_ state: FleetWiFiRollout.StepState) -> some View {
        switch state {
        case .waiting:
            Image(systemName: "clock").foregroundStyle(.secondary)
        case .sending, .confirming:
            ProgressView().controlSize(.small)
        case .moved:
            Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.color(.calm))
        case .failed:
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.color(.warn))
        case .handsOn:
            Image(systemName: "hand.point.right").foregroundStyle(.secondary)
        }
    }

    private func stateLine(_ state: FleetWiFiRollout.StepState) -> String {
        switch state {
        case .waiting: return FleetWiFiRollout.waitingReason
        case .sending: return "Handing over the new password…"
        case .confirming: return "Watching for it on the new network…"
        case .moved: return "On the new network — answered."
        case .failed(let why): return why
        case .handsOn: return "Takes the password through its own setup screen."
        }
    }

    private func closingLine(_ plan: FleetWiFiRollout.Plan) -> String {
        let total = plan.pushTargets.count
        let moved = runner.movedCount
        if moved == total && total > 0 {
            return "All \(total) answered on the new network. Every check here is a device that actually came back — not a hope."
        }
        return "\(moved) of \(total) answered on the new network. The rows above say exactly what happened to the rest."
    }
}
