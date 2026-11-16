// RefuseRedirects.swift
//
// Every client that dials a Canary or a hub on this network refuses HTTP
// redirects. The address gate (DeviceAPI.isPrivate) is checked on the URL a
// call starts with; a 302/307 from whatever answers that address would
// otherwise carry the request (its bearer token, and for a Wi-Fi hand-off
// the household password in the body) to wherever the Location names,
// public internet included. ATS blocks only cleartext off the network, so
// an https Location would pass it. With this delegate the redirect is not
// followed: the 3xx itself is the answer, and each caller treats a non-2xx
// as the failure it is.
//
// The tvOS Wall carries its own `RefuseRedirects` for its fleet client;
// this is the iPhone's twin.

import Foundation

/// A stateless task delegate whose one job is to decline every redirect.
final class RefuseRedirects: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    static let shared = RefuseRedirects()

    func urlSession(_ session: URLSession,
                    task: URLSessionTask,
                    willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

/// The one session for plain-http calls to a device on this network.
/// `.default` configuration on purpose: it shares the system cookie store
/// and cache exactly as `URLSession.shared` did (the WAP's `cv_session`
/// cookie from a claim's receipt rides the same store), plus the delegate
/// above. App-lifetime, like PinnedSessions: a session retains its delegate.
enum LANSession {
    static let plain: URLSession = URLSession(configuration: .default,
                                              delegate: RefuseRedirects.shared,
                                              delegateQueue: nil)
}
