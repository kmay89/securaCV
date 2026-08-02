// AboutView.swift  (watch app target)
//
// The wrist's About/health panel — the same build identity every SecuraCV
// surface shows (Shared/BuildInfo.swift reads the stamp scripts/stamp_build.sh
// baked into THIS bundle's Info.plist), plus the sync link's own state. RFC
// §7: "which version are you on?" must never be a support question, on any
// surface.

import SwiftUI

struct AboutView: View {
    @EnvironmentObject var store: WristStore

    var body: some View {
        NavigationStack {
            content
        }
    }

    private var content: some View {
        List {
            Section("About") {
                LabeledContent("Version", value: BuildInfo.version)
                LabeledContent("Build", value: BuildInfo.buildRev)
                LabeledContent("Firmware train", value: BuildInfo.firmwareTrain)
            }
            Section("Sync") {
                LabeledContent("iPhone", value: store.isPhoneReachable ? "Reachable" : "Not reachable")
                if let heard = store.lastHeardFromPhone {
                    LabeledContent("Last sync") {
                        Text(heard, style: .relative) + Text(" ago")
                    }
                } else if store.snapshot != nil {
                    LabeledContent("Last sync", value: "From cache")
                }
            }
            Section {
                Text("Your fleet's truth lives on your iPhone and your own devices. This app renders it — nothing is sent to any company server.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("About")
    }
}

#Preview("About — sample") {
    AboutView().environmentObject(WristStore.preview())
}
