// Gives the classes a packer decrypts to the app's class loader.
//
// On a device that runs a packer's ART hooks, the packer registers the decrypted dex with the runtime itself
// (art::DexFile::OpenMemory). The bridge cannot do that, so the classes would have to be shipped as extra classesN.dex
// files, which newer packers (Jiagu 2.4.x) reject: they walk the APK's central directory and give up when they find
// one. Instead the bridge builds an InMemoryDexClassLoader for every dex handed to OpenMemory and appends its dex
// elements to the app class loader's DexPathList, the way multidex libraries do.
//
// port_apk.py can ship its own, patched version of the game's dex files (hollowed natives replaced, see --unhollow) as
// lib/<abi>/liba32dex<N>.so. When they exist they are loaded instead of what the packer hands over.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "jni/handles.h"
#include "libc/system_imports.h"

static bool clear_exception(JNIEnv* jni, const char* what) {
  if (!jni->ExceptionCheck())
    return false;
  LOG("dexload: %s failed", what);
  jni->ExceptionDescribe();
  jni->ExceptionClear();
  return true;
}

// Reads <object>.<field> with reflection (getDeclaredField + setAccessible + get), through JNI.
static jobject reflect_field(JNIEnv* jni, jclass owner, jobject object, const char* field) {
  jclass clazz = jni->FindClass("java/lang/Class");
  jclass field_class = jni->FindClass("java/lang/reflect/Field");
  jobject f = jni->CallObjectMethod(owner, jni->GetMethodID(clazz, "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
                                    jni->NewStringUTF(field));
  if (clear_exception(jni, field) || !f)
    return nullptr;
  jni->CallVoidMethod(f, jni->GetMethodID(field_class, "setAccessible", "(Z)V"), JNI_TRUE);
  if (clear_exception(jni, "setAccessible"))
    return nullptr;
  jobject value = jni->CallObjectMethod(f, jni->GetMethodID(field_class, "get", "(Ljava/lang/Object;)Ljava/lang/Object;"), object);
  if (clear_exception(jni, "Field.get"))
    return nullptr;
  return value;
}

static bool set_reflect_field(JNIEnv* jni, jclass owner, jobject object, const char* field, jobject value) {
  jclass clazz = jni->FindClass("java/lang/Class");
  jclass field_class = jni->FindClass("java/lang/reflect/Field");
  jobject f = jni->CallObjectMethod(owner, jni->GetMethodID(clazz, "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
                                    jni->NewStringUTF(field));
  if (clear_exception(jni, field) || !f)
    return false;
  jni->CallVoidMethod(f, jni->GetMethodID(field_class, "setAccessible", "(Z)V"), JNI_TRUE);
  jni->CallVoidMethod(f, jni->GetMethodID(field_class, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V"), object, value);
  return !clear_exception(jni, "Field.set");
}

static void load_dex_into_app_loader(const uint8_t* dex, u32 size);

static void load_shipped_dex(const uint8_t* dex, u32 size) {
  load_dex_into_app_loader(dex, size);
}

// Dex files shipped next to the bridge (liba32dex2.so ...), in order.
static std::vector<std::string> shipped_dex_files() {
  std::vector<std::string> files;
  std::string dir = native_library_dir();
  if (dir.empty())
    return files;
  for (int n = 2; n < 10; n++) {
    char name[32];
    snprintf(name, sizeof name, "/liba32dex%d.so", n);
    std::string data;
    if (FILE* f = fopen((dir + name).c_str(), "rb")) {
      char chunk[65536];
      size_t got;
      while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
        data.append(chunk, got);
      fclose(f);
    }
    if (!data.empty())
      files.push_back(std::move(data));
  }
  return files;
}

// What the packer hands to OpenMemory: the first call loads the shipped dex files if there are any, otherwise the
// packer's own bytes (every call).
static void on_dex_handover(const uint8_t* dex, u32 size) {
  static bool shipped_checked = false, use_shipped = false;
  if (!shipped_checked) {
    shipped_checked = true;
    std::vector<std::string> shipped = shipped_dex_files();
    use_shipped = !shipped.empty();
    for (const std::string& data : shipped)
      load_shipped_dex((const uint8_t*)data.data(), (u32)data.size());
  }
  if (!use_shipped)
    load_dex_into_app_loader(dex, size);
}

static void load_dex_into_app_loader(const uint8_t* dex, u32 size) {
  JNIEnv* jni = env();
  if (!jni || size < 0x70)
    return;
  jni->PushLocalFrame(64);
  // The class that called System.load (the packer's StubApp) belongs to the app's class loader.
  jclass stub = jni->FindClass("com/stub/StubApp");
  if (clear_exception(jni, "FindClass StubApp") || !stub) {
    jni->PopLocalFrame(nullptr);
    return;
  }
  jclass class_class = jni->FindClass("java/lang/Class");
  jobject app_loader = jni->CallObjectMethod(stub, jni->GetMethodID(class_class, "getClassLoader", "()Ljava/lang/ClassLoader;"));

  uint8_t* copy = (uint8_t*)malloc(size);  // kept for the life of the process: the loader maps the buffer
  memcpy(copy, dex, size);
  jobject buffer = jni->NewDirectByteBuffer(copy, size);
  jclass imcl_class = jni->FindClass("dalvik/system/InMemoryDexClassLoader");
  jobject imcl = jni->NewObject(imcl_class,
                                jni->GetMethodID(imcl_class, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V"), buffer,
                                app_loader);
  if (clear_exception(jni, "InMemoryDexClassLoader") || !imcl) {
    jni->PopLocalFrame(nullptr);
    return;
  }

  jclass base_loader = jni->FindClass("dalvik/system/BaseDexClassLoader");
  jclass path_list_class = jni->FindClass("dalvik/system/DexPathList");
  jobject app_list = reflect_field(jni, base_loader, app_loader, "pathList");
  jobject new_list = reflect_field(jni, base_loader, imcl, "pathList");
  jobjectArray app_elements = app_list ? (jobjectArray)reflect_field(jni, path_list_class, app_list, "dexElements") : nullptr;
  jobjectArray new_elements = new_list ? (jobjectArray)reflect_field(jni, path_list_class, new_list, "dexElements") : nullptr;
  if (!app_elements || !new_elements) {
    LOG("dexload: cannot reach the dex elements (hidden API?)");
    jni->PopLocalFrame(nullptr);
    return;
  }
  jsize old_count = jni->GetArrayLength(app_elements), added = jni->GetArrayLength(new_elements);
  jclass element_class = jni->FindClass("dalvik/system/DexPathList$Element");
  jobjectArray merged = jni->NewObjectArray(old_count + added, element_class, nullptr);
  for (jsize i = 0; i < old_count; i++)
    jni->SetObjectArrayElement(merged, i, jni->GetObjectArrayElement(app_elements, i));
  for (jsize i = 0; i < added; i++)
    jni->SetObjectArrayElement(merged, old_count + i, jni->GetObjectArrayElement(new_elements, i));
  if (set_reflect_field(jni, path_list_class, app_list, "dexElements", merged))
    LOG("dexload: %u byte dex added to the app class loader (%d + %d elements)", size, old_count, added);
  jni->PopLocalFrame(nullptr);
}

void dexload_init() {
  g_dex_handover_hook = on_dex_handover;
}
