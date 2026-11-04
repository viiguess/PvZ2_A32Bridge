// The guest JavaVM and the JNIEnv entries that hand it out.
#include <string.h>

#include <string>

#include "jni/handles.h"

static void get_java_vm(Ctx& ctx, const Stub&) {
  *(u32*)ctx.p(1) = g_gvm;
  ctx.ret(0);
}

// JavaVM
static void attach_current_thread(Ctx& ctx, const Stub&) {
  env();
  *(u32*)ctx.p(1) = g_genv;
  ctx.ret(0);
}

static void detach_current_thread(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}

static void get_env(Ctx& ctx, const Stub&) {
  JNIEnv* jni = nullptr;
  int result = g_vm->GetEnv((void**)&jni, JNI_VERSION_1_6);
  if (result == JNI_OK)
    *(u32*)ctx.p(1) = g_genv;
  else
    *(u32*)ctx.p(1) = 0;
  ctx.ret(result);
}

void register_vm_imports(u32* table) {
  jni_set(table, 219, "GetJavaVM", get_java_vm);
}

// The guest JavaVM: a vtable of DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv and
// AttachCurrentThreadAsDaemon.
u32 make_vm_table() {
  u32 vtable = gdata_alloc(4 * 8, 4);
  u32* table = (u32*)H(vtable);
  table[0] = table[1] = table[2] = 0;  // reserved
  table[3] = stub_add("DestroyJavaVM", jni_return_zero);
  table[4] = stub_add("AttachCurrentThread", attach_current_thread);
  table[5] = stub_add("DetachCurrentThread", detach_current_thread);
  table[6] = stub_add("GetEnv", get_env);
  table[7] = stub_add("AttachCurrentThreadAsDaemon", attach_current_thread);
  return vtable;
}
