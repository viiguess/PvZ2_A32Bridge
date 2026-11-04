#include "host_env.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "fake_java.h"
#include "libc/stdio_imports.h"
#include "runtime/runtime.h"

namespace {

constexpr u32 kPid = 4242;
constexpr u32 kUid = 10123;

// ---- trivial imports --------------------------------------------------------------------------------------------
// A stub whose result is the integer stored in `fn`.
void return_constant(Ctx& ctx, const Stub& stub) {
  ctx.ret((u32)(intptr_t)stub.fn);
}

// ---- memory -----------------------------------------------------------------------------------------------------
// mmap: guest memory is one flat reservation, so a mapping is a fresh guest allocation (zeroed, or filled from the
// file). Protection flags are not enforced.
void import_mmap(Ctx& ctx, const Stub&) {  // (addr, len, prot, flags, fd, offset)
  constexpr u32 MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20;
  u32 addr = ctx.a(0), len = (ctx.a(1) + 4095) & ~4095u, flags = ctx.a(3);
  int fd = (int)ctx.a(4);
  u32 mem = (flags & MAP_FIXED) && addr ? addr : gmemalign(4096, len);
  if (!mem) {
    ctx.ret((u32)-1);
    return;
  }
  memset(H(mem), 0, len);
  if (fd >= 0 && !(flags & MAP_ANONYMOUS))
    (void)!pread(fd, H(mem), len, (off_t)ctx.a(5));
  fprintf(stderr, "[run] mmap(%08x, %x, prot %x, flags %x, fd %d) -> %08x\n", addr, len, ctx.a(2), flags, fd, mem);
  ctx.ret(mem);
}

// ---- strings ----------------------------------------------------------------------------------------------------
void import_strdup(Ctx& ctx, const Stub&) {
  const char* text = (const char*)ctx.p(0);
  u32 copy = gmalloc((u32)strlen(text) + 1);
  strcpy((char*)H(copy), text);
  ctx.ret(copy);
}

void import_strrchr(Ctx& ctx, const Stub&) {
  const char* base = (const char*)ctx.p(0);
  const char* hit = strrchr(base, (int)ctx.a(1));
  ctx.ret(hit ? ctx.a(0) + (u32)(hit - base) : 0);
}

// ---- system information ----------------------------------------------------------------------------------------
void import_uname(Ctx& ctx, const Stub&) {  // struct utsname: six 65-byte fields
  char* u = (char*)ctx.p(0);
  memset(u, 0, 65 * 6);
  strcpy(u, "Linux");
  strcpy(u + 65, "localhost");
  strcpy(u + 130, "5.10.43");
  strcpy(u + 195, "#1 SMP PREEMPT");
  strcpy(u + 260, "aarch64");
  ctx.ret(0);
}

void import_system_property_get(Ctx& ctx, const Stub&) {
  static const struct {
    const char* key;
    const char* value;
  } kProperties[] = {
      {"ro.build.version.sdk", "34"},
      {"ro.build.version.release", "14"},
      {"ro.product.model", "Pixel 5"},
      {"ro.product.manufacturer", "google"},
      {"ro.product.brand", "google"},
      {"ro.product.cpu.abi", "arm64-v8a"},
      {"ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a,armeabi"},
      {"ro.debuggable", "0"},
      {"ro.secure", "1"},
  };

  std::string key = (const char*)ctx.p(0), value;
  for (const auto& p : kProperties)
    if (key == p.key)
      value = p.value;
  fprintf(stderr, "[run] property %s = '%s'\n", key.c_str(), value.c_str());
  strcpy((char*)ctx.p(1), value.c_str());
  ctx.ret((u32)value.size());
}

void import_difftime(Ctx& ctx, const Stub&) {  // (time_t, time_t) -> double, returned in r0:r1
  double d = difftime((time_t)(int)ctx.a(0), (time_t)(int)ctx.a(1));
  uint64_t bits;
  memcpy(&bits, &d, 8);
  ctx.ret64(bits);
}

// ---- files ------------------------------------------------------------------------------------------------------
void import_pread(Ctx& ctx, const Stub&) {  // bionic's 32-bit pread takes a 32-bit offset
  ctx.ret((u32)pread(ctx.a(0), ctx.p(1), ctx.a(2), (off_t)ctx.a(3)));
}

void import_remove(Ctx& ctx, const Stub&) {
  ctx.ret((u32)remove((const char*)ctx.p(0)));
}

void import_lseek64(Ctx& ctx, const Stub&) {  // (fd, offset in r2:r3, whence on the stack) -> 64-bit result
  ctx.ret64((u64)lseek(ctx.a(0), (off_t)ctx.q(2), ctx.a(4)));
}

void import_noop(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

// dladdr(addr, Dl_info*): only the library itself is known. Dl_info = {fname, fbase, sname, saddr}.
void import_dladdr(Ctx& ctx, const Stub&) {
  u32* info = (u32*)ctx.p(1);
  bool ours = ctx.a(0) >= g_main.base && ctx.a(0) < g_main.base + g_main.size;
  info[0] = ours ? gstr(g_cfg.lib_path.c_str()) : 0;
  info[1] = ours ? g_main.base : 0;
  info[2] = info[3] = 0;
  ctx.ret(ours);
}

// ---- synthetic /proc/self/maps ----------------------------------------------------------------------------------
std::string g_maps_path;

const char* path_hook(const char* path) {
  return !strcmp(path, "/proc/self/maps") ? g_maps_path.c_str() : path;
}

}  // namespace

void register_host_imports() {
  stub_add("getpid", return_constant, (void*)(intptr_t)kPid);
  stub_add("gettid", return_constant, (void*)(intptr_t)kPid);
  stub_add("getuid", return_constant, (void*)(intptr_t)kUid);

  // Calls that succeed without doing anything: there are no signals, watches or other processes to manage.
  for (const char* name :
       {"munmap", "mprotect", "dlclose", "kill", "sigaltstack", "bsd_signal", "flock", "sleep", "pthread_rwlock_init",
        "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "pthread_rwlock_unlock"})
    stub_add(name, import_noop);
  // Calls that fail: inotify and wait() are not available, and there is no network.
  for (const char* name : {"inotify_init", "inotify_add_watch", "wait"})
    stub_add(name, return_constant, (void*)(intptr_t)-1);
  stub_add("gethostbyname", return_constant, (void*)0);

  stub_add("mmap", import_mmap);
  stub_add("strdup", import_strdup);
  stub_add("strrchr", import_strrchr);
  stub_add("uname", import_uname);
  stub_add("difftime", import_difftime);
  stub_add("__system_property_get", import_system_property_get);
  stub_add("pread", import_pread);
  stub_add("remove", import_remove);
  stub_add("lseek64", import_lseek64);
  stub_add("dladdr", import_dladdr);

  u32 page_size = gdata_alloc(4, 4);  // `__page_size` is a variable, not a function
  *(u32*)H(page_size) = 4096;
  sym_add("__page_size", page_size);

  g_path_hook = path_hook;
}

void write_fake_maps() {
  g_maps_path = g_cfg.work_dir + "/maps";
  FILE* f = fopen(g_maps_path.c_str(), "w");
  if (!f)
    return;
  static const char* const kSystemMappings[] = {
      "b6f00000-b6f20000 r-xp 00000000 103:00 301 /system/bin/linker",
      "b6f20000-b6f22000 r--p 00020000 103:00 301 /system/bin/linker",
      "b6f22000-b6f23000 rw-p 00022000 103:00 301 /system/bin/linker",
      "b6d00000-b6d70000 r-xp 00000000 103:00 302 /system/lib/libc.so",
      "b6d70000-b6d75000 r--p 00070000 103:00 302 /system/lib/libc.so",
      "b6d75000-b6d78000 rw-p 00075000 103:00 302 /system/lib/libc.so",
      "b6c00000-b6c05000 r-xp 00000000 103:00 303 /system/lib/libdl.so",
      "b6a00000-b6bf0000 r-xp 00000000 103:00 304 /system/lib/libart.so",
      "be000000-be021000 rw-p 00000000 00:00 0 [stack]",
  };
  fprintf(f, "%08x-%08x r-xp 00000000 fd:01 4242 %s\n", g_main.base, g_main.base + g_main.size, g_cfg.lib_path.c_str());
  for (const char* line : kSystemMappings)
    fprintf(f, "%s\n", line);
  fclose(f);
}
