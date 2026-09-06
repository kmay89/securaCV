// WristFeedback.swift  (watch adapter for the shared FeedbackPolicy)
//
// The wrist is the most intimate surface there is — which is exactly why it
// obeys the same one policy as the phone (Shared/FeedbackPolicy.swift):
// transitions and asked-for answers only, never churn. Taps are graded, no
// custom sounds: WKInterfaceDevice's semantic haptics already speak the
// watch's native language.

import Foundation

#if canImport(WatchKit)
import WatchKit

@MainActor
enum WristFeedback {
    static func play(_ event: FeedbackEvent?) {
        guard let event else { return }
        let device = WKInterfaceDevice.current()
        switch event {
        case .fleetEscalated(let severity):
            device.play(severity == .tamper ? .failure : .notification)
        case .allClear:
            device.play(.success)
        case .pathVerified:
            device.play(.success)
        case .pathFailed:
            device.play(.failure)
        }
    }

    /// The finding session's hand-feel, in the watch's own semantic
    /// vocabulary. WHEN a tap happens is the shared, host-tested grammar's
    /// decision (ProximityRanger.tick); this only chooses which native tap
    /// says it: "right direction" for closing in, the success tap on
    /// arrival, "wrong direction" for a close signal that vanished.
    static func play(finding tick: FindingTick?) {
        guard let tick else { return }
        let device = WKInterfaceDevice.current()
        switch tick {
        case .closer(let band):
            device.play(band >= .veryClose ? .directionUp : .click)
        case .arrived:
            device.play(.success)
        case .lost:
            device.play(.directionDown)
        }
    }
}

#else

@MainActor
enum WristFeedback {
    static func play(_ event: FeedbackEvent?) {}
    static func play(finding tick: FindingTick?) {}
}
#endif
