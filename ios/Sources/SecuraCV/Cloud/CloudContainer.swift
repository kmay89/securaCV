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
//   The first fix was to name the container — `CKContainer(identifier:)` —
//   on the theory that it skips the entitlement lookup. It does not (the
//   full story is below, at the `#if`): an unentitled process traps there
//   too.
//   What actually holds is a COMPILE-TIME rule: `CloudContainer.shared`
//   exists only under `#if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT`, and
//   the unsigned builds that cannot carry entitlements set that flag, so code
//   that could construct a container in such a build does not compile into
//   it. A missing entitlement degrades to "no iCloud today", never to a crash.
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

    // WHETHER THIS BUILD CAN TALK TO CLOUDKIT AT ALL — the `#if` around
    // `shared` below, and nothing else. (A runtime `isUsable` once sat here
    // beside the same `#if`; no caller ever read it, because every call site
    // is already compiled out by the guard, which is the real check.)
    //
    // Not "is the user signed in" — that is `CloudSync.isAvailable`, asked
    // later and answered by CloudKit. This is the question underneath it: may
    // this process construct a container without dying?
    //
    // It has to be answered at COMPILE time, and that is the whole lesson of
    // this file. Every runtime test is either wrong or unavailable:
    //
    //   * `CKContainer.default()` — raises an uncatchable ObjC CKException.
    //   * `CKContainer(identifier:)` — was the first fix here, on the theory
    //     that naming the container skips the entitlement lookup that
    //     `default()` dies in. It does not. CloudKit logs "Significant issue
    //     at CKContainer.m:748: your process must have a
    //     com.apple.developer.icloud-services entitlement" and then traps
    //     inside `__allocating_init(identifier:)` — EXC_BREAKPOINT, `brk 1`,
    //     which Swift cannot catch either. Same death, different signal
    //     (abrt -> trap), which is exactly how it read on CI.
    //   * `FileManager.default.ubiquityIdentityToken` — cheap and
    //     non-throwing, but it answers about iCloud DOCUMENTS. This app
    //     declares no ubiquity container, so gating on it risks switching
    //     iCloud off for every real user to protect a build nobody ships.
    //   * reading the entitlement from the code signature — `SecTask*` is not
    //     in the public iOS SDK.
    //
    // So the guard is the one fact known for certain before the app runs: a
    // build compiled without code signing cannot carry entitlements, and
    // therefore cannot use CloudKit no matter what it asks. `heal.sh` sets
    // `SECURACV_NO_CLOUDKIT` on exactly the builds it passes
    // `CODE_SIGNING_ALLOWED=NO` to, so the flag and the signing decision are
    // made in one place and cannot disagree.
    //
    // Signed builds — device, TestFlight, App Store — are untouched by this.

    #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
    /// The app's container, named rather than defaulted.
    ///
    /// Reachable only under `#if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT`
    /// — that compile-time guard IS the check. Construction itself is what
    /// traps in an unentitled process, so there is no safe way to hold one of
    /// these and discover the problem later.
    ///
    /// Computed rather than a stored `static let` on purpose: CloudKit already
    /// hands back the same container object for a given identifier, so there is
    /// nothing to cache, and a stored global of a non-Sendable framework type
    /// is exactly what `SWIFT_STRICT_CONCURRENCY: complete` would flag the day
    /// this project tightens it (see ios/project.yml).
    static var shared: CKContainer { CKContainer(identifier: identifier) }
    #endif
}
