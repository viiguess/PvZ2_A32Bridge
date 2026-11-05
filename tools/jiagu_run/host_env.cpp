// Imports whose answers differ from the shared libc layer (src/libc/system_imports.cpp): the runner pretends to be a
// fixed device and process so that runs are reproducible, and saves the dex a packer decrypts.
#include "host_env.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <string>

#include "fake_java.h"
#include "libc/system_imports.h"
#include "runtime/runtime.h"

namespace {

constexpr u32 kPid = 4242;
constexpr u32 kUid = 10123;

void return_constant(Ctx& ctx, const Stub& stub) {
  ctx.ret((u32)(intptr_t)stub.fn);
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

// The packer hands its decrypted dex to ART through art::DexFile::OpenMemory. Save it, then answer like the shared stub.
void import_open_memory(Ctx& ctx, const Stub&) {  // (const uint8_t* base, size_t size, ...)
  std::string path = g_cfg.work_dir + "/classes_unpacked.dex";
  FILE* file = fopen(path.c_str(), "wb");
  if (file) {
    fwrite(H(ctx.a(0)), 1, ctx.a(1), file);
    fclose(file);
  }
  fprintf(stderr, "[run] decrypted dex: %u bytes saved to %s\n", ctx.a(1), path.c_str());
  ctx.ret(fake_dex_file(ctx.a(0), ctx.a(1)));
}

}  // namespace

void register_host_imports() {
  stub_add("getpid", return_constant, (void*)(intptr_t)kPid);
  stub_add("gettid", return_constant, (void*)(intptr_t)kPid);
  stub_add("getuid", return_constant, (void*)(intptr_t)kUid);
  stub_add("__system_property_get", import_system_property_get);
  const char* const open_memory[] = {
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapES2_PS9_",
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_7OatFileEPS9_",
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPKNS_10OatDexFileEPS9_",
      "_ZN3art7DexFile10OpenMemoryEPKhjRKNSt3__112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEEjPNS_6MemMapEPS9_"};
  for (const char* name : open_memory)
    stub_add(name, import_open_memory);
}

void write_fake_maps() {
  install_fake_proc_maps();
}
