// DeviceStore.swift
//
// The list of Canaries the user has paired. Non-secret metadata only (id, name,
// type, base URL) — tokens live in the Keychain, referenced by id. This list is
// what syncs through the user's OWN iCloud (CloudKit private DB); see
// CloudSync.swift. We store nothing on any SecuraCV server, ever.

import Foundation

struct PairedDeviceRef: Identifiable, Codable, Hashable, Sendable {
    var id: String                 // device_id
    var name: String
    var deviceType: DeviceType
    var baseURL: URL?
    var pairedAt: Date
}

@MainActor
final class DeviceStore: ObservableObject {
    @Published private(set) var devices: [PairedDeviceRef] = []

    private let defaultsKey = "paired_devices_v1"
    private let tombstonesKey = "unpaired_tombstones_v1"

    init() { load() }

    func add(_ ref: PairedDeviceRef, token: String) {
        // Re-pairing is a fresh decision — the old removal must not veto it.
        if tombstones[ref.id] != nil {
            var stones = tombstones
            stones.removeValue(forKey: ref.id)
            tombstones = stones
        }
        try? TokenStore.set(token, for: ref.id)
        if let idx = devices.firstIndex(where: { $0.id == ref.id }) {
            devices[idx] = ref
        } else {
            devices.append(ref)
        }
        persist()
    }

    func remove(_ id: String) {
        devices.removeAll { $0.id == id }
        TokenStore.forget(id)
        // The pinned key goes with the pairing: re-pairing is a fresh
        // physical-presence ceremony, so trust is re-established on first
        // sight then (TOFU) — a factory-reset device with a new key must not
        // inherit a permanent "signature failed" from its previous life.
        PinnedKeyStore.forget(id)
        // The cloud copy goes too — `push` only writes devices still present,
        // so without an explicit delete the next hydration resurrects this
        // one. The tombstone below covers the gap: iCloud may be unavailable
        // right now, and a pull can race the delete.
        var stones = tombstones
        stones[id] = Date()
        tombstones = stones
        CloudSync.shared.delete(id)
        persist()
    }

    /// Unpair tombstones: device id → when THIS phone removed it. Read by
    /// `mergeFromCloud` so a cloud record that predates the removal cannot
    /// resurrect the device; a re-pair (here via `add`, or elsewhere via a
    /// NEWER `pairedAt`) wins over the stone.
    private var tombstones: [String: Date] {
        get { UserDefaults.standard.dictionary(forKey: tombstonesKey) as? [String: Date] ?? [:] }
        set { UserDefaults.standard.set(newValue, forKey: tombstonesKey) }
    }

    func token(for id: String) -> String? { TokenStore.token(for: id) }

    /// Build an authed API client for a paired device, if it's HTTP-pairable.
    func api(for ref: PairedDeviceRef) throws -> DeviceAPI {
        guard let url = ref.baseURL else { throw DeviceError.notPairable }
        guard let token = token(for: ref.id) else { throw DeviceError.notPairable }
        return try DeviceAPI(base: url, token: token)
    }

    // MARK: - persistence (local mirror; CloudKit is the cross-device source)

    private func persist() {
        if let data = try? JSONEncoder().encode(devices) {
            UserDefaults.standard.set(data, forKey: defaultsKey)
        }
        CloudSync.shared.push(devices)
    }

    private func load() {
        if let data = UserDefaults.standard.data(forKey: defaultsKey),
           let saved = try? JSONDecoder().decode([PairedDeviceRef].self, from: data) {
            devices = saved
        }
    }

    /// Merge a set pulled from iCloud (last-writer-wins per id by pairedAt).
    func mergeFromCloud(_ incoming: [PairedDeviceRef]) {
        var byID = Dictionary(devices.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        for ref in incoming {
            // A device this phone unpaired stays unpaired: a cloud copy no
            // newer than the removal is the record the delete hasn't caught
            // up with yet, not a decision. A re-pair made later on another
            // device carries a NEWER pairedAt and wins honestly.
            if let removed = tombstones[ref.id], ref.pairedAt <= removed { continue }
            if let existing = byID[ref.id], existing.pairedAt >= ref.pairedAt { continue }
            byID[ref.id] = ref
        }
        devices = byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        if let data = try? JSONEncoder().encode(devices) {
            UserDefaults.standard.set(data, forKey: defaultsKey)
        }
    }
}
