// Call<Type>Method, NewObject and Get/Set<Type>Field: decode the guest's arguments and invoke the host JNI.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "jni/bridge.h"
#include "jni/handles.h"

thread_local std::string t_last_call;

// ------------------------------------------------------------------ argument decoding

// Cursor over the arguments of a Call*Method / NewObject invocation.
struct ArgReader {
  Ctx* x;
  int ai;      // next argument index (variadic mode)
  u32 va;      // next slot of the va_list / jvalue array (guest address)
  int source;  // ArgSource

  u32 word() {
    if (source == ARGS_VALIST) {
      u32 value = *(u32*)H(va);
      va += 4;
      return value;
    }
    return x->a(ai++);
  }

  u64 dword() {
    if (source == ARGS_VALIST) {
      va = (va + 7) & ~7u;
      u64 value = *(u64*)H(va);
      va += 8;
      return value;
    }
    if (ai & 1)
      ai++;  // 64-bit values are register/stack aligned
    u64 value = x->q(ai);
    ai += 2;
    return value;
  }
};

static jobject object_arg(u32 handle, const std::string& sig, int position) {
  jobject object = hobj(handle);
  if (handle && !object)
    LOG("JNI object arg #%d of %s is not a valid handle: %08x", position, sig.c_str(), handle);
  return object;
}

// Converts the guest's arguments to jvalues according to the method signature.
static std::vector<jvalue> decode_args(const std::string& sig, ArgReader& reader) {
  std::vector<jvalue> out;
  int position = 0;
  size_t pos = 1;  // skip '('
  auto skip_type = [&]() {
    while (sig[pos] == '[')
      pos++;
    if (sig[pos] == 'L')
      while (sig[pos] != ';')
        pos++;
    pos++;
  };
  while (sig[pos] != ')') {
    char type = sig[pos];
    jvalue value;
    memset(&value, 0, sizeof value);
    if (reader.source == ARGS_JVALUES) {  // 8-byte guest jvalue slots
      uint8_t* slot = (uint8_t*)H(reader.va);
      reader.va += 8;
      switch (type) {
        case 'Z':
          value.z = slot[0];
          break;
        case 'B':
          value.b = (jbyte)slot[0];
          break;
        case 'C':
          value.c = *(uint16_t*)slot;
          break;
        case 'S':
          value.s = *(int16_t*)slot;
          break;
        case 'I':
          value.i = *(int32_t*)slot;
          break;
        case 'J':
          value.j = *(int64_t*)slot;
          break;
        case 'F':
          value.f = *(float*)slot;
          break;
        case 'D':
          value.d = *(double*)slot;
          break;
        default:
          value.l = object_arg(*(u32*)slot, sig, position);
      }
    } else {
      switch (type) {
        case 'Z':
          value.z = reader.word() & 0xff;
          break;
        case 'B':
          value.b = (jbyte)reader.word();
          break;
        case 'C':
          value.c = (jchar)reader.word();
          break;
        case 'S':
          value.s = (jshort)reader.word();
          break;
        case 'I':
          value.i = (jint)reader.word();
          break;
        case 'J':
          value.j = (jlong)reader.dword();
          break;
        case 'F': {  // variadic arguments promote float to double
          u64 bits = reader.dword();
          double number;
          memcpy(&number, &bits, 8);
          value.f = (float)number;
          break;
        }
        case 'D': {
          u64 bits = reader.dword();
          memcpy(&value.d, &bits, 8);
          break;
        }
        default:
          value.l = object_arg(reader.word(), sig, position);
      }
    }
    out.push_back(value);
    position++;
    skip_type();
  }
  return out;
}

#ifdef A32_TRACE
// Logs "name(sig)" plus the value of every String argument.
static void trace_call(JNIEnv* e, const MId& m, const std::vector<jvalue>& args) {
  std::string line = m.name + m.sig;
  const std::string& sig = m.sig;
  size_t arg = 0;
  for (size_t i = 1; i < sig.size() && sig[i] != ')'; i++, arg++) {
    while (sig[i] == '[')
      i++;
    if (sig[i] != 'L')
      continue;
    size_t end = sig.find(';', i);
    bool is_string = sig.compare(i, end - i + 1, "Ljava/lang/String;") == 0;
    if (is_string && args[arg].l) {
      const char* utf = e->GetStringUTFChars((jstring)args[arg].l, nullptr);
      line += std::string(" \"") + utf + "\"";
      e->ReleaseStringUTFChars((jstring)args[arg].l, utf);
    }
    i = end;
  }
  LOG("JNI %s", line.c_str());
}
#endif

// ------------------------------------------------------------------ Call*Method

// Handles every Call<Type>Method{,V,A}, CallNonvirtual<Type>Method{,V,A}, CallStatic<Type>Method{,V,A} and
// NewObject{,V,A}.
static void do_call(Ctx& ctx, int kind, int source, int return_type, bool is_new) {
  JNIEnv* jni = env();
  int arg_index = kind == CALL_NONVIRTUAL ? 4 : 3;  // (env, obj[, cls], methodID, ...)
  jobject obj = hobj(ctx.a(1));
  jclass cls = kind == CALL_NONVIRTUAL ? (jclass)hobj(ctx.a(2)) : (jclass)obj;
  MId method = mid(ctx.a(arg_index - 1));
  if (!method.m) {
    LOG("JNI call with unresolved method id %u, skipped", ctx.a(arg_index - 1));
    ctx.ret(0);
    return;
  }
  if (!obj) {  // a failed earlier lookup left the game with a null class/object; ART would abort
    LOG("JNI call %s%s with null %s (handle %08x), caller lr=%08x -> skipped", method.name.c_str(), method.sig.c_str(),
        kind == CALL_STATIC ? "class" : "object", ctx.a(1), cpu_lr(ctx.cpu));
    ctx.ret(0);
    return;
  }
#ifdef ORIG_SIG_HASH
  // The game compares its APK signature hash with the publisher's. The APK is re-signed, so answer with the original.
  if (kind == CALL_VIRTUAL && method.name == "hashCode" && method.sig == "()I") {
    static jclass signature = (jclass)jni->NewGlobalRef(jni->FindClass("android/content/pm/Signature"));
    if (jni->IsInstanceOf(obj, signature)) {
      ctx.ret((u32)(int32_t)ORIG_SIG_HASH);
      return;
    }
  }
#endif
  // The APK is re-signed: answer certificate queries about it with the original certificate (see original_certificate).
  if (kind == CALL_VIRTUAL && !original_certificate().empty() &&
      (method.name == "toByteArray" || method.name == "toCharsString" || method.name == "getCertificates")) {
    const std::string& der = original_certificate();
    static jclass signature = (jclass)jni->NewGlobalRef(jni->FindClass("android/content/pm/Signature"));
    static jclass jar_entry = (jclass)jni->NewGlobalRef(jni->FindClass("java/util/jar/JarEntry"));
    if (method.name != "getCertificates" && jni->IsInstanceOf(obj, signature)) {
      if (method.name == "toByteArray") {
        jbyteArray bytes = jni->NewByteArray((jsize)der.size());
        jni->SetByteArrayRegion(bytes, 0, (jsize)der.size(), (const jbyte*)der.data());
        ctx.ret(href(bytes, REF_LOCAL));
      } else {
        std::string hex;
        char two[3];
        for (char c : der) {
          snprintf(two, sizeof two, "%02x", (unsigned char)c);
          hex += two;
        }
        ctx.ret(href(jni->NewStringUTF(hex.c_str()), REF_LOCAL));
      }
      return;
    }
    if (method.name == "getCertificates" && jni->IsInstanceOf(obj, jar_entry)) {  // JarEntry.getCertificates()
      jclass factory_class = jni->FindClass("java/security/cert/CertificateFactory");
      jclass stream_class = jni->FindClass("java/io/ByteArrayInputStream");
      jclass cert_class = jni->FindClass("java/security/cert/Certificate");
      jbyteArray bytes = jni->NewByteArray((jsize)der.size());
      jni->SetByteArrayRegion(bytes, 0, (jsize)der.size(), (const jbyte*)der.data());
      jobject stream = jni->NewObject(stream_class, jni->GetMethodID(stream_class, "<init>", "([B)V"), bytes);
      jobject factory = jni->CallStaticObjectMethod(
          factory_class,
          jni->GetStaticMethodID(factory_class, "getInstance", "(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;"),
          jni->NewStringUTF("X.509"));
      jobject cert = jni->CallObjectMethod(
          factory, jni->GetMethodID(factory_class, "generateCertificate", "(Ljava/io/InputStream;)Ljava/security/cert/Certificate;"),
          stream);
      if (!jni->ExceptionCheck() && cert) {
        jobjectArray certs = jni->NewObjectArray(1, cert_class, cert);
        ctx.ret(href(certs, REF_LOCAL));
        return;
      }
      jni->ExceptionClear();  // fall through to the real answer
    }
  }
  t_last_call = method.name + method.sig;
  ArgReader reader{&ctx, arg_index, 0, source};
  if (source != ARGS_VARIADIC)
    reader.va = ctx.a(arg_index);
  std::vector<jvalue> args = decode_args(method.sig, reader);
#ifdef A32_TRACE
  trace_call(jni, method, args);
#endif
  args.push_back(jvalue{});  // keeps data() non-null for argument-less methods
  jvalue* jvalues = args.data();
  jvalue res;
  memset(&res, 0, sizeof res);

  if (is_new) {
    res.l = jni->NewObjectA(cls, method.m, jvalues);
    ctx.ret(href(res.l, REF_LOCAL));
    return;
  }
#define CALL(T, field)                                                                                 \
  res.field = kind == CALL_VIRTUAL      ? jni->Call##T##MethodA(obj, method.m, jvalues)                \
              : kind == CALL_NONVIRTUAL ? jni->CallNonvirtual##T##MethodA(obj, cls, method.m, jvalues) \
                                        : jni->CallStatic##T##MethodA(cls, method.m, jvalues)
  switch (return_type) {
    case T_OBJECT:
      CALL(Object, l);
      ctx.ret(href(res.l, REF_LOCAL));
      break;
    case T_BOOLEAN:
      CALL(Boolean, z);
      ctx.ret(res.z);
      break;
    case T_BYTE:
      CALL(Byte, b);
      ctx.ret((u32)(int32_t)res.b);
      break;
    case T_CHAR:
      CALL(Char, c);
      ctx.ret(res.c);
      break;
    case T_SHORT:
      CALL(Short, s);
      ctx.ret((u32)(int32_t)res.s);
      break;
    case T_INT:
      CALL(Int, i);
      ctx.ret((u32)res.i);
      break;
    case T_LONG:
      CALL(Long, j);
      ctx.ret64((u64)res.j);
      break;
    case T_FLOAT: {
      CALL(Float, f);
      u32 bits;
      memcpy(&bits, &res.f, 4);
      ctx.ret(bits);
      break;
    }
    case T_DOUBLE: {
      CALL(Double, d);
      u64 bits;
      memcpy(&bits, &res.d, 8);
      ctx.ret64(bits);
      break;
    }
    default:  // void
      if (kind == CALL_VIRTUAL)
        jni->CallVoidMethodA(obj, method.m, jvalues);
      else if (kind == CALL_NONVIRTUAL)
        jni->CallNonvirtualVoidMethodA(obj, cls, method.m, jvalues);
      else
        jni->CallStaticVoidMethodA(cls, method.m, jvalues);
  }
#undef CALL
  // The game never checks for Java exceptions (old SDKs throw on modern Android). Log and clear so that ART does
  // not abort on the next JNI call.
  if (jni->ExceptionCheck()) {
    LOG("JNI exception after %s%s (cleared)", method.name.c_str(), method.sig.c_str());
    jni->ExceptionDescribe();
    jni->ExceptionClear();
  }
}

static void call_method(Ctx& ctx, const Stub& stub) {
  int code = (int)(intptr_t)stub.fn;
  do_call(ctx, code / 100, code / 10 % 10, code % 10, false);
}

// NewObject(env, cls, mid, ...) has the same layout as a static call.
static void new_object(Ctx& ctx, const Stub& stub) {
  do_call(ctx, CALL_STATIC, (int)(intptr_t)stub.fn, T_OBJECT, true);
}

// ------------------------------------------------------------------ fields

// Get/Set[Static]<Type>Field. The stub code is: bit 8 = static, bit 4 = setter, low nibble = JType.
static void access_field(Ctx& ctx, const Stub& stub) {
  JNIEnv* jni = env();
  int code = (int)(intptr_t)stub.fn;
  bool is_static = code & 0x100, is_set = code & 0x10;
  int type = code & 0xf;
  jobject object = hobj(ctx.a(1));
  jfieldID field_id = fid(ctx.a(2)).f;
  if (!field_id || !object) {
    LOG("field access with null %s, skipped", field_id ? "object" : "id");
    ctx.ret(0);
    return;
  }
  ArgReader reader{&ctx, 3, 0, ARGS_VARIADIC};
#define FIELD(T, ctype, get_result, set_value)                                                                       \
  if (is_set) {                                                                                                      \
    ctype v = set_value;                                                                                             \
    if (is_static)                                                                                                   \
      jni->SetStatic##T##Field((jclass)object, field_id, v);                                                         \
    else                                                                                                             \
      jni->Set##T##Field(object, field_id, v);                                                                       \
  } else {                                                                                                           \
    ctype v = is_static ? jni->GetStatic##T##Field((jclass)object, field_id) : jni->Get##T##Field(object, field_id); \
    get_result;                                                                                                      \
  }
  switch (type) {
    case T_OBJECT:
      FIELD(Object, jobject, ctx.ret(href(v, REF_LOCAL)), hobj(reader.word())) break;
    case T_BOOLEAN:
      FIELD(Boolean, jboolean, ctx.ret(v), reader.word() & 0xff) break;
    case T_BYTE:
      FIELD(Byte, jbyte, ctx.ret((u32)(int32_t)v), (jbyte)reader.word()) break;
    case T_CHAR:
      FIELD(Char, jchar, ctx.ret(v), (jchar)reader.word()) break;
    case T_SHORT:
      FIELD(Short, jshort, ctx.ret((u32)(int32_t)v), (jshort)reader.word()) break;
    case T_INT:
      FIELD(Int, jint, ctx.ret((u32)v), (jint)reader.word()) break;
    case T_LONG:
      FIELD(Long, jlong, ctx.ret64((u64)v), (jlong)reader.dword()) break;
    case T_FLOAT:
      FIELD(
          Float, jfloat,
          {
            u32 bits;
            memcpy(&bits, &v, 4);
            ctx.ret(bits);
          },
          ({
            u32 bits = reader.word();
            float value;
            memcpy(&value, &bits, 4);
            value;
          }))
      break;
    case T_DOUBLE:
      FIELD(
          Double, jdouble,
          {
            u64 bits;
            memcpy(&bits, &v, 8);
            ctx.ret64(bits);
          },
          ({
            u64 bits = reader.dword();
            double value;
            memcpy(&value, &bits, 8);
            value;
          }))
      break;
  }
#undef FIELD
}

// ------------------------------------------------------------------ registration

void register_call_imports(u32* table) {
  // NewObject, NewObjectV, NewObjectA
  for (int source = 0; source < 3; source++)
    jni_set(table, 28 + source, "NewObject", new_object, source);
  // Call<Type>Method{,V,A} for the 10 return types, in the three kinds: virtual, nonvirtual, static
  for (int type = 0; type < T_COUNT; type++) {
    for (int source = 0; source < 3; source++) {
      jni_set(table, 34 + type * 3 + source, "CallMethod", call_method, CALL_VIRTUAL * 100 + source * 10 + type);
      jni_set(table, 64 + type * 3 + source, "CallNonvirtualMethod", call_method,
              CALL_NONVIRTUAL * 100 + source * 10 + type);
      jni_set(table, 114 + type * 3 + source, "CallStaticMethod", call_method, CALL_STATIC * 100 + source * 10 + type);
    }
  }
  // Get/Set[Static]<Type>Field for the 9 value types
  for (int type = 0; type < 9; type++) {
    jni_set(table, 95 + type, "GetField", access_field, type);
    jni_set(table, 104 + type, "SetField", access_field, 0x10 | type);
    jni_set(table, 145 + type, "GetStaticField", access_field, 0x100 | type);
    jni_set(table, 154 + type, "SetStaticField", access_field, 0x110 | type);
  }
}
