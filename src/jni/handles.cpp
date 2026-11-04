// Guest-visible JNI handles. The guest is 32-bit, so jobject/jmethodID/jfieldID are replaced by small integers.
#include "jni/handles.h"

#include <deque>
#include <mutex>
#include <vector>

JavaVM* g_vm;
u32 g_genv, g_gvm;

JNIEnv* env() {
  static thread_local JNIEnv* jni;
  if (jni)
    return jni;
  if (g_vm->GetEnv((void**)&jni, JNI_VERSION_1_6) != JNI_OK)
    g_vm->AttachCurrentThread(&jni, nullptr);
  return jni;
}

// ------------------------------------------------------------------ object references (see handles.h)

static std::vector<Ref> g_refs;
static std::deque<u32> g_free_refs;
static std::mutex g_ref_mu;
thread_local std::vector<u32> t_locals;

u32 href(jobject object, int kind) {
  if (!object)
    return 0;
  std::lock_guard<std::mutex> lock(g_ref_mu);
  u32 index;
  if (g_free_refs.size() > REUSE_DELAY) {
    index = g_free_refs.front();
    g_free_refs.pop_front();
    g_refs[index] = {object, kind};
  } else {
    index = (u32)g_refs.size();
    g_refs.push_back({object, kind});
  }
  u32 handle = REF_BASE + 4 * index;
  if (kind != REF_GLOBAL)
    t_locals.push_back(handle);
  return handle;
}

jobject hobj(u32 handle) {
  if (handle < REF_BASE)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_ref_mu);
  u32 index = (handle - REF_BASE) / 4;
  return index < g_refs.size() ? g_refs[index].o : nullptr;
}

void hfree(u32 handle) {
  if (handle < REF_BASE)
    return;
  std::lock_guard<std::mutex> lock(g_ref_mu);
  u32 index = (handle - REF_BASE) / 4;
  if (index < g_refs.size() && g_refs[index].o) {
    g_refs[index] = {nullptr, 0};
    g_free_refs.push_back(index);
  }
}

void frame_end(size_t mark) {
  while (t_locals.size() > mark) {
    hfree(t_locals.back());
    t_locals.pop_back();
  }
}

// ------------------------------------------------------------------ method and field ids
//
// Id 0 is a dummy so that a failed lookup (returned to the guest as 0) never resolves to a real method.
static std::vector<MId> g_mids(1);
static std::vector<FId> g_fids(1);
static std::mutex g_id_mu;

u32 mid_new(jmethodID method, const char* sig, bool is_static, const char* name) {
  if (!method)
    return 0;
  std::lock_guard<std::mutex> lock(g_id_mu);
  g_mids.push_back({method, sig, is_static, name});
  return (u32)g_mids.size() - 1;
}

MId mid(u32 id) {
  std::lock_guard<std::mutex> lock(g_id_mu);
  return g_mids[id < g_mids.size() ? id : 0];
}

u32 fid_new(jfieldID field, char type) {
  if (!field)
    return 0;
  std::lock_guard<std::mutex> lock(g_id_mu);
  g_fids.push_back({field, type});
  return (u32)g_fids.size() - 1;
}

FId fid(u32 id) {
  std::lock_guard<std::mutex> lock(g_id_mu);
  return g_fids[id < g_fids.size() ? id : 0];
}

void jni_return_zero(Ctx& ctx, const Stub&) {
  ctx.ret(0);
}
