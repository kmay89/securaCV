# Cut NvsManager's constructor, destructor, begin() and end() verbatim out of
# firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp, for
# test_nvs_manager_lock.cpp to compile against the real securacv_crypto.h and
# the stubs in stubs/nvs_manager (the rest of that file needs the Arduino
# crypto stack). Each function runs from the line that starts with its
# signature to the first line that is exactly "}" (or ends on the signature
# line, for a one-line body), and is preceded by a #line directive so the
# compiler names securacv_crypto.cpp's own lines. Exits non-zero, and the
# Makefile fails, unless it finds all four exactly once — a renamed or split
# function must fail the build, never compile an empty harness.
#
#   awk -f nvs_manager_cut.awk ../canary/lib/securacv_crypto/src/securacv_crypto.cpp
BEGIN {
  sig[1] = "NvsManager::NvsManager("
  sig[2] = "NvsManager::~NvsManager("
  sig[3] = "bool NvsManager::begin("
  sig[4] = "void NvsManager::end("
}
{
  start = 0
  for (i = 1; i <= 4; i++) {
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
  for (i = 1; i <= 4; i++) {
    if (bad == "" && seen[i] != 1) bad = "\"" sig[i] "\" found " (seen[i] + 0) " time(s), expected once"
  }
  if (bad != "") {
    print "nvs_manager_cut.awk: " bad " in " FILENAME > "/dev/stderr"
    exit 1
  }
}
