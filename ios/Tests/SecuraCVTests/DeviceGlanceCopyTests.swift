// DeviceGlanceCopyTests.swift — the one-line room and hub sentences.
//
// Shared/DeviceGlanceCopy is compiled by the Witness Wall (its card and
// detail screen) and the watch (row and detail). The Wall's own tests pin
// what its card says through `wallWellbeingLine`; these pin the sentences at
// the source, so a change here is seen by the phone's suite too.

import XCTest
@testable import SecuraCV

final class DeviceGlanceCopyTests: XCTestCase {

    func testSilenceIsNeverAnEmptyCalmRoom() {
        XCTAssertNil(DeviceGlanceCopy.wellbeingLine(present: nil, occupants: nil, breathing: nil))
        XCTAssertNil(DeviceGlanceCopy.wellbeingLine(present: nil, occupants: nil, breathing: nil,
                                                    seeing: "", seeingScore: 90))
    }

    func testTheRoomInThePhonesWords() {
        XCTAssertEqual(DeviceGlanceCopy.wellbeingLine(present: false, occupants: 0, breathing: nil),
                       "Room clear · 0 in the room")
        XCTAssertEqual(DeviceGlanceCopy.wellbeingLine(present: true, occupants: 1, breathing: true),
                       "Someone present · 1 in the room · breathing rhythm sensed")
    }

    func testTheCountTopsOutAtTwoPlus() {
        // The radar's contract is 0 / 1 / 2-meaning-2-or-more; it cannot
        // count a crowd, so no surface may print a bigger number.
        for n in [2, 3, 17] {
            XCTAssertEqual(DeviceGlanceCopy.wellbeingLine(present: nil, occupants: n, breathing: nil),
                           "2+ in the room")
        }
    }

    func testSeeingSpeaksOnlyTheFourWordVocabulary() {
        XCTAssertEqual(DeviceGlanceCopy.seeingPhrase("person"), "seeing a person")
        XCTAssertEqual(DeviceGlanceCopy.seeingPhrase("package"), "seeing a package")
        // Invariant II ends the list: anything finer, or unknown, is silence.
        for word in ["face", "Person", "dog", "plate", nil] as [String?] {
            XCTAssertNil(DeviceGlanceCopy.seeingPhrase(word), String(describing: word))
        }
        XCTAssertEqual(DeviceGlanceCopy.wellbeingLine(present: nil, occupants: nil, breathing: nil,
                                                      seeing: "package", seeingScore: 87),
                       "Seeing a package · 87%")
    }

    func testTheHubIsExplainedOnlyWhenThereIsSomethingToDo() {
        XCTAssertEqual(DeviceGlanceCopy.hubLine(.absent), "No hub yet — it works on its own")
        XCTAssertEqual(DeviceGlanceCopy.hubLine(.down), "Can't reach its hub")
        XCTAssertNil(DeviceGlanceCopy.hubLine(.ok))
        XCTAssertNil(DeviceGlanceCopy.hubLine(.unknown))
        // The gate callers use agrees with the sentence's own silence.
        for hub in [HubState.absent, .down, .ok, .unknown] {
            XCTAssertEqual(DeviceGlanceCopy.hubLine(hub) != nil, hub.needsAttention, "\(hub)")
        }
    }
}
