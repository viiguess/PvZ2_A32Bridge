// jiagu_run: runs a 32-bit ARM Android library (a packer stub such as libjiagu.so) on a Linux host inside the a32bridge
// runtime, against a fake JNI environment that logs everything the library asks for.
//
//   jiagu_run <lib.so> <apk> <workdir> [--package NAME] [--version-name NAME] [--version-code N]
//
// Environment:
//   JR_DUMP=<hexaddr>:<hexlen>[,...]   after the run, write those guest memory ranges to <workdir>/dump_<hexaddr>.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <algorithm>
#include <unistd.h>
#include <utility>
#include <string>
#include <vector>

#include "fake_java.h"
#include "fake_jni.h"
#include "host_env.h"
#include "libc/gles_imports.h"
#include "libc/libc.h"
#include "libc/system_imports.h"
#include "runtime/runtime.h"

// The host build has no GLES; the game's GL imports are not needed to run a packer.
void register_gles_imports() {
}

namespace {

u32 g_shell_base = 0, g_shell_size = 0;  // the shell dex mapping (see main)

constexpr size_t kGuestSpace = 1ull << 32;

bool reserve_guest_space() {
  void* mem =
      mmap(nullptr, kGuestSpace + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mem == MAP_FAILED) {
    perror("cannot reserve the 4 GiB guest address space");
    return false;
  }
  G = (uint8_t*)(((uintptr_t)mem + 0xfff) & ~0xfffull);
  return true;
}

std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos)
      end = text.size();
    if (end > start)
      items.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return items;
}

bool parse_args(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <lib.so> <apk> <workdir> [--package P] [--version-name V] [--version-code N]\n",
            argv[0]);
    return false;
  }
  g_cfg.lib_path = argv[1];
  g_cfg.apk_path = argv[2];
  g_cfg.work_dir = argv[3];
  for (int i = 4; i + 1 < argc; i += 2) {
    std::string flag = argv[i];
    if (flag == "--package")
      g_cfg.package = argv[i + 1];
    else if (flag == "--version-name")
      g_cfg.version_name = argv[i + 1];
    else if (flag == "--version-code")
      g_cfg.version_code = atoi(argv[i + 1]);
    else {
      fprintf(stderr, "unknown option %s\n", flag.c_str());
      return false;
    }
  }
  return true;
}

void dump_requested_memory() {
  const char* spec = getenv("JR_DUMP");
  if (!spec)
    return;
  std::string list = spec;
  for (size_t pos = 0; pos < list.size();) {
    size_t end = list.find(',', pos);
    std::string range = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? list.size() : end + 1;

    unsigned long addr = 0, len = 0;
    if (sscanf(range.c_str(), "%lx:%lx", &addr, &len) != 2)
      continue;
    char name[64];
    snprintf(name, sizeof name, "/dump_%lx.bin", addr);
    std::string path = g_cfg.work_dir + name;
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(G + addr, 1, len, f);
      fclose(f);
      fprintf(stderr, "[run] dumped %lx+%lx to %s\n", addr, len, path.c_str());
    }
  }
}

// Old packers (Jiagu <= 2.0.x) never call art::DexFile::OpenMemory themselves: they decrypt into fresh mappings and hand
// those to the runtime later. Find them: every readable page-aligned guest page that starts with a valid dex image.
void save_dex_images() {
  std::string first = g_cfg.work_dir + "/classes_unpacked.dex";
  if (access(first.c_str(), F_OK) == 0)
    return;  // OpenMemory already delivered them
  std::vector<std::pair<u32, u32>> found;  // guest address, size
  FILE* maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return;
  char line[512];
  while (fgets(line, sizeof line, maps)) {
    unsigned long lo, hi;
    char perms[8];
    if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3 || perms[0] != 'r')
      continue;
    unsigned long guest_lo = (unsigned long)G, guest_hi = (unsigned long)G + 0x80000000ul;
    lo = std::max(lo, guest_lo);
    hi = std::min(hi, guest_hi);
    for (unsigned long page = (lo + 0xfff) & ~0xffful; page + 0x70 <= hi; page += 0x1000) {
      const uint8_t* p = (const uint8_t*)page;
      if (memcmp(p, "dex\n03", 6) != 0)
        continue;
      u32 size = *(const u32*)(p + 32), header = *(const u32*)(p + 36);
      if (header != 0x70 || size < 0x70 || page + size > hi)
        continue;
      u32 sum = 1, s2 = 0;  // adler32 of bytes 12..size, as the dex header stores
      for (u32 i = 12; i < size; i++) {
        sum = (sum + p[i]) % 65521;
        s2 = (s2 + sum) % 65521;
      }
      if (((s2 << 16) | sum) != *(const u32*)(p + 8))
        continue;
      u32 guest = (u32)(page - (unsigned long)G);
      if (guest >= g_shell_base && guest < g_shell_base + g_shell_size)
        continue;  // the APK's own shell dex
      found.push_back({guest, size});
    }
  }
  fclose(maps);
  std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.second > b.second; });  // the game's dex first
  int count = 0;
  for (auto& image : found) {
    std::string path = g_cfg.work_dir + "/classes_unpacked" + (count ? std::to_string(count + 1) : "") + ".dex";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(H(image.first), 1, image.second, f);
      fclose(f);
    }
    fprintf(stderr, "[run] decrypted dex: %u bytes found at %08x, saved to %s\n", image.second, image.first, path.c_str());
    count++;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (!parse_args(argc, argv))
    return 1;
  java_prefetch();
  // Old Jiagu packers (version codes up to 865, i.e. Lenovo 2.2.2 and earlier) inline-hook ART: they read
  // kOatVersion and parse a libart.so that exports a long list of ART symbols (art_symbols.txt). Newer packers change
  // behaviour when they see those, so this is opt-out by version code (the env vars still override).
  if (g_cfg.version_code > 0 && g_cfg.version_code <= 865) {
    setenv("JR_OAT", "1", 0);
    if (!getenv("JR_LIBART_SYMS")) {
      std::string here = __FILE__;
      std::string list;
      if (FILE* f = fopen((here.substr(0, here.rfind('/')) + "/art_symbols.txt").c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
          line[strcspn(line, "\r\n")] = 0;
          if (line[0])
            list += (list.empty() ? "" : ",") + std::string(line);
        }
        fclose(f);
      }
      if (!list.empty())
        setenv("JR_LIBART_SYMS", list.c_str(), 1);
    }
  }
  if (!reserve_guest_space())
    return 2;

  setenv("A32_PROCESS_NAME", g_cfg.package.c_str(), 1);  // /proc/self/cmdline shows the app, as on a device
  libc_init();
  kuser_init();
  fake_jni_init();
  register_host_imports();
  install_syscalls();

  if (!load_elf(g_cfg.lib_path.c_str(), g_main)) {
    fprintf(stderr, "cannot load %s\n", g_cfg.lib_path.c_str());
    return 1;
  }
  write_fake_maps();
  fprintf(stderr, "[run] loaded at %08x, size %x, %zu init functions\n", g_main.base, g_main.size, g_main.init.size());

  // Old packers (Jiagu <= 2.0.x) patch the APK's classes.dex in place once the runtime has mapped it: find it in
  // /proc/self/maps. Map the shell dex there, and save it again after JNI_OnLoad.
  std::string shell_dex = getenv("JR_NO_SHELL") ? std::string() : apk_entry_bytes("classes.dex");
  u32 shell_base = 0;
  if (!shell_dex.empty()) {
    shell_base = gmalloc((u32)shell_dex.size() + 16);
    memcpy(H(shell_base), shell_dex.data(), shell_dex.size());
    fake_proc_maps_add_region(shell_base, (u32)shell_dex.size(),
                              "/data/dalvik-cache/arm/data@app@" + g_cfg.package + "-1@base.apk@classes.dex");
    g_shell_size = (u32)shell_dex.size();
    g_shell_base = shell_base;
    fprintf(stderr, "[run] shell classes.dex (%zu bytes) mapped at %08x\n", shell_dex.size(), shell_base);
  }

  // JR_LIBART_SYMS=name,name,...: old packers parse /system/lib/libart.so (file and in-memory image) and hook ART
  // functions. Build a tiny ELF exporting those symbols (as the bridge's stubs) and list it in the fake maps as libart.
  const char* libart_default =
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPS9_";
  // Empirical: the Jiagu 2.2.3 - 2.2.5 packers (version codes 866-890) stop short of registering their natives when they
  // find a libart image, every other build needs one (JR_LIBART_SYMS=none switches it off).
  if (!getenv("JR_LIBART_SYMS") && g_cfg.version_code >= 866 && g_cfg.version_code <= 890)
    libart_default = "none";
  const char* libart_syms = getenv("JR_LIBART_SYMS") ? getenv("JR_LIBART_SYMS") : libart_default;
  if (const char* wanted = strcmp(libart_syms, "none") ? libart_syms : nullptr) {
    u32 base = gmalloc(0x9000);
    base = (base + 0xfff) & ~0xfffu;
    std::string here = __FILE__;
    std::string command = "python3 '" + here.substr(0, here.rfind('/')) + "/fake_libart.py' '" + g_cfg.work_dir + "/libart_fake.so'";
    int listed = 0;
    for (const std::string& name : split_list(wanted)) {
      // A symbol the bridge does not implement still has to exist: the packer inline-hooks it. Point it at a spare spot
      // of the fake image (below the 0x7000 bytes copied in, so the hook can write there).
      u32 stub = stub_find(name);
      char value[32];
      snprintf(value, sizeof value, "=%x", stub ? stub - base : 0x4000 + 0x20 * listed);
      command += " '" + name + value + "'";
      listed++;
    }
    if (system(command.c_str()) == 0) {
      if (FILE* f = fopen((g_cfg.work_dir + "/libart_fake.so").c_str(), "rb")) {
        size_t got = fread(H(base), 1, 0x7000, f);
        fclose(f);
        setenv("A32_PATH_MAP", ("/system/lib/libart.so=" + g_cfg.work_dir + "/libart_fake.so").c_str(), 1);
        fake_proc_maps_add_region(base, 0x8000, "/system/lib/libart.so");
        fprintf(stderr, "[run] fake libart.so (%zu bytes, %d symbols) mapped at %08x\n", got, listed, base);
      }
    }
  }

  // JR_LINKER_SYMS=name,...: 2.2.8's packer parses the in-memory /system/bin/linker (the fake maps list it at b6f00000)
  // for these symbols. Same trick as the fake libart: a tiny ELF image at that address.
  if (const char* wanted = getenv("JR_LINKER_SYMS") ? getenv("JR_LINKER_SYMS") : "rtld_db_dlactivity") {
    u32 base = gmalloc(0x9000);
    base = (base + 0xfff) & ~0xfffu;
    std::string here = __FILE__;
    std::string command = "python3 '" + here.substr(0, here.rfind('/')) + "/fake_libart.py' '" + g_cfg.work_dir + "/linker_fake.so'";
    for (const std::string& name : split_list(wanted)) {
      u32 stub = stub_find(name);
      char value[32];
      snprintf(value, sizeof value, "=%x", stub ? stub - base : 0x1000);
      command += " '" + name + value + "'";
    }
    if (system(command.c_str()) == 0) {
      if (FILE* f = fopen((g_cfg.work_dir + "/linker_fake.so").c_str(), "rb")) {
        size_t got = fread(H(base), 1, 0x7000, f);
        fclose(f);
        fake_proc_maps_add_region(base, 0x8000, "/system/bin/linker");
        std::string map = getenv("A32_PATH_MAP") ? std::string(getenv("A32_PATH_MAP")) + ";" : std::string();
        setenv("A32_PATH_MAP", (map + "/system/bin/linker=" + g_cfg.work_dir + "/linker_fake.so").c_str(), 1);
        fprintf(stderr, "[run] fake linker image (%zu bytes) at %08x\n", got, base);
      }
    }
  }

  for (u32 init : g_main.init) {
    fprintf(stderr, "[run] init %08x\n", init);
    guest_callv(init, nullptr, 0, nullptr);
  }

  if (u32 on_load = image_sym(g_main, "JNI_OnLoad")) {
    u32 args[2] = {fake_jni_vm(), 0};
    u32 result = guest_callv(on_load, args, 2, nullptr);
    fprintf(stderr, "[run] JNI_OnLoad(%08x) returned %08x; %zu natives registered\n", on_load, result,
            registered_natives().size());
  } else {
    fprintf(stderr, "[run] the library has no JNI_OnLoad\n");
  }

  if (shell_base) {
    std::string path = g_cfg.work_dir + "/classes_shell_after.dex";
    if (FILE* f = fopen(path.c_str(), "wb")) {
      fwrite(H(shell_base), 1, shell_dex.size(), f);
      fclose(f);
    }
    fprintf(stderr, "[run] shell dex %s after the run (%s)\n",
            memcmp(H(shell_base), shell_dex.data(), shell_dex.size()) ? "CHANGED" : "unchanged", path.c_str());
  }

  // JR_CALL=name[,name...]: call registered natives (static or instance) with placeholder arguments, to see what they do.
  if (const char* calls = getenv("JR_CALL")) {
    for (const std::string& want : split_list(calls)) {
      for (const RegisteredNative& native : registered_natives()) {
        if (native.name != want)
          continue;
        char ret;
        std::vector<char> params = signature_args(native.sig, ret);
        std::vector<u32> args = {fake_jni_env(), class_handle(native.cls)};
        int objects = 0;
        for (char param : params)
          args.push_back(param != 'L' ? (u32)(getenv("JR_INT") ? atoi(getenv("JR_INT")) : 0) : objects++ == 0 ? new_instance("android/app/Application") : new_instance("android/content/Context"));
        fprintf(stderr, "[run] calling %s.%s%s\n", native.cls.c_str(), native.name.c_str(), native.sig.c_str());
        u32 result = guest_callv(native.fn, args.data(), (int)args.size(), nullptr);
        fprintf(stderr, "[run] %s returned %08x\n", native.name.c_str(), result);
      }
    }
  }

  save_dex_images();
  dump_requested_memory();
  return 0;
}
