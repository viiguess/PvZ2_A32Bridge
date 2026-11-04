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
};

u32 new_obj(const Obj& obj);
Obj* obj_of(u32 handle);  // nullptr for null / invalid handles
u32 class_handle(const std::string& name);
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
uint64_t java_call(const MethodId& method, u32 self);     // result of invoking `method` on `self`
uint64_t java_get_field(const FieldId& field, u32 self);  // value of a field (statics ignore `self`)
