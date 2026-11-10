// Classes, object references, exceptions, and method/field id lookup.
#include <string.h>

#include <string>

#include "jni/handles.h"

static void get_version(Ctx& ctx, const Stub&) {
  ctx.ret(JNI_VERSION_1_6);
}

static void find_class(Ctx& ctx, const Stub&) {
  JNIEnv* jni = env();
  jclass cls = jni->FindClass((const char*)ctx.p(1));
  if (!cls) {
    LOG("FindClass failed: %s", (const char*)ctx.p(1));
    jni->ExceptionClear();
  }
  ctx.ret(href(cls, 1));
}

static void alloc_object(Ctx& ctx, const Stub&) {
  jobject object = env()->AllocObject((jclass)hobj(ctx.a(1)));
  if (!object)
    env()->ExceptionClear();
  ctx.ret(href(object, 1));
}

static void get_object_class(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->GetObjectClass(hobj(ctx.a(1))), 1));
}

static void get_superclass(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->GetSuperclass((jclass)hobj(ctx.a(1))), 1));
}

static void is_instance_of(Ctx& ctx, const Stub&) {
  ctx.ret(env()->IsInstanceOf(hobj(ctx.a(1)), (jclass)hobj(ctx.a(2))));
}

static void is_same_object(Ctx& ctx, const Stub&) {
  ctx.ret(env()->IsSameObject(hobj(ctx.a(1)), hobj(ctx.a(2))));
}

static void new_global_ref(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->NewGlobalRef(hobj(ctx.a(1))), 2));
}

static void new_local_ref(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->NewLocalRef(hobj(ctx.a(1))), 1));
}

static void delete_global_ref(Ctx& ctx, const Stub&) {
  jobject object = hobj(ctx.a(1));
  if (object)
    env()->DeleteGlobalRef(object);
  hfree(ctx.a(1));
}

static void delete_local_ref(Ctx& ctx, const Stub&) {
  jobject object = hobj(ctx.a(1));
  if (object)
    env()->DeleteLocalRef(object);
  hfree(ctx.a(1));
}

static void pop_local_frame(Ctx& ctx, const Stub&) {
  ctx.ret(ctx.a(1));
}

static void exception_check(Ctx& ctx, const Stub&) {
  ctx.ret(env()->ExceptionCheck());
}

static void exception_clear(Ctx& ctx, const Stub&) {
  env()->ExceptionClear();
}

static void exception_describe(Ctx& ctx, const Stub&) {
  env()->ExceptionDescribe();
}

static void exception_occurred(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->ExceptionOccurred(), 1));
}

static void throw_object(Ctx& ctx, const Stub&) {
  ctx.ret(env()->Throw((jthrowable)hobj(ctx.a(1))));
}

static void throw_new(Ctx& ctx, const Stub&) {
  ctx.ret(env()->ThrowNew((jclass)hobj(ctx.a(1)), (const char*)ctx.p(2)));
}

static void get_method_id(Ctx& ctx, const Stub& stub) {
  bool is_static = !strcmp(stub.name, "GetStaticMethodID");
  JNIEnv* jni = env();
  jclass cls = (jclass)hobj(ctx.a(1));
  const char *name = (const char*)ctx.p(2), *signature = (const char*)ctx.p(3);
  if (!cls) {
    LOG("GetMethodID on null class: %s %s", name, signature);
    ctx.ret(0);
    return;
  }
  jmethodID method_id =
      is_static ? jni->GetStaticMethodID(cls, name, signature) : jni->GetMethodID(cls, name, signature);
  if (!method_id)
    LOG("GetMethodID failed: %s %s", name, signature);
  ctx.ret(mid_new(method_id, signature, is_static, name));
}

static void get_field_id(Ctx& ctx, const Stub& stub) {
  bool is_static = !strcmp(stub.name, "GetStaticFieldID");
  JNIEnv* jni = env();
  jclass cls = (jclass)hobj(ctx.a(1));
  const char *name = (const char*)ctx.p(2), *signature = (const char*)ctx.p(3);
  if (!cls) {
    LOG("GetFieldID on null class: %s %s", name, signature);
    ctx.ret(0);
    return;
  }
  jfieldID field_id = is_static ? jni->GetStaticFieldID(cls, name, signature) : jni->GetFieldID(cls, name, signature);
  if (!field_id)
    LOG("GetFieldID failed: %s %s", name, signature);
  ctx.ret(fid_new(field_id, signature[0]));
}

void register_object_imports(u32* table) {
  jni_set(table, 4, "GetVersion", get_version);
  jni_set(table, 6, "FindClass", find_class);
  jni_set(table, 10, "GetSuperclass", get_superclass);
  jni_set(table, 13, "Throw", throw_object);
  jni_set(table, 14, "ThrowNew", throw_new);
  jni_set(table, 15, "ExceptionOccurred", exception_occurred);
  jni_set(table, 16, "ExceptionDescribe", exception_describe);
  jni_set(table, 17, "ExceptionClear", exception_clear);
  jni_set(table, 19, "PushLocalFrame", jni_return_zero);
  jni_set(table, 20, "PopLocalFrame", pop_local_frame);
  jni_set(table, 21, "NewGlobalRef", new_global_ref);
  jni_set(table, 22, "DeleteGlobalRef", delete_global_ref);
  jni_set(table, 23, "DeleteLocalRef", delete_local_ref);
  jni_set(table, 27, "AllocObject", alloc_object);
  jni_set(table, 24, "IsSameObject", is_same_object);
  jni_set(table, 25, "NewLocalRef", new_local_ref);
  jni_set(table, 26, "EnsureLocalCapacity", jni_return_zero);
  jni_set(table, 31, "GetObjectClass", get_object_class);
  jni_set(table, 32, "IsInstanceOf", is_instance_of);
  jni_set(table, 33, "GetMethodID", get_method_id);
  jni_set(table, 94, "GetFieldID", get_field_id);
  jni_set(table, 113, "GetStaticMethodID", get_method_id);
  jni_set(table, 144, "GetStaticFieldID", get_field_id);
  jni_set(table, 217, "MonitorEnter", jni_return_zero);
  jni_set(table, 218, "MonitorExit", jni_return_zero);
  jni_set(table, 226, "NewWeakGlobalRef", new_global_ref);
  jni_set(table, 227, "DeleteWeakGlobalRef", delete_global_ref);
  jni_set(table, 228, "ExceptionCheck", exception_check);
  jni_set(table, 232, "GetObjectRefType", jni_return_zero);
}
