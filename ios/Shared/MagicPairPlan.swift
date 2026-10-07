// MagicPairPlan.swift  (SHARED — pure Foundation)
//
// What the nearby-Canary sheet does once the Canary said it joined — as a
// reducer, so every branch is a test and the view only follows it. Pure:
// no network, no CoreBluetooth; the sheet reads the claim, dials, and hands
// each outcome back in.
//
// The shape of the thing (Shared/ImprovWire.swift has the wire):
//
//   * Sense and Vision serve no companion service: nothing to pair over
//     HTTP, so the sheet watches the fleet for them, as it always has.
//   * A WAP hands the link that provisioned a one-time CLAIM over Bluetooth
//     — never its bearer token — and the phone spends it on the home LAN
//     with one unauthenticated GET of the claim URL. The receipt that comes
//     back is the one the BOOT-tap route serves; the key crossed Wi-Fi.
//   * The claim is single use and burns on a wrong guess, so the plan dials
//     at most twice, and the second time ONLY when the first never reached
//     the Canary (its `.local` name not resolving yet is the common case —
//     the address it reported stands in). A refusal (403) or any answer
//     that was not a receipt is final: the claim is spent, the Canary is
//     on the Wi-Fi, and the Fleet tab's BOOT-tap pairing is the next door.
//   * Two refusals before anything is dialed, the same two PairView
//     applies to a pasted receipt: a claim URL off the local network, and
//     an https URL with no certificate fingerprint to pin.

import Foundation

enum MagicPairPlan {
    /// What the sheet does next.
    enum Step: Equatable, Sendable {
        /// No companion service: the card's "watching this network" ending.
        case watchFleet
        /// Spend the claim at this URL; `pin` is the certificate fingerprint
        /// to hold an https Canary to (nil over http).
        case spendClaim(URL, pin: String?)
        /// The `.local` name never answered: once more, at the address the
        /// Canary reported.
        case retryViaIP(URL, pin: String?)
        /// The receipt is in hand — add the device, token in the Keychain.
        case paired
        /// On the Wi-Fi, not paired, with the way to pair it later.
        case notPairedTapBoot(String)
        /// Not dialed at all, and why.
        case refused(String)
    }

    /// How one dial of the claim URL ended, as the sheet reports it.
    enum FetchOutcome: Equatable, Sendable {
        /// A receipt decoded.
        case receipt
        /// Nothing answered: DNS, connection or timeout — the claim is
        /// unspent, a second host may be tried.
        case unreachable
        /// 403 — spent, expired or refused. Final.
        case refused
        /// Any other failure, in the error's words. Final: the claim may
        /// have been consumed by it.
        case other(String)
    }

    /// Every sentence the plan can show, in one place so the voice tests
    /// can sweep them (US spellings; a group of Canaries is a fleet).
    enum Copy {
        /// The joined-but-not-paired ending: the Fleet tab's BOOT-tap pairing.
        static let tapBootLater =
            "It's on your Wi-Fi. Once it shows on the Fleet tab, pair it with a short tap on its BOOT button."
        static let noClaim =
            "The Canary joined, but it handed this phone no claim to pair with. " + tapBootLater
        static let notPrivate =
            "Pairing refused: the Canary's claim points somewhere that is not a private address on your network, so it was not dialed."
        static let tlsNoPin =
            "Pairing refused: this Canary speaks https but named no certificate fingerprint, so its connection can't be checked. Update its firmware, then pair it from the Fleet tab with a short tap on its BOOT button."
        static let unreachable =
            "The Canary joined, but this phone couldn't reach it over your Wi-Fi to collect its pairing key — it may still be settling onto the network. " + tapBootLater
        static let refused =
            "The Canary joined, but declined the claim — it was already spent, or it expired. " + tapBootLater
        static func other(_ detail: String) -> String {
            let said = detail.trimmingCharacters(in: CharacterSet(charactersIn: ". \n"))
            return "The Canary joined, but collecting its pairing key failed: \(said). " + tapBootLater
        }
        static let receiptNotPrivate =
            "Pairing refused: the receipt points the Canary somewhere that is not a private address on your network."
        static let receiptTLSNoPin =
            "Pairing refused: the receipt names a secure (https) Canary but carries no certificate fingerprint, so its connection can't be checked. Update its firmware, then pair it from the Fleet tab with a short tap on its BOOT button."

        /// Every sentence, for the sweeps.
        static var all: [String] {
            [tapBootLater, noClaim, notPrivate, tlsNoPin, unreachable, refused, other("x"),
             receiptNotPrivate, receiptTLSNoPin]
        }
    }

    /// Does this URL speak TLS? The plan's one look at a scheme.
    static func isTLS(_ url: URL) -> Bool { url.scheme?.lowercased() == "https" }

    /// The Canary said it joined. `hasClaimService`: the link offered the
    /// companion service's CLAIM characteristic at all (false on a Sense or
    /// Vision). `claim`: what the read decoded, nil for "{}" or a bad body.
    /// `isPrivateHost`: the app's "nothing phones home" gate, handed in
    /// (DeviceAPI.isPrivate) so the rule and the gate cannot drift.
    static func afterJoin(hasClaimService: Bool,
                          claim: ImprovWire.Claim?,
                          isPrivateHost: (URL) -> Bool) -> Step {
        guard hasClaimService else { return .watchFleet }
        guard let claim, let url = claim.claimURL else { return .notPairedTapBoot(Copy.noClaim) }
        return dial(url, claim: claim, isPrivateHost: isPrivateHost)
    }

    /// One dial of the claim URL ended as `outcome`. `triedIP`: the dial
    /// was already the address fallback, so there is no third host.
    static func afterFetch(_ outcome: FetchOutcome,
                           claim: ImprovWire.Claim,
                           triedIP: Bool,
                           isPrivateHost: (URL) -> Bool) -> Step {
        switch outcome {
        case .receipt:
            return .paired
        case .unreachable:
            guard !triedIP, let ip = claim.staIP,
                  let url = ImprovWire.claimURL(claim, host: ip),
                  url != claim.claimURL,
                  isPrivateHost(url) else {
                return .notPairedTapBoot(Copy.unreachable)
            }
            if isTLS(url), claim.tlsCertFingerprint == nil {
                return .notPairedTapBoot(Copy.unreachable)
            }
            return .retryViaIP(url, pin: claim.tlsCertFingerprint)
        case .refused:
            return .notPairedTapBoot(Copy.refused)
        case .other(let detail):
            return .notPairedTapBoot(Copy.other(detail))
        }
    }

    /// The receipt is in hand: the same two gates PairView applies before a
    /// pasted receipt becomes a paired device — the base URL every later
    /// call dials must be private, and an https one must carry a pin.
    static func afterReceipt(baseURL: URL,
                             tlsCertFingerprint: String?,
                             isPrivateHost: (URL) -> Bool) -> Step {
        guard isPrivateHost(baseURL) else { return .refused(Copy.receiptNotPrivate) }
        if isTLS(baseURL), tlsCertFingerprint == nil { return .refused(Copy.receiptTLSNoPin) }
        return .paired
    }

    private static func dial(_ url: URL, claim: ImprovWire.Claim, isPrivateHost: (URL) -> Bool) -> Step {
        guard isPrivateHost(url) else { return .refused(Copy.notPrivate) }
        if isTLS(url), claim.tlsCertFingerprint == nil { return .refused(Copy.tlsNoPin) }
        return .spendClaim(url, pin: claim.tlsCertFingerprint)
    }
}
