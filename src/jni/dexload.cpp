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
#include <unistd.h>

#include <string>
#include <vector>

#include "jni/bridge.h"
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

static jobject g_spare_dexfile = nullptr;  // global ref: the DexFile of the smallest in-memory dex
static u32 g_spare_size = 0;

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
  // An arm64 bridge started by an x86_64 device is told "lib/arm64", but only the primary ABI's directory exists.
  std::string probe = dir + "/liba32dex2.so";
  if (access(probe.c_str(), R_OK) != 0) {
    std::string parent = dir.substr(0, dir.rfind('/'));
    for (const char* abi : {"x86_64", "arm64-v8a", "arm64", "x86"}) {
      std::string candidate = parent + "/" + abi;
      std::string file = candidate + "/liba32dex2.so";
      if (access(file.c_str(), R_OK) == 0) {
        dir = candidate;
        break;
      }
    }
  }
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
  jclass class_class = jni->FindClass("java/lang/Class");
  jobject app_loader = nullptr;
  jclass stub = jni->FindClass("com/stub/StubApp");
  if (!clear_exception(jni, "FindClass StubApp") && stub)
    app_loader = jni->CallObjectMethod(stub, jni->GetMethodID(class_class, "getClassLoader", "()Ljava/lang/ClassLoader;"));
  if (!app_loader) {  // packers whose shell class has another name: the main thread's context loader is the app loader
    jclass thread_class = jni->FindClass("java/lang/Thread");
    jobject thread = jni->CallStaticObjectMethod(
        thread_class, jni->GetStaticMethodID(thread_class, "currentThread", "()Ljava/lang/Thread;"));
    if (thread)
      app_loader = jni->CallObjectMethod(
          thread, jni->GetMethodID(thread_class, "getContextClassLoader", "()Ljava/lang/ClassLoader;"));
    clear_exception(jni, "context class loader");
  }
  if (!app_loader) {
    jni->PopLocalFrame(nullptr);
    return;
  }

  uint8_t* copy = (uint8_t*)malloc(size);  // kept for the life of the process: the loader maps the buffer
  memcpy(copy, dex, size);
  jobject buffer = jni->NewDirectByteBuffer(copy, size);

  jclass base_loader = jni->FindClass("dalvik/system/BaseDexClassLoader");
  jclass path_list_class = jni->FindClass("dalvik/system/DexPathList");
  jobject app_list = reflect_field(jni, base_loader, app_loader, "pathList");
  jobjectArray app_elements = app_list ? (jobjectArray)reflect_field(jni, path_list_class, app_list, "dexElements") : nullptr;

  // Preferred: DexPathList.makeInMemoryDexElements(ByteBuffer[], List) builds Elements that are not tied to any class
  // loader yet, so the app loader can define the classes. (An InMemoryDexClassLoader registers the dex with itself and
  // ART then refuses "multiple class loaders" when the app loader defines a class from it.)
  jobjectArray new_elements = nullptr;
  {
    jclass class_class2 = jni->FindClass("java/lang/Class");
    jclass method_class = jni->FindClass("java/lang/reflect/Method");
    jclass bb_class = jni->FindClass("java/nio/ByteBuffer");
    jclass list_iface = jni->FindClass("java/util/List");
    jclass arraylist = jni->FindClass("java/util/ArrayList");
    jobjectArray param_types = jni->NewObjectArray(2, class_class2, nullptr);
    jclass bb_array_class = jni->FindClass("[Ljava/nio/ByteBuffer;");
    jni->SetObjectArrayElement(param_types, 0, bb_array_class);
    jni->SetObjectArrayElement(param_types, 1, list_iface);
    jobject method = jni->CallObjectMethod(
        path_list_class,
        jni->GetMethodID(class_class2, "getDeclaredMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;"),
        jni->NewStringUTF("makeInMemoryDexElements"), param_types);
    if (!clear_exception(jni, "getDeclaredMethod makeInMemoryDexElements") && method) {
      jni->CallVoidMethod(method, jni->GetMethodID(method_class, "setAccessible", "(Z)V"), JNI_TRUE);
      jobjectArray buffers = jni->NewObjectArray(1, bb_class, buffer);
      jobject errors = jni->NewObject(arraylist, jni->GetMethodID(arraylist, "<init>", "()V"));
      jobjectArray args = jni->NewObjectArray(2, jni->FindClass("java/lang/Object"), nullptr);
      jni->SetObjectArrayElement(args, 0, buffers);
      jni->SetObjectArrayElement(args, 1, errors);
      jobject result = jni->CallObjectMethod(
          method, jni->GetMethodID(method_class, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"), nullptr, args);
      if (!clear_exception(jni, "makeInMemoryDexElements") && result)
        new_elements = (jobjectArray)result;
    }
  }
  if (!new_elements) {
    jclass imcl_class = jni->FindClass("dalvik/system/InMemoryDexClassLoader");
    jobject imcl = jni->NewObject(imcl_class,
                                  jni->GetMethodID(imcl_class, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V"), buffer,
                                  app_loader);
    if (clear_exception(jni, "InMemoryDexClassLoader") || !imcl) {
      jni->PopLocalFrame(nullptr);
      return;
    }
    jobject new_list = reflect_field(jni, base_loader, imcl, "pathList");
    new_elements = new_list ? (jobjectArray)reflect_field(jni, path_list_class, new_list, "dexElements") : nullptr;
  }
  if (!app_elements || !new_elements) {
    LOG("dexload: cannot reach the dex elements (hidden API?)");
    jni->PopLocalFrame(nullptr);
    return;
  }
  jsize old_count = jni->GetArrayLength(app_elements), added = jni->GetArrayLength(new_elements);
  if (added > 0 && (!g_spare_dexfile || size < g_spare_size)) {
    jclass element_cls = jni->FindClass("dalvik/system/DexPathList$Element");
    jobject first = jni->GetObjectArrayElement(new_elements, 0);
    jobject dex_file = first ? reflect_field(jni, element_cls, first, "dexFile") : nullptr;
    if (dex_file) {
      if (g_spare_dexfile)
        jni->DeleteGlobalRef(g_spare_dexfile);
      g_spare_dexfile = jni->NewGlobalRef(dex_file);
      g_spare_size = size;
    }
  }
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

// A valid, harmless DexFile (the one of the smallest dex handed over): old packers call DexFile.loadDex on the APK and
// wrap the result in a DexPathList.Element, which must not be null. Its classes are already in the app class loader.
jobject dexload_spare_dexfile() {
  JNIEnv* jni = env();
  return jni && g_spare_dexfile ? jni->NewLocalRef(g_spare_dexfile) : nullptr;
}
