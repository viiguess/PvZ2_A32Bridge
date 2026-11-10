#include "fake_jni.h"

#include <stdio.h>
#include <string.h>

#include "fake_java.h"
#include "runtime/runtime.h"

namespace {

// Names of the JNIEnv function table slots, in table order (generated from jni.h).
const char* const kSlotNames[] = {
#include "jni_names.inc"
};
constexpr int kNamedSlots = sizeof(kSlotNames) / sizeof(kSlotNames[0]);
constexpr int kTableSlots = 240;  // a few spare slots after the last one defined by JNI

u32 g_env, g_vm;
std::vector<RegisteredNative> g_natives;

struct Result {
  uint64_t value = 0;
  bool wide = false;  // the result is a 64-bit value (r0:r1)
};

bool starts(const std::string& s, const char* prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

bool ends(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string cstr(u32 guest) {
  return guest ? std::string((const char*)H(guest)) : std::string("(null)");
}

// ---- references and exceptions --------------------------------------------------------------------------------
bool handle_references(const std::string& n, Ctx& ctx, Result& r) {
  if (n == "GetVersion") {
    r.value = 0x00010006;
  } else if (n == "FindClass") {
    std::string name = cstr(ctx.a(1));
    fprintf(stderr, "[jni] FindClass %s\n", name.c_str());
    if (hidden_class(name)) {
      g_java_exception = true;
      r.value = 0;
    } else {
      r.value = class_handle(name);
    }
  } else if (n == "ExceptionCheck") {
    r.value = g_java_exception;
  } else if (n == "ExceptionOccurred") {
    r.value = g_java_exception ? new_instance("java/lang/ClassNotFoundException") : 0;
  } else if (n == "ExceptionClear") {
    g_java_exception = false;
  } else if (n == "GetObjectClass") {
    Obj* o = obj_of(ctx.a(1));
    r.value = class_handle(o ? o->cls : "java/lang/Object");
  } else if (n == "GetSuperclass") {
    r.value = class_handle("java/lang/Object");
  } else if (n == "NewGlobalRef" || n == "NewLocalRef" || n == "NewWeakGlobalRef" || n == "PopLocalFrame") {
    r.value = ctx.a(1);
  } else if (n == "IsSameObject") {
    r.value = ctx.a(1) == ctx.a(2);
  } else if (n == "IsInstanceOf") {
    r.value = 1;
  } else if (n == "ThrowNew") {
    fprintf(stderr, "[jni] ThrowNew %s: %s\n", describe('L', ctx.a(1)).c_str(), cstr(ctx.a(2)).c_str());
  } else if (n == "DeleteLocalRef" || n == "DeleteGlobalRef" || n == "DeleteWeakGlobalRef" || 
             n == "ExceptionDescribe" || n == "Throw" ||
             n == "MonitorEnter" || n == "MonitorExit" || n == "PushLocalFrame" || n == "EnsureLocalCapacity") {
    // nothing to do: there are no local frames and no pending exceptions
  } else {
    return false;
  }
  return true;
}

// ---- method and field ids, field access -----------------------------------------------------------------------
bool handle_ids_and_fields(const std::string& n, Ctx& ctx, Result& r) {
  if (n == "GetMethodID" || n == "GetStaticMethodID") {
    Obj* cls = obj_of(ctx.a(1));
    MethodId id{cls ? cls->cls : "?", cstr(ctx.a(2)), cstr(ctx.a(3)), n == "GetStaticMethodID"};
    fprintf(stderr, "[jni] %s %s.%s%s\n", n.c_str(), id.cls.c_str(), id.name.c_str(), id.sig.c_str());
    r.value = method_id_new(id);
  } else if (n == "GetFieldID" || n == "GetStaticFieldID") {
    Obj* cls = obj_of(ctx.a(1));
    FieldId id{cls ? cls->cls : "?", cstr(ctx.a(2)), cstr(ctx.a(3)), n == "GetStaticFieldID"};
    fprintf(stderr, "[jni] %s %s.%s %s\n", n.c_str(), id.cls.c_str(), id.name.c_str(), id.sig.c_str());
    r.value = field_id_new(id);
  } else if (starts(n, "Get") && ends(n, "Field")) {  // Get<Type>Field, GetStatic<Type>Field
    const FieldId* f = field_id(ctx.a(2));
    if (!f)
      return true;
    r.value = java_get_field(*f, ctx.a(1));
    r.wide = f->sig[0] == 'J' || f->sig[0] == 'D';
    fprintf(stderr, "[jni] %s %s.%s -> %s\n", n.c_str(), f->cls.c_str(), f->name.c_str(),
            describe(f->sig[0] == 'L' ? 'L' : 'I', r.value).c_str());
  } else if (starts(n, "Set") && ends(n, "Field")) {
    if (const FieldId* f = field_id(ctx.a(2)))
      fprintf(stderr, "[jni] %s %s.%s = %s\n", n.c_str(), f->cls.c_str(), f->name.c_str(),
              describe(f->sig[0] == 'L' ? 'L' : 'I', ctx.a(3)).c_str());
  } else {
    return false;
  }
  return true;
}

// ---- method calls -----------------------------------------------------------------------------------------------
// The three argument-passing conventions of Call<Type>Method / ...V / ...A.
struct ArgReader {
  enum Mode { VARIADIC, VA_LIST, JVALUES } mode;

  Ctx& ctx;
  int next = 0;    // VARIADIC: next argument slot
  u32 cursor = 0;  // VA_LIST: guest address of the next argument; JVALUES: the jvalue array
  int jvalue_index = 0;

  ArgReader(Ctx& c, Mode m, int first_slot) : mode(m), ctx(c), next(first_slot) {
    if (m != VARIADIC)
      cursor = c.a(first_slot);  // a va_list is passed by value: it is the pointer to the arguments
  }

  uint64_t read(char kind) {
    bool wide = kind == 'J' || kind == 'D';
    if (mode == JVALUES) {
      uint64_t v = *(uint64_t*)H(cursor + 8 * jvalue_index++);
      return wide ? v : (u32)v;
    }
    if (mode == VARIADIC) {
      if (wide && (next & 1))
        next++;  // 64-bit values start on an even register / stack slot
      uint64_t v = ctx.a(next) | (wide ? (uint64_t)ctx.a(next + 1) << 32 : 0);
      next += wide ? 2 : 1;
      return v;
    }
    if (wide)  // VA_LIST
      cursor = (cursor + 7) & ~7u;
    uint64_t v = wide ? *(uint64_t*)H(cursor) : *(u32*)H(cursor);
    cursor += wide ? 8 : 4;
    return v;
  }
};

bool handle_calls(const std::string& n, Ctx& ctx, Result& r) {
  bool is_new = starts(n, "NewObject");
  if (!is_new && !(starts(n, "Call") && (ends(n, "Method") || ends(n, "MethodV") || ends(n, "MethodA"))))
    return false;

  int method_slot = starts(n, "CallNonvirtual") ? 3 : 2;  // (env, obj[, class], methodID, args...)
  const MethodId* m = method_id(ctx.a(method_slot));
  static const MethodId unknown{"?", "?", "()V", false};
  if (!m)
    m = &unknown;

  char ret;
  std::vector<char> kinds = signature_args(m->sig, ret);
  ArgReader::Mode mode = ends(n, "A") ? ArgReader::JVALUES : ends(n, "V") ? ArgReader::VA_LIST : ArgReader::VARIADIC;
  ArgReader reader(ctx, mode, method_slot + 1);
  std::string args;
  std::vector<uint64_t> values;
  for (size_t i = 0; i < kinds.size(); i++) {
    values.push_back(reader.read(kinds[i]));
    args += (i ? ", " : "") + describe(kinds[i], values.back());
  }

  if (is_new) {
    r.value = new_instance(m->cls);
    java_constructed(*m, r.value, values);
  } else {
    r.value = java_call(*m, ctx.a(1), values);
  }
  r.wide = !is_new && (ret == 'J' || ret == 'D');
  fprintf(stderr, "[jni] %s %s.%s%s (%s) -> %s\n", n.c_str(), m->cls.c_str(), m->name.c_str(), m->sig.c_str(),
          args.c_str(), describe(is_new || ret == 'L' ? 'L' : 'I', r.value).c_str());
  return true;
}

// ---- strings and arrays -----------------------------------------------------------------------------------------
bool handle_strings(const std::string& n, Ctx& ctx, Result& r) {
  if (n == "NewStringUTF") {
    r.value = new_string(cstr(ctx.a(1)));
  } else if (n == "GetStringUTFChars") {
    Obj* o = obj_of(ctx.a(1));
    std::string text = o ? o->str : "";
    u32 copy = gmalloc((u32)text.size() + 1);
    memcpy(H(copy), text.c_str(), text.size() + 1);
    if (ctx.a(2))
      *(uint8_t*)H(ctx.a(2)) = 1;  // isCopy
    r.value = copy;
  } else if (n == "ReleaseStringUTFChars") {
    gfree(ctx.a(2));
  } else if (n == "GetStringUTFLength" || n == "GetStringLength") {
    Obj* o = obj_of(ctx.a(1));
    r.value = o ? (u32)o->str.size() : 0;
  } else {
    return false;
  }
  return true;
}

int element_size(const std::string& n) {  // New<Type>Array
  if (n == "NewByteArray" || n == "NewBooleanArray")
    return 1;
  if (n == "NewCharArray" || n == "NewShortArray")
    return 2;
  if (n == "NewLongArray" || n == "NewDoubleArray")
    return 8;
  return 4;
}

bool handle_arrays(const std::string& n, Ctx& ctx, Result& r) {
  Obj* arr = obj_of(ctx.a(1));
  if (n == "GetArrayLength") {
    r.value = arr ? arr->len : 0;
  } else if (n == "NewObjectArray") {
    Obj o;
    o.cls = "[obj";
    o.len = ctx.a(1);
    o.elems.assign(o.len, ctx.a(3));
    r.value = new_obj(o);
  } else if (starts(n, "New") && ends(n, "Array")) {
    Obj o;
    o.cls = "[prim";
    o.len = ctx.a(1);
    o.elem_size = element_size(n);
    o.buf = gmalloc(o.len * o.elem_size + 8);
    memset(H(o.buf), 0, o.len * o.elem_size);
    r.value = new_obj(o);
  } else if (n == "GetObjectArrayElement") {
    r.value = arr && ctx.a(2) < arr->elems.size() ? arr->elems[ctx.a(2)] : 0;
  } else if (n == "SetObjectArrayElement") {
    if (arr && ctx.a(2) < arr->elems.size())
      arr->elems[ctx.a(2)] = ctx.a(3);
  } else if (starts(n, "Get") && ends(n, "ArrayElements")) {
    r.value = arr ? arr->buf : 0;
    if (ctx.a(2))
      *(uint8_t*)H(ctx.a(2)) = 0;
  } else if (starts(n, "Release") && ends(n, "ArrayElements")) {
    // the guest sees the array's own buffer, so there is nothing to copy back
  } else if (starts(n, "Get") && ends(n, "ArrayRegion")) {  // (array, start, len, buf)
    if (arr)
      memcpy(H(ctx.a(4)), (uint8_t*)H(arr->buf) + ctx.a(2) * arr->elem_size, ctx.a(3) * arr->elem_size);
  } else if (starts(n, "Set") && ends(n, "ArrayRegion")) {
    if (arr)
      memcpy((uint8_t*)H(arr->buf) + ctx.a(2) * arr->elem_size, H(ctx.a(4)), ctx.a(3) * arr->elem_size);
  } else {
    return false;
  }
  return true;
}

// ---- natives and the VM -----------------------------------------------------------------------------------------
bool handle_natives(const std::string& n, Ctx& ctx, Result& r) {
  if (n == "RegisterNatives") {  // (env, class, JNINativeMethod*, count)
    Obj* cls = obj_of(ctx.a(1));
    for (u32 i = 0; i < ctx.a(3); i++) {
      u32* entry = (u32*)H(ctx.a(2) + 12 * i);  // {name, signature, fnPtr}
      RegisteredNative native{cls ? cls->cls : "?", cstr(entry[0]), cstr(entry[1]), entry[2]};
      fprintf(stderr, "[jni] RegisterNatives %s.%s%s -> %08x\n", native.cls.c_str(), native.name.c_str(),
              native.sig.c_str(), native.fn);
      g_natives.push_back(native);
    }
  } else if (n == "GetJavaVM") {
    *(u32*)H(ctx.a(1)) = g_vm;
  } else {
    return false;
  }
  return true;
}

void jni_handler(Ctx& ctx, const Stub& stub) {
  std::string n = kSlotNames[(intptr_t)stub.fn];
  Result r;
  bool handled = handle_references(n, ctx, r) || handle_ids_and_fields(n, ctx, r) || handle_calls(n, ctx, r) ||
                 handle_strings(n, ctx, r) || handle_arrays(n, ctx, r) || handle_natives(n, ctx, r);
  if (!handled)
    fprintf(stderr, "[jni] UNIMPLEMENTED %s(%08x, %08x, %08x, %08x)\n", n.c_str(), ctx.a(1), ctx.a(2), ctx.a(3),
            ctx.a(4));
  if (r.wide)
    ctx.ret64(r.value);
  else
    ctx.ret((u32)r.value);
}

// JavaVM: reserved0-2, DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon
void vm_handler(Ctx& ctx, const Stub& stub) {
  int slot = (int)(intptr_t)stub.fn;
  bool returns_env = slot == 4 || slot == 6 || slot == 7;  // (vm, JNIEnv** out, ...)
  if (returns_env)
    *(u32*)H(ctx.a(1)) = g_env;
  ctx.ret(0);
}

}  // namespace

void fake_jni_init() {
  u32 table = gdata_alloc(4 * kTableSlots, 4);
  u32* slots = (u32*)H(table);
  for (int i = 0; i < kTableSlots; i++) {
    if (i < kNamedSlots)
      slots[i] = stub_add(strdup((std::string("JNI#") + kSlotNames[i]).c_str()), jni_handler, (void*)(intptr_t)i);
    else
      slots[i] = stub_add(strdup(("JNI#" + std::to_string(i)).c_str()), nullptr);
  }
  g_env = gdata_alloc(4, 4);
  *(u32*)H(g_env) = table;

  u32 vm_table = gdata_alloc(4 * 8, 4);
  for (int i = 0; i < 8; i++)
    ((u32*)H(vm_table))[i] = stub_add(strdup(("JVM#" + std::to_string(i)).c_str()), vm_handler, (void*)(intptr_t)i);
  g_vm = gdata_alloc(4, 4);
  *(u32*)H(g_vm) = vm_table;
}

u32 fake_jni_vm() {
  return g_vm;
}

u32 fake_jni_env() {
  return g_env;
}

const std::vector<RegisteredNative>& registered_natives() {
  return g_natives;
}
