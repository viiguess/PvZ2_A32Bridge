// Builds the guest JNIEnv/JavaVM and implements the library entry point (JNI_OnLoad).
//
// Loading sequence: reserve the guest address space, register the HLE modules, load the original ARM library
// (lib<name>_a32.so, next to this one), run its constructors, then forward JNI_OnLoad to it.
#include "jni/bridge.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include "jni/handles.h"
#include "libc/libc.h"
#include "libc/system_imports.h"
#include "opensles.h"
#include "runtime/live_patch.h"
#include "runtime/mods.h"
#include "runtime/softfloat_accel.h"

// A JNIEnv is a pointer to a table of 235 function pointers; unimplemented slots log when called.
static constexpr int JNI_TABLE_SIZE = 235;

void jni_init(void* java_vm) {
  g_vm = (JavaVM*)java_vm;

  u32 table = gdata_alloc(4 * JNI_TABLE_SIZE, 4);
  u32* slots = (u32*)H(table);
  for (int i = 0; i < JNI_TABLE_SIZE; i++)
    slots[i] = stub_add(strdup(("JNI#" + std::to_string(i)).c_str()), nullptr);
  register_call_imports(slots);
  register_object_imports(slots);
  register_string_imports(slots);
  register_array_imports(slots);
  register_vm_imports(slots);
  register_native_imports(slots);

  g_genv = gdata_alloc(4, 4);
  *(u32*)H(g_genv) = table;
  g_gvm = gdata_alloc(4, 4);
  *(u32*)H(g_gvm) = make_vm_table();
}

u32 jni_guest_vm() {
  return g_gvm;
}

// The app's native library directory, "" if it cannot be determined. The application object does not exist yet when a
// library is loaded from Application.attachBaseContext, so the directory is derived from where the APK is mapped:
// /data/app/<package>-<id>/base.apk -> /data/app/<package>-<id>/lib/arm64.
static std::string native_library_dir() {
  FILE* maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return "";
  std::string result;
  char line[1024];
  while (fgets(line, sizeof line, maps)) {
    char* path = strchr(line, '/');
    char* apk = path ? strstr(path, "/base.apk") : nullptr;
    if (apk) {
      result.assign(path, apk - path);
#if defined(__x86_64__)
      result += "/lib/x86_64";
#else
      result += "/lib/arm64";
#endif
      break;
    }
  }
  fclose(maps);
  return result;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* java_vm, void*) {
  // The whole 4 GiB guest address space is one lazily-committed host reservation.
  void* reservation =
      mmap(nullptr, (1ull << 32) + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reservation == MAP_FAILED) {
    LOG("cannot reserve 4GB guest space");
    return JNI_ERR;
  }
  G = (uint8_t*)(((uintptr_t)reservation + 0xfff) & ~0xfffull);
  LOG("JNI_OnLoad enter");

  libc_init();
  kuser_init();
  jni_init(java_vm);
  sles_init();

  // lib<name>.so -> lib<name>_a32.so, in the same directory
  Dl_info info;
  dladdr((void*)&JNI_OnLoad, &info);
  std::string path = info.dli_fname;
  path.resize(path.size() - 3);
  path += "_a32.so";
  // A library that was copied away from the app's lib directory (a packer's stub unpacks itself into the app's data
  // directory) has no guest next to it: look in the native library directory under the library's own name.
  if (access(path.c_str(), R_OK) != 0) {
    std::string dir = native_library_dir();
    if (!dir.empty())
      path = dir + "/lib" A32_LIBNAME "_a32.so";
  }
  LOG("loading %s", path.c_str());
  if (!load_elf(path.c_str(), g_main))
    return JNI_ERR;
  LOG("elf loaded, %zu init fns", g_main.init.size());
  accelerate_soft_float(g_main);
  install_fake_proc_maps();
  install_syscalls();
  apply_live_patches(g_main);
  for (u32 constructor : g_main.init)
    guest_callv(constructor, nullptr, 0, nullptr);

  u32 on_load = image_sym(g_main, "JNI_OnLoad");
  LOG("payload loaded at %08x, guest JNI_OnLoad=%08x", g_main.base, on_load);
  jint version = JNI_VERSION_1_6;
  if (on_load) {
    u32 args[2] = {jni_guest_vm(), 0};
    version = (jint)guest_callv(on_load, args, 2, nullptr);
  }
  mods_load(path.substr(0, path.rfind('/')), jni_guest_vm());
  return version;
}
