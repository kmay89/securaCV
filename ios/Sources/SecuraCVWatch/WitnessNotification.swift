// WitnessNotification.swift  (watch app target)
//
// The wrist's own long-look for a witness alert (RFC
// docs/design/apple_watch_and_notifications.md §8, W1). The phone posts every
// witness alert in one category (Shared/NotificationIDs) with its witness id
// as the thread identifier; the system mirrors it to the watch, and this
// scene draws it instead of the generic layout. The Acknowledge / Mute
// actions the phone registered for the category ride along underneath, as
// before — this file changes how the alert LOOKS, never what it can do.
//
// What it shows, and from where:
//   * the alert's own words (title and body) — what happened, as the phone
//     said it at the time;
//   * which Canary, from the snapshot this watch last adopted (WristCache):
//     its name, its figure, its chain badge. Those are slow facts about the
//     device. The row's CURRENT severity or tamper state is deliberately not
//     drawn here — the cache may be older than the alert, and a long-look
//     that paired an old "all quiet" with a new alarm would contradict
//     itself.
// With no matching row (a storm summary, a Canary the capped snapshot left
// out, an empty cache) it falls back to the plain words.

import SwiftUI
import UserNotifications
import WatchKit

final class WitnessNotificationController: WKUserNotificationHostingController<WitnessNotificationView> {
    private var title = ""
    private var message = ""
    private var row: WristWitness?

    override var body: WitnessNotificationView {
        WitnessNotificationView(title: title, message: message, row: row)
    }

    override func didReceive(_ notification: UNNotification) {
        let content = notification.request.content
        title = content.title
        message = content.body
        // The phone sets the thread identifier to the witness id.
        let id = content.threadIdentifier
        row = id.isEmpty ? nil : WristCache.load()?.witnesses.first { $0.id == id }
    }
}

struct WitnessNotificationView: View {
    let title: String
    let message: String
    let row: WristWitness?

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            if let row {
                HStack(spacing: Theme.s) {
                    DeviceFigureIcon(row.deviceType,
                                     published: row.publishedType,
                                     hardware: row.hardware,
                                     size: 22)
                    Text(row.name)
                        .font(.headline)
                        .lineLimit(2)
                }
            } else if !title.isEmpty {
                Text(title).font(.headline)
            }
            Text(message)
                .font(.body)
            if let row {
                Label(row.badge.label, systemImage: row.badge.sfSymbol)
                    .font(.caption2)
                    .foregroundStyle(row.badge.isTrusted ? Theme.color(.calm) : .secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
