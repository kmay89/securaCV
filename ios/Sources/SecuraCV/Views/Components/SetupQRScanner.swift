// SetupQRScanner.swift
//
// Read the `WIFI:` QR a display draws on its glass for its own setup
// network — the system's own scanner (VisionKit's DataScannerViewController,
// a frozen OS primitive), shown as a sheet, handing back the first QR it
// sees. The grammar is parsed by SetupPortal.parseWiFiQR, which is pure
// and host-tested; this file is only the camera.
//
// Not every device can scan (no camera, or a denied permission) — the
// walkthrough asks `isSupported` first and offers the typed fields either
// way, so the scanner is a convenience, never a gate.

import SwiftUI
#if canImport(VisionKit)
import VisionKit
#endif

struct SetupQRScannerSheet: View {
    var onCode: (String) -> Void
    @Environment(\.dismiss) private var dismiss

    static var isSupported: Bool {
        #if canImport(VisionKit)
        return DataScannerViewController.isSupported && DataScannerViewController.isAvailable
        #else
        return false
        #endif
    }

    var body: some View {
        Group {
            #if canImport(VisionKit)
            if Self.isSupported {
                SetupQRScanner(onCode: onCode)
                    .ignoresSafeArea()
            } else {
                unsupported
            }
            #else
            unsupported
            #endif
        }
        .navigationTitle("Scan the glass")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
        }
    }

    private var unsupported: some View {
        ContentUnavailableView("No scanner here",
                               systemImage: "camera.slash",
                               description: Text("Type the network name and key printed under the QR instead."))
    }
}

#if canImport(VisionKit)
struct SetupQRScanner: UIViewControllerRepresentable {
    var onCode: (String) -> Void

    func makeUIViewController(context: Context) -> DataScannerViewController {
        let scanner = DataScannerViewController(
            recognizedDataTypes: [.barcode(symbologies: [.qr])],
            qualityLevel: .balanced,
            recognizesMultipleItems: false,
            isHighFrameRateTrackingEnabled: false,
            isHighlightingEnabled: true)
        scanner.delegate = context.coordinator
        try? scanner.startScanning()
        return scanner
    }

    func updateUIViewController(_ uiViewController: DataScannerViewController, context: Context) {}

    static func dismantleUIViewController(_ uiViewController: DataScannerViewController, coordinator: Coordinator) {
        uiViewController.stopScanning()
    }

    func makeCoordinator() -> Coordinator { Coordinator(onCode: onCode) }

    final class Coordinator: NSObject, DataScannerViewControllerDelegate {
        let onCode: (String) -> Void
        private var delivered = false
        init(onCode: @escaping (String) -> Void) { self.onCode = onCode }

        func dataScanner(_ dataScanner: DataScannerViewController, didAdd addedItems: [RecognizedItem],
                         allItems: [RecognizedItem]) {
            guard !delivered else { return }
            for item in addedItems {
                if case .barcode(let barcode) = item, let payload = barcode.payloadStringValue {
                    delivered = true
                    onCode(payload)
                    return
                }
            }
        }
    }
}
#endif
