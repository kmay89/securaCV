// CanaryActor.swift  (SHARED — the character, performed)
//
// The renderer half of the mood engine: takes a CanaryFace + CanaryPosture
// and performs it with the ONE canonical mascot (the "Canary" imageset,
// staged from brands/logo_512x512.png — composited, never redrawn). All
// acting is transform choreography — position, rotation, squash-and-stretch
// anchored at the feet — which is how the bird stays exactly the brand while
// still breathing, searching, calling, drooping.
//
// Animation principles, applied with restraint (the calm doctrine outranks
// showmanship):
//   * Squash & stretch: the breath pairs a y-stretch with a hair of
//     x-squash, anchored at the perch — mass, not scaling.
//   * The story is the fleet's truth: every motion maps 1:1 to a face the
//     mood engine named (the honesty rule). No idle randomness.
//   * Stillness is a pose too: Reduce Motion AND the watch's Always-On
//     Display (wrist down) both get the same story as a held posture;
//     `hidden` renders NOTHING — during a real alarm the character leaves
//     the stage to the instruments, by firmware rule.

// SecuraCV-Parity: every Apple surface that shows a device compiles this.
// (the one performer behind every bird; isLuminanceReduced is simply always
// false off the wrist, so the still-pose gate degrades to Reduce Motion)

import SwiftUI

struct CanaryActor: View {
    var face: CanaryFace
    var posture: CanaryPosture = .asFace
    var height: CGFloat = 56

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    // Wrist down: watchOS keeps the view on screen but dimmed (Always-On
    // Display), and a repeatForever animation left running there burns
    // battery for motion nobody is looking at. Apple's guidance is to hold
    // still; this is the same "held pose" the character already knows how to
    // strike for Reduce Motion, so idle costs nothing and looks deliberate.
    // Harmless on iOS/iPadOS, where the value is simply always false.
    @Environment(\.isLuminanceReduced) private var luminanceReduced
    @State private var beat = false   // the one phase flag; each face times it differently

    /// The one gate: either reason to be still means still.
    private var holdsStill: Bool { reduceMotion || luminanceReduced }

    var body: some View {
        if face == .hidden {
            EmptyView()   // never cute during a real alarm
        } else {
            bird
                .frame(height: height)
                .id(identity)                 // face change = clean restart
                // The phase flag must be DRIVEN by stillness, not merely
                // consulted alongside it. `.animation(_:value:)` only takes
                // effect when `beat` changes, so a repeatForever already in
                // flight keeps running no matter what `loop()` returns on a
                // later render — the wrist could drop and the bird would
                // breathe on. And a view first built while dimmed never set
                // `beat` at all, so raising the wrist left it frozen.
                // Both directions are handled here, at the one flag.
                .onAppear { beat = !holdsStill }
                .onChange(of: holdsStill) { _, still in
                    if still {
                        // Cancel the loop outright: re-setting the phase
                        // inside a transaction that disables animations
                        // snaps to the base pose instead of easing there.
                        var cut = Transaction()
                        cut.disablesAnimations = true
                        withTransaction(cut) { beat = false }
                    } else {
                        beat = true          // wrist up — the bird resumes
                    }
                }
                .accessibilityHidden(true)    // the words nearby carry the meaning
        }
    }

    private var identity: String { "\(face.rawValue)-\(posture.rawValue)" }

    @ViewBuilder
    private var bird: some View {
        let image = Image("Canary").resizable().scaledToFit()
        switch (face, posture) {
        case (.calm, _):
            // Slow breath at the perch — alive, unbothered.
            image
                .scaleEffect(x: beat ? 0.992 : 1.0,
                             y: beat ? 1.028 : 1.0,
                             anchor: .bottom)
                .animation(loop(2.6), value: beat)

        case (.worried, .searching):
            // Someone is late: the bird leans and looks along the glass
            // edge for them — anticipation in the lean, ease in the turn.
            image
                .rotationEffect(.degrees(beat ? 5 : -5), anchor: .bottom)
                .offset(x: beat ? height * 0.10 : -height * 0.10)
                .animation(loop(1.8), value: beat)

        case (.worried, .calling):
            // Someone is lost: head up, small urgent pulses — a call, not
            // a panic (the instruments do the alarming).
            image
                .rotationEffect(.degrees(-8), anchor: .bottom)
                .scaleEffect(x: beat ? 0.99 : 1.0,
                             y: beat ? 1.05 : 1.0,
                             anchor: .bottom)
                .animation(loop(1.1), value: beat)

        case (.worried, .asFace):
            // Unsettled without a name to look for (link trouble): a
            // quicker, shallower breath.
            image
                .scaleEffect(x: beat ? 0.994 : 1.0,
                             y: beat ? 1.02 : 1.0,
                             anchor: .bottom)
                .animation(loop(1.6), value: beat)

        case (.distressed, _):
            // Visibly unwell: drooped and slow — maintenance overdue reads
            // as weight, not drama.
            image
                .rotationEffect(.degrees(9), anchor: .bottom)
                .scaleEffect(x: 1.0, y: beat ? 0.985 : 1.0, anchor: .bottom)
                .opacity(0.92)
                .animation(loop(4.2), value: beat)

        case (.asleep, _):
            // Stillness IS the information; the faintest slow breath.
            image
                .opacity(0.75)
                .scaleEffect(x: 1.0, y: beat ? 1.012 : 1.0, anchor: .bottom)
                .animation(loop(5.0), value: beat)

        case (.hidden, _):
            EmptyView()
        }
    }

    private func loop(_ period: Double) -> Animation? {
        holdsStill ? nil : .easeInOut(duration: period).repeatForever(autoreverses: true)
    }
}

#Preview("The faces") {
    VStack(spacing: 24) {
        CanaryActor(face: .calm)
        CanaryActor(face: .worried, posture: .searching)
        CanaryActor(face: .worried, posture: .calling)
        CanaryActor(face: .distressed)
    }
    .padding()
}
