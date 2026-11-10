// A tiny fake Java world for the runner: object handles, classes, method/field ids, and canned answers for the
// Android APIs that packers query (package info, Build.*, the APK path, ...).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "common.h"

struct Config {
  std::string lib_path;  // the library being run
  std::string apk_path;  // the APK it believes it is part of (ApplicationInfo.sourceDir)
  std::string work_dir;  // stands in for the app's data directory
  std::string package = "com.popcap.pvz2cthdlx";
  std::string version_name = "2.2.7";
  int version_code = 891;
};

extern Config g_cfg;

// ---- objects ----------------------------------------------------------------------------------------------------
// A guest handle is REF_BASE + 4 * index into the object table.
enum : u32 { REF_BASE = 0x90000000 };

struct Obj {
  std::string cls;  // JNI class name, e.g. "java/lang/String"
  bool is_class = false;
  bool is_string = false;
  std::string str;
  std::vector<u32> elems;  // object arrays
  u32 buf = 0;             // primitive arrays: guest buffer
  u32 len = 0;
  int elem_size = 0;
  std::string data;  // InputStream: the bytes it serves (a JarFile entry of the APK)
  size_t pos = 0;    // InputStream: read position
  u32 wrapped = 0;   // BufferedInputStream & co: the stream it wraps
};

u32 new_obj(const Obj& obj);
Obj* obj_of(u32 handle);  // nullptr for null / invalid handles
u32 class_handle(const std::string& name);
// A class a stock device does not have (Xposed, Substrate...): lookups throw ClassNotFoundException.
bool hidden_class(const std::string& name);
extern bool g_java_exception;  // a (fake) exception is pending
u32 new_string(const std::string& text);
u32 new_instance(const std::string& cls);
std::string describe(char kind, uint64_t value);  // for logging: a JNI value as text

// ---- method and field ids ----------------------------------------------------------------------------------------
struct MethodId {
  std::string cls, name, sig;
  bool is_static;
};

struct FieldId {
  std::string cls, name, sig;
  bool is_static;
};

u32 method_id_new(const MethodId& id);
const MethodId* method_id(u32 id);
u32 field_id_new(const FieldId& id);
const FieldId* field_id(u32 id);

// Splits a method signature into argument type letters ('L' for objects and arrays) and the return type letter.
std::vector<char> signature_args(const std::string& sig, char& ret);

// ---- behaviour --------------------------------------------------------------------------------------------------
// result of invoking `method` on `self`; `args` are the raw argument values (handles for objects)
uint64_t java_call(const MethodId& method, u32 self, const std::vector<uint64_t>& args);
// called after `new cls(args)`: streams remember what they wrap, a JarFile remembers its path
void java_constructed(const MethodId& ctor, u32 object, const std::vector<uint64_t>& args);
uint64_t java_get_field(const FieldId& field, u32 self);  // value of a field (statics ignore `self`)

void java_prefetch();
std::string apk_entry_bytes(const std::string& entry);
