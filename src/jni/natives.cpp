// RegisterNatives: the guest's native methods are exposed to ART through a pool of host trampolines.
#include <string.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jni/handles.h"

// RegisterNatives -> pool of host trampolines
struct NativeSlot {
  u32 fn;
  std::string sig;
  bool is_static;
};

static std::vector<NativeSlot> g_slots;
static std::mutex g_slot_mu;

// The game keeps object parameters of its native methods in globals without NewGlobalRef (stale local refs "work"
// on ART). Pin each (method, param position) to a stable global handle so those cached values stay valid.
static std::mutex g_pin_mu;
static std::unordered_map<int, u32> g_pins;

static u32 pin(int key, jobject object) {
  if (!object)
    return 0;
  JNIEnv* jni = env();
  std::lock_guard<std::mutex> lock(g_pin_mu);
  auto it = g_pins.find(key);
  if (it != g_pins.end()) {
    jobject old = hobj(it->second);
    if (old && jni->IsSameObject(old, object))
      return it->second;
    if (old)
      jni->DeleteGlobalRef(old);
    hfree(it->second);
  }
  u32 handle = href(jni->NewGlobalRef(object), 2);
  g_pins[key] = handle;
  return handle;
}

static u64 native_call(int slot_index, JNIEnv*, jobject obj, const u64* int_args, const double* float_args,
                       char* retc) {
  NativeSlot method;
  {
    std::lock_guard<std::mutex> lock(g_slot_mu);
    method = g_slots[slot_index];
  }
  size_t mark = t_locals.size();
  u32 args[40];
  int arg_count = 0;
  args[arg_count++] = g_genv;
  args[arg_count++] = pin(slot_index * 32, obj);
  int param_index = 1;
  int int_index = 0, float_index = 0;
  const std::string& signature = method.sig;
  size_t pos = 1;
  while (signature[pos] != ')') {
    char type = signature[pos];
    switch (type) {
      case 'J': {
        if (arg_count & 1)
          args[arg_count++] = 0;
        u64 value = int_args[int_index++];
        args[arg_count++] = (u32)value;
        args[arg_count++] = (u32)(value >> 32);
        break;
      }
      case 'D': {
        if (arg_count & 1)
          args[arg_count++] = 0;
        u64 value;
        memcpy(&value, &float_args[float_index++], 8);
        args[arg_count++] = (u32)value;
        args[arg_count++] = (u32)(value >> 32);
        break;
      }
      case 'F': {
        u64 value;
        memcpy(&value, &float_args[float_index++], 8);
        args[arg_count++] = (u32)value;
        break;
      }
      case 'L':
      case '[':
        args[arg_count++] = pin(slot_index * 32 + param_index, (jobject)int_args[int_index++]);
        break;
      default:
        args[arg_count++] = (u32)int_args[int_index++];
    }
    param_index++;
    while (signature[pos] == '[')
      pos++;
    if (signature[pos] == 'L')
      while (signature[pos] != ';')
        pos++;
    pos++;
  }
  u32 result_hi = 0;
  u32 result_lo = guest_callv(method.fn, args, arg_count, &result_hi);
  char return_type = signature[signature.find(')') + 1];
  *retc = return_type;
  u64 res = 0;
  switch (return_type) {
    case 'V':
      break;
    case 'J':
      res = (u64)result_lo | (u64)result_hi << 32;
      break;
    case 'D':
      res = (u64)result_lo | (u64)result_hi << 32;
      break;
    case 'F':
      res = result_lo;
      break;
    case 'L':
    case '[':
      res = (u64)(uintptr_t)hobj(result_lo);
      break;
    case 'Z':
      res = result_lo & 0xff;
      break;
    default:
      res = (u64)(int64_t)(int32_t)result_lo;
  }
  frame_end(mark);
  return res;
}

// A native method reaches us as a C function with a fixed shape: JNIEnv, `this`, 12 integer-class arguments and 8
// floating-point arguments. The arm64 calling convention passes the first of each class in registers, so declaring
// them all lets us read whatever the Java signature actually used (unused ones hold garbage and are ignored).
#define TRAMPOLINE_PARAMS                                                                                            \
  JNIEnv *jni_env, jobject self, u64 int_arg0, u64 int_arg1, u64 int_arg2, u64 int_arg3, u64 int_arg4, u64 int_arg5, \
      u64 int_arg6, u64 int_arg7, u64 int_arg8, u64 int_arg9, u64 int_arg10, u64 int_arg11, double float_arg0,       \
      double float_arg1, double float_arg2, double float_arg3, double float_arg4, double float_arg5,                 \
      double float_arg6, double float_arg7
#define TRAMPOLINE_PACK                                                              \
  u64 int_args[12] = {int_arg0, int_arg1, int_arg2, int_arg3, int_arg4,  int_arg5,   \
                      int_arg6, int_arg7, int_arg8, int_arg9, int_arg10, int_arg11}; \
  double float_args[8] = {float_arg0, float_arg1, float_arg2, float_arg3,            \
                          float_arg4, float_arg5, float_arg6, float_arg7};           \
  char return_type

// One trampoline per (slot, return class): integer-class, float and double returns need different host signatures.
template <int K>
static u64 trampI(TRAMPOLINE_PARAMS) {
  TRAMPOLINE_PACK;
  return native_call(K, jni_env, self, int_args, float_args, &return_type);
}

template <int K>
static float trampF(TRAMPOLINE_PARAMS) {
  TRAMPOLINE_PACK;
  u64 raw = native_call(K, jni_env, self, int_args, float_args, &return_type);
  float result;
  u32 bits = (u32)raw;
  memcpy(&result, &bits, 4);
  return result;
}

template <int K>
static double trampD(TRAMPOLINE_PARAMS) {
  TRAMPOLINE_PACK;
  u64 raw = native_call(K, jni_env, self, int_args, float_args, &return_type);
  double result;
  memcpy(&result, &raw, 8);
  return result;
}

template <size_t... K>
static std::vector<void*> make_pool(std::index_sequence<K...>) {
  return {(void*)trampI<K>..., (void*)trampF<K>..., (void*)trampD<K>...};
}

static const int NSLOT = 192;
static std::vector<void*> g_pool = make_pool(std::make_index_sequence<NSLOT>());  // [I..., F..., D...]

static void register_natives(Ctx& ctx, const Stub&) {
  JNIEnv* jni = env();
  jclass cls = (jclass)hobj(ctx.a(1));
  u32* methods = (u32*)ctx.p(2);
  int count = ctx.a(3);
  for (int i = 0; i < count; i++) {
    const char* name = strdup((const char*)H(methods[3 * i]));
    const char* sig = strdup((const char*)H(methods[3 * i + 1]));
    int slot_index;
    {
      std::lock_guard<std::mutex> lock(g_slot_mu);
      slot_index = g_slots.size();
      g_slots.push_back({methods[3 * i + 2], sig, false});
    }
    if (slot_index >= NSLOT) {
      LOG("native slot pool exhausted");
      ctx.ret(-1);
      return;
    }
    char return_type = strchr(sig, ')')[1];
    void* fn = g_pool[(return_type == 'F' ? NSLOT : return_type == 'D' ? 2 * NSLOT : 0) + slot_index];
    JNINativeMethod native_method = {const_cast<char*>(name), const_cast<char*>(sig), fn};
    if (jni->RegisterNatives(cls, &native_method, 1) != JNI_OK)
      LOG("RegisterNatives failed: %s %s", name, sig);
  }
  ctx.ret(0);
}

void register_native_imports(u32* table) {
  jni_set(table, 215, "RegisterNatives", register_natives);
  jni_set(table, 216, "UnregisterNatives", [](Ctx& ctx, const Stub&) { ctx.ret(0); });
}
