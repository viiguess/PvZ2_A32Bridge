#include "fake_java.h"
#include "runtime/runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <map>

Config g_cfg;

// ---------------------------------------------------------------------------------------------------------------
// Object table
// ---------------------------------------------------------------------------------------------------------------
static std::vector<Obj> g_objs(1);  // index 0 is reserved: handle 0 is null
static std::map<std::string, u32> g_classes;

u32 new_obj(const Obj& obj) {
  g_objs.push_back(obj);
  return REF_BASE + 4 * (u32)(g_objs.size() - 1);
}

Obj* obj_of(u32 handle) {
  if (handle < REF_BASE || ((handle - REF_BASE) & 3))
    return nullptr;
  u32 index = (handle - REF_BASE) / 4;
  return index && index < g_objs.size() ? &g_objs[index] : nullptr;
}

u32 class_handle(const std::string& name) {
  auto it = g_classes.find(name);
  if (it != g_classes.end())
    return it->second;
  Obj cls;
  cls.cls = name;
  cls.is_class = true;
  return g_classes[name] = new_obj(cls);
}

u32 new_string(const std::string& text) {
  Obj s;
  s.cls = "java/lang/String";
  s.is_string = true;
  s.str = text;
  return new_obj(s);
}

u32 new_instance(const std::string& cls) {
  Obj o;
  o.cls = cls;
  return new_obj(o);
}

std::string describe(char kind, uint64_t value) {
  if (kind != 'L') {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)value);
    return buf;
  }
  Obj* o = obj_of((u32)value);
  if (!o)
    return value ? "ref?" : "null";
  if (o->is_string)
    return "\"" + o->str + "\"";
  return "<" + o->cls + ">" + (o->is_class ? "(class)" : "");
}

// ---------------------------------------------------------------------------------------------------------------
// Method and field ids (index 0 is invalid)
// ---------------------------------------------------------------------------------------------------------------
static std::vector<MethodId> g_methods(1);
static std::vector<FieldId> g_fields(1);

u32 method_id_new(const MethodId& id) {
  g_methods.push_back(id);
  return (u32)g_methods.size() - 1;
}

const MethodId* method_id(u32 id) {
  return id && id < g_methods.size() ? &g_methods[id] : nullptr;
}

u32 field_id_new(const FieldId& id) {
  g_fields.push_back(id);
  return (u32)g_fields.size() - 1;
}

const FieldId* field_id(u32 id) {
  return id && id < g_fields.size() ? &g_fields[id] : nullptr;
}

std::vector<char> signature_args(const std::string& sig, char& ret) {
  std::vector<char> args;
  size_t i = 1;  // skip '('
  while (i < sig.size() && sig[i] != ')') {
    if (sig[i] == 'L') {
      args.push_back('L');
      i = sig.find(';', i) + 1;
    } else if (sig[i] == '[') {
      while (sig[i] == '[')
        i++;
      if (sig[i] == 'L')
        i = sig.find(';', i);
      args.push_back('L');
      i++;
    } else {
      args.push_back(sig[i++]);
    }
  }
  ret = sig[sig.find(')') + 1];
  return args;
}

// ---------------------------------------------------------------------------------------------------------------
// Canned behaviour of the Android APIs
// ---------------------------------------------------------------------------------------------------------------
namespace {

const char kDeviceModel[] = "Pixel 5";
const char kDeviceMaker[] = "google";
const char kAndroidRelease[] = "14";
const int kSdkInt = 34;

// "Lfoo/Bar;" -> "foo/Bar"
std::string class_of_signature(const std::string& sig) {
  return sig.size() > 2 && sig[0] == 'L' ? sig.substr(1, sig.size() - 2) : "java/lang/Object";
}

bool ends_with(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Context.FOO_SERVICE -> "foo"; any other string constant gets a placeholder
std::string string_constant(const std::string& name) {
  if (!ends_with(name, "_SERVICE") || name.size() == 8)
    return "unknown";
  std::string value = name.substr(0, name.size() - 8);
  for (char& c : value)
    c = (char)tolower((unsigned char)c);
  return value;
}

}  // namespace

uint64_t java_call(const MethodId& m, u32 self) {
  const std::string& n = m.name;
  char ret;
  signature_args(m.sig, ret);

  if (n == "getPackageName")
    return new_string(g_cfg.package);
  if (n == "getApplicationInfo")
    return new_instance("android/content/pm/ApplicationInfo");
  // The packer stub (com.stub.StubApp) keeps where it unpacked its library in static fields and hands them back.
  if (m.cls == "com/stub/StubApp" && ret == 'L') {
    if (n == "getDir")
      return new_string(g_cfg.work_dir + "/.jiagu");
    if (n == "getJiaguSoName")
      return new_string("libjiagu.so");
    if (n == "getSoPath")
      return new_string(g_cfg.work_dir + "/.jiagu/libjiagu.so");
  }
  if (n == "getBytes" && self) {  // String.getBytes(...) -> byte[]
    Obj* text = obj_of(self);
    if (text && text->is_string) {
      Obj bytes;
      bytes.cls = "[prim";
      bytes.len = (u32)text->str.size();
      bytes.elem_size = 1;
      bytes.buf = gmalloc(bytes.len + 8);
      memcpy(H(bytes.buf), text->str.data(), bytes.len);
      return new_obj(bytes);
    }
  }
  if (n == "getFilesDir" || n == "getCacheDir" || n == "getDir")
    return new_instance("java/io/File");
  if (n == "getAbsolutePath" || n == "getPath")
    return new_string(g_cfg.work_dir + "/files");
  if (n == "getSourceDir" || n == "getPackageCodePath")
    return new_string(g_cfg.apk_path);
  if (n == "getApplicationContext" || n == "getBaseContext" || n == "getAppContext")
    return new_instance("android/app/Application");
  if (n == "getClassLoader")
    return new_instance("dalvik/system/PathClassLoader");
  if (n == "getLanguage")
    return new_string("zh");
  if (n == "getCountry")
    return new_string("CN");
  if (n == "getProperty")  // the only property packers ask for is java.vm.version: ART reports "2.x.y"
    return new_string("2.1.0");
  if (n == "get" || n == "getString")
    return new_string("");

  // Anything that returns an object gets a fresh instance of the declared type, so chains of calls keep working.
  // Strings that are not special-cased above stay null.
  if (ret == 'L' && m.sig.find(")Ljava/lang/String;") == std::string::npos)
    return new_instance(class_of_signature(m.sig.substr(m.sig.find(')') + 1)));
  (void)self;
  return 0;
}

uint64_t java_get_field(const FieldId& f, u32 self) {
  const std::string& n = f.name;
  if (n == "sourceDir" || n == "publicSourceDir")
    return new_string(g_cfg.apk_path);
  if (n == "dataDir")
    return new_string(g_cfg.work_dir);
  if (n == "nativeLibraryDir")
    return new_string(g_cfg.work_dir + "/lib");
  if (n == "className" || n == "name")  // ApplicationInfo.className: the app's Application class
    return new_string("com.stub.StubApp");
  if (n == "packageName")
    return new_string(g_cfg.package);
  if (n == "versionName")
    return new_string(g_cfg.version_name);
  if (n == "versionCode")
    return (uint64_t)g_cfg.version_code;
  if (n == "SDK_INT")
    return kSdkInt;
  if (n == "MODEL")
    return new_string(kDeviceModel);
  if (n == "MANUFACTURER" || n == "BRAND")
    return new_string(kDeviceMaker);
  if (n == "RELEASE")
    return new_string(kAndroidRelease);
  if (n == "SUPPORTED_ABIS") {
    Obj abis;
    abis.cls = "[obj";
    abis.elems = {new_string("arm64-v8a"), new_string("armeabi-v7a")};
    abis.len = (u32)abis.elems.size();
    return new_obj(abis);
  }
  if (f.sig == "Ljava/lang/String;")
    return new_string(string_constant(n));

  // Other object fields: one stable instance per (object, field), so repeated reads agree.
  if (f.sig[0] == 'L') {
    static std::map<std::pair<u32, u32>, u32> cache;
    u32& slot = cache[{self, (u32)(&f - &g_fields[0])}];
    if (!slot)
      slot = new_instance(class_of_signature(f.sig));
    return slot;
  }
  return 0;
}
