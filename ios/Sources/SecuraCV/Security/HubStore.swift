// HubStore.swift
//
// What the phone remembers about the hub it finished: where it is and
// whose it is (UserDefaults, non-secret), and the broker login it minted
// for the Canaries (Keychain, this device only — the same custody every
// per-device token gets). Nothing here is a Home Assistant session: the
// token the finishing run used is revoked when the run ends.

import Foundation

struct HubRecord: Codable, Equatable, Sendable {
    var baseURL: URL
    var name: String
    var ownerUsername: String
    var finishedAt: Date?
}

enum HubStore {
    static let key = "hub_record_v1"

    static func load(from defaults: UserDefaults = .standard) -> HubRecord? {
        guard let data = defaults.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(HubRecord.self, from: data)
    }

    static func save(_ record: HubRecord, to defaults: UserDefaults = .standard) {
        if let data = try? JSONEncoder().encode(record) { defaults.set(data, forKey: key) }
    }

    static func clear(in defaults: UserDefaults = .standard) {
        defaults.removeObject(forKey: key)
    }
}

/// The broker account the hub's Mosquitto was given for the Canaries —
/// minted on this phone, so this phone can hand it to a device later.
enum HubSecretStore {
    private static let service = "com.securacv.witness.hub"
    private static let userAccount = "broker_username"
    private static let passAccount = "broker_password"

    struct BrokerLogin: Equatable, Sendable {
        var username: String
        var password: String
    }

    static func brokerLogin() -> BrokerLogin? {
        guard let u = Keychain.get(account: userAccount, service: service).flatMap({ String(data: $0, encoding: .utf8) }),
              let p = Keychain.get(account: passAccount, service: service).flatMap({ String(data: $0, encoding: .utf8) }),
              !u.isEmpty, !p.isEmpty else { return nil }
        return BrokerLogin(username: u, password: p)
    }

    static func set(_ login: BrokerLogin) throws {
        try Keychain.set(Data(login.username.utf8), account: userAccount, service: service)
        try Keychain.set(Data(login.password.utf8), account: passAccount, service: service)
    }

    static func forget() {
        Keychain.delete(account: userAccount, service: service)
        Keychain.delete(account: passAccount, service: service)
    }
}
