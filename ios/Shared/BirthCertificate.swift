// BirthCertificate.swift  (SHARED — phone, wrist, widgets)
//
// A Canary's name, derived from its key rather than remembered.
//
// WHY THIS EXISTS
//   The Hatchery mints a certificate at first flash — a name, a house, a
//   lineage — and used to keep it in the flasher's own preferences. The device
//   never learned its name, nothing synced it, and so the phone holding that
//   same Canary had no certificate to show. Any name it invented would have
//   been a second name for one bird.
//
//   So the certificate is DERIVED from the device's Ed25519 public-key
//   fingerprint. The name is a rendering of the key: same input, same output,
//   on every surface, forever. Nothing is stored and nothing is synced, so
//   there is nothing to drift — the property the fleet figures already have,
//   applied to identity.
//
//   It also makes the name CHECKABLE. When a device asserts its own name (the
//   firmware derives the same certificate at first boot and serves it), the
//   app doesn't have to believe it: `verify(claimed:matches:)` recomputes from
//   the key. A device that answers to a name its key doesn't produce is not a
//   device with a nickname — it is a device whose identity doesn't add up, and
//   that is worth seeing.
//
// AGREEMENT ACROSS LANGUAGES
//   The algorithm also lives in canary-local/tools/hatchery/derive.mjs (the
//   browser Lab and the Mac Flasher). Copying an algorithm by hand is how two
//   surfaces quietly disagree, so HatchSpec.swift is GENERATED from the same
//   spec and carries fingerprint→name vectors computed by the JavaScript;
//   `BirthCertificateTests` asserts this file reproduces every one.
//
// NOT CRYPTOGRAPHIC, ON PURPOSE
//   The fingerprint is already a SHA-256 digest. The generator below only has
//   to spread it fairly and identically in two languages, so it is FNV-1a
//   seeding xorshift32 — synchronous in a browser, eight lines in Swift, and
//   no dependency in either.

import Foundation

/// What a Canary is called, and the small print behind it.
public struct BirthCertificate: Hashable, Sendable {
    /// The bare given name ("Pip"), before title and house.
    public let base: String
    /// The whole name ("Lady Pip of Perchwood") — what the owner calls it.
    public let name: String
    /// The product line, from what the device published about itself.
    public let species: String
    /// "the Third of its name, of Perchwood".
    public let lineage: String
    /// The device's own slug when it has one, else its fingerprint.
    public let ringId: String
    public let motto: String
    public let craft: String
}

public enum BirthCertificateDerivation {
    // MARK: - the shared generator (mirrors derive.mjs, byte for byte)

    /// FNV-1a (32-bit). ASCII in, seed out — fingerprints are hex, so the
    /// JavaScript's `charCodeAt & 0xff` and this UTF-8 walk see the same bytes.
    public static func fnv1a(_ text: String) -> UInt32 {
        var h: UInt32 = 0x811c_9dc5
        for byte in text.utf8 {
            h ^= UInt32(byte)
            // h *= 16777619, written as shifts so both languages wrap the
            // same way in 32-bit lanes.
            h = h &+ ((h << 1) &+ (h << 4) &+ (h << 7) &+ (h << 8) &+ (h << 24))
        }
        return h
    }

    /// xorshift32 as a [0,1) stream. Zero is xorshift's fixed point, so a
    /// zero seed is nudged — the same nudge the JavaScript makes.
    public struct SeededRNG {
        private var state: UInt32

        public init(seed text: String) {
            let s = fnv1a(text)
            state = s == 0 ? 0x9e37_79b9 : s
        }

        public mutating func next() -> Double {
            state ^= state << 13
            state ^= state >> 17
            state ^= state << 5
            return Double(state) / 4_294_967_296.0
        }

        /// The one selection primitive, so the order of consumption is
        /// obvious and matches the JavaScript's.
        public mutating func pick<T>(_ items: [T]) -> T? {
            guard !items.isEmpty else { return nil }
            return items[Int(next() * Double(items.count))]
        }
    }

    // MARK: - the certificate

    /// Derive the certificate for a device from its fingerprint.
    ///
    /// Returns nil when there is no fingerprint — a Canary we have not yet
    /// pinned a key for has no identity to render, and inventing one would be
    /// exactly the nickname this design replaces.
    public static func certificate(fingerprint: String,
                                   species: String = "Canary",
                                   deviceID: String = "") -> BirthCertificate? {
        let fp = fingerprint.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !fp.isEmpty, !HatchSpec.first.isEmpty else { return nil }

        var rng = SeededRNG(seed: fp)
        guard let base = rng.pick(HatchSpec.first) else { return nil }
        let wantsTitle = !HatchSpec.titles.isEmpty && rng.next() < HatchSpec.titleChance
        let house = rng.pick(HatchSpec.house) ?? ""
        // Ordinals are 1-based in the spec (index 0 is empty), so the draw is
        // over the real entries only. Derived rather than counted: "Nth of its
        // name" was never a fact about the world, only about the order someone
        // happened to flash things in.
        let nth = HatchSpec.ordinals.count > 1
            ? 1 + Int(rng.next() * Double(HatchSpec.ordinals.count - 1))
            : 1
        let ordinal = HatchSpec.ordinals.indices.contains(nth)
            ? HatchSpec.ordinals[nth] : "the \(nth)th"
        let title = wantsTitle ? (rng.pick(HatchSpec.titles).map { $0 + " " } ?? "") : ""
        let motto = rng.pick(HatchSpec.mottoes) ?? ""

        let houseSuffix = house.isEmpty ? "" : ", " + house.replacingOccurrences(
            of: "^the ", with: "", options: .regularExpression)
        let slug = deviceID.trimmingCharacters(in: .whitespacesAndNewlines)

        return BirthCertificate(
            base: base,
            name: title + base + (house.isEmpty ? "" : " " + house),
            species: species,
            lineage: ordinal + " of its name" + houseSuffix,
            ringId: slug.isEmpty ? fp.uppercased() : slug,
            motto: motto,
            craft: HatchSpec.craft)
    }

    /// Does the name a device CLAIMS match the one its key produces?
    ///
    /// The firmware derives its own certificate at first boot and serves it,
    /// and this is why the app never has to take that at face value. A
    /// mismatch is not cosmetic: the name is a function of the key, so two
    /// different answers mean the thing answering is not the thing whose key
    /// we pinned.
    public static func verify(claimed name: String, fingerprint: String) -> Bool {
        guard let derived = certificate(fingerprint: fingerprint) else { return false }
        return derived.name.compare(name.trimmingCharacters(in: .whitespacesAndNewlines),
                                    options: [.caseInsensitive]) == .orderedSame
    }
}
