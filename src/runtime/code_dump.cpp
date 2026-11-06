#include "runtime/code_dump.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <set>
#include <string>

#include "runtime/runtime.h"

static std::string files_directory() {
  char package[256] = {};
  if (FILE* f = fopen("/proc/self/cmdline", "r")) {
    (void)!fread(package, 1, sizeof package - 1, f);
    fclose(f);
  }
  return package[0] && !strchr(package, '/') ? std::string("/data/data/") + package + "/files" : "";
}

void code_dump_on_flush(u32 start, u32 size) {
  static const u32 kMinSize = 1 << 20;  // whole loaded images, not small patches
  if (size < kMinSize)
    return;
  static std::mutex mutex;
  static std::set<u32> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  std::string directory = files_directory();
  if (directory.empty() || access((directory + "/a32_dump_code").c_str(), F_OK) != 0 || !dumped.insert(start).second)
    return;
  char path[512];
  snprintf(path, sizeof path, "%s/a32_code_%08x.bin", directory.c_str(), start);
  if (FILE* out = fopen(path, "wb")) {
    fwrite(G + start, 1, size, out);
    fclose(out);
    LOG("code dump: %08x-%08x -> %s", start, start + size, path);
  }
}
