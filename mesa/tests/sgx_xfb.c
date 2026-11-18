/*
 * Copyright © 2026 servidorjarvis1-svg
 * SPDX-License-Identifier: MIT
 */

/*
 * Transform feedback (SGX_DEBUG=xfb, an ES 3.0 context): the vertex program
 * stores what it captures with st.a.d (docs/GL3.md H4), the draw expands
 * strips and fans into their primitives on the CPU, one slot a vertex.
 * Each case reads the buffer back with glMapBufferRange:
 *  1. interleaved, GL_TRIANGLES under GL_RASTERIZER_DISCARD: a vec4 and a
 *     float, 6 vertices
 *  2. separate attributes: gl_Position and a float in two buffers
 *  3. a triangle strip of 5: 9 vertices, the odd triangle 2 1 3
 *  4. a triangle fan of 5: 0 1 2, 0 2 3, 0 3 4
 *  5. a line strip of 4: 0 1, 1 2, 2 3; points
 *  6. glDrawElements with an index buffer (3 1 2 0 ...)
 *  7. TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN
 *  8. pause / resume: the second draw appends after the first, the draw in
 *     between captures nothing
 *  9. captured while drawn (no discard): the pixels and the capture
 * 10. mediump outputs
 * 11. a flat int output (raw integer bits)
 * 12. the capture drawn from: its positions as the next draw's vertices
 * 13. glBindBufferRange at an offset
 *
 *   MESA_GLES_VERSION_OVERRIDE=3.0 SGX_DEBUG=glsl130,xfb DISPLAY=:0 sgx_xfb
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define W 64
#define H 64

static int fails;
static const float NA = NAN;

static GLuint
prog(const char *vs, const char *fs, const char *const *vars, int nvars,
     GLenum mode)
{
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
         char l[2048];
         glGetShaderInfoLog(s, sizeof l, NULL, l);
         printf("compile: %s\n", l);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "a");
   glBindAttribLocation(p, 1, "k");
   if (nvars)
      glTransformFeedbackVaryings(p, nvars, vars, mode);
   glLinkProgram(p);
   GLint ok;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      char l[2048];
      glGetProgramInfoLog(p, sizeof l, NULL, l);
      printf("link: %s\n", l);
   }
   return p;
}

static const char *fs_white = "#version 300 es\nprecision mediump float;\n"
                              "out vec4 o;\nvoid main() { o = vec4(1.0); }\n";

/* a: position (x, y), k: the vertex's own number */
static void
attribs(const float *xy, const float *k)
{
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, xy);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, k);
}

static GLuint
buffer(GLsizeiptr bytes)
{
   GLuint b;
   void *z = calloc(1, bytes);

   glGenBuffers(1, &b);
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, b);
   /* a pattern no capture writes: 0xdeadbeef */
   memset(z, 0, bytes);
   for (GLsizeiptr i = 0; i < bytes / 4; i++)
      ((unsigned *)z)[i] = 0xdeadbeef;
   glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, bytes, z, GL_STATIC_READ);
   free(z);
   return b;
}

/* the buffer's first n floats against want; a NaN in want: 0xdeadbeef */
static void
expect(const char *name, GLuint b, GLintptr off, const float *want, int n,
       float tol)
{
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, b);
   const float *got = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, off,
                                       n * 4, GL_MAP_READ_BIT);
   int bad = -1;

   if (!got) {
      printf("%-48s map failed 0x%x: FAIL\n", name, glGetError());
      fails++;
      return;
   }
   for (int i = 0; i < n && bad < 0; i++) {
      unsigned gu;
      memcpy(&gu, &got[i], 4);
      if (isnan(want[i]) ? gu != 0xdeadbeef : fabsf(got[i] - want[i]) > tol)
         bad = i;
   }
   if (bad < 0) {
      printf("%-48s %d floats: OK\n", name, n);
   } else {
      unsigned gu;
      memcpy(&gu, &got[bad], 4);
      printf("%-48s [%d] got %g (0x%08x) want %g: FAIL\n", name, bad,
             got[bad], gu, want[bad]);
      printf("   got from [%d]:", bad);
      for (int i = bad; i < n && i < bad + 12; i++)
         printf(" %g", got[i]);
      printf("\n");
      fails++;
   }
   glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
}

static void
check_pixel(const char *name, int x, int y, int v)
{
   unsigned char p[4];

   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
   int ok = p[0] == v && p[1] == v && p[2] == v;
   printf("%-48s (%2d,%2d) got %3u %3u %3u want %3d: %s\n", name, x, y, p[0],
          p[1], p[2], v, ok ? "OK" : "FAIL");
   fails += !ok;
}


/* k's of a strip / fan / line strip of n, in GL's capture order */
static int
order(GLenum mode, int n, float *out)
{
   int m = 0;

   for (int t = 0; mode == GL_TRIANGLE_STRIP && t + 2 < n; t++) {
      int a = t & 1 ? t + 1 : t, b = t & 1 ? t : t + 1;
      out[m++] = a, out[m++] = b, out[m++] = t + 2;
   }
   for (int t = 0; mode == GL_TRIANGLE_FAN && t + 2 < n; t++)
      out[m++] = 0, out[m++] = t + 1, out[m++] = t + 2;
   for (int t = 0; mode == GL_LINE_STRIP && t + 1 < n; t++)
      out[m++] = t, out[m++] = t + 1;
   return m;
}

/* 13: glBindBufferRange at an offset */
static void
bind_range(GLuint pf, const float *xy, const float *k, const char *name)
{
{
   GLuint b = buffer(512);
   float want[64 + 7];

   glUseProgram(pf);
   attribs(xy, k);
   glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b, 256, 256);
   glEnable(GL_RASTERIZER_DISCARD);
   glBeginTransformFeedback(GL_TRIANGLES);
   glDrawArrays(GL_TRIANGLES, 3, 6);
   glEndTransformFeedback();
   glDisable(GL_RASTERIZER_DISCARD);
   for (int i = 0; i < 64; i++)
      want[i] = NA;
   for (int i = 0; i < 6; i++)
      want[64 + i] = (i + 3) * 10 + 1;
   want[70] = NA;
   expect(name, b, 0, want, 71, 1e-5f);
}
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
   GLint mi = 0, ms = 0, mb = 0;
   glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, &mi);
   glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, &ms);
   glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, &mb);
   printf("interleaved %d, separate %d x %d: %s\n", mi, mb, ms,
          mi >= 64 && ms >= 4 && mb >= 4 ? "OK" : "FAIL");
   fails += !(mi >= 64 && ms >= 4 && mb >= 4);

   GLuint ct, fb;
   glGenTextures(1, &ct);
   glBindTexture(GL_TEXTURE_2D, ct);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
   glGenFramebuffers(1, &fb);
   glBindFramebuffer(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          ct, 0);
   glViewport(0, 0, W, H);

   static const char *vs_vk =
      "#version 300 es\nin vec2 a;\nin float k;\nout vec4 v;\nout float f;\n"
      "void main() { v = vec4(a, k, k * 2.0 + 0.5); f = k * 10.0 + 1.0;\n"
      " gl_Position = vec4(a, 0.0, 1.0); }\n";
   static const char *fs_vk =
      "#version 300 es\nprecision mediump float;\nin vec4 v;\nin float f;\n"
      "out vec4 o;\nvoid main() { o = vec4(1.0) + 0.0 * (v + f); }\n";
   float xy[64], k[64];
   for (int i = 0; i < 32; i++) {
      xy[2 * i] = (i % 3) * 0.25f - 0.5f;
      xy[2 * i + 1] = (i / 3) * 0.125f - 0.5f;
      k[i] = (float)i;
   }

   /* 1: interleaved v, f; discard */
   {
      const char *vars[] = {"v", "f"};
      GLuint p = prog(vs_vk, fs_vk, vars, 2, GL_INTERLEAVED_ATTRIBS);
      GLuint b = buffer(256 * 4);
      float want[6 * 5 + 2];

      glUseProgram(p);
      attribs(xy, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      for (int i = 0; i < 6; i++) {
         float *w = want + 5 * i;
         w[0] = xy[2 * i], w[1] = xy[2 * i + 1], w[2] = k[i];
         w[3] = k[i] * 2 + 0.5f, w[4] = k[i] * 10 + 1;
      }
      want[30] = want[31] = NA;
      expect("1 interleaved vec4+float, discard", b, 0, want, 32, 1e-5f);
      printf("   glGetError 0x%x\n", glGetError());
   }

   /* 2: separate: gl_Position, f */
   {
      const char *vars[] = {"gl_Position", "f"};
      GLuint p = prog(vs_vk, fs_vk, vars, 2, GL_SEPARATE_ATTRIBS);
      GLuint b0 = buffer(256), b1 = buffer(256);
      float w0[3 * 4 + 1], w1[3 + 1];

      glUseProgram(p);
      attribs(xy, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b0);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, b1);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 3, 3);
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      for (int i = 0; i < 3; i++) {
         w0[4 * i] = xy[2 * (i + 3)], w0[4 * i + 1] = xy[2 * (i + 3) + 1];
         w0[4 * i + 2] = 0, w0[4 * i + 3] = 1;
         w1[i] = k[i + 3] * 10 + 1;
      }
      w0[12] = w1[3] = NA;
      expect("2 separate: gl_Position (buffer 0)", b0, 0, w0, 13, 1e-5f);
      expect("2 separate: f (buffer 1)", b1, 0, w1, 4, 1e-5f);
   }

   /* 3-5: strip, fan, line strip, points: k in capture order */
   const char *vars_f[] = {"f"};
   GLuint pf = prog(vs_vk, fs_vk, vars_f, 1, GL_INTERLEAVED_ATTRIBS);
   {
      static const struct {
         const char *name;
         GLenum mode, prim;
         int n;
      } cs[] = {
         {"3 triangle strip of 5", GL_TRIANGLE_STRIP, GL_TRIANGLES, 5},
         {"4 triangle fan of 5", GL_TRIANGLE_FAN, GL_TRIANGLES, 5},
         {"5 line strip of 4", GL_LINE_STRIP, GL_LINES, 4},
         {"5 points, 7", GL_POINTS, GL_POINTS, 7},
      };
      for (unsigned t = 0; t < sizeof cs / sizeof cs[0]; t++) {
         GLuint b = buffer(256);
         float ord[64], want[64];
         int m = cs[t].mode == GL_POINTS ? cs[t].n
                                         : order(cs[t].mode, cs[t].n, ord);

         if (cs[t].mode == GL_POINTS)
            for (int i = 0; i < m; i++)
               ord[i] = i;
         glUseProgram(pf);
         attribs(xy, k);
         glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
         glEnable(GL_RASTERIZER_DISCARD);
         glBeginTransformFeedback(cs[t].prim);
         glDrawArrays(cs[t].mode, 0, cs[t].n);
         glEndTransformFeedback();
         glDisable(GL_RASTERIZER_DISCARD);
         for (int i = 0; i < m; i++)
            want[i] = ord[i] * 10 + 1;
         want[m] = NA;
         expect(cs[t].name, b, 0, want, m + 1, 1e-5f);
      }
   }

   /* 6: indexed */
   {
      static const GLushort ix[] = {3, 1, 2, 0, 5, 4};
      GLuint b = buffer(256), ib;
      float want[7];

      glGenBuffers(1, &ib);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ix, ix, GL_STATIC_DRAW);
      glUseProgram(pf);
      attribs(xy, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_TRIANGLES);
      /* ES 3.0: no glDrawElements while a capture is active (an error,
       * nothing drawn); paused, it draws and captures nothing */
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
      GLenum e6 = glGetError();
      glPauseTransformFeedback();
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
      GLenum e6p = glGetError();
      glResumeTransformFeedback();
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      printf("%-48s active 0x%x want 0x502, paused 0x%x want 0: %s\n",
             "6 glDrawElements while capturing", e6, e6p,
             e6 == GL_INVALID_OPERATION && !e6p ? "OK" : "FAIL");
      fails += !(e6 == GL_INVALID_OPERATION && !e6p);
      for (int i = 0; i < 7; i++)
         want[i] = NA;
      (void)ix;
      expect("6 nothing captured", b, 0, want, 7, 1e-5f);
   }

   /* 7: the primitives written query; 8: pause / resume */
   {
      GLuint b = buffer(256), q;
      GLuint res = 0;
      float want[13];

      glGenQueries(1, &q);
      glUseProgram(pf);
      attribs(xy, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glPauseTransformFeedback();
      glDrawArrays(GL_TRIANGLES, 12, 3);
      glResumeTransformFeedback();
      glDrawArrays(GL_TRIANGLES, 6, 6);
      glEndTransformFeedback();
      glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
      glDisable(GL_RASTERIZER_DISCARD);
      glGetQueryObjectuiv(q, GL_QUERY_RESULT, &res);
      printf("%-48s got %u want 4: %s\n", "7 PRIMITIVES_WRITTEN", res,
             res == 4 ? "OK" : "FAIL");
      fails += res != 4;
      for (int i = 0; i < 12; i++)
         want[i] = i * 10 + 1;
      want[12] = NA;
      expect("8 pause / resume appends", b, 0, want, 13, 1e-5f);
   }

   /* 9: drawn and captured */
   {
      GLuint b = buffer(256);
      static const float quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
      float want[7];

      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glUseProgram(pf);
      attribs(quad, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glEndTransformFeedback();
      for (int i = 0; i < 6; i++)
         want[i] = i * 10 + 1;
      want[6] = NA;
      check_pixel("9 drawn while captured: pixel", 20, 40, 255);
      expect("9 drawn while captured: capture", b, 0, want, 7, 1e-5f);
   }

   /* 10: mediump outputs */
   {
      static const char *vs_m =
         "#version 300 es\nprecision mediump float;\nin vec2 a;\nin float k;\n"
         "out float f;\nvoid main() { f = k * 0.5 + 0.25;\n"
         " gl_Position = vec4(a, 0.0, 1.0); }\n";
      GLuint p = prog(vs_m, fs_white, vars_f, 1, GL_INTERLEAVED_ATTRIBS);
      GLuint b = buffer(256);
      float want[7];

      glUseProgram(p);
      attribs(xy, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      for (int i = 0; i < 6; i++)
         want[i] = i * 0.5f + 0.25f;
      want[6] = NA;
      expect("10 mediump float", b, 0, want, 7, 1e-3f);
   }

   /* 11: a flat int */
   {
      static const char *vs_i =
         "#version 300 es\nin vec2 a;\nin float k;\nflat out int n;\n"
         "void main() { n = int(k) * 65536 + 7 - int(k);\n"
         " gl_Position = vec4(a, 0.0, 1.0); }\n";
      static const char *fs_i =
         "#version 300 es\nprecision mediump float;\nflat in int n;\n"
         "out vec4 o;\nvoid main() { o = vec4(float(n & 1)); }\n";
      const char *vars[] = {"n"};
      GLuint p = prog(vs_i, fs_i, vars, 1, GL_INTERLEAVED_ATTRIBS);
      GLuint b = buffer(256);

      glUseProgram(p);
      attribs(xy, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, b);
      const int *got = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 7 * 4,
                                        GL_MAP_READ_BIT);
      int ok = got != NULL;
      for (int i = 0; ok && i < 6; i++)
         ok = got[i] == i * 65536 + 7 - i;
      ok = ok && (unsigned)got[6] == 0xdeadbeef;
      printf("%-48s %d %d %d .. %d: %s\n", "11 flat int", got ? got[0] : 0,
             got ? got[1] : 0, got ? got[2] : 0, got ? got[5] : 0,
             ok ? "OK" : "FAIL");
      fails += !ok;
      glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
   }

   bind_range(pf, xy, k, "13 glBindBufferRange offset 256, before 12");

   /* 12: the capture's positions drawn from */
   {
      static const char *vs_p =
         "#version 300 es\nin vec2 a;\nin float k;\nout vec2 pos;\n"
         "void main() { pos = a * 2.0; gl_Position = vec4(a, 0.0, 1.0); }\n";
      const char *vars[] = {"pos"};
      GLuint p = prog(vs_p, fs_white, vars, 1, GL_INTERLEAVED_ATTRIBS);
      GLuint b = buffer(256);
      static const float half[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f,
                                   -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  0.5f};

      glUseProgram(p);
      attribs(half, k);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
      glEnable(GL_RASTERIZER_DISCARD);
      glBeginTransformFeedback(GL_TRIANGLES);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glEndTransformFeedback();
      glDisable(GL_RASTERIZER_DISCARD);
      glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
      /* the doubled quad covers the whole target */
      glClear(GL_COLOR_BUFFER_BIT);
      glUseProgram(pf);
      glBindBuffer(GL_ARRAY_BUFFER, b);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
      glDisableVertexAttribArray(1);
      glVertexAttrib1f(1, 0.0f);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      check_pixel("12 capture drawn from: corner", 2, 2, 255);
      check_pixel("12 capture drawn from: corner", W - 3, H - 3, 255);
   }

   bind_range(pf, xy, k, "13 glBindBufferRange offset 256, after 12");

   GLenum e = glGetError();
   printf("GL error 0x%x\n", e);
   printf("result: %d failed\n", fails + (e != 0));
   return fails || e;
}
