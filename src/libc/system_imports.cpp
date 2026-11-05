#include "libc/system_imports.h"

#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#ifndef A32_HOST_RUNNER  // bionic only: a Linux host has no system properties (jiagu_run answers them itself)
#include <sys/system_properties.h>
#endif
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "libc/libc.h"
#include "libc/stdio_imports.h"
#include "runtime/runtime.h"

static void return_zero(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void return_minus_one(Ctx& ctx, const Stub&) {
  ctx.ret((u32)-1);
}

static void import_getpid(Ctx& ctx, const Stub&) {
  ctx.ret((u32)getpid());
}

static void import_getuid(Ctx& ctx, const Stub&) {
  ctx.ret((u32)getuid());
}

// mmap: guest memory is one flat reservation, so a mapping is a fresh guest allocation (zeroed, or filled from the
// file). Protection flags are not enforced and munmap does nothing.
static void import_mmap(Ctx& ctx, const Stub&) {  // (addr, len, prot, flags, fd, offset)
  constexpr u32 MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20;
  u32 addr = ctx.a(0), length = (ctx.a(1) + 4095) & ~4095u, flags = ctx.a(3);
  int fd = (int)ctx.a(4);
  u32 memory = (flags & MAP_FIXED) && addr ? addr : gmemalign(4096, length);
  if (!memory) {
    ctx.ret((u32)-1);
    return;
  }
  memset(H(memory), 0, length);
  if (fd >= 0 && !(flags & MAP_ANONYMOUS))
    (void)!pread(fd, H(memory), length, (off_t)ctx.a(5));
  LOG("mmap(%08x, %x, prot %x, flags %x, fd %d) -> %08x", addr, length, ctx.a(2), flags, fd, memory);
  ctx.ret(memory);
}

static void import_strdup(Ctx& ctx, const Stub&) {
  const char* text = (const char*)ctx.p(0);
  u32 copy = gmalloc((u32)strlen(text) + 1);
  strcpy((char*)H(copy), text);
  ctx.ret(copy);
}

static void import_strrchr(Ctx& ctx, const Stub&) {
  const char* base = (const char*)ctx.p(0);
  const char* hit = strrchr(base, (int)ctx.a(1));
  ctx.ret(hit ? ctx.a(0) + (u32)(hit - base) : 0);
}

static void import_uname(Ctx& ctx, const Stub&) {  // struct utsname: six 65-byte fields
  char* info = (char*)ctx.p(0);
  memset(info, 0, 65 * 6);
  strcpy(info, "Linux");
  strcpy(info + 65, "localhost");
  strcpy(info + 130, "5.10.43");
  strcpy(info + 195, "#1 SMP PREEMPT");
  strcpy(info + 260, "armv8l");
  ctx.ret(0);
}

static void import_system_property_get(Ctx& ctx, const Stub&) {
#ifdef A32_HOST_RUNNER
  ((char*)ctx.p(1))[0] = 0;
  ctx.ret(0);
#else
  char value[PROP_VALUE_MAX] = "";
  int length = __system_property_get((const char*)ctx.p(0), value);
  strcpy((char*)ctx.p(1), value);
  ctx.ret((u32)length);
#endif
}

static void import_difftime(Ctx& ctx, const Stub&) {  // (time_t, time_t) -> double in r0:r1
  double difference = difftime((time_t)(int)ctx.a(0), (time_t)(int)ctx.a(1));
  uint64_t bits;
  memcpy(&bits, &difference, 8);
  ctx.ret64(bits);
}

static void import_pread(Ctx& ctx, const Stub&) {  // bionic's 32-bit pread takes a 32-bit offset
  ctx.ret((u32)pread(ctx.a(0), ctx.p(1), ctx.a(2), (off_t)ctx.a(3)));
}

static void import_remove(Ctx& ctx, const Stub&) {
  ctx.ret((u32)remove((const char*)ctx.p(0)));
}

static void import_lseek64(Ctx& ctx, const Stub&) {  // (fd, offset in r2:r3, whence on the stack) -> 64-bit result
  ctx.ret64((u64)lseek(ctx.a(0), (off_t)ctx.q(2), ctx.a(4)));
}

// dladdr(addr, Dl_info*): only the guest library itself is known. Dl_info = {fname, fbase, sname, saddr}.
static void import_dladdr(Ctx& ctx, const Stub&) {
  u32* info = (u32*)ctx.p(1);
  bool ours = ctx.a(0) >= g_main.base && ctx.a(0) < g_main.base + g_main.size;
  info[0] = ours ? gstr(g_main.path.c_str()) : 0;
  info[1] = ours ? g_main.base : 0;
  info[2] = info[3] = 0;
  ctx.ret(ours);
}

// ---- art::DexFile ---------------------------------------------------------------------------------------------------
static u32 g_dex_file;

u32 fake_dex_file(u32 base, u32 size) {
  if (!g_dex_file) {
    u32 no_op = stub_add("fake_dexfile_method", return_zero);
    u32 vtable = gdata_alloc(4 * 64, 4);
    for (int i = 0; i < 64; i++)
      ((u32*)H(vtable))[i] = no_op;
    g_dex_file = gdata_alloc(0x100, 4);
    memset(H(g_dex_file), 0, 0x100);
    ((u32*)H(g_dex_file))[0] = vtable;
  }
  ((u32*)H(g_dex_file))[1] = base;
  ((u32*)H(g_dex_file))[2] = size;
  return g_dex_file;
}

static void import_open_memory(Ctx& ctx, const Stub&) {  // (const uint8_t* base, size_t size, ...)
  LOG("art::DexFile::OpenMemory(%08x, %u)", ctx.a(0), ctx.a(1));
  ctx.ret(fake_dex_file(ctx.a(0), ctx.a(1)));
}

// ---- /proc/self/maps ------------------------------------------------------------------------------------------------
static char g_maps_path[32];

// "/proc/self/maps" and "/proc/<pid>/maps" (packers build the second form from getpid()).
static bool is_proc_maps(const char* path) {
  size_t length = strlen(path);
  return !strncmp(path, "/proc/", 6) && length > 11 && !strcmp(path + length - 5, "/maps");
}

static const char* maps_path_hook(const char* path) {
  return is_proc_maps(path) && g_maps_path[0] ? g_maps_path : path;
}

void install_fake_proc_maps() {
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
  int fd = (int)syscall(SYS_memfd_create, "maps", 0);  // an anonymous file, reachable as /proc/self/fd/<n>
  if (fd < 0)
    return;
  FILE* file = fdopen(fd, "w");
  if (!file)
    return;
  fprintf(file, "%08x-%08x r-xp 00000000 fd:01 4242 %s\n", g_main.base, g_main.base + g_main.size,
          g_main.path.c_str());
  for (const char* line : kSystemMappings)
    fprintf(file, "%s\n", line);
  fflush(file);  // the stream stays open so the descriptor keeps the file alive
  snprintf(g_maps_path, sizeof g_maps_path, "/proc/self/fd/%d", fd);
  g_path_hook = maps_path_hook;
}

void register_system_imports() {
  stub_add("getpid", import_getpid);
  stub_add("gettid", import_getpid);
  stub_add("getuid", import_getuid);
  // Succeed without doing anything: there are no signals, other processes or locks to manage.
  for (const char* name : {"munmap", "mprotect", "dlclose", "kill", "sigaltstack", "bsd_signal", "flock", "sleep",
                           "pthread_rwlock_init", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
                           "pthread_rwlock_unlock", "gethostbyname"})
    stub_add(name, return_zero);
  for (const char* name : {"inotify_init", "inotify_add_watch", "wait"})
    stub_add(name, return_minus_one);
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

  const char* const open_memory[] = {
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapES2_PS9_",
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_7OatFileEPS9_",
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_10OatDexFileEPS9_",
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPS9_"};
  for (const char* name : open_memory)
    stub_add(name, import_open_memory);

  u32 page_size = gdata_alloc(4, 4);  // `__page_size` is a variable, not a function
  *(u32*)H(page_size) = 4096;
  sym_add("__page_size", page_size);
}
