// Empty arm64 library installed under the names of the 32-bit mod libraries (libczProject.so, ...): the game's
// System.loadLibrary calls succeed on 64-bit devices, where the bridge itself runs the real 32-bit libraries.
#include <jni.h>

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)vm;
  (void)reserved;
  return JNI_VERSION_1_6;
}
