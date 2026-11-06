#include "libc/compat_imports.h"

#include <dlfcn.h>

#include "libc/forward.h"
#include "runtime/jit.h"

// ---- libgcc helpers (division by zero returns 0, like libgcc's default __aeabi_idiv0) ----
static void set_r2_r3(Ctx& ctx, u64 value) {
  auto& regs = ctx.cpu->jit->Regs();
  regs[2] = (u32)value;
  regs[3] = (u32)(value >> 32);
}

static void import_idiv(Ctx& ctx, const Stub&) {
  int32_t a = (int32_t)ctx.a(0), b = (int32_t)ctx.a(1);
  ctx.ret(b == 0 ? 0 : (b == -1 ? (u32)(0u - (u32)a) : (u32)(a / b)));
}

static void import_uidiv(Ctx& ctx, const Stub&) {
  u32 b = ctx.a(1);
  ctx.ret(b ? ctx.a(0) / b : 0);
}

static void import_idivmod(Ctx& ctx, const Stub&) {  // quotient in r0, remainder in r1
  int32_t a = (int32_t)ctx.a(0), b = (int32_t)ctx.a(1);
  u32 quotient = 0, remainder = (u32)a;
  if (b == -1) {
    quotient = 0u - (u32)a;
    remainder = 0;
  } else if (b) {
    quotient = (u32)(a / b);
    remainder = (u32)(a % b);
  }
  ctx.ret64((u64)remainder << 32 | quotient);
}

static void import_uidivmod(Ctx& ctx, const Stub&) {
  u32 a = ctx.a(0), b = ctx.a(1);
  ctx.ret64(b ? (u64)(a % b) << 32 | (a / b) : (u64)a << 32);
}

static void import_ldivmod(Ctx& ctx, const Stub&) {  // quotient in r0:r1, remainder in r2:r3
  int64_t a = (int64_t)ctx.q(0), b = (int64_t)ctx.q(2);
  int64_t quotient = 0, remainder = a;
  if (b == -1) {
    quotient = (int64_t)(0ull - (u64)a);
    remainder = 0;
  } else if (b) {
    quotient = a / b;
    remainder = a % b;
  }
  set_r2_r3(ctx, (u64)remainder);
  ctx.ret64((u64)quotient);
}

static void import_uldivmod(Ctx& ctx, const Stub&) {
  u64 a = ctx.q(0), b = ctx.q(2);
  set_r2_r3(ctx, b ? a % b : a);
  ctx.ret64(b ? a / b : 0);
}

static void import_lmul(Ctx& ctx, const Stub&) {
  ctx.ret64(ctx.q(0) * ctx.q(2));
}

// ---- old libstdc++: operator new / delete on the guest heap ----
static void import_new(Ctx& ctx, const Stub&) {
  ctx.ret(gmalloc(ctx.a(0) ? ctx.a(0) : 1));
}

static void import_delete(Ctx& ctx, const Stub&) {
  gfree(ctx.a(0));
}

// ---- small libc functions ----
static void return_zero(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void import_valloc(Ctx& ctx, const Stub&) {
  ctx.ret(gmemalign(4096, ctx.a(0)));
}

void register_compat_imports() {
  hle("__aeabi_idiv", import_idiv);
  hle("__aeabi_uidiv", import_uidiv);
  hle("__aeabi_idivmod", import_idivmod);
  hle("__aeabi_uidivmod", import_uidivmod);
  hle("__aeabi_ldivmod", import_ldivmod);
  hle("__aeabi_uldivmod", import_uldivmod);
  hle("__aeabi_lmul", import_lmul);
  for (const char* name : {"_Znwj", "_Znaj"})
    hle(name, import_new);
  for (const char* name : {"_ZdlPv", "_ZdaPv"})
    hle(name, import_delete);
  for (const char* name : {"atexit", "dlerror", "tmpfile"})  // no teardown, no error text, unsupported
    hle(name, return_zero);
  hle("valloc", import_valloc);
  // getc's slow path in old bionic headers; guest FILEs never buffer, so it is fgetc
  stub_add("__srget", generic, dlsym(RTLD_DEFAULT, "fgetc"), "F>i");
  gen("inet_aton", "pp>i");
  gen("getpeername", "ipp>i");
  gen("getsockopt", "iiipp>i");

  // char* tzname[2] is a variable
  u32 names = gdata_alloc(8, 4);
  ((u32*)H(names))[0] = gstr("UTC");
  ((u32*)H(names))[1] = gstr("UTC");
  sym_add("tzname", names);
}
