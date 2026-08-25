// DemoFleet.swift
//
// The seeded fleet for demos, the Simulator, and SwiftUI previews. The
// Simulator has no CoreBluetooth and (usually) no Canaries on its network, so
// without this the app demos as an empty shell; with it, every surface renders
// real-shaped data through the SAME Witness/TimelineEvent primitives the live
// transports produce — the demo has no rendering path of its own to drift.
//
// Ground rules (enforced by DemoFleetTests):
//   * Every id is prefixed "demo-" so sample data can never collide with, or
//     be mistaken for, a paired device.
//   * Nothing seeded ever exceeds `.notice` severity, carries a `.failed`
//     badge, or claims tamper — the demo can never fake an alarm, so
//     AlertCenter stays silent and "pure red only for a real alarm" holds
//     even in a demo.
//   * Timeline buckets sit on 10-minute boundaries (Invariant III), exactly
//     like real chain records.

import Foundation

enum DemoFleet {
    static let defaultsKey = "demo_mode_v1"

    /// Every seeded id starts with this (the ground rule above) — the one
    /// string fleet-wide verbs filter on so sample rows are never counted,
    /// muted, or reported as if they were paired hardware.
    static let idPrefix = "demo-"

    /// Explicit override: `-SecuraCVDemo` as an Xcode scheme argument, or
    /// `SECURACV_DEMO=1` in the environment (CI, `xcrun simctl launch`).
    static var forcedOn: Bool {
        ProcessInfo.processInfo.arguments.contains("-SecuraCVDemo")
            || ProcessInfo.processInfo.environment["SECURACV_DEMO"] == "1"
    }

    /// What demo mode should be at launch: an explicit override wins, then
    /// the user's last in-app choice, then ON in the Simulator when nothing
    /// is paired (first launch demos instead of showing an empty shell) and
    /// OFF on a real phone.
    static func defaultEnabled(hasPairedDevices: Bool) -> Bool {
        if forcedOn { return true }
        if let stored = UserDefaults.standard.object(forKey: defaultsKey) as? Bool {
            return stored
        }
        #if targetEnvironment(simulator)
        return !hasPairedDevices
        #else
        return false
        #endif
    }

    static func remember(_ on: Bool) {
        UserDefaults.standard.set(on, forKey: defaultsKey)
    }

    // MARK: - the seeded fleet

    static func witnesses(now: Date = Date()) -> [Witness] {
        var frontDoor = Witness(id: "demo-wap-a3f7", deviceType: .wap, name: "Front Door")
        frontDoor.publishedType = "canary-wap"       // what the mDNS advert says
        frontDoor.room = "Porch"
        frontDoor.fingerprint = "a3f7 91c2 0d44 be21"
        frontDoor.link = .online
        frontDoor.lastSeen = now
        frontDoor.badge = .verified
        frontDoor.chainLength = 1284
        frontDoor.firmware = "2.4.0-wap"
        frontDoor.rssiDBM = -52
        frontDoor.lastEvent = "Someone at the front door"
        frontDoor.lastEventAt = bucket(now.addingTimeInterval(-35 * 60))
        frontDoor.lastEventSeverity = .notice

        var garage = Witness(id: "demo-wap-5d20", deviceType: .wap, name: "Garage")
        garage.room = "Garage"
        garage.fingerprint = "5d20 cc18 7a02 913f"
        garage.link = .online
        garage.lastSeen = now
        garage.badge = .verified
        garage.chainLength = 862
        garage.firmware = "2.4.0-wap"
        garage.rssiDBM = -67
        garage.batteryPct = 78
        garage.lastEvent = "Vehicle in the driveway"
        garage.lastEventAt = bucket(now.addingTimeInterval(-130 * 60))
        garage.lastEventSeverity = .ok            // long since quiet again

        var nursery = Witness(id: "demo-sense-1c9e", deviceType: .sense, name: "Nursery")
        nursery.room = "Nursery"
        nursery.fingerprint = "1c9e 04ab d371 e88c"
        nursery.link = .online
        nursery.lastSeen = now
        nursery.badge = .verified
        nursery.chainLength = 903
        nursery.firmware = "2.4.0"
        nursery.rssiDBM = -58
        nursery.radarPresent = true
        nursery.radarOccupants = 1
        nursery.breathingLock = true
        nursery.tempC = 21.5
        nursery.humidityPct = 47

        var driveway = Witness(id: "demo-vision-77b0", deviceType: .vision, name: "Driveway")
        driveway.room = "Driveway"
        driveway.fingerprint = "77b0 3f6d 218a c405"
        driveway.link = .online
        driveway.lastSeen = now
        driveway.badge = .signed
        driveway.chainLength = 447
        driveway.firmware = "2.4.0"
        driveway.rssiDBM = -61
        // What its optical pipeline reports seeing right now — the claim a
        // real vision Canary broadcasts in its v2 presence beacon. Seeded
        // fresh so the detail screen demos the "Seeing now" row; the same
        // aging rule that governs a real beacon (Witness.seeingNow) applies
        // to the demo too.
        driveway.seeingClass = .vehicle
        driveway.seeingScore = 84
        driveway.seeingAt = now

        // The nightstand display — the fleet's across-the-room face, whose
        // behind-glass beacon boots canary yellow since the 2.3.2 train.
        var nightstand = Witness(id: "demo-display-9f31", deviceType: .display, name: "Nightstand")
        // "canary-nightstand" is published by two different panels, so the
        // figure map deliberately omits it — this row demos the honest
        // fallback (the symbol), exactly as a real nightstand would today.
        nightstand.publishedType = "canary-nightstand"
        nightstand.room = "Bedroom"
        // A display WATCHES the fleet; it holds no witness key and never
        // mints its own chain (canary_display config.h) — so no fingerprint,
        // no signing badge, no chain length. Anything else would demo an
        // impossible trust state.
        nightstand.link = .online
        nightstand.lastSeen = now
        nightstand.firmware = "2.4.0"
        nightstand.rssiDBM = -49

        // The nightlight — the C3 pocket clock/lamp/companion. Same
        // display-class trust posture as the nightstand: it watches nothing,
        // holds no witness key, mints no chain.
        var nightlight = Witness(id: "demo-nightlight-c3a1", deviceType: .nightlight, name: "Nightlight")
        nightlight.publishedType = "canary-nightlight"
        nightlight.room = "Kids' room"
        nightlight.link = .online
        nightlight.lastSeen = now
        nightlight.firmware = "2.4.0"
        nightlight.rssiDBM = -55

        var mailbox = Witness(id: "demo-wap-e412", deviceType: .wap, name: "Mailbox")
        mailbox.fingerprint = "e412 9b55 60fe a2d7"
        mailbox.link = .online
        mailbox.lastSeen = now
        mailbox.seenViaBLE = true
        mailbox.batteryPct = 64

        return [frontDoor, garage, nursery, driveway, nightstand, nightlight, mailbox]
    }

    static func timeline(now: Date = Date()) -> [TimelineEvent] {
        func ev(_ seq: Int, _ deviceID: String, _ name: String, _ zone: String,
                _ headline: String, _ sev: Severity, _ badge: TrustBadge,
                minutesAgo: Double, symbol: String = "sparkle") -> TimelineEvent {
            TimelineEvent(id: "\(deviceID)#\(seq)", deviceID: deviceID, deviceName: name,
                          zone: zone, headline: headline, severity: sev, badge: badge,
                          timeBucket: bucket(now.addingTimeInterval(-minutesAgo * 60)),
                          symbol: symbol)
        }
        return [
            ev(1284, "demo-wap-a3f7", "Front Door", "front door",
               "Someone at the front door", .notice, .verified, minutesAgo: 35,
               symbol: "figure.stand"),
            ev(447, "demo-vision-77b0", "Driveway", "driveway",
               "Vehicle in the driveway", .notice, .signed, minutesAgo: 130,
               symbol: "car.fill"),
            ev(1283, "demo-wap-a3f7", "Front Door", "front door",
               "Package left at the front door", .notice, .verified, minutesAgo: 260,
               symbol: "shippingbox.fill"),
            ev(902, "demo-sense-1c9e", "Nursery", "nursery",
               "Morning — room back in use", .ok, .verified, minutesAgo: 350,
               symbol: "sun.max.fill"),
            ev(1282, "demo-wap-a3f7", "Front Door", "front door",
               "Fleet self-test passed", .ok, .verified, minutesAgo: 480,
               symbol: "checkmark.seal.fill"),
        ]
    }

    /// Coarse 10-minute bucket — Invariant III, the same math as
    /// FleetStore.bucket (never a precise second).
    static func bucket(_ date: Date) -> Date {
        Date(timeIntervalSince1970: (date.timeIntervalSince1970 / 600).rounded(.down) * 600)
    }

    /// A store pre-seeded for SwiftUI previews — no transports, no async, so
    /// the canvas renders instantly and previews never depend on hardware.
    @MainActor
    static func previewStore(now: Date = Date()) -> FleetStore {
        let store = FleetStore()
        store.demoMode = true
        store.witnesses = witnesses(now: now).sorted { $0.effectiveSeverity > $1.effectiveSeverity }
        store.timeline = timeline(now: now)
        store.heartbeat.recordBeat()
        return store
    }
}
