// CloudSync.swift
//
// The "iCloud job that runs" — implemented as CloudKit's PRIVATE database. The
// device list, alert rules, and event digests live in the USER's own iCloud;
// SecuraCV has no server in the loop and cannot read any of it (Invariant IV,
// local ownership, realized as infrastructure). This is how a second iPhone or
// an iPad "just has your fleet." Secrets never come here — only Keychain holds
// tokens and pinned keys, device-bound.
//
// Kept behind a tiny protocol so the pure model/test target never links
// CloudKit; the concrete implementation runs only on device.

import Foundation
#if canImport(CloudKit)
import CloudKit
#endif

@MainActor
final class CloudSync {
    static let shared = CloudSync()

    /// True once the user's iCloud account is actually usable. When false
    /// (not signed in, restricted), the app stays fully functional locally —
    /// iCloud is convenience, never a gate.
    ///
    /// This used to be a `var` that the app was supposed to set and never
    /// did, so it was `false` for every user forever and silently disabled
    /// BOTH the device sync below and anything else that gated on it. A flag
    /// nobody assigns is indistinguishable from a feature nobody built, which
    /// is why it now derives from the container instead of from a promise.
    private(set) var isAvailable: Bool = false

    private init() {}

    /// Ask CloudKit whether this user has an account we may use. Cheap, safe
    /// to call repeatedly, and the only thing that may set `isAvailable`.
    ///
    /// In a build that cannot be entitled, none of this is compiled: the
    /// `#if` below is false and `isAvailable` stays false, because CONSTRUCTING
    /// a CKContainer is itself what dies in an unentitled process — there is no
    /// object to hold and discover the problem from later. See
    /// CloudContainer.swift for why that has to be settled at compile time.
    @discardableResult
    func refreshAvailability() async -> Bool {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let status = try? await CloudContainer.shared.accountStatus()
        isAvailable = (status == .available)
        #else
        isAvailable = false
        #endif
        return isAvailable
    }

    func push(_ devices: [PairedDeviceRef]) {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard isAvailable else { return }
        let db = CloudContainer.shared.privateCloudDatabase
        for ref in devices {
            let record = CKRecord(recordType: "PairedDevice", recordID: .init(recordName: ref.id))
            record["name"] = ref.name as CTKValue
            record["deviceType"] = ref.deviceType.rawValue as CTKValue
            record["baseURL"] = ref.baseURL?.absoluteString as CTKValue?
            record["pairedAt"] = ref.pairedAt as CTKValue
            db.save(record) { _, _ in /* best-effort; local mirror is source */ }
        }
        #endif
    }

    /// Delete one device's cloud record — the other half of `push`. Without
    /// it, unpairing never sticks: the next `pull` re-adds the removed device
    /// token-less, it polls dark, and raises a false "gone dark" alert.
    /// Best-effort like `push`; DeviceStore's local tombstone covers the gap
    /// until the delete lands.
    func delete(_ deviceID: String) {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard isAvailable else { return }
        let db = CloudContainer.shared.privateCloudDatabase
        db.delete(withRecordID: .init(recordName: deviceID)) { _, _ in /* best-effort */ }
        #endif
    }

    /// Pull the private-DB copy on launch / on iCloud-account change.
    func pull() async -> [PairedDeviceRef] {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard isAvailable else { return [] }
        let db = CloudContainer.shared.privateCloudDatabase
        let query = CKQuery(recordType: "PairedDevice", predicate: NSPredicate(value: true))
        guard let result = try? await db.records(matching: query) else { return [] }
        return result.matchResults.compactMap { _, res in
            guard let rec = try? res.get() else { return nil }
            let idString = rec.recordID.recordName
            let name = rec["name"] as? String ?? idString
            let type = DeviceType(rawValue: rec["deviceType"] as? String ?? "") ?? .unknown
            let url = (rec["baseURL"] as? String).flatMap(URL.init(string:))
            let paired = rec["pairedAt"] as? Date ?? .distantPast
            return PairedDeviceRef(id: idString, name: name, deviceType: type, baseURL: url, pairedAt: paired)
        }
        #else
        return []
        #endif
    }
}

#if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
// Small alias so the assignment lines above read cleanly regardless of the
// exact CKRecordValue spelling across SDK versions.
private typealias CTKValue = CKRecordValue
#endif
