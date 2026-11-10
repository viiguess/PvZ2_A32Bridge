#include "fake_java.h"
#include "runtime/runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

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
  // android.os.Build.*: a real-looking Pixel 5 (packers treat "unknown" fingerprints/hardware as an emulator)
  static const std::map<std::string, std::string> kBuild = {
      {"FINGERPRINT", "google/redfin/redfin:14/UP1A.231105.001/10817346:user/release-keys"},
      {"HARDWARE", "redfin"}, {"PRODUCT", "redfin"}, {"BOARD", "redfin"}, {"DEVICE", "redfin"},
      {"TAGS", "release-keys"}, {"TYPE", "user"}, {"HOST", "abfarm-release-rbe-64-00123"}, {"ID", "UP1A.231105.001"},
      {"USER", "android-build"}, {"DISPLAY", "UP1A.231105.001"}, {"BOOTLOADER", "slider-1.0-10800000"},
      {"SECURITY_PATCH", "2023-11-05"}, {"RADIO", "g7250-00200-230902-B-10800000"}, {"INCREMENTAL", "10817346"}, {"CODENAME", "REL"},
  };
  auto build = kBuild.find(name);
  if (build != kBuild.end())
    return build->second;
  if (!ends_with(name, "_SERVICE") || name.size() == 8)
    return "unknown";
  std::string value = name.substr(0, name.size() - 8);
  for (char& c : value)
    c = (char)tolower((unsigned char)c);
  return value;
}

}  // namespace

namespace {

// Bytes of one entry of the APK (the packer checks the APK's own AndroidManifest.xml and signature files).
std::string read_apk_entry(const std::string& apk, const std::string& entry) {
  std::string command = "python3 -c 'import sys,zipfile;sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' '" +
                        apk + "' '" + entry + "' 2>/dev/null";
  std::string data;
  if (FILE* pipe = popen(command.c_str(), "r")) {
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, pipe)) > 0)
      data.append(chunk, got);
    pclose(pipe);
  }
  return data;
}

// The stream that actually holds the bytes: BufferedInputStream & co. only wrap another stream.
Obj* stream_of(u32 handle) {
  for (int depth = 0; depth < 8; depth++) {
    Obj* o = obj_of(handle);
    if (!o || !o->wrapped)
      return o;
    handle = o->wrapped;
  }
  return obj_of(handle);
}

// DER of the APK's signing certificate (apk_cert.py), which PackageInfo.signatures[0] stands for.
const std::string& apk_certificate() {
  static std::string der;
  static bool loaded = false;
  if (loaded)
    return der;
  loaded = true;
  std::string here = __FILE__;
  std::string command = "python3 '" + here.substr(0, here.rfind('/')) + "/apk_cert.py' '" + g_cfg.apk_path + "' 2>/dev/null";
  std::string hex;
  if (FILE* pipe = popen(command.c_str(), "r")) {
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, pipe)) > 0)
      hex.append(chunk, got);
    pclose(pipe);
  }
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    if (isxdigit((unsigned char)hex[i]) && isxdigit((unsigned char)hex[i + 1]))
      der.push_back((char)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
  fprintf(stderr, "[run] signing certificate: %zu bytes\n", der.size());
  return der;
}

std::string apk_subject() {
  static std::string subject;
  static bool loaded = false;
  if (loaded)
    return subject;
  loaded = true;
  std::string here = __FILE__;
  std::string command = "python3 '" + here.substr(0, here.rfind('/')) + "/apk_cert.py' '" + g_cfg.apk_path + "' --subject 2>/dev/null";
  if (FILE* pipe = popen(command.c_str(), "r")) {
    char chunk[1024];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, pipe)) > 0)
      subject.append(chunk, got);
    pclose(pipe);
  }
  while (!subject.empty() && (subject.back() == 10 || subject.back() == 13))
    subject.pop_back();
  fprintf(stderr, "[run] signing certificate subject: %s\n", subject.c_str());
  return subject;
}

bool is_stream_read(const MethodId& m) {
  return m.name == "read" && (m.cls.find("InputStream") != std::string::npos || m.cls.find("nl/") == 0);
}

}  // namespace

// The certificate comes from a child process: fetch it before the guest runs, not in the middle of a JNI call.
void java_prefetch() {
  apk_certificate();
  apk_subject();
}

std::string apk_entry_bytes(const std::string& entry) {
  return read_apk_entry(g_cfg.apk_path, entry);
}

bool g_java_exception = false;

bool hidden_class(const std::string& name) {
  std::string lower = name;
  for (char& c : lower)
    c = (char)tolower((unsigned char)c);
  return lower.find("xposed") != std::string::npos || lower.find("substrate") != std::string::npos ||
         lower.find("frida") != std::string::npos;
}

void java_constructed(const MethodId& ctor, u32 object, const std::vector<uint64_t>& args) {
  Obj* o = obj_of(object);
  if (!o)
    return;
  if (ctor.cls == "java/lang/String" && ctor.sig.rfind("([B", 0) == 0 && !args.empty()) {
    if (Obj* b = obj_of((u32)args[0])) {
      if (b->cls == "[prim" && b->elem_size == 1) {
        o->str.assign((const char*)H(b->buf), b->len);
        fprintf(stderr, "[jni] String(byte[]) = \"%s\"\n", o->str.c_str());
      }
    }
  } else if (ctor.cls == "java/util/jar/JarFile" || ctor.cls == "java/util/zip/ZipFile") {
    if (!args.empty())
      if (Obj* path = obj_of((u32)args[0]))
        o->str = path->str;
  } else if (ctor.sig.find("Ljava/io/InputStream;") != std::string::npos && !args.empty()) {
    o->wrapped = (u32)args[0];
  }
}

uint64_t java_call(const MethodId& m, u32 self, const std::vector<uint64_t>& a) {
  const std::string& n = m.name;
  char ret;
  signature_args(m.sig, ret);

  if (n == "hashCode" && m.cls == "java/util/Arrays" && !a.empty()) {  // Arrays.hashCode(byte[])
    if (Obj* b = obj_of((u32)a[0])) {
      if (b->cls == "[prim" && b->elem_size == 1) {
        int32_t hash = 1;
        for (u32 i = 0; i < b->len; i++)
          hash = 31 * hash + (int8_t)*(const char*)H(b->buf + i);
        return (uint32_t)hash;
      }
    }
    return 0;
  }
  // android.content.pm.Signature: the APK's real certificate (the packer hashes it and compares with the original's).
  if (m.cls == "android/content/pm/Signature") {
    const std::string& der = apk_certificate();
    if (n == "hashCode") {  // Arrays.hashCode(byte[])
      int32_t hash = 1;
      for (char c : der)
        hash = 31 * hash + (int8_t)c;
      return (uint32_t)hash;
    }
    if (n == "toByteArray") {
      Obj bytes;
      bytes.cls = "[prim";
      bytes.len = (u32)der.size();
      bytes.elem_size = 1;
      bytes.buf = gmalloc(bytes.len + 8);
      memcpy(H(bytes.buf), der.data(), bytes.len);
      return new_obj(bytes);
    }
    if (n == "toCharsString") {
      std::string hex;
      char two[3];
      for (char c : der) {
        snprintf(two, sizeof two, "%02x", (uint8_t)c);
        hex += two;
      }
      return new_string(hex);
    }
  }
  // CertificateFactory.generateCertificate(new ByteArrayInputStream(sig)).getEncoded(): same DER back.
  if (n == "getEncoded" && m.cls.find("Certificate") != std::string::npos) {
    const std::string& der = apk_certificate();
    Obj bytes;
    bytes.cls = "[prim";
    bytes.len = (u32)der.size();
    bytes.elem_size = 1;
    bytes.buf = gmalloc(bytes.len + 8);
    memcpy(H(bytes.buf), der.data(), bytes.len);
    return new_obj(bytes);
  }
  if (n == "toHexString" && m.cls == "java/lang/Long" && !a.empty()) {
    char text[32];
    snprintf(text, sizeof text, "%llx", (unsigned long long)a[0]);
    return new_string(text);
  }

  // JarFile / InputStream: serve the real bytes of the APK's entries, then end-of-file.
  if ((n == "getJarEntry" || n == "getEntry") && !a.empty()) {
    u32 entry = new_instance("java/util/jar/JarEntry");
    if (Obj* e = obj_of(entry))
      if (Obj* name = obj_of((u32)a[0])) {
        e->str = name->str;
        if (Obj* jar = obj_of(self))
          e->data = jar->str.empty() ? g_cfg.apk_path : jar->str;  // the archive the entry belongs to
      }
    return entry;
  }
  if (n == "getInputStream" && !a.empty()) {
    u32 stream = new_instance("java/io/InputStream");
    Obj* entry = obj_of((u32)a[0]);
    Obj* s = obj_of(stream);
    if (s && entry)
      s->data = read_apk_entry(entry->data.empty() ? g_cfg.apk_path : entry->data, entry->str);
    return stream;
  }
  if (is_stream_read(m)) {
    Obj* s = stream_of(self);
    if (!s)
      return (uint64_t)(uint32_t)-1;
    size_t left = s->data.size() - std::min(s->pos, s->data.size());
    if (m.sig == "()I")  // read(): one byte
      return left ? (uint8_t)s->data[s->pos++] : (uint64_t)(uint32_t)-1;
    Obj* buffer = a.empty() ? nullptr : obj_of((u32)a[0]);
    if (!buffer)
      return (uint64_t)(uint32_t)-1;
    u32 offset = m.sig == "([BII)I" && a.size() > 1 ? (u32)a[1] : 0;
    u32 want = m.sig == "([BII)I" && a.size() > 2 ? (u32)a[2] : buffer->len;
    size_t count = std::min<size_t>(std::min<size_t>(want, left), buffer->len > offset ? buffer->len - offset : 0);
    if (!left)
      return (uint64_t)(uint32_t)-1;
    memcpy((uint8_t*)H(buffer->buf) + offset, s->data.data() + s->pos, count);
    s->pos += count;
    return count;
  }
  if (n == "available" && m.sig == "()I") {
    Obj* s = stream_of(self);
    return s ? (uint64_t)(s->data.size() - std::min(s->pos, s->data.size())) : 0;
  }

  if (n == "isConnected" || n == "isAvailable" || n == "isConnectedOrConnecting")
    return 1;  // a packer that finds no network gives up (the real device is online)
  if (n == "encode" && m.cls == "java/net/URLEncoder" && !a.empty()) {  // "Pixel 5" -> "Pixel+5"
    Obj* text = obj_of((u32)a[0]);
    std::string out;
    for (char c : text ? text->str : "") {
      char hex[4];
      if (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '*' || c == '_')
        out += c;
      else if (c == ' ')
        out += '+';
      else {
        snprintf(hex, sizeof hex, "%%%02X", (uint8_t)c);
        out += hex;
      }
    }
    return new_string(out);
  }
  if (n == "getBSSID" || n == "getMacAddress")  // WifiInfo: Android 10+ hides the MAC behind this constant
    return new_string("02:00:00:00:00:00");
  if (n == "getSSID")
    return new_string("\"wifi\"");
  if (n == "getCertificates" && m.cls.find("JarEntry") != std::string::npos) {  // verified entry: the signer chain
    Obj certs;
    certs.cls = "[obj";
    certs.elems = {new_instance("java/security/cert/Certificate")};
    certs.len = 1;
    return new_obj(certs);
  }
  if (n == "getName" && m.cls == "java/security/Principal")
    return new_string(apk_subject());
  if (n == "getMsg" && m.cls == "com/jg/bh/BH")  // the packer's own message hook: no message
    return new_string("");
  if (n == "getTypeName")  // NetworkInfo: a null type name makes the packer show its error dialog and quit
    return new_string("WIFI");
  if (n == "getType" && m.sig == "()I")
    return 1;  // TYPE_WIFI
  if (n == "getPackageName" || n == "currentPackageName" || n == "currentProcessName")
    return new_string(g_cfg.package);  // the newest packers ask ActivityThread, and stall when it answers null
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
  if ((n == "findClass" || n == "loadClass" || n == "forName") && !a.empty()) {  // ClassLoader lookups: a real class handle
    Obj* name = obj_of((u32)a[0]);
    if (name && name->is_string) {
      std::string cls = name->str;
      for (char& c : cls)
        if (c == '.')
          c = '/';
      if (hidden_class(cls)) {
        g_java_exception = true;
        return 0;
      }
      return class_handle(cls);
    }
  }
  if (n == "getPackagesForUid") {  // only the app's own uid has a package
    if (a.empty() || (int32_t)a[0] != 10123)
      return 0;
    Obj names;
    names.cls = "[obj";
    names.elems = {new_string(g_cfg.package)};
    names.len = 1;
    return new_obj(names);
  }
  if (n == "myUid" || n == "getCallingUid")
    return 10123;
  if (n == "myPid" || n == "getCallingPid")
    return 4242;
  if (n == "getStackTrace") {  // Thread.getStackTrace(): an array (the packer walks it looking for hook frameworks)
    Obj frames;
    frames.cls = "[obj";
    frames.len = 0;
    return new_obj(frames);
  }
  if (n == "getLaunchIntentForPackage")  // no emulator/launcher packages are installed
    return 0;
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
  if (n == "signatures" && f.cls == "android/content/pm/PackageInfo") {  // GET_SIGNATURES: one Signature
    Obj signatures;
    signatures.cls = "[obj";
    signatures.elems = {new_instance("android/content/pm/Signature")};
    signatures.len = 1;
    return new_obj(signatures);
  }
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
  if (n == "uid")  // ApplicationInfo.uid: the same app uid getuid() reports (0 would mean root)
    return 10123;
  if (n == "densityDpi")
    return 440;
  if (n == "noncompatWidthPixels" || n == "widthPixels")
    return 1080;
  if (n == "noncompatHeightPixels" || n == "heightPixels")
    return 2340;
  if (n == "SDK_INT")
    return getenv("JR_SDK") ? (uint64_t)atoi(getenv("JR_SDK")) : kSdkInt;
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
