// The JNI bridge: a 32-bit JNIEnv/JavaVM facade for the guest, built from import stubs.
//
// Library entry point (JNI_OnLoad): reserve the guest address space, register the HLE modules, load the original
// ARM library (lib<name>_a32.so, next to this one), run its constructors, then forward JNI_OnLoad to it.
#pragma once

#include "common.h"

void jni_init(void* java_vm);  // builds the guest JNIEnv and JavaVM
u32 jni_guest_vm();            // guest-visible JavaVM*

// Each module fills in the JNIEnv function-table slots it implements (see jni/handles.h: jni_set).
void register_call_imports(u32* table);    // Call<Type>Method, NewObject, Get/Set<Type>Field
void register_object_imports(u32* table);  // classes, references, exceptions, method/field ids
void register_string_imports(u32* table);
void register_array_imports(u32* table);   // arrays and direct buffers
void register_native_imports(u32* table);  // RegisterNatives
void register_vm_imports(u32* table);      // GetJavaVM
u32 make_vm_table();                       // vtable of the guest JavaVM
