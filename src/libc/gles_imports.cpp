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
                               {"glCompressedTexImage2D", "uiuiiiio>v"},
                               // shared by ES 1.1 and 2.0, and the rest of 2.0 (games that resolve every entry point)
                               {"glBindBuffer", "uu>v"},
                               {"glBufferData", "uuou>v"},
                               {"glBufferSubData", "uuuo>v"},
                               {"glDeleteBuffers", "ip>v"},
                               {"glGenBuffers", "ip>v"},
                               {"glIsBuffer", "u>i"},
                               {"glGetBufferParameteriv", "uup>v"},
                               {"glBlendColor", "ffff>v"},
                               {"glBlendEquation", "u>v"},
                               {"glBlendEquationSeparate", "uu>v"},
                               {"glBlendFuncSeparate", "uuuu>v"},
                               {"glClearStencil", "i>v"},
                               {"glCompressedTexSubImage2D", "uiiiiiuio>v"},
                               {"glCopyTexImage2D", "uiuiiiii>v"},
                               {"glCopyTexSubImage2D", "uiiiiiii>v"},
                               {"glFinish", "v>v"},
                               {"glFlush", "v>v"},
                               {"glGetBooleanv", "up>v"},
                               {"glGetFloatv", "up>v"},
                               {"glGetTexParameterfv", "uup>v"},
                               {"glGetTexParameteriv", "uup>v"},
                               {"glHint", "uu>v"},
                               {"glIsEnabled", "u>i"},
                               {"glPolygonOffset", "ff>v"},
                               {"glReadPixels", "iiiiuup>v"},
                               {"glSampleCoverage", "fi>v"},
                               {"glStencilFunc", "uiu>v"},
                               {"glStencilMask", "u>v"},
                               {"glStencilOp", "uuu>v"},
                               {"glStencilFuncSeparate", "uuiu>v"},
                               {"glStencilMaskSeparate", "uu>v"},
                               {"glStencilOpSeparate", "uuuu>v"},
                               {"glTexParameterf", "uuf>v"},
                               {"glTexParameterfv", "uup>v"},
                               {"glTexParameteriv", "uup>v"},
                               {"glGenerateMipmap", "u>v"},
                               {"glBindRenderbuffer", "uu>v"},
                               {"glDeleteRenderbuffers", "ip>v"},
                               {"glGenRenderbuffers", "ip>v"},
                               {"glRenderbufferStorage", "uuii>v"},
                               {"glFramebufferRenderbuffer", "uuuu>v"},
                               {"glIsFramebuffer", "u>i"},
                               {"glIsRenderbuffer", "u>i"},
                               {"glGetRenderbufferParameteriv", "uup>v"},
                               {"glGetFramebufferAttachmentParameteriv", "uuup>v"},
                               {"glDetachShader", "uu>v"},
                               {"glValidateProgram", "u>v"},
                               {"glGetAttribLocation", "up>i"},
                               {"glGetActiveAttrib", "uuipppp>v"},
                               {"glGetActiveUniform", "uuipppp>v"},
                               {"glGetAttachedShaders", "uipp>v"},
                               {"glGetShaderPrecisionFormat", "uupp>v"},
                               {"glGetShaderSource", "uipp>v"},
                               {"glGetUniformfv", "uip>v"},
                               {"glGetUniformiv", "uip>v"},
                               {"glGetVertexAttribfv", "uup>v"},
                               {"glGetVertexAttribiv", "uup>v"},
                               {"glReleaseShaderCompiler", "v>v"},
                               {"glShaderBinary", "ipupi>v"},
                               {"glUniform1f", "if>v"},
                               {"glUniform2f", "iff>v"},
                               {"glUniform3f", "ifff>v"},
                               {"glUniform4f", "iffff>v"},
                               {"glUniform2i", "iii>v"},
                               {"glUniform3i", "iiii>v"},
                               {"glUniform4i", "iiiii>v"},
                               {"glUniform1fv", "iip>v"},
                               {"glUniform2fv", "iip>v"},
                               {"glUniform3fv", "iip>v"},
                               {"glUniform1iv", "iip>v"},
                               {"glUniform2iv", "iip>v"},
                               {"glUniform3iv", "iip>v"},
                               {"glUniform4iv", "iip>v"},
                               {"glUniformMatrix2fv", "iiip>v"},
                               {"glUniformMatrix3fv", "iiip>v"},
                               {"glVertexAttrib1f", "uf>v"},
                               {"glVertexAttrib2f", "uff>v"},
                               {"glVertexAttrib3f", "ufff>v"},
                               {"glVertexAttrib4f", "uffff>v"},
                               {"glVertexAttrib1fv", "up>v"},
                               {"glVertexAttrib2fv", "up>v"},
                               {"glVertexAttrib3fv", "up>v"},
                               {"glVertexAttrib4fv", "up>v"}};
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
                               {"glVertexPointer", "iuio>v"},
                               {"glAlphaFuncx", "ui>v"},
                               {"glClearColorx", "iiii>v"},
                               {"glClearDepthx", "i>v"},
                               {"glClipPlanef", "up>v"},
                               {"glClipPlanex", "up>v"},
                               {"glColor4f", "ffff>v"},
                               {"glColor4ub", "uuuu>v"},
                               {"glColor4x", "iiii>v"},
                               {"glDepthRangex", "ii>v"},
                               {"glFogf", "uf>v"},
                               {"glFogfv", "up>v"},
                               {"glFogx", "ui>v"},
                               {"glFogxv", "up>v"},
                               {"glFrustumf", "ffffff>v"},
                               {"glFrustumx", "iiiiii>v"},
                               {"glGetClipPlanef", "up>v"},
                               {"glGetClipPlanex", "up>v"},
                               {"glGetFixedv", "up>v"},
                               {"glGetLightfv", "uup>v"},
                               {"glGetLightxv", "uup>v"},
                               {"glGetMaterialfv", "uup>v"},
                               {"glGetMaterialxv", "uup>v"},
                               {"glGetTexEnvfv", "uup>v"},
                               {"glGetTexEnviv", "uup>v"},
                               {"glGetTexEnvxv", "uup>v"},
                               {"glGetTexParameterxv", "uup>v"},
                               {"glLightModelf", "uf>v"},
                               {"glLightModelfv", "up>v"},
                               {"glLightModelx", "ui>v"},
                               {"glLightModelxv", "up>v"},
                               {"glLightf", "uuf>v"},
                               {"glLightfv", "uup>v"},
                               {"glLightx", "uui>v"},
                               {"glLightxv", "uup>v"},
                               {"glLineWidthx", "i>v"},
                               {"glLoadMatrixx", "p>v"},
                               {"glLogicOp", "u>v"},
                               {"glMaterialf", "uuf>v"},
                               {"glMaterialfv", "uup>v"},
                               {"glMaterialx", "uui>v"},
                               {"glMaterialxv", "uup>v"},
                               {"glMultiTexCoord4f", "uffff>v"},
                               {"glMultiTexCoord4x", "uiiii>v"},
                               {"glMultMatrixf", "p>v"},
                               {"glMultMatrixx", "p>v"},
                               {"glNormal3f", "fff>v"},
                               {"glNormal3x", "iii>v"},
                               {"glOrthof", "ffffff>v"},
                               {"glOrthox", "iiiiii>v"},
                               {"glPointParameterf", "uf>v"},
                               {"glPointParameterfv", "up>v"},
                               {"glPointParameterx", "ui>v"},
                               {"glPointParameterxv", "up>v"},
                               {"glPointSize", "f>v"},
                               {"glPointSizePointerOES", "uio>v"},
                               {"glPointSizex", "i>v"},
                               {"glPolygonOffsetx", "ii>v"},
                               {"glRotatef", "ffff>v"},
                               {"glRotatex", "iiii>v"},
                               {"glSampleCoveragex", "ii>v"},
                               {"glScalex", "iii>v"},
                               {"glTexEnvfv", "uup>v"},
                               {"glTexEnvi", "uui>v"},
                               {"glTexEnviv", "uup>v"},
                               {"glTexEnvx", "uui>v"},
                               {"glTexEnvxv", "uup>v"},
                               {"glTexParameterx", "uui>v"},
                               {"glTexParameterxv", "uup>v"},
                               {"glTranslatef", "fff>v"},
                               {"glTranslatex", "iii>v"}};
  for (const GlFn& fn_desc : gles1)
    gen(fn_desc.name, fn_desc.sig, "libGLESv1_CM.so");

  hle("glShaderSource", import_glShaderSource);
  hle("glGetString", import_glGetString);
}
