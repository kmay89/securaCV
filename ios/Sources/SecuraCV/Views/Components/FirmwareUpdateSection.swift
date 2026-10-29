// FirmwareUpdateSection.swift
//
// "Software update" on one Canary's screen: what it runs, whether something
// newer is out, and one tap to install it — the device does the work and the
// phone watches (FirmwareUpdate.swift has the contract and the honesty
// rules). Mounted by DeviceDetailView for every display (open on the home
// network) and for a paired WAP (its bearer token, its pinned https).
//
// The poll follows SensingNowSection's while-visible idiom: `.task(id:)` is
// canceled when the screen goes away, so nothing runs in the background.
// It reads once on appear, then polls only while a run is in flight or a
// restart is being watched — an idle Canary is not asked anything twice.

import SwiftUI

struct FirmwareUpdateSection: View {
    let transport: any FirmwareTransport
    /// How the confirmation names the device ("Kitchen Glance").
    let deviceName: String

    @State private var status: FirmwareStatus?
    @State private var unsupported = false
    @State private var misses = 0
    @State private var asking = false
    @State private var note: String?
    @State private var confirmInstall = false
    /// Set when an install is requested: the version it is replacing. While
    /// set, the poll is watching a run through to the restart and back.
    @State private var replacing: String?
    @State private var sawRestart = false
    /// When the Canary went quiet or said Rebooting — the restart's clock.
    /// Started there, not at the install request: a slow download that is
    /// still answering with progress is never timed out as a lost restart.
    @State private var restartSince: ContinuousClock.Instant?
    @State private var outcome: FirmwareOutcome?
    @State private var restartTimedOut = false
    /// Bumped to restart the poll after a kick.
    @State private var pollToken = 0
    /// When the device accepted a check or install. The 200 arrives before
    /// its engine leaves Idle, so the first read after a kick can still say
    /// Idle; until a working step shows (or the grace runs out) the poll
    /// keeps reading instead of taking that stale Idle as the result.
    @State private var kickedAt: ContinuousClock.Instant?

    private var phase: FirmwarePhase? { status.map(FirmwareUpdate.phase) }
    /// How long a kicked run may take to show its first working step.
    private static let kickGrace: Duration = .seconds(10)
    private var watching: Bool { replacing != nil }

    var body: some View {
        Section {
            if unsupported {
                Text("This Canary's software is older than updates from the phone. Update it once from a computer with the SecuraCV Flasher; after that, updates happen here.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else if let status {
                LabeledContent("Software", value: status.installed.isEmpty ? "Unknown" : status.installed)
                if let outcome { outcomeRow(outcome) }
                if kickedAt != nil && !runShowing {
                    // The device said yes; its engine hasn't left Idle yet.
                    HStack {
                        Text("Starting")
                        Spacer()
                        ProgressView()
                    }
                    .foregroundStyle(.secondary)
                } else {
                    phaseRows(status)
                }
                if let note {
                    Label(note, systemImage: "info.circle")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let notes = status.releaseNotes, phase.map(isAvailable) == true {
                    DisclosureGroup("What's new") {
                        Text(notes)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            } else if misses >= 3 {
                Text("Not reachable right now. It has to be on the same Wi-Fi as this phone.")
                    .foregroundStyle(.secondary)
            } else {
                HStack {
                    Text("Asking it what it runs")
                    Spacer()
                    ProgressView()
                }
                .foregroundStyle(.secondary)
            }
        } header: {
            Text("Software update")
        } footer: {
            Text(footerText)
        }
        .task(id: pollToken) { await poll() }
        .confirmationDialog(installTitle, isPresented: $confirmInstall, titleVisibility: .visible) {
            Button("Install") { Task { await install() } }
            Button("Not now", role: .cancel) {}
        } message: {
            Text("It downloads the update itself, checks its signature, and restarts on its own — usually back in about a minute. If the new version fails its health check, it goes back to the current one by itself.")
        }
    }

    // MARK: - rows

    @ViewBuilder private func phaseRows(_ s: FirmwareStatus) -> some View {
        switch FirmwareUpdate.phase(s) {
        case .notChecked:
            checkButton("Check for update")
        case .upToDate:
            HStack {
                Label("Up to date", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(Theme.color(.calm))
                Spacer()
                checkButton("Check again").font(.footnote)
            }
            if let lingering = FirmwareUpdate.lingeringNote(s) { lingeringRow(lingering) }
        case .available(let version):
            Label("Version \(version) is ready", systemImage: "arrow.down.circle.fill")
                .foregroundStyle(Theme.color(.info))
            Button {
                confirmInstall = true
            } label: {
                Label("Install \(version)", systemImage: "square.and.arrow.down")
            }
            .buttonStyle(.borderedProminent)
            .disabled(asking)
            if let lingering = FirmwareUpdate.lingeringNote(s) { lingeringRow(lingering) }
        case .working(let step, let progress):
            VStack(alignment: .leading, spacing: Theme.xs) {
                Text(step.title)
                if let progress {
                    ProgressView(value: Double(progress), total: 100)
                    Text("\(progress)%").font(.caption).foregroundStyle(.secondary)
                } else {
                    ProgressView()
                }
            }
        case .restarting:
            restartingRow
        case .failed(let sentence):
            if case .rolledBack? = outcome {
                // The outcome row above already says it, in the same words.
                checkButton("Check again")
            } else {
                Label(sentence, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(Theme.color(.warn))
                    .fixedSize(horizontal: false, vertical: true)
                checkButton("Try again")
            }
        }
        if restartTimedOut {
            Text("It hasn't answered since it restarted. Give it a minute, check it has power, then open this screen again.")
                .font(.footnote)
                .foregroundStyle(Theme.color(.warn))
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var restartingRow: some View {
        HStack {
            Text(replacing.map { _ in "Restarting into the new version" } ?? "Restarting")
            Spacer()
            ProgressView()
        }
        .foregroundStyle(.secondary)
    }

    private func lingeringRow(_ text: String) -> some View {
        Text(text)
            .font(.footnote)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder private func outcomeRow(_ o: FirmwareOutcome) -> some View {
        switch o {
        case .updated(let version):
            Label("Updated to \(version)", systemImage: "checkmark.seal.fill")
                .foregroundStyle(Theme.color(.calm))
        case .rolledBack(let still, let reason):
            VStack(alignment: .leading, spacing: Theme.xs) {
                Label("Still on \(still)", systemImage: "arrow.uturn.backward.circle")
                    .foregroundStyle(Theme.color(.warn))
                Text(reason)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func checkButton(_ title: String) -> some View {
        Button(title) { Task { await check() } }
            .disabled(asking)
    }

    private var installTitle: String {
        if case .available(let v)? = phase { return "Install \(v) on \(deviceName)?" }
        return "Install the update on \(deviceName)?"
    }

    private var footerText: String {
        var parts = ["The Canary fetches updates itself and installs one only if its signature checks out against the release key it was built with. This phone only asks and watches."]
        if let auto = status?.autoUpdate {
            parts.append(auto ? "Automatic updates are on for this Canary." : "Automatic updates are off for this Canary.")
        }
        return parts.joined(separator: " ")
    }

    /// A working step or the restart is what the device last said.
    private var runShowing: Bool {
        switch phase {
        case .working?, .restarting?: return true
        default: return false
        }
    }

    private func isAvailable(_ p: FirmwarePhase) -> Bool {
        if case .available = p { return true }
        return false
    }

    // MARK: - actions

    private func check() async {
        asking = true
        note = nil
        outcome = nil
        defer { asking = false }
        let answer = try? await transport.kick(install: false)
        await handle(kick: answer)
    }

    private func install() async {
        guard let s = status else { return }
        asking = true
        note = nil
        outcome = nil
        restartTimedOut = false
        defer { asking = false }
        let answer = try? await transport.kick(install: true)
        if answer == .started || answer == .busy {
            replacing = s.installed
            sawRestart = false
            restartSince = nil
        }
        await handle(kick: answer)
    }

    private func handle(kick answer: FirmwareKick?) async {
        switch answer {
        case .started?:
            kickedAt = ContinuousClock.now
            pollToken += 1
        case .busy?:
            note = "An update is already running on it — following along."
            kickedAt = ContinuousClock.now
            pollToken += 1
        case .unsupported?:
            unsupported = true
        case .refused(let sentence)?:
            note = sentence
        case nil:
            note = "Couldn't reach it. Make sure this phone is on the same Wi-Fi, then try again."
        }
    }

    // MARK: - the poll

    private func poll() async {
        while !Task.isCancelled {
            let read: FirmwareStatus??
            do { read = .some(try await transport.status()) } catch { read = nil }

            switch read {
            case .some(nil):
                unsupported = true
                return
            case .some(let s?):
                misses = 0
                absorb(s)
            case nil:
                misses += 1
                if watching { markRestart() }
            }

            if let wait = nextWait() {
                try? await Task.sleep(for: wait)
            } else {
                return
            }
        }
    }

    /// Fold one successful read into the screen, finishing a watched run
    /// once the Canary is idle again.
    private func absorb(_ s: FirmwareStatus) {
        status = s
        switch FirmwareUpdate.phase(s) {
        case .working, .restarting: kickedAt = nil  // the run has shown itself
        default: break
        }
        guard let before = replacing else { return }
        switch FirmwareUpdate.phase(s) {
        case .restarting:
            markRestart()
        case .working:
            break
        case .failed:
            // A previous run's Error can still be on the wire right after
            // the kick; only a failure after the run showed itself counts.
            if kickedAt != nil { break }
            // After a restart this is the usual rollback read (no manifest
            // yet, the health-check error kept): say what it came back on.
            // Before one, the engine stopped and its sentence is on screen.
            if sawRestart { outcome = FirmwareUpdate.outcome(before: before, after: s) }
            replacing = nil
        default:
            // Still the pre-run Idle: the engine hasn't started yet.
            if kickedAt != nil && !sawRestart && s.installed == before { break }
            if sawRestart || s.installed != before {
                outcome = FirmwareUpdate.outcome(before: before, after: s)
            }
            // An install that found nothing newer ends here too, idle and
            // unchanged, with the phase saying "Up to date".
            replacing = nil
        }
    }

    private func markRestart() {
        sawRestart = true
        if restartSince == nil { restartSince = ContinuousClock.now }
    }

    /// The next poll's delay, or nil to stop until the person acts.
    private func nextWait() -> Duration? {
        if let k = kickedAt, ContinuousClock.now - k >= Self.kickGrace {
            kickedAt = nil  // no run showed up; take what the device says
        }
        if watching {
            if let started = restartSince,
               ContinuousClock.now - started > FirmwareUpdate.restartPatience {
                restartTimedOut = true
                replacing = nil
                return nil
            }
            if kickedAt != nil { return .seconds(1) }
            return phase.flatMap(FirmwareUpdate.pollInterval) ?? .seconds(3)
        }
        if kickedAt != nil { return .seconds(1) }
        if status == nil { return misses < 3 ? .seconds(2) : nil }
        return phase.flatMap(FirmwareUpdate.pollInterval)
    }
}
