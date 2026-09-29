# Cut named functions verbatim out of one firmware source file, for a host
# test to compile against the real headers and the stubs in stubs/ (the rest
# of that file needs the Arduino stack). `sigs` is the list, separated by "|":
# each entry is the start of a signature line, matched at column 0. Each
# function runs from the line that starts with its signature to the first
# line that is exactly "}" (or ends on the signature line, for a one-line
# body), and is preceded by a #line directive so the compiler names the
# firmware file's own lines. Exits non-zero, and the Makefile fails, unless it
# finds every signature exactly once — a renamed or split function must fail
# the build, never compile an empty harness.
#
#   awk -v sigs='NvsManager::NvsManager(|void NvsManager::end(' \
#       -f cut_functions.awk ../canary/lib/securacv_crypto/src/securacv_crypto.cpp
#
# Users (firmware/tests_host/Makefile): test_nvs_manager_lock.cpp (NvsManager's
# constructor, destructor, begin() and end()), test_nvs_store_result.cpp (the
# NVS store helpers and the puts under them) and test_chain_persist.cpp
# (securacv_witness.cpp's chain persist and birth-stamp glue, and
# witness_create_record_gps(), whose body the recipe greps for its call into
# that glue).
BEGIN {
  n = split(sigs, sig, "|")
  if (n == 0) bad = "no signatures given (-v sigs='a(|b(')"
}
{
  start = 0
  for (i = 1; i <= n; i++) {
    if (index($0, sig[i]) == 1) { start = i; seen[i]++ }
  }
  if (start) {
    if (inside) { bad = "a signature inside another function's body"; exit 1 }
    printf "#line %d \"%s\"\n", FNR, FILENAME
    inside = 1
  }
  if (inside) print
  if (inside && ($0 == "}" || (start && substr($0, length($0), 1) == "}"))) inside = 0
}
END {
  if (bad == "" && inside) bad = "a function with no closing \"}\" line"
  for (i = 1; i <= n; i++) {
    if (bad == "" && seen[i] != 1) bad = "\"" sig[i] "\" found " (seen[i] + 0) " time(s), expected once"
  }
  if (bad != "") {
    print "cut_functions.awk: " bad " in " FILENAME > "/dev/stderr"
    exit 1
  }
}
