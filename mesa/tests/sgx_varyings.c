/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * ES 3.0's varying limits (SGX_DEBUG=glsl130,bigvary, an ES 3.0 context).
 * The SGX545's ten texture coordinate sets hold 40 components; ES 3.0 asks
 * 15 varying vectors and Chromium's ES 3.0 contexts 64 components out of a
 * vertex program and 60 into a fragment one:
 *  1. GL_MAX_VERTEX_OUTPUT_COMPONENTS >= 64,
 *     GL_MAX_FRAGMENT_INPUT_COMPONENTS >= 60, GL_MAX_VARYING_VECTORS >= 15
 *  2. ten vec4 varyings, which fit the sets: they link, and each reaches
 *     the fragment program (the pixel the last one plus a byte of each)
 *  3. twelve vec4 varyings, which don't: the link fails, with a reason in
 *     the info log, instead of linking and drawing nothing
 *
 *   SGX_DEBUG=glsl130,bigvary DISPLAY=:0 sgx_varyings
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 16
#define H 16

static int fails;

/* a program passing n vec4 varyings v0..v(n-1), each from the attribute c
 * (1/255) so the link can't fold them: vk = c * (k + 1, 1, 2, 4), and the
 * fragment program reads every component: (n, 7n, n(n+1)/2) / 255 */
static GLuint
prog_n(int n, GLint *linked, char *log, size_t log_size)
{
   char vs[4096], fs[4096];
   size_t a = 0, b = 0;

   a += snprintf(vs + a, sizeof vs - a,
                 "#version 300 es\nin vec2 p;\nin float c;\n");
   b += snprintf(fs + b, sizeof fs - b,
                 "#version 300 es\nprecision highp float;\nout vec4 o;\n");
   for (int k = 0; k < n; k++) {
      a += snprintf(vs + a, sizeof vs - a, "out vec4 v%d;\n", k);
      b += snprintf(fs + b, sizeof fs - b, "in vec4 v%d;\n", k);
   }
   a += snprintf(vs + a, sizeof vs - a, "void main() {\n");
   b += snprintf(fs + b, sizeof fs - b,
                 "void main() {\n   float s = 0.0, x = 0.0;\n");
   for (int k = 0; k < n; k++) {
      a += snprintf(vs + a, sizeof vs - a,
                       "   v%d = c * vec4(%d.0, 1.0, 2.0, 4.0);\n", k, k + 1);
      b += snprintf(fs + b, sizeof fs - b,
                    "   s += v%d.y + v%d.z + v%d.w; x += v%d.x;\n", k, k, k,
                    k);
   }
   a += snprintf(vs + a, sizeof vs - a,
                 "   gl_Position = vec4(p, 0.0, 1.0);\n}\n");
   b += snprintf(fs + b, sizeof fs - b,
                 "   o = vec4(v%d.x, s, x, 1.0);\n}\n", n - 1);

   GLuint p = glCreateProgram();
   const char *src[2] = {vs, fs};
   GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(ty[i]);
      GLint ok;

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         glGetShaderInfoLog(s, log_size, NULL, log);
         printf("compile: %s\n", log);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "p");
   glBindAttribLocation(p, 1, "c");
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, linked);
   log[0] = 0;
   glGetProgramInfoLog(p, log_size, NULL, log);
   return p;
}

static void
limit(const char *name, GLenum e, GLint want)
{
   GLint v = 0;

   glGetIntegerv(e, &v);
   printf("%-44s %d (>= %d): %s\n", name, v, want, v >= want ? "OK" : "FAIL");
   fails += v < want;
}

int
main(void)
{
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min;

   if (!eglInitialize(d, &maj, &min)) {
      printf("eglInitialize failed\n");
      return 1;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   eglChooseConfig(d, ca, &cfg, 1, &n);
   const EGLint xa3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
   EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa3);
   if (c == EGL_NO_CONTEXT ||
       !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      printf("no ES 3.0 context 0x%x\n", eglGetError());
      return 1;
   }
   printf("%s\n", glGetString(GL_VERSION));

   /* 1 */
   limit("1 MAX_VERTEX_OUTPUT_COMPONENTS", GL_MAX_VERTEX_OUTPUT_COMPONENTS, 64);
   limit("1 MAX_FRAGMENT_INPUT_COMPONENTS", GL_MAX_FRAGMENT_INPUT_COMPONENTS,
         60);
   limit("1 MAX_VARYING_VECTORS", GL_MAX_VARYING_VECTORS, 15);

   GLuint ct, fb;
   glGenTextures(1, &ct);
   glBindTexture(GL_TEXTURE_2D, ct);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          ct, 0);
   glViewport(0, 0, W, H);
   glClearColor(0, 0, 0, 0);

   char log[2048];
   GLint linked = 0;

   /* 2: ten fit; the pixel is (10, 70, 55, 255) */
   {
      static const float q[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
      GLuint p = prog_n(10, &linked, log, sizeof log);
      unsigned char got[4] = {0};

      if (linked) {
         glUseProgram(p);
         glEnableVertexAttribArray(0);
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q);
         glDisableVertexAttribArray(1);
         glVertexAttrib1f(1, 1.0f / 255.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         glDrawArrays(GL_TRIANGLES, 0, 6);
         glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
      } else {
         printf("   link: %s\n", log);
      }
      int ok = linked && abs(got[0] - 10) <= 1 && abs(got[1] - 70) <= 1 &&
               abs(got[2] - 55) <= 1 && got[3] == 255;
      printf("%-44s %3d %3d %3d %3d (want  10  70  55 255): %s\n",
             "2 ten vec4 varyings link and arrive", got[0], got[1], got[2],
             got[3], ok ? "OK" : "FAIL");
      fails += !ok;
   }
   /* 3: twelve don't fit the sets: no link, and a reason */
   {
      prog_n(12, &linked, log, sizeof log);
      int ok = !linked && log[0];
      printf("%-44s linked %d, log \"%.60s\": %s\n",
             "3 twelve vec4 varyings fail to link", linked, log,
             ok ? "OK" : "FAIL");
      fails += !ok;
   }

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != 0));
   return fails || e;
}
