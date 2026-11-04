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

#include <string>

#include "fake_java.h"
#include "fake_jni.h"
#include "host_env.h"
#include "libc/gles_imports.h"
#include "libc/libc.h"
#include "runtime/runtime.h"

// The host build has no GLES; the game's GL imports are not needed to run a packer.
void register_gles_imports() {
}

namespace {

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

}  // namespace

int main(int argc, char** argv) {
  if (!parse_args(argc, argv) || !reserve_guest_space())
    return 2;

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

  dump_requested_memory();
  return 0;
}
