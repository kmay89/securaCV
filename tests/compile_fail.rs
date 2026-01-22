#[test]
fn compile_fail_no_raw_export() {
    let t = trybuild::TestCases::new();
    t.compile_fail("tests/compile_fail/*.rs");
}
