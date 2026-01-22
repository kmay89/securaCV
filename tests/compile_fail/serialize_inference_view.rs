// Rationale: inference views must stay non-serializable to prevent raw export paths.
use serde::Serialize;
use witness_kernel::InferenceView;

#[derive(Serialize)]
struct Wrapper<'a> {
    view: InferenceView<'a>,
}

fn main() {}
