// JNI exports that the Java VM resolves by *name* (the game's other native methods go through RegisterNatives and
// need no entry here). Each one forwards to the same-named function in the guest library.
//
// This list is specific to the game being ported; add or remove entries to match the Java `native` declarations
// that the game does not register explicitly.
#include "jni/handles.h"

static void forward(const char* symbol, jobject self, jlong value) {
  u32 fn = image_sym(g_main, symbol);
  if (!fn)
    return;
  size_t mark = t_locals.size();
  u32 args[5] = {g_genv, href(self, REF_BORROWED), 0, (u32)value, (u32)((u64)value >> 32)};  // (env, this, jlong)
  guest_callv(fn, args, 5, nullptr);
  frame_end(mark);
}

extern "C" {

JNIEXPORT void JNICALL Java_com_popcap_pcsp_marketing_draper_DraperHtmlActivity_notifyHtmlClicked(JNIEnv*, jobject self,
                                                                                                  jlong v) {
  forward("Java_com_popcap_pcsp_marketing_draper_DraperHtmlActivity_notifyHtmlClicked", self, v);
}

JNIEXPORT void JNICALL Java_com_popcap_pcsp_marketing_draper_DraperHtmlActivity_notifyHtmlClosed(JNIEnv*, jobject self,
                                                                                                 jlong v) {
  forward("Java_com_popcap_pcsp_marketing_draper_DraperHtmlActivity_notifyHtmlClosed", self, v);
}

}  // extern "C"
