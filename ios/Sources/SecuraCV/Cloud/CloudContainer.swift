// CloudContainer.swift
//
// ONE way to reach CloudKit, and it is not `CKContainer.default()`.
//
// WHY THIS FILE EXISTS
//   `CKContainer.default()` resolves its container by reading the app's
//   `com.apple.developer.icloud-container-identifiers` entitlement. When that
//   entitlement is absent it does not return nil and it does not throw a Swift
//   error — it raises an Objective-C `CKException`:
//
//     *** Terminating app due to uncaught exception 'CKException',
//         reason: 'containerIdentifier can not be nil'
//
//   Swift cannot catch an Objective-C exception. `try?` does nothing here: the
//   `try?` is on `accountStatus()`, and the app is already dead inside
//   `default()` before the await is reached. The process aborts.
//
//   An unsigned build has no entitlements at all. CI builds exactly that
//   (`CODE_SIGNING_ALLOWED=NO` in ios/scripts/heal.sh), so the first launch
//   that touched CloudKit killed the app before the test runner could even
//   connect — "Early unexpected exit, operation never finished bootstrapping."
//   Every iOS test failed, and none of them were about iCloud.
//
//   Naming the container explicitly avoids the entitlement lookup entirely.
//   `CKContainer(identifier:)` is handed a non-nil string, so the nil path is
//   never taken; if the app then turns out not to be entitled to that
//   container, the failure arrives as a `CKError` on the *operation* — an
//   ordinary Swift error the existing `try?` and `isAvailable` gates already
//   handle by leaving iCloud switched off. A missing entitlement should
//   degrade to "no iCloud today", never to a crash.
//
// THE RULE
//   Nothing in this app may call `CKContainer.default()`. Reach for
//   `CloudContainer.shared` instead. `scripts/lint_cloudkit_container.py`
//   fails the build on a new `.default()` call site, and cross-checks the
//   identifier below against BOTH entitlements files so the string here cannot
//   drift from the one the app is actually signed with.

import Foundation
#if canImport(CloudKit)
import CloudKit
#endif

enum CloudContainer {
    /// The CloudKit container this app owns. Must match
    /// `com.apple.developer.icloud-container-identifiers` in
    /// `ios/Support/SecuraCV.entitlements` and `SecuraCV.dev.entitlements`;
    /// the linter asserts all three agree.
    static let identifier = "iCloud.com.securacv.witness"

    #if canImport(CloudKit)
    /// The app's container, named rather than defaulted.
    ///
    /// Computed rather than a stored `static let` on purpose: CloudKit already
    /// hands back the same container object for a given identifier, so there is
    /// nothing to cache, and a stored global of a non-Sendable framework type
    /// is exactly what `SWIFT_STRICT_CONCURRENCY: complete` would flag the day
    /// this project tightens it (see ios/project.yml).
    ///
    /// Nothing here touches the network. An unentitled or signed-out app gets a
    /// perfectly valid object whose *operations* then fail politely.
    static var shared: CKContainer { CKContainer(identifier: identifier) }
    #endif
}
