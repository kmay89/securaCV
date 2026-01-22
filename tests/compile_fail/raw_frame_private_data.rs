// Rationale: raw frame bytes must remain private and inaccessible.
use witness_kernel::RawFrame;

fn main() {
    let frame: RawFrame = unsafe { std::mem::MaybeUninit::zeroed().assume_init() };
    let _bytes = frame.data;
}
