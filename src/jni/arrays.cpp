// Arrays and direct byte buffers.
#include <string.h>

#include <string>

#include "jni/handles.h"

static void get_array_length(Ctx& ctx, const Stub&) {
  jobject array = hobj(ctx.a(1));
  // shortcut: null arrays come from Java calls that failed on modern Android; report length 0 instead of letting ART
  // abort
  if (!array) {
    LOG("GetArrayLength(null) handle=%08x after %s", ctx.a(1), t_last_call.c_str());
    ctx.ret(0);
    return;
  }
  ctx.ret(env()->GetArrayLength((jarray)array));
}

static void new_object_array(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->NewObjectArray(ctx.a(1), (jclass)hobj(ctx.a(2)), hobj(ctx.a(3))), 1));
}

static void get_object_array_element(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->GetObjectArrayElement((jobjectArray)hobj(ctx.a(1)), ctx.a(2)), 1));
}

static void set_object_array_element(Ctx& ctx, const Stub&) {
  env()->SetObjectArrayElement((jobjectArray)hobj(ctx.a(1)), ctx.a(2), hobj(ctx.a(3)));
}

// primitive arrays: type index 0..7 = Boolean,Byte,Char,Short,Int,Long,Float,Double
static const int ELEMENT_SIZE[8] = {1, 1, 2, 2, 4, 8, 4, 8};

static void new_primitive_array(Ctx& ctx, const Stub& stub) {
  JNIEnv* jni = env();
  jsize length = ctx.a(1);
  jarray array = nullptr;
  switch ((int)(intptr_t)stub.fn) {
    case 0:
      array = jni->NewBooleanArray(length);
      break;
    case 1:
      array = jni->NewByteArray(length);
      break;
    case 2:
      array = jni->NewCharArray(length);
      break;
    case 3:
      array = jni->NewShortArray(length);
      break;
    case 4:
      array = jni->NewIntArray(length);
      break;
    case 5:
      array = jni->NewLongArray(length);
      break;
    case 6:
      array = jni->NewFloatArray(length);
      break;
    case 7:
      array = jni->NewDoubleArray(length);
      break;
  }
  ctx.ret(href(array, 1));
}

static void get_array_region(Ctx& ctx, const Stub& stub) {
  JNIEnv* jni = env();
  jarray array = (jarray)hobj(ctx.a(1));
  jsize start = ctx.a(2), length = ctx.a(3);
  void* buffer = ctx.p(4);
  switch ((int)(intptr_t)stub.fn) {
    case 0:
      jni->GetBooleanArrayRegion((jbooleanArray)array, start, length, (jboolean*)buffer);
      break;
    case 1:
      jni->GetByteArrayRegion((jbyteArray)array, start, length, (jbyte*)buffer);
      break;
    case 2:
      jni->GetCharArrayRegion((jcharArray)array, start, length, (jchar*)buffer);
      break;
    case 3:
      jni->GetShortArrayRegion((jshortArray)array, start, length, (jshort*)buffer);
      break;
    case 4:
      jni->GetIntArrayRegion((jintArray)array, start, length, (jint*)buffer);
      break;
    case 5:
      jni->GetLongArrayRegion((jlongArray)array, start, length, (jlong*)buffer);
      break;
    case 6:
      jni->GetFloatArrayRegion((jfloatArray)array, start, length, (jfloat*)buffer);
      break;
    case 7:
      jni->GetDoubleArrayRegion((jdoubleArray)array, start, length, (jdouble*)buffer);
      break;
  }
}

static void set_array_region(Ctx& ctx, const Stub& stub) {
  JNIEnv* jni = env();
  jarray array = (jarray)hobj(ctx.a(1));
  jsize start = ctx.a(2), length = ctx.a(3);
  const void* buffer = ctx.p(4);
  switch ((int)(intptr_t)stub.fn) {
    case 0:
      jni->SetBooleanArrayRegion((jbooleanArray)array, start, length, (const jboolean*)buffer);
      break;
    case 1:
      jni->SetByteArrayRegion((jbyteArray)array, start, length, (const jbyte*)buffer);
      break;
    case 2:
      jni->SetCharArrayRegion((jcharArray)array, start, length, (const jchar*)buffer);
      break;
    case 3:
      jni->SetShortArrayRegion((jshortArray)array, start, length, (const jshort*)buffer);
      break;
    case 4:
      jni->SetIntArrayRegion((jintArray)array, start, length, (const jint*)buffer);
      break;
    case 5:
      jni->SetLongArrayRegion((jlongArray)array, start, length, (const jlong*)buffer);
      break;
    case 6:
      jni->SetFloatArrayRegion((jfloatArray)array, start, length, (const jfloat*)buffer);
      break;
    case 7:
      jni->SetDoubleArrayRegion((jdoubleArray)array, start, length, (const jdouble*)buffer);
      break;
  }
}

// Get<T>ArrayElements: guest copy; Release copies back unless JNI_ABORT
static void get_array_elements(Ctx& ctx, const Stub& stub) {
  int type = (int)(intptr_t)stub.fn;
  jarray array = (jarray)hobj(ctx.a(1));
  jsize length = env()->GetArrayLength(array);
  u32 guest_buffer = gmalloc(length * ELEMENT_SIZE[type] + 8);
  JNIEnv* jni = env();
  switch (type) {
    case 0:
      jni->GetBooleanArrayRegion((jbooleanArray)array, 0, length, (jboolean*)H(guest_buffer));
      break;
    case 1:
      jni->GetByteArrayRegion((jbyteArray)array, 0, length, (jbyte*)H(guest_buffer));
      break;
    case 2:
      jni->GetCharArrayRegion((jcharArray)array, 0, length, (jchar*)H(guest_buffer));
      break;
    case 3:
      jni->GetShortArrayRegion((jshortArray)array, 0, length, (jshort*)H(guest_buffer));
      break;
    case 4:
      jni->GetIntArrayRegion((jintArray)array, 0, length, (jint*)H(guest_buffer));
      break;
    case 5:
      jni->GetLongArrayRegion((jlongArray)array, 0, length, (jlong*)H(guest_buffer));
      break;
    case 6:
      jni->GetFloatArrayRegion((jfloatArray)array, 0, length, (jfloat*)H(guest_buffer));
      break;
    case 7:
      jni->GetDoubleArrayRegion((jdoubleArray)array, 0, length, (jdouble*)H(guest_buffer));
      break;
  }
  if (ctx.a(2))
    *(uint8_t*)ctx.p(2) = 1;
  ctx.ret(guest_buffer);
}

static void release_array_elements(Ctx& ctx, const Stub& stub) {
  int type = (int)(intptr_t)stub.fn;
  jarray array = (jarray)hobj(ctx.a(1));
  u32 guest_buffer = ctx.a(2), mode = ctx.a(3);
  JNIEnv* jni = env();
  jsize length = jni->GetArrayLength(array);
  if (mode != 2 /*JNI_ABORT*/) {
    switch (type) {
      case 0:
        jni->SetBooleanArrayRegion((jbooleanArray)array, 0, length, (jboolean*)H(guest_buffer));
        break;
      case 1:
        jni->SetByteArrayRegion((jbyteArray)array, 0, length, (jbyte*)H(guest_buffer));
        break;
      case 2:
        jni->SetCharArrayRegion((jcharArray)array, 0, length, (jchar*)H(guest_buffer));
        break;
      case 3:
        jni->SetShortArrayRegion((jshortArray)array, 0, length, (jshort*)H(guest_buffer));
        break;
      case 4:
        jni->SetIntArrayRegion((jintArray)array, 0, length, (jint*)H(guest_buffer));
        break;
      case 5:
        jni->SetLongArrayRegion((jlongArray)array, 0, length, (jlong*)H(guest_buffer));
        break;
      case 6:
        jni->SetFloatArrayRegion((jfloatArray)array, 0, length, (jfloat*)H(guest_buffer));
        break;
      case 7:
        jni->SetDoubleArrayRegion((jdoubleArray)array, 0, length, (jdouble*)H(guest_buffer));
        break;
    }
  }
  if (mode != 1 /*JNI_COMMIT*/)
    gfree(guest_buffer);
}

// *ArrayCritical: element type is unknown to the API; recover it from the array's class name
static int primitive_array_type(jarray a) {
  JNIEnv* jni = env();
  jclass array_class = jni->GetObjectClass(a);
  jclass class_class = jni->FindClass("java/lang/Class");
  jstring class_name =
      (jstring)jni->CallObjectMethod(array_class, jni->GetMethodID(class_class, "getName", "()Ljava/lang/String;"));
  const char* name = jni->GetStringUTFChars(class_name, nullptr);
  const char* type_chars = "ZBCSIJFD";
  const char* found = strchr(type_chars, name[1]);
  int type = found ? (int)(found - type_chars) : 1;
  jni->ReleaseStringUTFChars(class_name, name);
  return type;
}

static void get_primitive_array_critical(Ctx& ctx, const Stub& stub) {
  Stub typed = stub;
  typed.fn = (void*)(intptr_t)primitive_array_type((jarray)hobj(ctx.a(1)));
  get_array_elements(ctx, typed);
}

static void release_primitive_array_critical(Ctx& ctx, const Stub& stub) {
  Stub typed = stub;
  typed.fn = (void*)(intptr_t)primitive_array_type((jarray)hobj(ctx.a(1)));
  release_array_elements(ctx, typed);
}

// Guest memory lives inside the host reservation G, so a guest block can back a Java direct buffer as-is.
static void new_direct_byte_buffer(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->NewDirectByteBuffer(H(ctx.a(1)), (jlong)ctx.q(2)), 1));
}

static void get_direct_buffer_address(Ctx& ctx, const Stub&) {
  uint8_t* address = (uint8_t*)env()->GetDirectBufferAddress(hobj(ctx.a(1)));
  // shortcut: Java-allocated direct buffers live outside the 4GB guest window and are unreachable; return NULL
  if (address < G || address >= G + (1ull << 32)) {
    if (address)
      LOG("GetDirectBufferAddress: buffer outside guest space");
    ctx.ret(0);
    return;
  }
  ctx.ret(GA(address));
}

static void get_direct_buffer_capacity(Ctx& ctx, const Stub&) {
  ctx.ret64((u64)env()->GetDirectBufferCapacity(hobj(ctx.a(1))));
}

void register_array_imports(u32* table) {
  jni_set(table, 171, "GetArrayLength", get_array_length);
  jni_set(table, 172, "NewObjectArray", new_object_array);
  jni_set(table, 173, "GetObjectArrayElement", get_object_array_element);
  jni_set(table, 174, "SetObjectArrayElement", set_object_array_element);
  for (int type = 0; type < 8; type++) {  // Boolean, Byte, Char, Short, Int, Long, Float, Double
    jni_set(table, 175 + type, "NewArray", new_primitive_array, type);
    jni_set(table, 183 + type, "GetArrayElements", get_array_elements, type);
    jni_set(table, 191 + type, "ReleaseArrayElements", release_array_elements, type);
    jni_set(table, 199 + type, "GetArrayRegion", get_array_region, type);
    jni_set(table, 207 + type, "SetArrayRegion", set_array_region, type);
  }
  jni_set(table, 222, "GetPrimitiveArrayCritical", get_primitive_array_critical);
  jni_set(table, 223, "ReleasePrimitiveArrayCritical", release_primitive_array_critical);
  jni_set(table, 229, "NewDirectByteBuffer", new_direct_byte_buffer);
  jni_set(table, 230, "GetDirectBufferAddress", get_direct_buffer_address);
  jni_set(table, 231, "GetDirectBufferCapacity", get_direct_buffer_capacity);
}
