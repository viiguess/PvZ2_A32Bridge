// Shared pieces of the JNI bridge modules.
//
// The guest sees a 32-bit JNIEnv whose function table is made of import stubs. Each stub decodes its arguments
// (object references are small integer handles, see handles.cpp), calls the real 64-bit JNI, and encodes the
// result back.
#pragma once

#include <jni.h>

#include <string>
#include <vector>

#include "jni/bridge.h"
#include "runtime/runtime.h"

// ---- environment ----------------------------------------------------------------------------------------------
extern JavaVM* g_vm;
extern u32 g_genv, g_gvm;  // guest JNIEnv* / JavaVM*

JNIEnv* env();  // the calling thread's JNIEnv, attaching the thread if needed

// ---- object references ----------------------------------------------------------------------------------------
// A guest handle is REF_BASE + 4 * (index into the reference table). Freed slots are reused first-in-first-out and
// only after REUSE_DELAY other slots were freed, because the game keeps stale handles around (it assumes Dalvik's
// lenient local references): an immediately reused slot would make such a handle silently alias a new object, which
// ART then rejects as the wrong type.
enum : u32 { REF_BASE = 0x90000000, REUSE_DELAY = 8192 };

enum RefKind { REF_BORROWED = 0, REF_LOCAL = 1, REF_GLOBAL = 2 };

struct Ref {
  jobject o;
  int kind;  // RefKind
};

u32 href(jobject o, int kind);  // jobject -> guest handle (0 for null); locals are released with their native frame
jobject hobj(u32 handle);       // guest handle -> jobject (null if invalid)
void hfree(u32 handle);

// Local handles created while a guest native method runs are released when it returns.
extern thread_local std::vector<u32> t_locals;
void frame_end(size_t mark);

// ---- method and field ids -------------------------------------------------------------------------------------
struct MId {
  jmethodID m;
  std::string sig;
  bool is_static;
  std::string name;
};

struct FId {
  jfieldID f;
  char type;
};

u32 mid_new(jmethodID method, const char* sig, bool is_static, const char* name = "");
MId mid(u32 id);
u32 fid_new(jfieldID f, char type);
FId fid(u32 id);

// ---- calls and fields (calls.cpp) -----------------------------------------------------------------------------
enum CallKind { CALL_VIRTUAL, CALL_NONVIRTUAL, CALL_STATIC };

enum ArgSource { ARGS_VARIADIC, ARGS_VALIST, ARGS_JVALUES };  // Call*Method, Call*MethodV, Call*MethodA

enum JType { T_OBJECT, T_BOOLEAN, T_BYTE, T_CHAR, T_SHORT, T_INT, T_LONG, T_FLOAT, T_DOUBLE, T_VOID, T_COUNT };

extern thread_local std::string t_last_call;  // diagnostics: the last Java method this thread invoked

// ---- table construction ---------------------------------------------------------------------------------------
// Registers stub `handler` as JNIEnv function `slot`; `code` is handed back to the handler as stub.fn.
inline void jni_set(u32* table, int slot, const char* name, Handler handler, intptr_t code = 0) {
  table[slot] = stub_add(name, handler, (void*)code);
}

void jni_return_zero(Ctx& ctx, const Stub& stub);  // handler for entries that need no work (MonitorEnter, ...)
