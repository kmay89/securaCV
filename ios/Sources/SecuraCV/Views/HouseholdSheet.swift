// HouseholdSheet.swift
//
// "If nobody answers." The screen where an owner lets a second person be
// told, and where someone who accepted an invitation can see exactly what
// they signed up for.
//
// The copy carries most of the weight here, because this is the one place in
// the app where a user is deciding what somebody ELSE gets to know. Two rules
// it follows:
//
//   * Say what they can't see, not just what they can. "They'll be told an
//     alarm went unanswered" is only half the sentence; the half that earns
//     the tap is "they can't see your Canaries, your names, your history or
//     any footage."
//   * Never count an invitation as a person. Somebody who hasn't accepted
//     reaches nobody, and the summary line says so (HouseholdRelay.summary) —
//     "2 people are told" while one of them never tapped the link is the
//     comfortable lie this whole app is built not to tell.

import SwiftUI
#if canImport(CloudKit)
import CloudKit
#endif
#if canImport(UIKit)
import UIKit
#endif

struct HouseholdSheet: View {
    @ObservedObject private var household = HouseholdShare.shared
    @Environment(\.dismiss) private var dismiss
    @State private var showingSharing = false
    @State private var confirmingStop = false
    @State private var working = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Text(HouseholdRelay.summary(household.members))
                        .font(.subheadline)
                } header: {
                    Text("If nobody answers")
                } footer: {
                    // The rationing, stated where someone is deciding whether
                    // to turn it on. A second person's phone is only worth
                    // carrying if it stays quiet.
                    Text("Only an alarm that has gone unanswered reaches them — never your everyday alerts, and never more than once for the same alarm.")
                }

                // The condition the whole ladder rests on, said before it is
                // relied on rather than discovered the night it mattered.
                Section {
                    Label(HouseholdRelay.watchRequirement, systemImage: "eye")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                if case .unavailable(let why) = household.state {
                    Section {
                        Label(why, systemImage: "exclamationmark.triangle")
                            .font(.footnote)
                            .foregroundStyle(Theme.color(.warn))
                    }
                }

                if !household.members.isEmpty {
                    Section("Who") {
                        ForEach(household.members) { member in
                            HouseholdMemberRow(member: member)
                        }
                    }
                }

                Section {
                    Button {
                        Task {
                            working = true
                            if await household.prepareShare() { showingSharing = true }
                            working = false
                        }
                    } label: {
                        Label(household.members.isEmpty ? "Invite someone…" : "Invite someone else…",
                              systemImage: "person.badge.plus")
                    }
                    .disabled(working)

                    if household.state.isSharing, !household.members.isEmpty {
                        Button(role: .destructive) { confirmingStop = true } label: {
                            Label("Stop telling anyone", systemImage: "person.slash")
                        }
                    }
                } footer: {
                    Text(HouseholdRelay.invitationExplanation)
                }

                if household.isHelpingSomeone {
                    Section {
                        if let blocked = household.participantBlocked {
                            // Being on the share is not the same as being
                            // reachable, and only this device can know the
                            // difference. The owner's screen can't see it.
                            Label(blocked, systemImage: "exclamationmark.triangle")
                                .font(.footnote)
                                .foregroundStyle(Theme.color(.warn))
                        } else {
                            Label("You'll be told if an alarm there goes unanswered.",
                                  systemImage: "bell.badge")
                                .font(.footnote)
                        }
                    } header: {
                        Text("You help watch someone's fleet")
                    } footer: {
                        Text("You can see that an alarm wasn't answered — nothing else. Their Canaries, names, history and footage stay theirs. To stop, remove yourself from the share in the Files or iCloud sharing sheet they sent you.")
                    }
                }
            }
            .navigationTitle("Household")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .confirmationDialog("Stop telling anyone?", isPresented: $confirmingStop,
                                titleVisibility: .visible) {
                Button("Stop sharing", role: .destructive) {
                    Task { await household.stopSharing() }
                }
            } message: {
                Text("Everyone you invited loses access immediately. Your own alerts are unaffected.")
            }
            .sheet(isPresented: $showingSharing, onDismiss: {
                // Acceptance happens on someone else's device, so the roster
                // is only ever learned by asking again.
                Task { await household.refreshMembers() }
            }) {
                sharingSheet
            }
            .task {
                await household.refreshMembers()
                await household.refreshParticipation()
            }
        }
    }

    /// Apple's own sharing sheet does the invitation, the link, and the
    /// revoke. We hand it a share and stay out of the way — there is no
    /// invitation state of ours to leak, because there is no invitation state
    /// of ours at all.
    @ViewBuilder private var sharingSheet: some View {
        #if canImport(UIKit) && canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        if let share = household.activeShare {
            CloudSharingSheet(share: share, container: CloudContainer.shared)
        } else {
            HouseholdSheetFallback(text: "iCloud couldn't set up sharing. Try again in a moment.")
        }
        #else
        HouseholdSheetFallback(text: "Telling someone else needs iCloud.")
        #endif
    }
}

/// Shown instead of the sharing sheet when there is nothing to share — a
/// plain sentence rather than an empty screen the user has to guess about.
struct HouseholdSheetFallback: View {
    let text: String
    var body: some View {
        VStack(spacing: Theme.m) {
            Image(systemName: "icloud.slash").font(.largeTitle).foregroundStyle(.secondary)
            Text(text).font(.subheadline).multilineTextAlignment(.center)
        }
        .padding(Theme.l)
    }
}

struct HouseholdMemberRow: View {
    let member: HouseholdMember

    var body: some View {
        HStack(spacing: Theme.s) {
            Image(systemName: member.isOwner ? "person.fill" : "person")
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
            Text(member.name)
            Spacer(minLength: Theme.s)
            Text(statusLabel)
                .font(.caption)
                .foregroundStyle(member.status == .accepted ? Theme.color(.calm) : .secondary)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(member.name), \(statusLabel)")
    }

    private var statusLabel: String {
        switch member.status {
        case .owner: return "You"
        case .accepted: return "Can be told"
        case .invited: return "Hasn't joined yet"
        }
    }
}

#if canImport(UIKit) && canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
/// Apple's sharing controller, wrapped. Read-only participation on purpose:
/// a household member never writes anything back, so the share grants nothing
/// that could be used to change the owner's data.
struct CloudSharingSheet: UIViewControllerRepresentable {
    let share: CKShare
    let container: CKContainer

    func makeUIViewController(context: Context) -> UICloudSharingController {
        let controller = UICloudSharingController(share: share, container: container)
        controller.delegate = context.coordinator
        controller.availablePermissions = [.allowPrivate, .allowReadOnly]
        return controller
    }

    func updateUIViewController(_ controller: UICloudSharingController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject, UICloudSharingControllerDelegate {
        func itemTitle(for controller: UICloudSharingController) -> String? {
            // What the invited person sees before they accept. It names the
            // ONE thing they are being asked to receive.
            "Unanswered alarms"
        }

        func cloudSharingController(_ controller: UICloudSharingController,
                                    failedToSaveShareWithError error: Error) {
            Task { @MainActor in await HouseholdShare.shared.refreshMembers() }
        }

        func cloudSharingControllerDidSaveShare(_ controller: UICloudSharingController) {
            Task { @MainActor in await HouseholdShare.shared.refreshMembers() }
        }

        func cloudSharingControllerDidStopSharing(_ controller: UICloudSharingController) {
            Task { @MainActor in await HouseholdShare.shared.refreshMembers() }
        }
    }
}
#endif

#Preview("Household — nobody invited") {
    HouseholdSheet()
}
