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
//   * "Paired" is when YOU added it — not a birthday. A Canary's real
//     born-on is the moment its key was generated, which lives on the device;
//     until it tells us, this card says "paired", because a birthday we
//     inferred would be the first decorative thing on a screen whose whole
//     job is being checkable.
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

    /// What the device said it is, at full precision when it published one —
    /// the same string the figure lookup resolves against, so the picture and
    /// the species line can never name two different products.
    private var species: String {
        if let published = witness.publishedType, !published.isEmpty {
            return FleetFigure.forDeviceType(published)?.title ?? published
        }
        return witness.deviceType.role
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
