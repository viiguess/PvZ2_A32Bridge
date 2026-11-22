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

#include <mutex>
#include <string>
#include <vector>

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
  ctx.ret((u32)remove(guest_host_path((const char*)ctx.p(0))));
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

void (*g_dex_handover_hook)(const uint8_t* dex, u32 size) = nullptr;

// Android 6+ builds return std::unique_ptr<const DexFile> through a hidden first argument: (result*, base, size, ...).
template <bool kSret>
static void import_open_memory(Ctx& ctx, const Stub&) {  // (const uint8_t* base, size_t size, ...)
  const int shift = kSret ? 1 : 0;
  LOG("art::DexFile::OpenMemory(%08x, %u)", ctx.a(shift), ctx.a(shift + 1));
  if (g_dex_handover_hook)
    g_dex_handover_hook((const uint8_t*)H(ctx.a(shift)), ctx.a(shift + 1));
  u32 dex_file = fake_dex_file(ctx.a(shift), ctx.a(shift + 1));
  if (kSret) {
    *(u32*)ctx.p(0) = dex_file;
    ctx.ret(ctx.a(0));
  } else
    ctx.ret(dex_file);
}

// ---- /proc/self/maps ------------------------------------------------------------------------------------------------
static char g_maps_path[256];
static int g_maps_memfd = -1;
static std::mutex g_maps_mutex;

struct MappedImage {
  u32 base, size;
  std::string name;
};

static std::vector<MappedImage> g_maps_images;  // the guest library, then companion libraries (mods)

// "/proc/self/maps" and "/proc/<pid>/maps" (packers build the second form from getpid()).
static bool is_proc_maps(const char* path) {
  size_t length = strlen(path);
  return !strncmp(path, "/proc/", 6) && length > 11 && !strcmp(path + length - 5, "/maps");
}

static const char* maps_path_hook(const char* path) {
  // A32_PATH_MAP=<guest path>=<host path>: serve a host file for one guest path (e.g. a synthetic libart.so).
  if (const char* map = getenv("A32_PATH_MAP")) {  // several mappings are separated by ';'
    static std::string host_file;
    for (const char* entry = map; entry && *entry;) {
      const char* end = strchr(entry, ';');
      size_t entry_length = end ? (size_t)(end - entry) : strlen(entry);
      const char* eq = (const char*)memchr(entry, '=', entry_length);
      if (eq && (size_t)(eq - entry) == strlen(path) && !strncmp(path, entry, eq - entry)) {
        host_file.assign(eq + 1, entry + entry_length - (eq + 1));
        return host_file.c_str();
      }
      entry = end ? end + 1 : nullptr;
    }
  }
  // Root detection looks for su/busybox in every PATH directory; a Linux host has them, a stock phone does not.
  {
    size_t length = strlen(path);
    if ((length >= 3 && !strcmp(path + length - 3, "/su")) || (length >= 8 && !strcmp(path + length - 8, "/busybox")))
      return "/nonexistent/a32_hidden";
  }
  // A32_PROCESS_NAME: the process name an Android app sees in /proc/<pid>/cmdline (packers compare it with the package).
  if (const char* name = getenv("A32_PROCESS_NAME")) {
    size_t length = strlen(path);
    if (!strncmp(path, "/proc/", 6) && length > 8 && !strcmp(path + length - 8, "/cmdline")) {
      static const char* const kFile = "/tmp/a32_proc_cmdline";
      if (FILE* f = fopen(kFile, "wb")) {
        fwrite(name, 1, strlen(name) + 1, f);
        fclose(f);
        return kFile;
      }
    }
  }
  // Anti-debug checks parse /proc/<getpid()>/stat. The guest's pid is fake, so serve a plausible stat line.
  if (!strncmp(path, "/proc/", 6) && isdigit((unsigned char)path[6])) {
    size_t length = strlen(path);
    if (length > 5 && !strcmp(path + length - 5, "/stat") && access(path, R_OK) != 0) {
      static const char kStat[] =
          "4242 (app_process) S 1 4242 4242 0 -1 1077936448 5000 0 0 0 20 10 0 0 20 0 18 0 1000 1500000000 30000 "
          "4294967295 1 1 0 0 0 0 4612 0 1073775864 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
      static const char* const kFile = "/tmp/a32_proc_stat";
      if (FILE* f = fopen(kFile, "w")) {
        fputs(kStat, f);
        fclose(f);
        return kFile;
      }
    }
  }
  return is_proc_maps(path) && g_maps_path[0] ? g_maps_path : path;
}

// lib<name>_a32.so is listed as lib<name>.so: hook libraries look the game library up by its original name.
std::string shown_name(const std::string& path) {
  size_t suffix = path.rfind("_a32.so");
  return suffix != std::string::npos && suffix + 7 == path.size() ? path.substr(0, suffix) + ".so" : path;
}

static void write_maps() {  // under g_maps_mutex
  static const char* const kSystemMappings[] = {
      "b6f00000-b6f20000 r-xp 00000000 103:00 301 /system/bin/linker",
      "b6f20000-b6f22000 r--p 00020000 103:00 301 /system/bin/linker",
      "b6f22000-b6f23000 rw-p 00022000 103:00 301 /system/bin/linker",
      "b6d00000-b6d70000 r-xp 00000000 103:00 302 /system/lib/libc.so",
      "b6d70000-b6d75000 r--p 00070000 103:00 302 /system/lib/libc.so",
      "b6d75000-b6d78000 rw-p 00075000 103:00 302 /system/lib/libc.so",
      "b6c00000-b6c05000 r-xp 00000000 103:00 303 /system/lib/libdl.so",
      "b6a00000-b6bf0000 r-xp 00000000 103:00 304 /system/lib/libart.so",
      // The second stage of newer packers (Jiagu 2.3.2+) looks these up by name before it registers its natives.
      "b6900000-b6930000 r-xp 00000000 103:00 305 /system/lib/libz.so",
      "b6930000-b6931000 rw-p 00030000 103:00 305 /system/lib/libz.so",
      "b6880000-b6890000 r-xp 00000000 103:00 306 /system/lib/liblog.so",
      "b6890000-b6891000 rw-p 00010000 103:00 306 /system/lib/liblog.so",
      "b6800000-b6840000 r-xp 00000000 103:00 307 /system/lib/libm.so",
      "b6780000-b67a0000 r-xp 00000000 103:00 308 /system/lib/libstdc++.so",
      "b6700000-b6760000 r-xp 00000000 103:00 309 /system/lib/libutils.so",
      "b6680000-b66d0000 r-xp 00000000 103:00 310 /system/lib/libcutils.so",
      "b6600000-b6660000 r-xp 00000000 103:00 311 /system/lib/libandroid_runtime.so",
      "b6580000-b65c0000 r-xp 00000000 103:00 312 /system/lib/libandroid.so",
      "b6500000-b6540000 r-xp 00000000 103:00 313 /system/lib/libnativehelper.so",
      "be000000-be021000 rw-p 00000000 00:00 0 [stack]",
  };
  std::string text;
  char line[600];
  for (const MappedImage& image : g_maps_images) {
    snprintf(line, sizeof line, "%08x-%08x r-xp 00000000 fd:01 4242 %s\n", image.base, image.base + image.size,
             image.name.c_str());
    text += line;
  }
  for (const char* mapping : kSystemMappings) {
    bool replaced = false;  // a region added by the runner stands in for this system library
    for (const MappedImage& image : g_maps_images)
      replaced |= strrchr(mapping, ' ') && image.name == strrchr(mapping, ' ') + 1;
    if (!replaced)
      (text += mapping) += '\n';
  }

  if (g_maps_memfd >= 0) {
    (void)!ftruncate(g_maps_memfd, 0);
    (void)!pwrite(g_maps_memfd, text.data(), text.size(), 0);
    return;
  }
  if (FILE* file = fopen(g_maps_path, "w")) {
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
  }
}

// The app's cache directory (some devices' SELinux policy forbids reopening a memfd), else a memfd (Linux host).
static void choose_maps_file() {
  char package[256] = {};
  if (FILE* f = fopen("/proc/self/cmdline", "r")) {
    (void)!fread(package, 1, sizeof package - 1, f);
    fclose(f);
  }
  if (package[0] && !strchr(package, '/')) {
    snprintf(g_maps_path, sizeof g_maps_path, "/data/data/%s/cache/a32_maps", package);
    if (FILE* probe = fopen(g_maps_path, "w")) {
      fclose(probe);
      return;
    }
  }
  g_maps_memfd = (int)syscall(SYS_memfd_create, "maps", 0);
  if (g_maps_memfd < 0) {
    g_maps_path[0] = 0;
    return;
  }
  snprintf(g_maps_path, sizeof g_maps_path, "/proc/self/fd/%d", g_maps_memfd);
}

void install_fake_proc_maps() {
  std::lock_guard<std::mutex> lock(g_maps_mutex);
  choose_maps_file();
  if (!g_maps_path[0])
    return;
  g_maps_images.insert(g_maps_images.begin(), MappedImage{g_main.base, g_main.size, shown_name(g_main.path)});
  write_maps();
  g_path_hook = maps_path_hook;
}

void fake_proc_maps_add(const Image& image) {
  std::lock_guard<std::mutex> lock(g_maps_mutex);
  g_maps_images.push_back(MappedImage{image.base, image.size, shown_name(image.path)});
  if (g_maps_path[0])
    write_maps();
}

void fake_proc_maps_add_region(u32 base, u32 size, const std::string& name) {
  std::lock_guard<std::mutex> lock(g_maps_mutex);
  g_maps_images.push_back(MappedImage{base, size, name});
  if (g_maps_path[0])
    write_maps();
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
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPS9_",
      // Jiagu 2.1.x - 2.2.0 call OpenCommon (unique_ptr result, like the Android 7 OpenMemory)
      "_ZN3art7DexFile10OpenCommonEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPKNS_10OatDexFileEbbPS9_PNS0_12VerifyResultE"};
  for (const char* name : open_memory)
    stub_add(name, strstr(name, "OatFile") || strstr(name, "OatDexFile") ? import_open_memory<true> : import_open_memory<false>);

  u32 page_size = gdata_alloc(4, 4);  // `__page_size` is a variable, not a function
  *(u32*)H(page_size) = 4096;
  sym_add("__page_size", page_size);
}
