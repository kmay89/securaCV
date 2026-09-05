// CanaryVoiceBubble.swift
//
// The speech bubble — the visible half of the bird helper, docked to the top
// of the window over every section. The website's bubble anchors under the
// header bird and docks to the viewport top when the header scrolls away; an
// app has no header bird, which is a case the origin already defined: pages
// with no mascot run "permanently docked", with a small bird riding inside
// the bubble. That is exactly this view.
//
// The bird inside is the real character wearing its real face — CanaryActor
// fed the store's published mood, never an expression invented for the
// message (one mood engine, many renderers). During a real alarm the face is
// .hidden and this view renders nothing at all: never cute during a real
// alarm, even mid-sentence.
//
// A tone is worn as a small accent bar, not a repaint — the site tints the
// bubble's border the same quiet way. Text elides before the action does: the thing
// you can act on never gets squeezed out. Motion is opt-in (Reduce Motion
// gets an instant, still bubble), and the ✕ is the only dismissal that
// counts toward quieting the chatter.

import SwiftUI

struct CanaryVoiceBubble: View {
    @EnvironmentObject var store: FleetStore
    @ObservedObject var voice: CanaryVoiceStage
    /// The shell owns the section switch; the bubble only asks for it.
    var onNavigate: (AppSection) -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        Group {
            if let m = voice.showing, store.canaryFace != .hidden {
                bubble(m)
                    .transition(reduceMotion
                        ? .opacity
                        : .move(edge: .top).combined(with: .opacity))
            }
        }
        .animation(reduceMotion ? nil : .spring(duration: 0.35), value: voice.showing)
    }

    private func bubble(_ m: CanaryVoiceMessage) -> some View {
        HStack(spacing: Theme.s) {
            CanaryActor(face: store.canaryFace, posture: store.canaryPosture, height: 26)
            VStack(alignment: .leading, spacing: Theme.xs) {
                Text(m.text)
                    .font(.footnote)
                    .lineLimit(3)
                    .fixedSize(horizontal: false, vertical: true)
                if let action = m.action {
                    Button(action.label) {
                        onNavigate(action.section)
                        voice.actionTaken()
                    }
                    .font(.footnote.weight(.semibold))
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.color(.info))
                }
            }
            Spacer(minLength: 0)
            Button {
                voice.dismiss()
            } label: {
                Image(systemName: "xmark")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .padding(Theme.s)          // a finger-sized target
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Dismiss")
        }
        .padding(.vertical, Theme.s)
        .padding(.leading, Theme.m)
        .padding(.trailing, Theme.xs)
        .background(.ultraThinMaterial,
                    in: RoundedRectangle(cornerRadius: Theme.corner, style: .continuous))
        .overlay(alignment: .leading) {
            if let role = Self.accent(for: m.tone) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(Theme.color(role))
                    .frame(width: 3)
                    .padding(.vertical, Theme.s)
                    .padding(.leading, Theme.xs)
            }
        }
        .frame(maxWidth: 420)                  // stays a bubble on iPad
        .padding(.horizontal, Theme.m)
        .padding(.top, Theme.s)
        .accessibilityElement(children: .contain)
    }

    /// The site's tone borders, said in Theme roles: chat stays unmarked,
    /// info wears the accent, good the calm green, warn the caution orange.
    /// Never .alert red — a real alarm doesn't speak here at all.
    static func accent(for tone: CanaryVoiceTone) -> Theme.Role? {
        switch tone {
        case .chat: return nil
        case .info: return .info
        case .good: return .calm
        case .warn: return .warn
        }
    }
}

#Preview("The bird has a note") {
    struct Host: View {
        @StateObject private var voice = CanaryVoiceStage(
            keeper: CanaryVoiceKeeper(defaults: UserDefaults(suiteName: "preview-canary-voice")!))
        var body: some View {
            VStack {
                CanaryVoiceBubble(voice: voice) { _ in }
                Spacer()
                Button("Say something good") {
                    voice.say("A clean week together.", tone: .good)
                }
                Button("Raise a notice") {
                    voice.say("Paired key changed for Porch — check before trusting.",
                              tone: .warn, key: "preview-warn")
                }
            }
            .environmentObject(DemoFleet.previewStore())
        }
    }
    return Host()
}
