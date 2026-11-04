// OpenGL ES: calls are forwarded to the host drivers; a few take pointer-to-pointer arguments and need conversion.
#include "libc/gles_imports.h"

#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "libc/forward.h"

// ------------------------------------------------------------------ GL special cases
static void import_glShaderSource(Ctx& ctx, const Stub&) {
  typedef void (*Fn)(GLuint, GLsizei, const char* const*, const GLint*);
  static Fn fn = (Fn)dlsym(dlopen("libGLESv2.so", RTLD_NOW), "glShaderSource");
  int count = ctx.a(1);
  std::vector<const char*> sources(count);
  u32* arr = (u32*)ctx.p(2);
  for (int i = 0; i < count; i++)
    sources[i] = (const char*)H(arr[i]);
  fn(ctx.a(0), count, sources.data(), (const GLint*)ctx.p(3));
}

static void import_glGetString(Ctx& ctx, const Stub&) {
  typedef const unsigned char* (*Fn)(unsigned);
  static Fn fn = (Fn)dlsym(dlopen("libGLESv2.so", RTLD_NOW), "glGetString");
  const char* text = (const char*)fn(ctx.a(0));
  ctx.ret(text ? gstr(text) : 0);
}

void register_gles_imports() {
  struct GlFn {
    const char* name;
    const char* sig;
  };

  // OpenGL ES 2.0
  static const GlFn gles2[] = {{"glIsTexture", "u>i"},
                               {"glIsShader", "u>i"},
                               {"glIsProgram", "u>i"},
                               {"glTexParameteri", "uii>v"},
                               {"glPixelStorei", "ii>v"},
                               {"glBindTexture", "uu>v"},
                               {"glTexImage2D", "uiiiiiuuo>v"},
                               {"glTexSubImage2D", "uiiiiiuuo>v"},
                               {"glGenTextures", "ip>v"},
                               {"glGetIntegerv", "up>v"},
                               {"glDisable", "u>v"},
                               {"glEnable", "u>v"},
                               {"glDeleteProgram", "u>v"},
                               {"glCreateProgram", "v>u"},
                               {"glAttachShader", "uu>v"},
                               {"glBindAttribLocation", "uup>v"},
                               {"glGetUniformLocation", "up>i"},
                               {"glCreateShader", "u>u"},
                               {"glCompileShader", "u>v"},
                               {"glGetShaderiv", "uup>v"},
                               {"glGetShaderInfoLog", "uipp>v"},
                               {"glDeleteShader", "u>v"},
                               {"glLinkProgram", "u>v"},
                               {"glGetProgramiv", "uup>v"},
                               {"glGetProgramInfoLog", "uipp>v"},
                               {"glUniformMatrix4fv", "iiip>v"},
                               {"glUniform4fv", "iip>v"},
                               {"glUniform1i", "ii>v"},
                               {"glActiveTexture", "u>v"},
                               {"glDrawArrays", "uii>v"},
                               {"glClearColor", "ffff>v"},
                               {"glClear", "u>v"},
                               {"glClearDepthf", "f>v"},
                               {"glGenFramebuffers", "ip>v"},
                               {"glBindFramebuffer", "uu>v"},
                               {"glFramebufferTexture2D", "uuuui>v"},
                               {"glCheckFramebufferStatus", "u>u"},
                               {"glGetError", "v>u"},
                               {"glDeleteTextures", "ip>v"},
                               {"glDeleteFramebuffers", "ip>v"},
                               {"glVertexAttribPointer", "uiuiio>v"},
                               {"glEnableVertexAttribArray", "u>v"},
                               {"glDisableVertexAttribArray", "u>v"},
                               {"glLineWidth", "f>v"},
                               {"glBlendFunc", "uu>v"},
                               {"glFrontFace", "u>v"},
                               {"glUseProgram", "u>v"},
                               {"glColorMask", "iiii>v"},
                               {"glDepthMask", "i>v"},
                               {"glDepthFunc", "u>v"},
                               {"glCullFace", "u>v"},
                               {"glViewport", "iiii>v"},
                               {"glDepthRangef", "ff>v"},
                               {"glScissor", "iiii>v"},
                               {"glCompressedTexImage2D", "uiuiiiio>v"}};
  for (const GlFn& fn_desc : gles2)
    gen(fn_desc.name, fn_desc.sig, "libGLESv2.so");

  // OpenGL ES 1.x fixed function (older games); only present in libGLESv1_CM
  static const GlFn gles1[] = {{"glAlphaFunc", "uf>v"},
                               {"glBindFramebufferOES", "uu>v"},
                               {"glCheckFramebufferStatusOES", "u>u"},
                               {"glClientActiveTexture", "u>v"},
                               {"glColorPointer", "iuio>v"},
                               {"glDeleteFramebuffersOES", "ip>v"},
                               {"glDisableClientState", "u>v"},
                               {"glDrawElements", "uiuo>v"},
                               {"glEnableClientState", "u>v"},
                               {"glFramebufferTexture2DOES", "uuuui>v"},
                               {"glGenFramebuffersOES", "ip>v"},
                               {"glLoadIdentity", "v>v"},
                               {"glLoadMatrixf", "p>v"},
                               {"glMatrixMode", "u>v"},
                               {"glNormalPointer", "uio>v"},
                               {"glPopMatrix", "v>v"},
                               {"glPushMatrix", "v>v"},
                               {"glScalef", "fff>v"},
                               {"glShadeModel", "u>v"},
                               {"glTexCoordPointer", "iuio>v"},
                               {"glTexEnvf", "uuf>v"},
                               {"glVertexPointer", "iuio>v"}};
  for (const GlFn& fn_desc : gles1)
    gen(fn_desc.name, fn_desc.sig, "libGLESv1_CM.so");

  hle("glShaderSource", import_glShaderSource);
  hle("glGetString", import_glGetString);
}
