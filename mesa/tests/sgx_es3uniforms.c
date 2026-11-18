/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * ES 3.0's uniform and texture-unit limits (SGX_DEBUG=glsl130, an ES 3.0
 * context), each reported and then used:
 *  1. GL_MAX_VERTEX_UNIFORM_VECTORS >= 256, GL_MAX_FRAGMENT_UNIFORM_VECTORS
 *     >= 224, GL_MAX_TEXTURE_IMAGE_UNITS >= 16 and
 *     GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS >= 32
 *  2. a fragment program with 223 vec4 uniforms and an int (224 vectors),
 *     reading the last through the int
 *  3. the same in a vertex program, 255 vec4 and the int (256), passed on
 *     as a varying
 *  4. a fragment program sampling 16 units at once, each a 1x1 texture of
 *     its own colour, the pixel the sum of a byte from each
 *
 *   SGX_DEBUG=glsl130 DISPLAY=:0 sgx_es3uniforms
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 16
#define H 16

static int fails;

static GLuint
prog(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   const char *src[2] = {vs, fs};
   GLenum ty[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLint ok;

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(ty[i]);

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char l[2048];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         printf("compile: %s\n", l);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "a");
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      char l[2048];
      glGetProgramInfoLog(p, sizeof l, NULL, l);
      printf("link: %s\n", l);
   }
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

/* a full-target quad, then the centre pixel against want (+-1 a byte) */
static void
draw_check(const char *name, GLuint p, const unsigned char *want)
{
   static const float q[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
   unsigned char got[4];

   glUseProgram(p);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 6);
   glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
   int ok = 1;
   for (int c = 0; c < 4; c++)
      ok &= abs(got[c] - want[c]) <= 1;
   printf("%-44s %3d %3d %3d %3d (want %3d %3d %3d %3d): %s\n", name, got[0],
          got[1], got[2], got[3], want[0], want[1], want[2], want[3],
          ok ? "OK" : "FAIL");
   fails += !ok;
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
   limit("1 MAX_VERTEX_UNIFORM_VECTORS", GL_MAX_VERTEX_UNIFORM_VECTORS, 256);
   limit("1 MAX_FRAGMENT_UNIFORM_VECTORS", GL_MAX_FRAGMENT_UNIFORM_VECTORS,
         224);
   limit("1 MAX_TEXTURE_IMAGE_UNITS", GL_MAX_TEXTURE_IMAGE_UNITS, 16);
   limit("1 MAX_COMBINED_TEXTURE_IMAGE_UNITS",
         GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 32);

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

   static const char *vs_plain = "#version 300 es\nin vec2 a;\n"
                                 "void main() { gl_Position = vec4(a, 0.0, 1.0); }\n";
   /* 2: u[222] read through a uniform index, so every vec4 is live */
   {
      static const char *fs =
         "#version 300 es\nprecision highp float;\nuniform vec4 u[223];\n"
         "uniform int i;\nout vec4 o;\nvoid main() { o = u[i]; }\n";
      GLuint p = prog(vs_plain, fs);
      float v[223 * 4];
      static const unsigned char want[4] = {32, 64, 128, 255};

      for (int k = 0; k < 223 * 4; k++)
         v[k] = 0.0f;
      v[222 * 4 + 0] = 32 / 255.0f;
      v[222 * 4 + 1] = 64 / 255.0f;
      v[222 * 4 + 2] = 128 / 255.0f;
      v[222 * 4 + 3] = 1.0f;
      glUseProgram(p);
      glUniform4fv(glGetUniformLocation(p, "u"), 223, v);
      glUniform1i(glGetUniformLocation(p, "i"), 222);
      draw_check("2 fragment program, u[222] of 223 and an int", p, want);
   }
   /* 3: the same in a vertex program */
   {
      static const char *vs =
         "#version 300 es\nin vec2 a;\nuniform vec4 u[255];\nuniform int i;\n"
         "out vec4 c;\n"
         "void main() { c = u[i]; gl_Position = vec4(a, 0.0, 1.0); }\n";
      static const char *fs = "#version 300 es\nprecision highp float;\n"
                              "in vec4 c;\nout vec4 o;\nvoid main() { o = c; }\n";
      GLuint p = prog(vs, fs);
      float v[255 * 4];
      static const unsigned char want[4] = {200, 100, 50, 255};

      for (int k = 0; k < 255 * 4; k++)
         v[k] = 0.0f;
      v[254 * 4 + 0] = 200 / 255.0f;
      v[254 * 4 + 1] = 100 / 255.0f;
      v[254 * 4 + 2] = 50 / 255.0f;
      v[254 * 4 + 3] = 1.0f;
      glUseProgram(p);
      glUniform4fv(glGetUniformLocation(p, "u"), 255, v);
      glUniform1i(glGetUniformLocation(p, "i"), 254);
      draw_check("3 vertex program, u[254] of 255 and an int", p, want);
   }
   /* 4: 16 units; unit k holds red = k + 1, so the sum of the reds is 136 */
   {
      static const char *fs =
         "#version 300 es\nprecision highp float;\nuniform sampler2D s[16];\n"
         "out vec4 o;\nvoid main() {\n"
         "   float r = 0.0;\n"
         "   r += texture(s[0], vec2(0.5)).r; r += texture(s[1], vec2(0.5)).r;\n"
         "   r += texture(s[2], vec2(0.5)).r; r += texture(s[3], vec2(0.5)).r;\n"
         "   r += texture(s[4], vec2(0.5)).r; r += texture(s[5], vec2(0.5)).r;\n"
         "   r += texture(s[6], vec2(0.5)).r; r += texture(s[7], vec2(0.5)).r;\n"
         "   r += texture(s[8], vec2(0.5)).r; r += texture(s[9], vec2(0.5)).r;\n"
         "   r += texture(s[10], vec2(0.5)).r; r += texture(s[11], vec2(0.5)).r;\n"
         "   r += texture(s[12], vec2(0.5)).r; r += texture(s[13], vec2(0.5)).r;\n"
         "   r += texture(s[14], vec2(0.5)).r; r += texture(s[15], vec2(0.5)).r;\n"
         "   o = vec4(r, texture(s[15], vec2(0.5)).g, 0.0, 1.0);\n"
         "}\n";
      GLuint p = prog(vs_plain, fs);
      GLuint t[16];
      GLint units[16];
      static const unsigned char want[4] = {136, 16 * 15, 0, 255};

      glGenTextures(16, t);
      for (int k = 0; k < 16; k++) {
         unsigned char px[4] = {(unsigned char)(k + 1),
                                (unsigned char)(16 * k), 0, 255};

         glActiveTexture(GL_TEXTURE0 + k);
         glBindTexture(GL_TEXTURE_2D, t[k]);
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, px);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         units[k] = k;
      }
      glActiveTexture(GL_TEXTURE0);
      glUseProgram(p);
      glUniform1iv(glGetUniformLocation(p, "s"), 16, units);
      draw_check("4 16 units in one fragment program", p, want);
   }

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != 0));
   return fails || e;
}
