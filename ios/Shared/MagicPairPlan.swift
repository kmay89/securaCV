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
//     on the Wi-Fi, and its row under "Ready to pair" on the Fleet tab
//     says how to finish (PairView: its recovery kit, or a fresh setup).
//   * "Nothing answered" is NOT final while the claim lives: the claim was
//     never spent, so the sheet offers Try again until it expires — the
//     common case is a phone that was off Wi-Fi, or a `.local` name that
//     had not settled yet, and both fix themselves in seconds.
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
        case notPaired(String)
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
        /// The joined-but-not-paired ending: where to finish. It used to
        /// promise "pair it from the Fleet tab with a short tap on its BOOT
        /// button" — a flow the Fleet tab never had (a BOOT-tap receipt
        /// names the WAP's setup-network address, which is gone once it
        /// joins). Its row under Ready to pair is real: PairView takes its
        /// recovery kit, or walks a fresh setup.
        static let pairLater =
            "It's on your Wi-Fi but not paired with this phone yet — tap it under Ready to pair on the Fleet tab to finish."
        static let noClaim =
            "The Canary joined, but it handed this phone no claim to pair with. " + pairLater
        static let notPrivate =
            "Pairing refused: the Canary's claim points somewhere that is not a private address on your network, so it was not dialed."
        static let tlsNoPin =
            "Pairing refused: this Canary speaks https but named no certificate fingerprint, so its connection can't be checked. Update its firmware, then set it up from this phone again."
        static let unreachable =
            "The Canary joined, but this phone couldn't reach it over your Wi-Fi yet to collect its pairing key — it may still be settling in. Try again in a moment; its key waits three minutes."
        /// The same "nothing answered", when the phone itself was off
        /// Wi-Fi — the cause named, and the one thing to do about it.
        static let phoneOffWiFi =
            "This phone is off Wi-Fi, so it couldn't collect the Canary's pairing key — the key is only handed over on your home network, never over Bluetooth. Join your Wi-Fi, then tap Try again; its key waits three minutes."
        /// The retry window closed with nothing reached.
        static let claimExpired =
            "The Canary is on your Wi-Fi, but its one-time pairing key expired before this phone could reach it. " + pairLater
        static let refused =
            "The Canary joined, but declined the claim — it was already spent, or it expired. " + pairLater
        static func other(_ detail: String) -> String {
            let said = detail.trimmingCharacters(in: CharacterSet(charactersIn: ". \n"))
            return "The Canary joined, but collecting its pairing key failed: \(said). " + pairLater
        }
        static let receiptNotPrivate =
            "Pairing refused: the receipt points the Canary somewhere that is not a private address on your network."
        static let receiptTLSNoPin =
            "Pairing refused: the receipt names a secure (https) Canary but carries no certificate fingerprint, so its connection can't be checked. Update its firmware, then set it up from this phone again."

        /// Every sentence, for the sweeps.
        static var all: [String] {
            [pairLater, noClaim, notPrivate, tlsNoPin, unreachable, phoneOffWiFi, claimExpired,
             refused, other("x"), receiptNotPrivate, receiptTLSNoPin]
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
        guard let claim, let url = claim.claimURL else { return .notPaired(Copy.noClaim) }
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
                return .notPaired(Copy.unreachable)
            }
            if isTLS(url), claim.tlsCertFingerprint == nil {
                return .notPaired(Copy.unreachable)
            }
            return .retryViaIP(url, pin: claim.tlsCertFingerprint)
        case .refused:
            return .notPaired(Copy.refused)
        case .other(let detail):
            return .notPaired(Copy.other(detail))
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

    /// How long a claim lives once read, when the claim does not say
    /// (claim_ticket.h mints it for 180 s; `expires_in` overrides).
    static let defaultClaimLifetime: TimeInterval = 180

    /// May the sheet offer "Try again" — dial the same claim once more?
    /// Only when the last dial never reached the Canary (`.unreachable`:
    /// the claim is unspent, the firmware spends it only on a request it
    /// received) and the claim is still alive, with a few seconds' margin
    /// so a retry is not sent into the expiry. A refusal, any other
    /// answer, or a receipt never retries.
    static func mayRetry(after outcome: FetchOutcome?, claim: ImprovWire.Claim,
                         readAt: Date, now: Date) -> Bool {
        guard outcome == .unreachable else { return false }
        let lifetime = claim.expiresIn.map { TimeInterval(max(0, $0)) } ?? defaultClaimLifetime
        return now < readAt.addingTimeInterval(lifetime - 5)
    }

    /// The note for a dial that reached nobody: the phone being off Wi-Fi
    /// is the cause worth naming (NetworkVantage knows it without any
    /// permission); otherwise the Canary may still be settling in.
    static func unreachableNote(phoneOnWiFi: Bool) -> String {
        phoneOnWiFi ? Copy.unreachable : Copy.phoneOffWiFi
    }

    private static func dial(_ url: URL, claim: ImprovWire.Claim, isPrivateHost: (URL) -> Bool) -> Step {
        guard isPrivateHost(url) else { return .refused(Copy.notPrivate) }
        if isTLS(url), claim.tlsCertFingerprint == nil { return .refused(Copy.tlsNoPin) }
        return .spendClaim(url, pin: claim.tlsCertFingerprint)
    }
}
