// Import stubs, the exported-symbol table, and the Linux kuser helpers.
#include "runtime/stubs.h"

#include <string.h>

#include <mutex>
#include <unordered_map>

// ---------------------------------------------------------------- symbols / stubs
static const u32 kMaxStubs = 65536;  // limited by the 16-bit svc immediate
static Stub* g_stubs[kMaxStubs];
static u32 g_nstubs = 1;                       // slot 0 = RET_ADDR
static u32 g_asm_next = STUB_BASE + 0x200000;  // hand-assembled stubs (setjmp...)
static std::unordered_map<std::string, u32> g_syms;
static std::mutex g_sym_mu;

static void unimpl(Ctx& ctx, const Stub& stub) {
  LOG("UNIMPLEMENTED import called: %s", stub.name);
  ctx.ret(0);
}

u32 stub_add(const char* name, Handler handler, void* fn, const char* sig) {
  std::lock_guard<std::mutex> lock(g_sym_mu);
  if (g_nstubs >= kMaxStubs) {
    LOG("stub table full");
    return 0;
  }
  u32 index = g_nstubs++;
  g_stubs[index] = new Stub{name, handler ? handler : unimpl, fn, sig, (int)index};
  u32* code = (u32*)(G + STUB_BASE + 8 * index);
  code[0] = 0xEF000000 | index;  // svc #idx (idx < 65536)
  code[1] = 0xE12FFF1E;          // bx lr
  u32 addr = STUB_BASE + 8 * index;
  g_syms[name] = addr;
  return addr;
}

const Stub* stub_by_index(u32 index) {
  return index < g_nstubs ? g_stubs[index] : nullptr;
}

u32 sym_add_asm(const char* name, const u32* words, int count) {
  std::lock_guard<std::mutex> lock(g_sym_mu);
  u32 addr = g_asm_next;
  memcpy(G + addr, words, 4 * count);
  g_asm_next += (4 * count + 7) & ~7u;
  g_syms[name] = addr;
  return addr;
}

void sym_add(const char* name, u32 addr) {
  std::lock_guard<std::mutex> lock(g_sym_mu);
  g_syms[name] = addr;
}

u32 stub_find(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_sym_mu);
  auto it = g_syms.find(name);
  return it == g_syms.end() ? 0 : it->second;
}

std::vector<std::pair<std::string, u32>> sym_all() {
  std::lock_guard<std::mutex> lock(g_sym_mu);
  return std::vector<std::pair<std::string, u32>>(g_syms.begin(), g_syms.end());
}

// Pre-ARMv6 binaries call the Linux kuser helpers (atomics/TLS) at fixed addresses in the top page.
void kuser_init() {
  static const u32 barrier[] = {0xF57FF05B, 0xE12FFF1E};  // dmb ish; bx lr
  static const u32 cmpxchg[] = {0xE1923F9F, 0xE0533000, 0x01823F91, 0x03330001,
                                0x0AFFFFFA, 0xE2730000, 0xE12FFF1E};  // ldrex/strex loop
  static const u32 get_tls[] = {0xEE1D0F70, 0xE12FFF1E};              // mrc p15,0,r0,c13,c0,3; bx lr
  memcpy(G + KUSER_BASE + 0xfa0, barrier, sizeof barrier);
  memcpy(G + KUSER_BASE + 0xfc0, cmpxchg, sizeof cmpxchg);
  memcpy(G + KUSER_BASE + 0xfe0, get_tls, sizeof get_tls);
  *(u32*)(G + KUSER_BASE + 0xffc) = 5;
}
