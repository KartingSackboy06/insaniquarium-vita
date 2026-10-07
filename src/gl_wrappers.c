/*
 * OpenGL ES entry points are fetched by the game through SDL_GL_GetProcAddress.
 * Functions that take float arguments need soft-float ("aapcs") wrappers;
 * everything else is returned straight from vitaGL.
 */
#include <string.h>
#include "common.h"

#if defined(__arm__)
#define PCS __attribute__((pcs("aapcs")))
#else
#define PCS
#endif

extern void *SDL_GL_GetProcAddress(const char *name);

/* host (hard-float) vitaGL prototypes */
extern void glClearColor(float, float, float, float);
extern void glBlendColor(float, float, float, float) __attribute__((weak));   /* not in vitaGL */
extern void glClearDepthf(float);
extern void glDepthRangef(float, float);
extern void glLineWidth(float);
extern void glPolygonOffset(float, float);
extern void glSampleCoverage(float, unsigned char) __attribute__((weak)); /* not in vitaGL */
extern void glTexParameterf(unsigned, unsigned, float);
extern void glUniform1f(int, float);
extern void glUniform2f(int, float, float);
extern void glUniform3f(int, float, float, float);
extern void glUniform4f(int, float, float, float, float);
extern void glVertexAttrib1f(unsigned, float);
extern void glVertexAttrib2f(unsigned, float, float);
extern void glVertexAttrib3f(unsigned, float, float, float);
extern void glVertexAttrib4f(unsigned, float, float, float, float);

PCS static void w_glClearColor(float a, float b, float c, float d) { glClearColor(a, b, c, d); }
PCS static void w_glBlendColor(float a, float b, float c, float d) { if (glBlendColor) glBlendColor(a, b, c, d); }
PCS static void w_glClearDepthf(float a) { glClearDepthf(a); }
PCS static void w_glDepthRangef(float a, float b) { glDepthRangef(a, b); }
PCS static void w_glLineWidth(float a) { glLineWidth(a); }
PCS static void w_glPolygonOffset(float a, float b) { glPolygonOffset(a, b); }
PCS static void w_glSampleCoverage(float a, unsigned char b) { if (glSampleCoverage) glSampleCoverage(a, b); }
PCS static void w_glTexParameterf(unsigned t, unsigned p, float v) { glTexParameterf(t, p, v); }
PCS static void w_glUniform1f(int l, float a) { glUniform1f(l, a); }
PCS static void w_glUniform2f(int l, float a, float b) { glUniform2f(l, a, b); }
PCS static void w_glUniform3f(int l, float a, float b, float c) { glUniform3f(l, a, b, c); }
PCS static void w_glUniform4f(int l, float a, float b, float c, float d) { glUniform4f(l, a, b, c, d); }
PCS static void w_glVertexAttrib1f(unsigned i, float a) { glVertexAttrib1f(i, a); }
PCS static void w_glVertexAttrib2f(unsigned i, float a, float b) { glVertexAttrib2f(i, a, b); }
PCS static void w_glVertexAttrib3f(unsigned i, float a, float b, float c) { glVertexAttrib3f(i, a, b, c); }
PCS static void w_glVertexAttrib4f(unsigned i, float a, float b, float c, float d) { glVertexAttrib4f(i, a, b, c, d); }

#define E(n) { #n, (void *)w_##n }
static const struct { const char *name; void *fn; } k_wrapped[] = {
    E(glClearColor), E(glBlendColor), E(glClearDepthf), E(glDepthRangef), E(glLineWidth),
    E(glPolygonOffset), E(glSampleCoverage), E(glTexParameterf),
    E(glUniform1f), E(glUniform2f), E(glUniform3f), E(glUniform4f),
    E(glVertexAttrib1f), E(glVertexAttrib2f), E(glVertexAttrib3f), E(glVertexAttrib4f),
};

void *ov_SDL_GL_GetProcAddress(const char *name) {
    for (unsigned i = 0; i < sizeof k_wrapped / sizeof k_wrapped[0]; i++)
        if (!strcmp(name, k_wrapped[i].name)) return k_wrapped[i].fn;
    void *p = SDL_GL_GetProcAddress(name);
    if (!p) { static int n; if (n++ < 200) LOG("GL function missing in vitaGL: %s", name); }
    return p;
}
