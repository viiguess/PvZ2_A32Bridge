// The guest-visible JNIEnv / JavaVM, implemented on top of the fake Java world (fake_java.h).
//
// Every JNI call is logged to stderr as "[jni] ..."; calls that are not implemented are logged as UNIMPLEMENTED and
// return 0, which is how new requirements of a packer are discovered.
#pragma once

#include <string>
#include <vector>

#include "common.h"

struct RegisteredNative {
  std::string cls, name, sig;  // e.g. "com/stub/StubApp", "interface5", "(Landroid/app/Application;)V"
  u32 fn;                      // guest address of the implementation
};

void fake_jni_init();                                       // builds the JNIEnv and JavaVM tables in guest memory
u32 fake_jni_vm();                                          // guest JavaVM*
u32 fake_jni_env();                                         // guest JNIEnv*
const std::vector<RegisteredNative>& registered_natives();  // everything the guest passed to RegisterNatives
