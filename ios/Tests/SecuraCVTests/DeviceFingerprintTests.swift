// DeviceFingerprintTests.swift
//
// Pins the fingerprint derivation to a vector computed INDEPENDENTLY of this
// implementation (plain SHA-256 over the documented preimage), so the Swift
// port can't drift from the firmware's `compute_fingerprint()`:
//
//     sha256("securacv:pubkey:fingerprint" || 0x00 || pubkey[32])[0..<8]
//
// If this fails, the app and the devices disagree about identity — which shows
// up in the field as beacons never matching their paired Canary.

import XCTest
import CryptoKit
@testable import SecuraCV

final class DeviceFingerprintTests: XCTestCase {

    /// pubkey = 00 01 02 … 1f
    private let pubkey = Data((0..<32).map { UInt8($0) })

    func testMatchesTheIndependentlyComputedVector() {
        XCTAssertEqual(DeviceFingerprint.hex(forPublicKey: pubkey), "f2f46527cd927662",
                       "the derivation must match sha256(domain || 0x00 || pubkey)[0..<8]")
    }

    func testFingerprintIsEightBytes() {
        XCTAssertEqual(DeviceFingerprint.bytes(forPublicKey: pubkey)?.count, 8)
        XCTAssertEqual(DeviceFingerprint.hex(forPublicKey: pubkey)?.count, 16,
                       "16 hex chars, matching Witness.fingerprint")
    }

    /// The domain separator is what stops this hash colliding with any other
    /// SHA-256 of the same key elsewhere in the system.
    func testDomainSeparationActuallyApplies() {
        let bare = SHA256.hash(data: pubkey).prefix(8)
            .map { String(format: "%02x", $0) }.joined()
        XCTAssertNotEqual(DeviceFingerprint.hex(forPublicKey: pubkey), bare,
                          "an undomained SHA-256 of the key must not be what we produce")
        XCTAssertEqual(DeviceFingerprint.domain, "securacv:pubkey:fingerprint",
                       "the domain string is a wire constant — changing it re-identifies every device")
    }

    /// Anything that isn't a 32-byte Ed25519 key must be refused, not hashed.
    func testRejectsKeysThatAreNotThirtyTwoBytes() {
        XCTAssertNil(DeviceFingerprint.bytes(forPublicKey: Data()))
        XCTAssertNil(DeviceFingerprint.bytes(forPublicKey: Data(repeating: 0, count: 31)))
        XCTAssertNil(DeviceFingerprint.bytes(forPublicKey: Data(repeating: 0, count: 33)))
        XCTAssertNil(DeviceFingerprint.hex(forPublicKey: Data(repeating: 0, count: 16)))
    }

    /// The end-to-end reason this type exists: the last two bytes of the
    /// fingerprint are what the BLE beacon carries, so a beacon from a device
    /// whose key we pinned must match its Witness row.
    func testBeaconSuffixMatchesTheDerivedFingerprint() {
        let fp = DeviceFingerprint.bytes(forPublicKey: pubkey)!
        let beacon = FleetBeacon.parse(manufacturerData:
            FleetBeacon.encode(flags: 0, batteryPct: nil, healthPct: nil,
                               chainHeight: 0, fpB0: fp[6], fpB1: fp[7]))!

        XCTAssertTrue(beacon.matches(fingerprint: DeviceFingerprint.hex(forPublicKey: pubkey)!),
                      "the beacon's fp2 must be the last 2 bytes of the derived fingerprint")
        XCTAssertEqual(beacon.fingerprintSuffix, "7662")
    }
}
