//  ContentProvider.swift — the Top Shelf, told only by the cache.
//
//  tvOS runs this in its own extension process, on its own schedule, with the
//  app possibly not running at all. Everything it shows comes from ShelfCache
//  — the summary the Wall wrote on its last live refresh — and a cache older
//  than ShelfSnapshot.maxAge renders as NOTHING. The wall can label a
//  remembered fleet stale; the shelf has no room for a label, so its only
//  honest degraded state is an empty shelf.
//
//  Deliberately absent, and load-bearing in their absence:
//    * networking — a shelf that fetched for itself could disagree with the
//      wall about the same fleet at the same moment;
//    * CloudKit — an unentitled process constructing a CKContainer traps
//      uncatchably (the four-day iPhone lesson), and this process is
//      entitled to the app group and nothing else;
//    * the Rust core — nothing here verifies; it repeats what was verified.

import Foundation
import TVServices

final class ContentProvider: TVTopShelfContentProvider {

    override func loadTopShelfContent(completionHandler: @escaping (TVTopShelfContent?) -> Void) {
        // A missing cache, an unreadable cache, and an answer past its
        // freshness window all land in the same place: nil, an empty shelf.
        guard let snapshot = ShelfCache.load(), snapshot.isCurrent() else {
            completionHandler(nil)
            return
        }
        completionHandler(Self.content(for: snapshot))
    }

    /// One section, one item, one line: the fleet's own summary. The shelf is
    /// a glance above the icon, not a second wall — and never a video
    /// surface, same as everything else in this app.
    private static func content(for snapshot: ShelfSnapshot) -> TVTopShelfContent {
        let item = TVTopShelfSectionedItem(identifier: "fleet-status")
        item.title = snapshot.shelfTitle
        let section = TVTopShelfItemCollection(items: [item])
        section.title = "Your fleet"
        return TVTopShelfSectionedContent(sections: [section])
    }
}
