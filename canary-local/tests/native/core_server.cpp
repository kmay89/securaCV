// canary-local/tests/native/core_server.cpp — one Lab WebAssembly core,
// built natively, answering the page tests over a pipe (sweep A40).
//
// cores.js (beside this file) compiles the sources build.sh hands em++ for a
// core with g++, links this file in, and talks to the process one line at a
// time. It is opt-in (LAB_CORES=native) and test-only: the Lab and CI's
// default runs load the committed dist, and this file is never compiled
// into it. The export table it serves, core_exports.inc, is written by
// cores.js from the core's bindings source (its EMSCRIPTEN_KEEPALIVE
// signatures), so an export added there is served without an edit here.
//
// The protocol (requests on stdin, one reply line per request on stdout):
//
//   c <name> <i32>...   call an export; the args are the i32s wasm would
//                       pass (cores.js applies ToInt32, as a wasm call does)
//                       →  =v                 void
//                          =n <i32>           a number, as wasm's i32 reads
//                          =s <text>          a C string (refused if it
//                                             holds a line break)
//                          =p <address>       a pointer, in hex
//   m <address> <len>   open a window on memory a pointer export returned
//   w <address> <hex>   write bytes inside an open window
//   r <address> <len>   read bytes inside an open window  →  =b <hex>
//
// Anything wrong (an unknown export, a bad line, a window nobody opened)
// answers "! <why>", and cores.js throws it. The firmware's own stdout is
// sent to stderr before the first request, so a printf in a firmware source
// cannot land in the reply stream.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

namespace {

struct Ret {
  char kind;          // 'v' void, 'n' number, 's' string, 'p' pointer
  int32_t num;
  const char* str;
  const void* ptr;
};

Ret ret_void() { return Ret{'v', 0, nullptr, nullptr}; }
Ret ret_num(int32_t v) { return Ret{'n', v, nullptr, nullptr}; }
Ret ret_str(const char* s) { return Ret{'s', 0, s, nullptr}; }
Ret ret_ptr(const void* p) { return Ret{'p', 0, nullptr, p}; }

struct Export {
  const char* name;
  int argc;
  Ret (*call)(const int32_t* a);
};

}  // namespace

// extern "C" declarations of the core's exports + kExports[], generated.
#include "core_exports.inc"

namespace {

FILE* g_out = nullptr;
std::map<uintptr_t, size_t> g_windows;   // address → length, opened by "m"
std::map<uintptr_t, bool> g_returned;    // every pointer an export returned

void reply(const std::string& s) {
  fputs(s.c_str(), g_out);
  fputc('\n', g_out);
  fflush(g_out);
}

void fail(const std::string& why) { reply("! " + why); }

bool parse_address(const char* s, uintptr_t* out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  const unsigned long long v = strtoull(s, &end, 16);
  if (*end) return false;
  *out = (uintptr_t)v;
  return true;
}

bool parse_size(const char* s, size_t* out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  const unsigned long long v = strtoull(s, &end, 10);
  if (*end) return false;
  *out = (size_t)v;
  return true;
}

// [addr, addr+len) inside one window "m" opened.
bool in_window(uintptr_t addr, size_t len) {
  auto it = g_windows.upper_bound(addr);
  if (it == g_windows.begin()) return false;
  --it;
  return addr >= it->first && addr + len <= it->first + it->second;
}

const Export* find_export(const char* name) {
  for (const Export& e : kExports) {
    if (strcmp(e.name, name) == 0) return &e;
  }
  return nullptr;
}

void do_call(std::vector<char*>& tok) {
  if (tok.size() < 2) return fail("c needs an export name");
  const Export* e = find_export(tok[1]);
  if (!e) return fail(std::string("no export ") + tok[1]);
  std::vector<int32_t> args((size_t)(e->argc > 0 ? e->argc : 1), 0);
  for (size_t i = 2; i < tok.size() && (int)(i - 2) < e->argc; ++i) {
    char* end = nullptr;
    const long long v = strtoll(tok[i], &end, 10);
    if (*end || v < INT32_MIN || v > INT32_MAX) {
      return fail(std::string("argument is not an i32: ") + tok[i]);
    }
    args[i - 2] = (int32_t)v;
  }
  const Ret r = e->call(args.data());
  char head[64];
  switch (r.kind) {
    case 'v':
      return reply("=v");
    case 'n':
      snprintf(head, sizeof(head), "=n %ld", (long)r.num);
      return reply(head);
    case 's':
      if (!r.str) return reply("=s");   // NULL reads as "" through UTF8ToString
      if (strchr(r.str, '\n') || strchr(r.str, '\r')) {
        return fail(std::string(e->name) + " returned a string with a line break");
      }
      return reply(std::string("=s ") + r.str);
    case 'p':
      g_returned[(uintptr_t)r.ptr] = true;
      snprintf(head, sizeof(head), "=p %llx", (unsigned long long)(uintptr_t)r.ptr);
      return reply(head);
  }
  fail("unknown return kind");
}

void do_window(std::vector<char*>& tok) {
  uintptr_t addr = 0;
  size_t len = 0;
  if (tok.size() != 3 || !parse_address(tok[1], &addr) || !parse_size(tok[2], &len)) {
    return fail("m needs <address> <len>");
  }
  if (!g_returned.count(addr)) return fail("m: no export returned that address");
  g_windows[addr] = len;
  reply("=v");
}

void do_write(std::vector<char*>& tok) {
  uintptr_t addr = 0;
  if (tok.size() != 3 || !parse_address(tok[1], &addr)) return fail("w needs <address> <hex>");
  const size_t hex = strlen(tok[2]);
  if (hex % 2) return fail("w: odd hex length");
  const size_t len = hex / 2;
  if (!in_window(addr, len)) return fail("w: outside every open window");
  unsigned char* dst = (unsigned char*)addr;
  for (size_t i = 0; i < len; ++i) {
    char pair[3] = {tok[2][2 * i], tok[2][2 * i + 1], 0};
    char* end = nullptr;
    const unsigned long b = strtoul(pair, &end, 16);
    if (*end) return fail("w: not hex");
    dst[i] = (unsigned char)b;
  }
  reply("=v");
}

void do_read(std::vector<char*>& tok) {
  uintptr_t addr = 0;
  size_t len = 0;
  if (tok.size() != 3 || !parse_address(tok[1], &addr) || !parse_size(tok[2], &len)) {
    return fail("r needs <address> <len>");
  }
  if (!in_window(addr, len)) return fail("r: outside every open window");
  static const char kHex[] = "0123456789abcdef";
  std::string out = "=b ";
  out.reserve(3 + 2 * len);
  const unsigned char* src = (const unsigned char*)addr;
  for (size_t i = 0; i < len; ++i) {
    out += kHex[src[i] >> 4];
    out += kHex[src[i] & 15];
  }
  reply(out);
}

}  // namespace

int main() {
  // The reply stream is a private copy of stdout; fd 1 becomes stderr, so
  // anything a firmware source prints goes to the test's stderr instead.
  const int proto = dup(1);
  if (proto < 0 || dup2(2, 1) < 0) return 2;
  g_out = fdopen(proto, "w");
  if (!g_out) return 2;

  std::string line;
  char chunk[4096];
  while (fgets(chunk, sizeof(chunk), stdin)) {
    line += chunk;
    if (line.empty() || line.back() != '\n') continue;   // a long line: keep reading
    line.pop_back();
    std::vector<char*> tok;
    for (char* t = strtok(&line[0], " "); t; t = strtok(nullptr, " ")) tok.push_back(t);
    if (tok.empty()) fail("empty request");
    else if (!strcmp(tok[0], "c")) do_call(tok);
    else if (!strcmp(tok[0], "m")) do_window(tok);
    else if (!strcmp(tok[0], "w")) do_write(tok);
    else if (!strcmp(tok[0], "r")) do_read(tok);
    else fail(std::string("unknown request ") + tok[0]);
    line.clear();
  }
  return 0;
}
