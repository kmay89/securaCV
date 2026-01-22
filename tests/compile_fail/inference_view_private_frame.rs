// Rationale: inference views must not expose the underlying raw frame.
use witness_kernel::InferenceView;

fn main() {
    let view: InferenceView<'static> =
        unsafe { std::mem::MaybeUninit::zeroed().assume_init() };
    let _frame = view.frame;
}
