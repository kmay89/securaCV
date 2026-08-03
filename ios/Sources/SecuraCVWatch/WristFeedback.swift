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
}

#else

@MainActor
enum WristFeedback {
    static func play(_ event: FeedbackEvent?) {}
}
#endif
