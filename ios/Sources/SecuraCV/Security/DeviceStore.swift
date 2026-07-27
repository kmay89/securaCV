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

    init() { load() }

    func add(_ ref: PairedDeviceRef, token: String) {
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
        persist()
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
            if let existing = byID[ref.id], existing.pairedAt >= ref.pairedAt { continue }
            byID[ref.id] = ref
        }
        devices = byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        if let data = try? JSONEncoder().encode(devices) {
            UserDefaults.standard.set(data, forKey: defaultsKey)
        }
    }
}
