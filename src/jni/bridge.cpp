// Builds the guest JNIEnv/JavaVM and implements the library entry point (JNI_OnLoad).
//
// Loading sequence: reserve the guest address space, register the HLE modules, load the original ARM library
// (lib<name>_a32.so, next to this one), run its constructors, then forward JNI_OnLoad to it.
#include "jni/bridge.h"

#include <dlfcn.h>
#include <sys/mman.h>

#include <string>

#include "jni/handles.h"
#include "libc/libc.h"
#include "opensles.h"
#include "runtime/live_patch.h"

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
  LOG("loading %s", path.c_str());
  if (!load_elf(path.c_str(), g_main))
    return JNI_ERR;
  LOG("elf loaded, %zu init fns", g_main.init.size());
  apply_live_patches(g_main);
  for (u32 constructor : g_main.init)
    guest_callv(constructor, nullptr, 0, nullptr);

  u32 on_load = image_sym(g_main, "JNI_OnLoad");
  LOG("payload loaded at %08x, guest JNI_OnLoad=%08x", g_main.base, on_load);
  if (!on_load)
    return JNI_VERSION_1_6;
  u32 args[2] = {jni_guest_vm(), 0};
  return (jint)guest_callv(on_load, args, 2, nullptr);
}
