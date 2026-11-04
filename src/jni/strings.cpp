// Strings. Guest copies of string contents live on the guest heap and are freed on release.
#include <string.h>

#include <string>

#include "jni/handles.h"

static void new_string_utf(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->NewStringUTF((const char*)ctx.p(1)), 1));
}

static void get_string_utf_length(Ctx& ctx, const Stub&) {
  jobject string = hobj(ctx.a(1));
  ctx.ret(string ? env()->GetStringUTFLength((jstring)string) : 0);
}

static void get_string_length(Ctx& ctx, const Stub&) {
  jobject string = hobj(ctx.a(1));
  ctx.ret(string ? env()->GetStringLength((jstring)string) : 0);
}

static void get_string_utf_chars(Ctx& ctx, const Stub&) {
  jstring string = (jstring)hobj(ctx.a(1));
  // shortcut: null/stale jstring (failed Java call) -> NULL instead of an ART abort
  if (!string) {
    LOG("GetStringUTFChars(null) handle=%08x after %s", ctx.a(1), t_last_call.c_str());
    ctx.ret(0);
    return;
  }
  const char* chars = env()->GetStringUTFChars(string, nullptr);
  u32 size = strlen(chars) + 1, guest_copy = gmalloc(size);
  memcpy(H(guest_copy), chars, size);
  env()->ReleaseStringUTFChars(string, chars);
  if (ctx.a(2))
    *(uint8_t*)ctx.p(2) = 1;
  ctx.ret(guest_copy);
}

static void release_string_chars(Ctx& ctx, const Stub&) {
  gfree(ctx.a(2));
}

static void get_string_chars(Ctx& ctx, const Stub&) {
  jstring string = (jstring)hobj(ctx.a(1));
  jsize size = env()->GetStringLength(string);
  u32 guest_copy = gmalloc(2 * size + 2);
  env()->GetStringRegion(string, 0, size, (jchar*)H(guest_copy));
  ((jchar*)H(guest_copy))[size] = 0;
  if (ctx.a(2))
    *(uint8_t*)ctx.p(2) = 1;
  ctx.ret(guest_copy);
}

static void new_string(Ctx& ctx, const Stub&) {
  ctx.ret(href(env()->NewString((const jchar*)ctx.p(1), ctx.a(2)), 1));
}

static void get_string_region(Ctx& ctx, const Stub&) {
  env()->GetStringRegion((jstring)hobj(ctx.a(1)), ctx.a(2), ctx.a(3), (jchar*)ctx.p(4));
}

static void get_string_utf_region(Ctx& ctx, const Stub&) {
  env()->GetStringUTFRegion((jstring)hobj(ctx.a(1)), ctx.a(2), ctx.a(3), (char*)ctx.p(4));
}

void register_string_imports(u32* table) {
  jni_set(table, 163, "NewString", new_string);
  jni_set(table, 164, "GetStringLength", get_string_length);
  jni_set(table, 165, "GetStringChars", get_string_chars);
  jni_set(table, 166, "ReleaseStringChars", release_string_chars);
  jni_set(table, 167, "NewStringUTF", new_string_utf);
  jni_set(table, 168, "GetStringUTFLength", get_string_utf_length);
  jni_set(table, 169, "GetStringUTFChars", get_string_utf_chars);
  jni_set(table, 170, "ReleaseStringUTFChars", release_string_chars);
  jni_set(table, 220, "GetStringRegion", get_string_region);
  jni_set(table, 221, "GetStringUTFRegion", get_string_utf_region);
  jni_set(table, 224, "GetStringCritical", get_string_chars);
  jni_set(table, 225, "ReleaseStringCritical", release_string_chars);
}
