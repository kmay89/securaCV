// Rationale: raw frames must never be serialized or exported outside the kernel boundary.
use serde::Serialize;
use witness_kernel::RawFrame;

#[derive(Serialize)]
struct Wrapper {
    frame: RawFrame,
}

fn main() {}
