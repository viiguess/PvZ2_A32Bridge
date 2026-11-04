// Import stubs and the exported-symbol table.
//
// Every function the guest imports is resolved to an 8-byte stub in guest memory (`svc #index; bx lr`). When the
// guest executes the svc, the JIT calls back into the host with the call's register state wrapped in a `Ctx`.
#pragma once

#include <string>

#include "common.h"

struct Cpu;

// One guest -> host call. Arguments follow the 32-bit AAPCS: r0-r3, then the stack.
struct Ctx {
  Cpu* cpu;
  u32 r[4], sp;
  u32 o0 = 0, o1 = 0;  // return registers r0 / r1
  bool wide = false;   // true when the result is 64-bit (r0:r1)

  u32 a(int i) const {
    return i < 4 ? r[i] : *(u32*)H(sp + 4 * (i - 4));
  }  // i-th 32-bit argument

  void* p(int i) const {
    return H(a(i));
  }  // i-th argument as a host pointer

  u64 q(int i) const {
    return (u64)a(i) | (u64)a(i + 1) << 32;
  }  // 64-bit argument; caller aligns i

  void ret(u32 v) {
    o0 = v;
  }

  void ret64(u64 v) {
    o0 = (u32)v;
    o1 = (u32)(v >> 32);
    wide = true;
  }
};

struct Stub;
typedef void (*Handler)(Ctx& ctx, const Stub& stub);

struct Stub {
  const char* name;
  Handler handler;
  void* fn;         // handler-specific payload: a host function pointer or a small integer code
  const char* sig;  // signature string for the generic forwarder (see libc/forward.h)
  int index;
};

// Registers an import and returns its guest address. A null handler logs and returns 0 when called.
u32 stub_add(const char* name, Handler handler, void* fn = nullptr, const char* sig = nullptr);
const Stub* stub_by_index(u32 index);    // nullptr if out of range
u32 stub_find(const std::string& name);  // guest address of a symbol, 0 if unknown

void sym_add(const char* name, u32 guest_addr);              // export a data symbol
u32 sym_add_asm(const char* name, const u32* words, int n);  // export hand-assembled guest code

// Pre-ARMv6 code calls the Linux "kuser helpers" (atomics, TLS) at fixed addresses in the top page.
void kuser_init();
