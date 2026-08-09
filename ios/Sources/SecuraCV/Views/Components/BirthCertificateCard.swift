// BirthCertificateCard.swift
//
// Who this Canary is, on the screen where you're looking at it. Sits directly
// under the turntable, because the picture and the name answer the same
// question and neither one was typed by a human: the figure is resolved from
// what the device published about itself, and the name is derived from its
// key (BirthCertificate.swift).
//
// THE RULE THIS CARD FOLLOWS: every line is a fact the device or this phone
// can produce, labeled as what it actually is.
//
//   * "Born" is the device's own answer, and only when the device has earned
//     the word. A Canary's real born-on is the day its key was generated,
//     which lives on the device — so it reports it (`/api/fleet` `born_day`)
//     and this card shows it. Three cases, three different lines:
//       - the device says born, exactly    → "Born"
//       - the device says it was first dated later than it was made
//         → "First dated", with the reason, because calling a week on a
//           workshop shelf a birthday would be a guess wearing a fact's
//           clothes
//       - the device has never met a clock → no born line at all
//     "Paired" is shown alongside and is never a substitute: it is when YOU
//     added it, a fact about this phone. Two people looking at the same
//     Canary see the same birth day and different pairing dates, which is
//     exactly right.
//   * The name is shown with its derivation, not as a label: it comes from
//     the key, so it can be recomputed by anyone, and it changes only if the
//     key changes — which would honestly be a different bird.
//   * A device with no pinned key gets no certificate and says why, rather
//     than a placeholder name that would look like an answer.

import SwiftUI

struct BirthCertificateCard: View {
    let witness: Witness
    /// When this phone paired it, if it did. Nil for a Canary we can see but
    /// have not paired — it has no relationship with us to date.
    var pairedAt: Date?

    private var certificate: BirthCertificate? {
        BirthCertificateDerivation.certificate(fingerprint: witness.fingerprint,
                                               species: species,
                                               deviceID: witness.id)
    }

    /// What the device said it is, at full precision when it published one.
    ///
    /// Named from the shipped-product table rather than from the figure's
    /// title, and the difference is not cosmetic: a figure's title names ONE
    /// product, and some boards serve two (the 7" glass is both the Dash 7
    /// and the Nightstand 7), so reading the species off the picture would
    /// print "Canary Dash 7" on a Nightstand's own certificate — a wrong fact
    /// on the one card whose entire job is being checkable.
    ///
    /// The raw wire string is not a fallback either. It used to be, and it
    /// put "canary-nightstand7" on the certificate for any type the figure
    /// map didn't carry — an identifier where a name belongs. A type this
    /// build has never heard of falls back to the coarse family, which is
    /// less specific and still true.
    private var species: String {
        DeviceNaming.productName(published: witness.publishedType,
                                 hardware: witness.hardware)
            ?? witness.deviceType.role
    }

    var body: some View {
        if let certificate {
            VStack(alignment: .leading, spacing: Theme.s) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(HatchSpec.kicker.uppercased())
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .accessibilityHidden(true)
                    Text(certificate.name)
                        .font(.title3.weight(.semibold))
                    Text(certificate.lineage)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                if !certificate.motto.isEmpty {
                    Text("“\(certificate.motto)”")
                        .font(.footnote.italic())
                        .foregroundStyle(.secondary)
                }

                Divider()

                CertificateRow(label: "Species", value: certificate.species)
                CertificateRow(label: "Ring ID", value: certificate.ringId, monospaced: true)
                CertificateRow(label: "Fingerprint",
                               value: witness.fingerprint.isEmpty ? "—" : witness.fingerprint,
                               monospaced: true)
                if !witness.firmware.isEmpty {
                    CertificateRow(label: "Firmware", value: witness.firmware)
                }
                // The device's own day, labeled as precisely as the device
                // earned. Above "Paired" because it is the older fact.
                if let bornOn = witness.bornOn {
                    CertificateRow(label: witness.bornExact ? "Born" : "First dated",
                                   value: bornOn.formatted(date: .abbreviated, time: .omitted))
                }
                if let pairedAt {
                    CertificateRow(label: "Paired",
                                   value: pairedAt.formatted(date: .abbreviated, time: .omitted))
                }
                if witness.chainLength > 0 {
                    CertificateRow(label: "Witnessed", value: "\(witness.chainLength) sealed events")
                }

                // The claim this card is really making, said plainly. It is
                // also the reason there is no "rename" button here: the name
                // is not a field, it is a rendering of the key.
                Text("This name comes from the device's own key — any SecuraCV app derives the same one, and it changes only if the key does.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)

                // Said once, where the claim is, rather than left for someone
                // to wonder why one Canary says "Born" and another doesn't.
                if witness.bornOn != nil, !witness.bornExact {
                    Text("This Canary met a clock later than it was made, so this is the first day it could date itself — its real birthday is on or before it.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, Theme.xs)
            .accessibilityElement(children: .combine)
            .accessibilityLabel("\(certificate.name), \(certificate.lineage), \(certificate.species)")
        } else {
            // No key pinned yet — a heard beacon, or a device we haven't
            // paired. Say what's missing and what would fix it.
            VStack(alignment: .leading, spacing: Theme.xs) {
                Label("No certificate yet", systemImage: "seal")
                    .font(.subheadline.weight(.medium))
                Text("A Canary's name is derived from its key. Pair this one and its key gets pinned here — then the certificate appears, the same on every app you open it in.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, Theme.xs)
        }
    }
}

/// One labeled fact. Monospaced where the value is an identifier a person
/// might compare character by character.
private struct CertificateRow: View {
    let label: String
    let value: String
    var monospaced = false

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Spacer(minLength: Theme.m)
            Text(value)
                .font(monospaced ? .caption.monospaced() : .caption)
                .multilineTextAlignment(.trailing)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(label): \(value)")
    }
}

#if DEBUG
#Preview("Certificate — demo fleet") {
    List {
        ForEach(DemoFleet.witnesses().prefix(3)) { w in
            Section(w.displayName) {
                BirthCertificateCard(witness: w, pairedAt: Date())
            }
        }
    }
}
#endif
