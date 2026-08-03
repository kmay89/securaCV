// HoneycombGeometryTests.swift
//
// The comb's math, pinned: slots cycle N then N−1, centers really are
// hexagonally packed (every pair at least one full pitch apart — cells can
// never overlap, at any fleet size), and the comb's height grows row by row.

import XCTest
@testable import SecuraCV

final class HoneycombGeometryTests: XCTestCase {
    func testSlotsCycleFullRowThenOffsetRow() {
        // perRow 3 → rows of 3, 2, 3, 2 …
        let expected: [(row: Int, col: Int)] = [
            (0, 0), (0, 1), (0, 2),
            (1, 0), (1, 1),
            (2, 0), (2, 1), (2, 2),
            (3, 0),
        ]
        for (index, want) in expected.enumerated() {
            let got = HoneycombGeometry.slot(index: index, perRow: 3)
            XCTAssertEqual(got.row, want.row, "index \(index)")
            XCTAssertEqual(got.col, want.col, "index \(index)")
        }
    }

    func testNoTwoCellsCanEverOverlap() {
        let diameter = 96.0, gap = 10.0, pitch = diameter + gap
        var centers: [(Double, Double)] = []
        for i in 0..<24 {
            let s = HoneycombGeometry.slot(index: i, perRow: 3)
            centers.append(HoneycombGeometry.center(row: s.row, col: s.col,
                                                    diameter: diameter, gap: gap))
        }
        for i in 0..<centers.count {
            for j in (i + 1)..<centers.count {
                let dx = centers[i].0 - centers[j].0
                let dy = centers[i].1 - centers[j].1
                let distance = (dx * dx + dy * dy).squareRoot()
                // Hex packing: nearest neighbours sit exactly one pitch
                // apart. Tolerance is a float-noise allowance (1 µpoint),
                // not a design allowance — cells still provably can't touch.
                XCTAssertGreaterThanOrEqual(distance, pitch - 1e-6,
                                            "cells \(i) and \(j) too close")
            }
        }
    }

    func testOddRowsAreShiftedHalfAPitch() {
        let even = HoneycombGeometry.center(row: 0, col: 0, diameter: 96, gap: 10)
        let odd = HoneycombGeometry.center(row: 1, col: 0, diameter: 96, gap: 10)
        XCTAssertEqual(odd.x - even.x, (96 + 10) / 2, accuracy: 1e-9)
    }

    func testHeightGrowsByRowPitch() {
        let d = 96.0, g = 10.0
        let one = HoneycombGeometry.height(count: 1, perRow: 3, diameter: d, gap: g)
        XCTAssertEqual(one, d, accuracy: 1e-9)
        let secondRow = HoneycombGeometry.height(count: 4, perRow: 3, diameter: d, gap: g)
        XCTAssertEqual(secondRow, d + (d + g) * HoneycombGeometry.rowPitchFactor, accuracy: 1e-9)
        XCTAssertEqual(HoneycombGeometry.height(count: 0, perRow: 3, diameter: d, gap: g), 0)
    }

    func testCellsPerRowFitsTheWidthItIsGiven() {
        // iPhone-ish width: first cell 96, each further cell needs 106.
        XCTAssertEqual(HoneycombGeometry.cellsPerRow(width: 393, diameter: 96, gap: 10), 3)
        // iPad detail column.
        XCTAssertEqual(HoneycombGeometry.cellsPerRow(width: 700, diameter: 96, gap: 10), 6)
        // Never fewer than two, even absurdly narrow.
        XCTAssertEqual(HoneycombGeometry.cellsPerRow(width: 50, diameter: 96, gap: 10), 2)
    }
}
