// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <string.h>
#include <stdlib.h>
#include <GL/glx.h>
#include <xcb/xcb.h>
#include <xcb/render.h>
#include <stdio.h>

#include "compiler.h"
#include "string_utils.h"
#include "log.h"
#include "config.h"
#include "common.h"
#include "utils.h"
#include "win.h"
#include "region.h"

#include "opengl.h"

static inline int
glx_cmp_fbconfig_cmpattr(session_t *ps,
    const glx_fbconfig_t *pfbc_a, const glx_fbconfig_t *pfbc_b,
    int attr) {
  int attr_a = 0, attr_b = 0;

  // TODO: Error checking
  glXGetFBConfigAttrib(ps->dpy, pfbc_a->cfg, attr, &attr_a);
  glXGetFBConfigAttrib(ps->dpy, pfbc_b->cfg, attr, &attr_b);

  return attr_a - attr_b;
}

/**
 * Compare two GLX FBConfig's to find the preferred one.
 */
static int
glx_cmp_fbconfig(session_t *ps,
    const glx_fbconfig_t *pfbc_a, const glx_fbconfig_t *pfbc_b) {
  int result = 0;

  if (!pfbc_a)
    return -1;
  if (!pfbc_b)
    return 1;
  int tmpattr;

  // Avoid 10-bit colors
  glXGetFBConfigAttrib(ps->dpy, pfbc_a->cfg, GLX_RED_SIZE, &tmpattr);
  if (tmpattr != 8)
    return -1;

  glXGetFBConfigAttrib(ps->dpy, pfbc_b->cfg, GLX_RED_SIZE, &tmpattr);
  if (tmpattr != 8)
    return 1;

#define P_CMPATTR_LT(attr) { if ((result = glx_cmp_fbconfig_cmpattr(ps, pfbc_a, pfbc_b, (attr)))) return -result; }
#define P_CMPATTR_GT(attr) { if ((result = glx_cmp_fbconfig_cmpattr(ps, pfbc_a, pfbc_b, (attr)))) return result; }

  P_CMPATTR_LT(GLX_BIND_TO_TEXTURE_RGBA_EXT);
  P_CMPATTR_LT(GLX_DOUBLEBUFFER);
  P_CMPATTR_LT(GLX_STENCIL_SIZE);
  P_CMPATTR_LT(GLX_DEPTH_SIZE);
  P_CMPATTR_GT(GLX_BIND_TO_MIPMAP_TEXTURE_EXT);

  return 0;
}

/**
 * @brief Update the FBConfig of given depth.
 */
static inline void
glx_update_fbconfig_bydepth(session_t *ps, int depth, glx_fbconfig_t *pfbcfg) {
  // Make sure the depth is sane
  if (depth < 0 || depth > OPENGL_MAX_DEPTH)
    return;

  // Compare new FBConfig with current one
  if (glx_cmp_fbconfig(ps, ps->psglx->fbconfigs[depth], pfbcfg) < 0) {
    log_trace("(depth %d): %p overrides %p, target %#x.", depth,
              pfbcfg->cfg,
              ps->psglx->fbconfigs[depth] ? ps->psglx->fbconfigs[depth]->cfg:
                                            0,
              pfbcfg->texture_tgts);
    if (!ps->psglx->fbconfigs[depth]) {
      ps->psglx->fbconfigs[depth] = cmalloc(glx_fbconfig_t);
    }
    (*ps->psglx->fbconfigs[depth]) = *pfbcfg;
  }
}

/**
 * Get GLX FBConfigs for all depths.
 */
static bool
glx_update_fbconfig(session_t *ps) {
  // Acquire all FBConfigs and loop through them
  int nele = 0;
  GLXFBConfig* pfbcfgs = glXGetFBConfigs(ps->dpy, ps->scr, &nele);

  for (GLXFBConfig *pcur = pfbcfgs; pcur < pfbcfgs + nele; pcur++) {
    glx_fbconfig_t fbinfo = {
      .cfg = *pcur,
      .texture_fmt = 0,
      .texture_tgts = 0,
      .y_inverted = false,
    };
    int id = (int) (pcur - pfbcfgs);
    int depth = 0, depth_alpha = 0, val = 0;

    // Skip over multi-sampled visuals
    // http://people.freedesktop.org/~glisse/0001-glx-do-not-use-multisample-visual-config-for-front-o.patch
#ifdef GLX_SAMPLES
    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_SAMPLES, &val)
        && val > 1)
      continue;
#endif

    if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BUFFER_SIZE, &depth)
        || Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_ALPHA_SIZE, &depth_alpha)) {
      log_error("Failed to retrieve buffer size and alpha size of FBConfig %d.", id);
      continue;
    }
    if (Success != glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_TARGETS_EXT, &fbinfo.texture_tgts)) {
      log_error("Failed to retrieve BIND_TO_TEXTURE_TARGETS_EXT of FBConfig %d.", id);
      continue;
    }

    int visualdepth = 0;
    {
      XVisualInfo *pvi = glXGetVisualFromFBConfig(ps->dpy, *pcur);
      if (!pvi) {
        // On nvidia-drivers-325.08 this happens slightly too often...
        // log_error("Failed to retrieve X Visual of FBConfig %d.", id);
        continue;
      }
      visualdepth = pvi->depth;
      cxfree(pvi);
    }

    bool rgb = false;
    bool rgba = false;

    if (depth >= 32 && depth_alpha && Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_RGBA_EXT, &val) && val)
      rgba = true;

    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_BIND_TO_TEXTURE_RGB_EXT, &val) && val)
      rgb = true;

    if (Success == glXGetFBConfigAttrib(ps->dpy, *pcur, GLX_Y_INVERTED_EXT, &val))
      fbinfo.y_inverted = val;

    {
      int tgtdpt = depth - depth_alpha;
      if (tgtdpt == visualdepth && tgtdpt < 32 && rgb) {
        fbinfo.texture_fmt = GLX_TEXTURE_FORMAT_RGB_EXT;
        glx_update_fbconfig_bydepth(ps, tgtdpt, &fbinfo);
      }
    }

    if (depth == visualdepth && rgba) {
      fbinfo.texture_fmt = GLX_TEXTURE_FORMAT_RGBA_EXT;
      glx_update_fbconfig_bydepth(ps, depth, &fbinfo);
    }
  }

  cxfree(pfbcfgs);

  // Sanity checks
  if (!ps->psglx->fbconfigs[ps->depth]) {
    log_error("No FBConfig found for default depth %d.", ps->depth);
    return false;
  }

  if (!ps->psglx->fbconfigs[32]) {
    log_error("No FBConfig found for depth 32. Expect crazy things.");
  }

  log_trace("%d-bit: %p, 32-bit: %p", ps->depth, ps->psglx->fbconfigs[ps->depth]->cfg,
            ps->psglx->fbconfigs[32]->cfg);

  return true;
}

static inline XVisualInfo *
get_visualinfo_from_visual(session_t *ps, xcb_visualid_t visual) {
  XVisualInfo vreq = { .visualid = visual };
  int nitems = 0;

  return XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
}

#ifdef DEBUG_GLX_DEBUG_CONTEXT
static inline GLXFBConfig
get_fbconfig_from_visualinfo(session_t *ps, const XVisualInfo *visualinfo) {
  int nelements = 0;
  GLXFBConfig *fbconfigs = glXGetFBConfigs(ps->dpy, visualinfo->screen,
      &nelements);
  for (int i = 0; i < nelements; ++i) {
    int visual_id = 0;
    if (Success == glXGetFBConfigAttrib(ps->dpy, fbconfigs[i], GLX_VISUAL_ID, &visual_id)
        && visual_id == visualinfo->visualid)
      return fbconfigs[i];
  }

  return NULL;
}

static void
glx_debug_msg_callback(GLenum source, GLenum type,
    GLuint id, GLenum severity, GLsizei length, const GLchar *message,
    GLvoid *userParam) {
  log_trace("source 0x%04X, type 0x%04X, id %u, severity 0x%0X, \"%s\"",
            source, type, id, severity, message);
}
#endif

/**
 * Initialize OpenGL.
 */
bool
glx_init(session_t *ps, bool need_render) {
  bool success = false;
  XVisualInfo *pvis = NULL;

  // Check for GLX extension
  if (!ps->glx_exists) {
    if (glXQueryExtension(ps->dpy, &ps->glx_event, &ps->glx_error))
      ps->glx_exists = true;
    else {
      log_error("No GLX extension.");
      goto glx_init_end;
    }
  }

  if (ps->o.glx_swap_method > CGLX_MAX_BUFFER_AGE) {
    log_error("glx-swap-method is too big");
    goto glx_init_end;
  }

  // Get XVisualInfo
  pvis = get_visualinfo_from_visual(ps, ps->vis);
  if (!pvis) {
    log_error("Failed to acquire XVisualInfo for current visual.");
    goto glx_init_end;
  }

  // Ensure the visual is double-buffered
  if (need_render) {
    int value = 0;
    if (Success != glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
      log_error("Root visual is not a GL visual.");
      goto glx_init_end;
    }

    if (Success != glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value)
        || !value) {
      log_error("Root visual is not a double buffered GL visual.");
      goto glx_init_end;
    }
  }

  // Ensure GLX_EXT_texture_from_pixmap exists
  if (need_render && !glx_hasglxext(ps, "GLX_EXT_texture_from_pixmap"))
    goto glx_init_end;

  // Initialize GLX data structure
  if (!ps->psglx) {
    static const glx_session_t CGLX_SESSION_DEF = CGLX_SESSION_INIT;
    ps->psglx = cmalloc(glx_session_t);
    memcpy(ps->psglx, &CGLX_SESSION_DEF, sizeof(glx_session_t));

    for (int i = 0; i < MAX_BLUR_PASS; ++i) {
      glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];
      ppass->unifm_factor_center = -1;
      ppass->unifm_offset_x = -1;
      ppass->unifm_offset_y = -1;
    }
  }

  glx_session_t *psglx = ps->psglx;

  if (!psglx->context) {
    // Get GLX context
#ifndef DEBUG_GLX_DEBUG_CONTEXT
    psglx->context = glXCreateContext(ps->dpy, pvis, None, GL_TRUE);
#else
    {
      GLXFBConfig fbconfig = get_fbconfig_from_visualinfo(ps, pvis);
      if (!fbconfig) {
        log_error("Failed to get GLXFBConfig for root visual %#lx.", pvis->visualid);
        goto glx_init_end;
      }

      f_glXCreateContextAttribsARB p_glXCreateContextAttribsARB =
        (f_glXCreateContextAttribsARB)
        glXGetProcAddress((const GLubyte *) "glXCreateContextAttribsARB");
      if (!p_glXCreateContextAttribsARB) {
        log_error("Failed to get glXCreateContextAttribsARB().");
        goto glx_init_end;
      }

      static const int attrib_list[] = {
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_DEBUG_BIT_ARB,
        None
      };
      psglx->context = p_glXCreateContextAttribsARB(ps->dpy, fbconfig, NULL,
          GL_TRUE, attrib_list);
    }
#endif

    if (!psglx->context) {
      log_error("Failed to get GLX context.");
      goto glx_init_end;
    }

    // Attach GLX context
    if (!glXMakeCurrent(ps->dpy, get_tgt_window(ps), psglx->context)) {
      log_error("Failed to attach GLX context.");
      goto glx_init_end;
    }

#ifdef DEBUG_GLX_DEBUG_CONTEXT
    {
      f_DebugMessageCallback p_DebugMessageCallback =
        (f_DebugMessageCallback)
        glXGetProcAddress((const GLubyte *) "glDebugMessageCallback");
      if (!p_DebugMessageCallback) {
        log_error("Failed to get glDebugMessageCallback(0.");
        goto glx_init_end;
      }
      p_DebugMessageCallback(glx_debug_msg_callback, ps);
    }
#endif

  }

  // Ensure we have a stencil buffer. X Fixes does not guarantee rectangles
  // in regions don't overlap, so we must use stencil buffer to make sure
  // we don't paint a region for more than one time, I think?
  if (need_render && !ps->o.glx_no_stencil) {
    GLint val = 0;
    glGetIntegerv(GL_STENCIL_BITS, &val);
    if (!val) {
      log_error("Target window doesn't have stencil buffer.");
      goto glx_init_end;
    }
  }

  // Check GL_ARB_texture_non_power_of_two, requires a GLX context and
  // must precede FBConfig fetching
  if (need_render)
    psglx->has_texture_non_power_of_two = glx_hasglext(
        "GL_ARB_texture_non_power_of_two");

  // Acquire function addresses
  if (need_render) {
#ifdef DEBUG_GLX_MARK
    psglx->glStringMarkerGREMEDY = (f_StringMarkerGREMEDY)
      glXGetProcAddress((const GLubyte *) "glStringMarkerGREMEDY");
    psglx->glFrameTerminatorGREMEDY = (f_FrameTerminatorGREMEDY)
      glXGetProcAddress((const GLubyte *) "glFrameTerminatorGREMEDY");
#endif

    psglx->glXBindTexImageProc = (f_BindTexImageEXT)
      glXGetProcAddress((const GLubyte *) "glXBindTexImageEXT");
    psglx->glXReleaseTexImageProc = (f_ReleaseTexImageEXT)
      glXGetProcAddress((const GLubyte *) "glXReleaseTexImageEXT");
    if (!psglx->glXBindTexImageProc || !psglx->glXReleaseTexImageProc) {
      log_error("Failed to acquire glXBindTexImageEXT() / glXReleaseTexImageEXT().");
      goto glx_init_end;
    }
  }

  // Acquire FBConfigs
  if (need_render && !glx_update_fbconfig(ps))
    goto glx_init_end;

  // Render preparations
  if (need_render) {
    glx_on_root_change(ps);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_BLEND);

    if (!ps->o.glx_no_stencil) {
      // Initialize stencil buffer
      glClear(GL_STENCIL_BUFFER_BIT);
      glDisable(GL_STENCIL_TEST);
      glStencilMask(0x1);
      glStencilFunc(GL_EQUAL, 0x1, 0x1);
    }

    // Clear screen
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    // glXSwapBuffers(ps->dpy, get_tgt_window(ps));
  }

  success = true;

glx_init_end:
  cxfree(pvis);

  if (!success)
    glx_destroy(ps);

  return success;
}

static void
glx_free_prog_main(session_t *ps, glx_prog_main_t *pprogram) {
  if (!pprogram)
    return;
  if (pprogram->prog) {
    glDeleteProgram(pprogram->prog);
    pprogram->prog = 0;
  }
  pprogram->unifm_opacity = -1;
  pprogram->unifm_invert_color = -1;
  pprogram->unifm_tex = -1;
}

/**
 * Destroy GLX related resources.
 */
void
glx_destroy(session_t *ps) {
  if (!ps->psglx)
    return;

  // Free all GLX resources of windows
  for (win *w = ps->list; w; w = w->next)
    free_win_res_glx(ps, w);

  // Free GLSL shaders/programs
  for (int i = 0; i < MAX_BLUR_PASS; ++i) {
    glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];
    if (ppass->frag_shader)
      glDeleteShader(ppass->frag_shader);
    if (ppass->prog)
      glDeleteProgram(ppass->prog);
  }

  glx_free_prog_main(ps, &ps->glx_prog_win);

  glx_check_err(ps);

  // Free FBConfigs
  for (int i = 0; i <= OPENGL_MAX_DEPTH; ++i) {
    free(ps->psglx->fbconfigs[i]);
    ps->psglx->fbconfigs[i] = NULL;
  }

  // Destroy GLX context
  if (ps->psglx->context) {
    glXDestroyContext(ps->dpy, ps->psglx->context);
    ps->psglx->context = NULL;
  }

  free(ps->psglx);
  ps->psglx = NULL;
}

/**
 * Reinitialize GLX.
 */
bool
glx_reinit(session_t *ps, bool need_render) {
  // Reinitialize VSync as well
  vsync_deinit(ps);

  glx_destroy(ps);
  if (!glx_init(ps, need_render)) {
    log_error("Failed to initialize GLX.");
    return false;
  }

  if (!vsync_init(ps)) {
    log_error("Failed to initialize VSync.");
    return false;
  }

  return true;
}

/**
 * Callback to run on root window size change.
 */
void
glx_on_root_change(session_t *ps) {
  glViewport(0, 0, ps->root_width, ps->root_height);

  // Initialize matrix, copied from dcompmgr
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, ps->root_width, 0, ps->root_height, -1000.0, 1000.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

/**
 * Initialize GLX blur filter.
 */
bool
glx_init_conv_blur(session_t *ps) {
  assert(ps->o.blur_kerns[0]);

  // Allocate PBO if more than one blur kernel is present
  if (ps->o.blur_kerns[1]) {
    // Try to generate a framebuffer
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (!fbo) {
      log_error("Failed to generate Framebuffer. Cannot do multi-pass blur with GLX"
                " backend.");
      return false;
    }
    glDeleteFramebuffers(1, &fbo);
  }

  {
    char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
    // Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
    // Thanks to hiciu for reporting.
    setlocale(LC_NUMERIC, "C");

    static const char *FRAG_SHADER_BLUR_PREFIX =
      "#version 110\n"
      "%s"
      "uniform float offset_x;\n"
      "uniform float offset_y;\n"
      "uniform float factor_center;\n"
      "uniform %s tex_scr;\n"
      "\n"
      "void main() {\n"
      "  vec4 sum = vec4(0.0, 0.0, 0.0, 0.0);\n";
    static const char *FRAG_SHADER_BLUR_ADD =
      "  sum += float(%.7g) * %s(tex_scr, vec2(gl_TexCoord[0].x + offset_x * float(%d), gl_TexCoord[0].y + offset_y * float(%d)));\n";
    static const char *FRAG_SHADER_BLUR_ADD_GPUSHADER4 =
      "  sum += float(%.7g) * %sOffset(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y), ivec2(%d, %d));\n";
    static const char *FRAG_SHADER_BLUR_SUFFIX =
      "  sum += %s(tex_scr, vec2(gl_TexCoord[0].x, gl_TexCoord[0].y)) * factor_center;\n"
      "  gl_FragColor = sum / (factor_center + float(%.7g));\n"
      "}\n";

    const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
    const char *sampler_type = (use_texture_rect ?
        "sampler2DRect": "sampler2D");
    const char *texture_func = (use_texture_rect ?
        "texture2DRect": "texture2D");
    const char *shader_add = FRAG_SHADER_BLUR_ADD;
    char *extension = strdup("");
    if (use_texture_rect)
      mstrextend(&extension, "#extension GL_ARB_texture_rectangle : require\n");
    if (ps->o.glx_use_gpushader4) {
      mstrextend(&extension, "#extension GL_EXT_gpu_shader4 : require\n");
      shader_add = FRAG_SHADER_BLUR_ADD_GPUSHADER4;
    }

    for (int i = 0; i < MAX_BLUR_PASS && ps->o.blur_kerns[i]; ++i) {
      xcb_render_fixed_t *kern = ps->o.blur_kerns[i];
      if (!kern)
        break;

      glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];

      // Build shader
      {
        int wid = XFIXED_TO_DOUBLE(kern[0]), hei = XFIXED_TO_DOUBLE(kern[1]);
        int nele = wid * hei - 1;
        unsigned int len = strlen(FRAG_SHADER_BLUR_PREFIX) +
                           strlen(sampler_type) +
                           strlen(extension) +
                           (strlen(shader_add) + strlen(texture_func) + 42) * nele +
                           strlen(FRAG_SHADER_BLUR_SUFFIX) +
                           strlen(texture_func) + 12 + 1;
        char *shader_str = ccalloc(len, char);
        char *pc = shader_str;
        sprintf(pc, FRAG_SHADER_BLUR_PREFIX, extension, sampler_type);
        pc += strlen(pc);
        assert(strlen(shader_str) < len);

        double sum = 0.0;
        for (int j = 0; j < hei; ++j) {
          for (int k = 0; k < wid; ++k) {
            if (hei / 2 == j && wid / 2 == k)
              continue;
            double val = XFIXED_TO_DOUBLE(kern[2 + j * wid + k]);
            if (0.0 == val)
              continue;
            sum += val;
            sprintf(pc, shader_add, val, texture_func, k - wid / 2, j - hei / 2);
            pc += strlen(pc);
            assert(strlen(shader_str) < len);
          }
        }

        sprintf(pc, FRAG_SHADER_BLUR_SUFFIX, texture_func, sum);
        assert(strlen(shader_str) < len);
        ppass->frag_shader = glx_create_shader(GL_FRAGMENT_SHADER, shader_str);
        free(shader_str);
      }

      if (!ppass->frag_shader) {
        log_error("Failed to create fragment shader %d.", i);
        return false;
      }

      // Build program
      ppass->prog = glx_create_program(&ppass->frag_shader, 1);
      if (!ppass->prog) {
        log_error("Failed to create GLSL program.");
        return false;
      }

      // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      ppass->target = glGetUniformLocation(ppass->prog, name); \
      if (ppass->target < 0) { \
        log_error("Failed to get location of %d-th uniform '" name "'. Might be troublesome.", i); \
      } \
    }

      P_GET_UNIFM_LOC("factor_center", unifm_factor_center);
      if (!ps->o.glx_use_gpushader4) {
        P_GET_UNIFM_LOC("offset_x", unifm_offset_x);
        P_GET_UNIFM_LOC("offset_y", unifm_offset_y);
      }

#undef P_GET_UNIFM_LOC
    }
    free(extension);

    // Restore LC_NUMERIC
    setlocale(LC_NUMERIC, lc_numeric_old);
    free(lc_numeric_old);
  }


  glx_check_err(ps);

  return true;
}

bool
glx_init_kawase_blur(session_t *ps) {
  {
    // Try to generate a framebuffer
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (!fbo) {
      log_error("(): Failed to generate Framebuffer. Cannot do "
          "multi-pass blur with GLX backend.");
      return false;
    }
    glDeleteFramebuffers(1, &fbo);
  }

  {
    char *lc_numeric_old = strdup(setlocale(LC_NUMERIC, NULL));
    // Enforce LC_NUMERIC locale "C" here to make sure decimal point is sane
    // Thanks to hiciu for reporting.
    setlocale(LC_NUMERIC, "C");

    static const char *FRAG_SHADER_PREFIX =
      "#version 110\n"
      "%s"  // extensions
      "uniform float offset;\n"
      "uniform vec2 halfpixel;\n"
      "uniform vec2 fulltex;\n"
      "uniform %s tex_scr;\n" // sampler2D | sampler2DRect
      "vec4 clamp_tex(vec2 uv)\n"
      "{\n"
      "  return %s(tex_scr, clamp(uv, vec2(0), fulltex));\n" // texture2D | texture2DRect
      "}\n"
      "\n"
      "void main()\n"
      "{\n"
      "  vec2 uv = (gl_TexCoord[0].xy / fulltex);\n"
      "  \n";

    // Fragment shader (Dual Kawase Blur) - Downsample
    static const char *FRAG_SHADER_KAWASE_DOWN =
      "  vec4 sum = clamp_tex(uv) * 4.0;\n"
      "  sum += clamp_tex(uv - halfpixel.xy * offset);\n"
      "  sum += clamp_tex(uv + halfpixel.xy * offset);\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x, -halfpixel.y) * offset);\n"
      "  sum += clamp_tex(uv - vec2(halfpixel.x, -halfpixel.y) * offset);\n"
      "\n"
      "  gl_FragColor = sum / 8.0;\n"
      "}\n";

    // Fragment shader (Dual Kawase Blur) - Upsample
    static const char *FRAG_SHADER_KAWASE_UP =
      "  vec4 sum = clamp_tex(uv + vec2(-halfpixel.x * 2.0, 0.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(-halfpixel.x, halfpixel.y) * offset) * 2.0;\n"
      "  sum += clamp_tex(uv + vec2(0.0, halfpixel.y * 2.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x, halfpixel.y) * offset) * 2.0;\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x * 2.0, 0.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(halfpixel.x, -halfpixel.y) * offset) * 2.0;\n"
      "  sum += clamp_tex(uv + vec2(0.0, -halfpixel.y * 2.0) * offset);\n"
      "  sum += clamp_tex(uv + vec2(-halfpixel.x, -halfpixel.y) * offset) * 2.0;\n"
      "\n"
      "  gl_FragColor = sum / 12.0;\n"
      "}\n";

    const bool use_texture_rect = !ps->psglx->has_texture_non_power_of_two;
    const char *sampler_type = (use_texture_rect ?
        "sampler2DRect": "sampler2D");
    const char *texture_func = (use_texture_rect ?
        "texture2DRect": "texture2D");
    char *extension = strdup("");
    if (use_texture_rect)
      mstrextend(&extension, "#extension GL_ARB_texture_rectangle : require\n");

    // Build kawase downsample shader
    glx_blur_pass_t *down_pass = &ps->psglx->blur_passes[0];
    {
      int len = strlen(FRAG_SHADER_PREFIX) + strlen(extension) + strlen(sampler_type) + strlen(texture_func) + strlen(FRAG_SHADER_KAWASE_DOWN) + 1;
      char *shader_str = calloc(len, sizeof(char));
      if (!shader_str) {
        log_error("(): Failed to allocate %d bytes for shader string.", len);
        return false;
      }

      char *pc = shader_str;
      sprintf(pc, FRAG_SHADER_PREFIX, extension, sampler_type, texture_func);
      pc += strlen(pc);
      assert(strlen(shader_str) < len);

      sprintf(pc, FRAG_SHADER_KAWASE_DOWN);
      assert(strlen(shader_str) < len);
      down_pass->frag_shader = glx_create_shader(GL_FRAGMENT_SHADER, shader_str);
      free(shader_str);

      if (!down_pass->frag_shader) {
        log_error("(): Failed to create kawase downsample fragment shader.");
        return false;
      }

      // Build program
      down_pass->prog = glx_create_program(&down_pass->frag_shader, 1);
      if (!down_pass->prog) {
        log_error("(): Failed to create GLSL program.");
        return false;
      }

      // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      down_pass->target = glGetUniformLocation(down_pass->prog, name); \
      if (down_pass->target < 0) { \
        log_error("(): Failed to get location of kawase downsample uniform '" name "'. Might be troublesome."); \
      } \
    }
      P_GET_UNIFM_LOC("offset", unifm_offset);
      P_GET_UNIFM_LOC("halfpixel", unifm_halfpixel);
      P_GET_UNIFM_LOC("fulltex", unifm_fulltex);
#undef P_GET_UNIFM_LOC
    }

    // Build kawase downsample shader
    glx_blur_pass_t *up_pass = &ps->psglx->blur_passes[1];
    {
      int len = strlen(FRAG_SHADER_PREFIX) + strlen(extension) + strlen(sampler_type) + strlen(texture_func) + strlen(FRAG_SHADER_KAWASE_UP) + 1;
      char *shader_str = calloc(len, sizeof(char));
      if (!shader_str) {
        log_error("(): Failed to allocate %d bytes for shader string.", len);
        return false;
      }

      char *pc = shader_str;
      sprintf(pc, FRAG_SHADER_PREFIX, extension, sampler_type, texture_func);
      pc += strlen(pc);
      assert(strlen(shader_str) < len);

      sprintf(pc, FRAG_SHADER_KAWASE_UP);
      assert(strlen(shader_str) < len);
      up_pass->frag_shader = glx_create_shader(GL_FRAGMENT_SHADER, shader_str);
      free(shader_str);

      if (!up_pass->frag_shader) {
        log_error("(): Failed to create kawase upsample fragment shader.");
        return false;
      }

      // Build program
      up_pass->prog = glx_create_program(&up_pass->frag_shader, 1);
      if (!up_pass->prog) {
        log_error("(): Failed to create GLSL program.");
        return false;
      }

      // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      up_pass->target = glGetUniformLocation(up_pass->prog, name); \
      if (up_pass->target < 0) { \
        log_error("(): Failed to get location of kawase upsample uniform '" name "'. Might be troublesome."); \
      } \
    }
      P_GET_UNIFM_LOC("offset", unifm_offset);
      P_GET_UNIFM_LOC("halfpixel", unifm_halfpixel);
      P_GET_UNIFM_LOC("fulltex", unifm_fulltex);
#undef P_GET_UNIFM_LOC
    }

    free(extension);

    // Restore LC_NUMERIC
    setlocale(LC_NUMERIC, lc_numeric_old);
    free(lc_numeric_old);
  }

  glx_check_err(ps);

  return true;}

bool
glx_init_blur(session_t *ps) {
  switch (ps->o.blur_method) {
    case BLRMTHD_CONV:
      return glx_init_conv_blur(ps);
    case BLRMTHD_KAWASE:
      return glx_init_kawase_blur(ps);
    default:
      return false;
  }
}

/**
 * Load a GLSL main program from shader strings.
 */
bool
glx_load_prog_main(session_t *ps,
    const char *vshader_str, const char *fshader_str,
    glx_prog_main_t *pprogram) {
  assert(pprogram);

  // Build program
  pprogram->prog = glx_create_program_from_str(vshader_str, fshader_str);
  if (!pprogram->prog) {
    log_error("Failed to create GLSL program.");
    return false;
  }

  // Get uniform addresses
#define P_GET_UNIFM_LOC(name, target) { \
      pprogram->target = glGetUniformLocation(pprogram->prog, name); \
      if (pprogram->target < 0) { \
        log_error("Failed to get location of uniform '" name "'. Might be troublesome."); \
      } \
    }
  P_GET_UNIFM_LOC("opacity", unifm_opacity);
  P_GET_UNIFM_LOC("invert_color", unifm_invert_color);
  P_GET_UNIFM_LOC("tex", unifm_tex);
#undef P_GET_UNIFM_LOC

  glx_check_err(ps);

  return true;
}

/**
 * Bind a X pixmap to an OpenGL texture.
 */
bool
glx_bind_pixmap(session_t *ps, glx_texture_t **pptex, xcb_pixmap_t pixmap,
    unsigned width, unsigned height, unsigned depth) {
  if (ps->o.backend != BKEND_GLX && ps->o.backend != BKEND_XR_GLX_HYBRID)
    return true;

  if (!pixmap) {
    log_error("Binding to an empty pixmap %#010x. This can't work.", pixmap);
    return false;
  }

  glx_texture_t *ptex = *pptex;
  bool need_release = true;

  // Allocate structure
  if (!ptex) {
    static const glx_texture_t GLX_TEX_DEF = {
      .texture = 0,
      .glpixmap = 0,
      .pixmap = 0,
      .target = 0,
      .width = 0,
      .height = 0,
      .depth = 0,
      .y_inverted = false,
    };

    ptex = cmalloc(glx_texture_t);
    allocchk(ptex);
    memcpy(ptex, &GLX_TEX_DEF, sizeof(glx_texture_t));
    *pptex = ptex;
  }

  // Release pixmap if parameters are inconsistent
  if (ptex->texture && ptex->pixmap != pixmap) {
    glx_release_pixmap(ps, ptex);
  }

  // Create GLX pixmap
  if (!ptex->glpixmap) {
    need_release = false;

    // Retrieve pixmap parameters, if they aren't provided
    if (!(width && height && depth)) {
      Window rroot = None;
      int rx = 0, ry = 0;
      unsigned rbdwid = 0;
      if (!XGetGeometry(ps->dpy, pixmap, &rroot, &rx, &ry,
            &width, &height, &rbdwid, &depth)) {
        log_error("Failed to query info of pixmap %#010x.", pixmap);
        return false;
      }
      if (depth > OPENGL_MAX_DEPTH) {
        log_error("Requested depth %d higher than %d.", depth,
                  OPENGL_MAX_DEPTH);
        return false;
      }
    }

    const glx_fbconfig_t *pcfg = ps->psglx->fbconfigs[depth];
    if (!pcfg) {
      log_error("Couldn't find FBConfig with requested depth %d.", depth);
      return false;
    }

    // Determine texture target, copied from compiz
    // The assumption we made here is the target never changes based on any
    // pixmap-specific parameters, and this may change in the future
    GLenum tex_tgt = 0;
    if (GLX_TEXTURE_2D_BIT_EXT & pcfg->texture_tgts
        && ps->psglx->has_texture_non_power_of_two)
      tex_tgt = GLX_TEXTURE_2D_EXT;
    else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & pcfg->texture_tgts)
      tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
    else if (!(GLX_TEXTURE_2D_BIT_EXT & pcfg->texture_tgts))
      tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
    else
      tex_tgt = GLX_TEXTURE_2D_EXT;

    log_debug("depth %d, tgt %#x, rgba %d", depth, tex_tgt,
              (GLX_TEXTURE_FORMAT_RGBA_EXT == pcfg->texture_fmt));

    GLint attrs[] = {
        GLX_TEXTURE_FORMAT_EXT,
        pcfg->texture_fmt,
        GLX_TEXTURE_TARGET_EXT,
        tex_tgt,
        0,
    };

    ptex->glpixmap = glXCreatePixmap(ps->dpy, pcfg->cfg, pixmap, attrs);
    ptex->pixmap = pixmap;
    ptex->target = (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D:
        GL_TEXTURE_RECTANGLE);
    ptex->width = width;
    ptex->height = height;
    ptex->depth = depth;
    ptex->y_inverted = pcfg->y_inverted;
  }
  if (!ptex->glpixmap) {
    log_error("Failed to allocate GLX pixmap.");
    return false;
  }

  glEnable(ptex->target);

  // Create texture
  if (!ptex->texture) {
    need_release = false;

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(ptex->target, texture);

    glTexParameteri(ptex->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(ptex->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(ptex->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(ptex->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(ptex->target, 0);

    ptex->texture = texture;
  }
  if (!ptex->texture) {
    log_error("Failed to allocate texture.");
    return false;
  }

  glBindTexture(ptex->target, ptex->texture);

  // The specification requires rebinding whenever the content changes...
  // We can't follow this, too slow.
  if (need_release)
    ps->psglx->glXReleaseTexImageProc(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);

  ps->psglx->glXBindTexImageProc(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT, NULL);

  // Cleanup
  glBindTexture(ptex->target, 0);
  glDisable(ptex->target);

  glx_check_err(ps);

  return true;
}

/**
 * @brief Release binding of a texture.
 */
void
glx_release_pixmap(session_t *ps, glx_texture_t *ptex) {
  // Release binding
  if (ptex->glpixmap && ptex->texture) {
    glBindTexture(ptex->target, ptex->texture);
    ps->psglx->glXReleaseTexImageProc(ps->dpy, ptex->glpixmap, GLX_FRONT_LEFT_EXT);
    glBindTexture(ptex->target, 0);
  }

  // Free GLX Pixmap
  if (ptex->glpixmap) {
    glXDestroyPixmap(ps->dpy, ptex->glpixmap);
    ptex->glpixmap = 0;
  }

  glx_check_err(ps);
}

/**
 * Preprocess function before start painting.
 */
void
glx_paint_pre(session_t *ps, region_t *preg) {
  ps->psglx->z = 0.0;
  // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Get buffer age
  bool trace_damage = (ps->o.glx_swap_method < 0 || ps->o.glx_swap_method > 1);

  // Trace raw damage regions
  region_t newdamage;
  pixman_region32_init(&newdamage);
  if (trace_damage)
    copy_region(&newdamage, preg);

  // We use GLX buffer_age extension to decide which pixels in
  // the back buffer is reusable, and limit our redrawing
  int buffer_age = 0;

  // Query GLX_EXT_buffer_age for buffer age
  if (ps->o.glx_swap_method == SWAPM_BUFFER_AGE) {
    unsigned val = 0;
    glXQueryDrawable(ps->dpy, get_tgt_window(ps),
        GLX_BACK_BUFFER_AGE_EXT, &val);
    buffer_age = val;
  }

  // Buffer age too high
  if (buffer_age > CGLX_MAX_BUFFER_AGE + 1)
    buffer_age = 0;

  assert(buffer_age >= 0);

  if (buffer_age) {
    // Determine paint area
      for (int i = 0; i < buffer_age - 1; ++i)
        pixman_region32_union(preg, preg, &ps->all_damage_last[i]);
  } else
    // buffer_age == 0 means buffer age is not available, paint everything
    copy_region(preg, &ps->screen_reg);

  if (trace_damage) {
    // XXX use a circular queue instead of memmove
    pixman_region32_fini(&ps->all_damage_last[CGLX_MAX_BUFFER_AGE - 1]);
    memmove(ps->all_damage_last + 1, ps->all_damage_last,
        (CGLX_MAX_BUFFER_AGE - 1) * sizeof(region_t));
    ps->all_damage_last[0] = newdamage;
  }

  glx_set_clip(ps, preg);

#ifdef DEBUG_GLX_PAINTREG
  glx_render_color(ps, 0, 0, ps->root_width, ps->root_height, 0, *preg, NULL);
#endif

  glx_check_err(ps);
}

/**
 * Set clipping region on the target window.
 */
void
glx_set_clip(session_t *ps, const region_t *reg) {
  // Quit if we aren't using stencils
  if (ps->o.glx_no_stencil)
    return;

  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);

  if (!reg)
    return;

  int nrects;
  const rect_t *rects = pixman_region32_rectangles((region_t *)reg, &nrects);

  if (nrects == 1) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(rects[0].x1, ps->root_height-rects[0].y2,
        rects[0].x2-rects[0].x1, rects[0].y2-rects[0].y1);
  }

  glx_check_err(ps);
}

#define P_PAINTREG_START(var) \
  region_t reg_new; \
  int nrects; \
  const rect_t *rects; \
  pixman_region32_init_rect(&reg_new, dx, dy, width, height); \
  pixman_region32_intersect(&reg_new, &reg_new, (region_t *)reg_tgt); \
  rects = pixman_region32_rectangles(&reg_new, &nrects); \
  glBegin(GL_QUADS); \
 \
  for (int ri = 0; ri < nrects; ++ri) { \
    rect_t var = rects[ri];

#define P_PAINTREG_END() \
  } \
  glEnd(); \
 \
  pixman_region32_fini(&reg_new);

static inline GLuint
glx_gen_texture(session_t *ps, GLenum tex_tgt, int width, int height) {
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (!tex) return 0;
  glEnable(tex_tgt);
  glBindTexture(tex_tgt, tex);
  glTexParameteri(tex_tgt, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(tex_tgt, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(tex_tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(tex_tgt, 0, GL_RGB, width, height, 0, GL_RGB,
      GL_UNSIGNED_BYTE, NULL);
  glBindTexture(tex_tgt, 0);

  return tex;
}

static inline void
glx_copy_region_to_tex(session_t *ps, GLenum tex_tgt, int basex, int basey,
    int dx, int dy, int width, int height) {
  if (width > 0 && height > 0)
    glCopyTexSubImage2D(tex_tgt, 0, dx - basex, dy - basey,
        dx, ps->root_height - dy - height, width, height);
}

/**
 * Blur contents in a particular region.
 *
 * XXX seems to be way to complex for what it does
 */
bool
glx_conv_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor_center,
    const region_t *reg_tgt,
    glx_blur_cache_t *pbc) {
  const bool more_passes = ps->psglx->blur_passes[1].prog;
  const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
  const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
  bool ret = false;

  // Calculate copy region size
  glx_blur_cache_t ibc = { .width = 0, .height = 0 };
  if (!pbc)
    pbc = &ibc;

  int mdx = dx, mdy = dy, mwidth = width, mheight = height;
  //log_trace("%d, %d, %d, %d", mdx, mdy, mwidth, mheight);

  /*
  if (ps->o.resize_damage > 0) {
    int inc_x = 0, inc_y = 0;
    for (int i = 0; i < MAX_BLUR_PASS; ++i) {
      XFixed *kern = ps->o.blur_kerns[i];
      if (!kern) break;
      inc_x += XFIXED_TO_DOUBLE(kern[0]) / 2;
      inc_y += XFIXED_TO_DOUBLE(kern[1]) / 2;
    }
    inc_x = min_i(ps->o.resize_damage, inc_x);
    inc_y = min_i(ps->o.resize_damage, inc_y);

    mdx = max_i(dx - inc_x, 0);
    mdy = max_i(dy - inc_y, 0);
    int mdx2 = min_i(dx + width + inc_x, ps->root_width),
        mdy2 = min_i(dy + height + inc_y, ps->root_height);
    mwidth = mdx2 - mdx;
    mheight = mdy2 - mdy;
  }
  */

  GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
  if (ps->psglx->has_texture_non_power_of_two)
    tex_tgt = GL_TEXTURE_2D;

  // Free textures if size inconsistency discovered
  if (mwidth != pbc->width || mheight != pbc->height)
    free_glx_bc_resize(ps, pbc);

  // Generate FBO and textures if needed
  if (!pbc->textures[0])
    pbc->textures[0] = glx_gen_texture(ps, tex_tgt, mwidth, mheight);
  GLuint tex_scr = pbc->textures[0];
  if (more_passes && !pbc->textures[1])
    pbc->textures[1] = glx_gen_texture(ps, tex_tgt, mwidth, mheight);
  pbc->width = mwidth;
  pbc->height = mheight;
  GLuint tex_scr2 = pbc->textures[1];
  if (more_passes && !pbc->fbo)
    glGenFramebuffers(1, &pbc->fbo);
  const GLuint fbo = pbc->fbo;

  if (!tex_scr || (more_passes && !tex_scr2)) {
    log_error("Failed to allocate texture.");
    goto glx_conv_blur_dst_end;
  }
  if (more_passes && !fbo) {
    log_error("Failed to allocate framebuffer.");
    goto glx_conv_blur_dst_end;
  }

  // Read destination pixels into a texture
  glEnable(tex_tgt);
  glBindTexture(tex_tgt, tex_scr);
  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, mheight);
  /*
  if (tex_scr2) {
    glBindTexture(tex_tgt, tex_scr2);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, dx - mdx);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, dy + height,
        mwidth, mdy + mheight - dy - height);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, dy, dx - mdx, height);
    glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, dx + width, dy,
        mdx + mwidth - dx - width, height);
  } */

  // Texture scaling factor
  GLfloat texfac_x = 1.0f, texfac_y = 1.0f;
  if (GL_TEXTURE_2D == tex_tgt) {
    texfac_x /= mwidth;
    texfac_y /= mheight;
  }

  // Paint it back
  if (more_passes) {
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
  }

  bool last_pass = false;
  for (int i = 0; !last_pass; ++i) {
    last_pass = !ps->psglx->blur_passes[i + 1].prog;
    assert(i < MAX_BLUR_PASS - 1);
    const glx_blur_pass_t *ppass = &ps->psglx->blur_passes[i];
    assert(ppass->prog);

    assert(tex_scr);
    glBindTexture(tex_tgt, tex_scr);

    if (!last_pass) {
      static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
          GL_TEXTURE_2D, tex_scr2, 0);
      glDrawBuffers(1, DRAWBUFS);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
          != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Framebuffer attachment failed.");
        goto glx_conv_blur_dst_end;
      }
    }
    else {
      static const GLenum DRAWBUFS[2] = { GL_BACK };
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDrawBuffers(1, DRAWBUFS);
      if (have_scissors)
        glEnable(GL_SCISSOR_TEST);
      if (have_stencil)
        glEnable(GL_STENCIL_TEST);
    }

    // Color negation for testing...
    // glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    // glTexEnvf(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
    // glTexEnvf(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glUseProgram(ppass->prog);
    if (ppass->unifm_offset_x >= 0)
      glUniform1f(ppass->unifm_offset_x, texfac_x);
    if (ppass->unifm_offset_y >= 0)
      glUniform1f(ppass->unifm_offset_y, texfac_y);
    if (ppass->unifm_factor_center >= 0)
      glUniform1f(ppass->unifm_factor_center, factor_center);

    {
      P_PAINTREG_START(crect) {
        const GLfloat rx = (crect.x1 - mdx) * texfac_x;
        const GLfloat ry = (mheight - (crect.y1 - mdy)) * texfac_y;
        const GLfloat rxe = rx + (crect.x2 - crect.x1) * texfac_x;
        const GLfloat rye = ry - (crect.y2 - crect.y1) * texfac_y;
        GLfloat rdx = crect.x1 - mdx;
        GLfloat rdy = mheight - crect.y1 + mdy;
        if (last_pass) {
          rdx = crect.x1;
          rdy = ps->root_height - crect.y1;
        }
        GLfloat rdxe = rdx + (crect.x2 - crect.x1);
        GLfloat rdye = rdy - (crect.y2 - crect.y1);

        //log_trace("%f, %f, %f, %f -> %f, %f, %f, %f", rx, ry, rxe, rye, rdx,
        //          rdy, rdxe, rdye);

        glTexCoord2f(rx, ry);
        glVertex3f(rdx, rdy, z);

        glTexCoord2f(rxe, ry);
        glVertex3f(rdxe, rdy, z);

        glTexCoord2f(rxe, rye);
        glVertex3f(rdxe, rdye, z);

        glTexCoord2f(rx, rye);
        glVertex3f(rdx, rdye, z);
      } P_PAINTREG_END();
    }

    glUseProgram(0);

    // Swap tex_scr and tex_scr2
    {
      GLuint tmp = tex_scr2;
      tex_scr2 = tex_scr;
      tex_scr = tmp;
    }
  }

  ret = true;

glx_conv_blur_dst_end:
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(tex_tgt, 0);
  glDisable(tex_tgt);
  if (have_scissors)
    glEnable(GL_SCISSOR_TEST);
  if (have_stencil)
    glEnable(GL_STENCIL_TEST);

  if (&ibc == pbc) {
    free_glx_bc(ps, pbc);
  }

  glx_check_err(ps);

  return ret;
}

bool
glx_kawase_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    const region_t *reg_tgt,
    glx_blur_cache_t *pbc) {
  const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
  const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);
  bool ret = false;

  int iterations = ps->o.blur_strength.iterations;
  float offset = ps->o.blur_strength.offset;

  // Calculate copy region size
  glx_blur_cache_t ibc = { .width = 0, .height = 0 };
  if (!pbc)
    pbc = &ibc;

  int mdx = dx, mdy = dy, mwidth = width, mheight = height;

  GLenum tex_tgt = GL_TEXTURE_RECTANGLE;
  if (ps->psglx->has_texture_non_power_of_two)
    tex_tgt = GL_TEXTURE_2D;

  // Free textures if size inconsistency discovered
  if (mwidth != pbc->width || mheight != pbc->height)
    free_glx_bc_resize(ps, pbc);

  // Generate FBO and textures if needed
  if (!pbc->textures[0])
    pbc->textures[0] = glx_gen_texture(ps, tex_tgt, mwidth, mheight);
  GLuint tex_scr = pbc->textures[0];

  // Check if we can scale down blur_strength.iterations
  while ((mwidth / (1 << (iterations-1))) < 1 || (mheight / (1 << (iterations-1))) < 1)
    --iterations;

  assert(iterations < MAX_BLUR_PASS);
  for (int i = 1; i <= iterations; i++) {    if (!pbc->textures[i])
      pbc->textures[i] = glx_gen_texture(ps, tex_tgt, mwidth / (1 << (i-1)), mheight / (1 << (i-1)));
  }

  pbc->width = mwidth;
  pbc->height = mheight;

  if (!pbc->fbo)
    glGenFramebuffers(1, &pbc->fbo);
  const GLuint fbo = pbc->fbo;

  if (!tex_scr) {
    log_error("(): Failed to allocate texture.");
    goto glx_kawase_blur_dst_end;
  }
  for (int i = 1; i <= iterations; i++) {
    if (!pbc->textures[i]) {
      log_error("(): Failed to allocate additional textures.");
      goto glx_kawase_blur_dst_end;
    }
  }
  if (!fbo) {
    log_error("(): Failed to allocate framebuffer.");
    goto glx_kawase_blur_dst_end;
  }

  // Read destination pixels into a texture
  glEnable(tex_tgt);
  glBindTexture(tex_tgt, tex_scr);
  glx_copy_region_to_tex(ps, tex_tgt, mdx, mdy, mdx, mdy, mwidth, mheight);

  // Paint it back
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);

  // First pass(es): Kawase Downsample
  for (int i = 1; i <= iterations; i++) {
    const glx_blur_pass_t *down_pass = &ps->psglx->blur_passes[0];
    assert(down_pass->prog);

    int tex_width = mwidth / (1 << (i-1)), tex_height = mheight / (1 << (i-1));
    GLuint tex_src2 = pbc->textures[i - 1];
    GLuint tex_dest = pbc->textures[i];

    assert(tex_src2);
    assert(tex_dest);
    glBindTexture(tex_tgt, tex_src2);

    static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_TEXTURE_2D, tex_dest, 0);
    glDrawBuffers(1, DRAWBUFS);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      log_error("(): Framebuffer attachment failed.");
      goto glx_kawase_blur_dst_end;
    }

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glUseProgram(down_pass->prog);
    if (down_pass->unifm_offset >= 0)
        glUniform1f(down_pass->unifm_offset, offset);
    if (down_pass->unifm_halfpixel >= 0)
        glUniform2f(down_pass->unifm_halfpixel, 0.5 / tex_width, 0.5 / tex_height);
    if (down_pass->unifm_fulltex >= 0)
        glUniform2f(down_pass->unifm_fulltex, tex_width, tex_height);

    // Start actual rendering
    P_PAINTREG_START(crect);
    {
      const GLfloat rx = crect.x1 - mdx;
      const GLfloat ry = mheight - (crect.y1 - mdy);
      const GLfloat rxe = rx + (crect.x2 - crect.x1);
      const GLfloat rye = ry - (crect.y2 - crect.y1);

      glTexCoord2f(rx, ry);
      glVertex3f(rx, ry, z);

      glTexCoord2f(rxe, ry);
      glVertex3f(rxe, ry, z);

      glTexCoord2f(rxe, rye);
      glVertex3f(rxe, rye, z);

      glTexCoord2f(rx, rye);
      glVertex3f(rx, rye, z);
    }
    P_PAINTREG_END();
  }

  // Second pass(es): Kawase Upsample
  for (int i = iterations; i >= 1; i--) {
    const glx_blur_pass_t *up_pass = &ps->psglx->blur_passes[1];
    bool is_last = (i == 1);
    assert(up_pass->prog);

    int tex_width = mwidth / (1 << (i-2)), tex_height = mheight / (1 << (i-2));
    if (is_last) {
      tex_width = mwidth, tex_height = mheight;
    }
    GLuint tex_src2 = pbc->textures[i];
    GLuint tex_dest = pbc->textures[i - 1];

    assert(tex_src2);
    assert(tex_dest);
    glBindTexture(tex_tgt, tex_src2);

    if (!is_last) {
      static const GLenum DRAWBUFS[2] = { GL_COLOR_ATTACHMENT0 };
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, tex_dest, 0);
      glDrawBuffers(1, DRAWBUFS);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("(): Framebuffer attachment failed.");
        goto glx_kawase_blur_dst_end;
      }
    } else {
      static const GLenum DRAWBUFS[2] = { GL_BACK };
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDrawBuffers(1, DRAWBUFS);
      if (have_scissors)
        glEnable(GL_SCISSOR_TEST);
      if (have_stencil)
        glEnable(GL_STENCIL_TEST);
    }

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glUseProgram(up_pass->prog);
    if (up_pass->unifm_offset >= 0)
        glUniform1f(up_pass->unifm_offset, offset);
    if (up_pass->unifm_halfpixel >= 0)
        glUniform2f(up_pass->unifm_halfpixel, 0.5 / tex_width, 0.5 / tex_height);
    if (up_pass->unifm_fulltex >= 0)
        glUniform2f(up_pass->unifm_fulltex, tex_width, tex_height);

    // Start actual rendering
    P_PAINTREG_START(crect);
    {
      const GLfloat rx = crect.x1 - mdx;
      const GLfloat ry = mheight - (crect.y1 - mdy);
      const GLfloat rxe = rx + (crect.x2 - crect.x1);
      const GLfloat rye = ry - (crect.y2 - crect.y1);
      GLfloat rdx = rx;
      GLfloat rdy = ry;
      GLfloat rdxe = rxe;
      GLfloat rdye = rye;

      if (is_last) {
        rdx = crect.x1;
        rdy = ps->root_height - crect.y1;
        rdxe = rdx + (crect.x2 - crect.x1);
        rdye = rdy - (crect.y2 - crect.y1);
      }

      glTexCoord2f(rx, ry);
      glVertex3f(rdx, rdy, z);

      glTexCoord2f(rxe, ry);
      glVertex3f(rdxe, rdy, z);

      glTexCoord2f(rxe, rye);
      glVertex3f(rdxe, rdye, z);

      glTexCoord2f(rx, rye);
      glVertex3f(rdx, rdye, z);
    }
    P_PAINTREG_END();
  }

  glUseProgram(0);
  ret = true;

glx_kawase_blur_dst_end:
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(tex_tgt, 0);
  glDisable(tex_tgt);
  if (have_scissors)
    glEnable(GL_SCISSOR_TEST);
  if (have_stencil)
    glEnable(GL_STENCIL_TEST);

  if (&ibc == pbc) {
    free_glx_bc(ps, pbc);
  }

  return ret;
}

bool
glx_blur_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor_center,
    const region_t *reg_tgt,
    glx_blur_cache_t *pbc) {
  assert(ps->psglx->blur_passes[0].prog);

  bool ret;
  switch (ps->o.blur_method) {
    case BLRMTHD_CONV:
      ret = glx_conv_blur_dst(ps, dx, dy, width, height, z,
        factor_center, reg_tgt, pbc);
      break;
    case BLRMTHD_KAWASE:
      ret = glx_kawase_blur_dst(ps, dx, dy, width, height, z,
        reg_tgt, pbc);
      break;
    default:
      ret = false;
      break;
  }
  glx_check_err(ps);

  return ret;
}

bool
glx_dim_dst(session_t *ps, int dx, int dy, int width, int height, float z,
    GLfloat factor, const region_t *reg_tgt) {
  // It's possible to dim in glx_render(), but it would be over-complicated
  // considering all those mess in color negation and modulation
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(0.0f, 0.0f, 0.0f, factor);

  {
    P_PAINTREG_START(crect) {
      // XXX what does all of these variables mean?
      GLint rdx = crect.x1;
      GLint rdy = ps->root_height - crect.y1;
      GLint rdxe = rdx + (crect.x2 - crect.x1);
      GLint rdye = rdy - (crect.y2 - crect.y1);

      glVertex3i(rdx, rdy, z);
      glVertex3i(rdxe, rdy, z);
      glVertex3i(rdxe, rdye, z);
      glVertex3i(rdx, rdye, z);
    }
    P_PAINTREG_END();
  }

  glEnd();

  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glDisable(GL_BLEND);

  glx_check_err(ps);

  return true;
}

/**
 * @brief Render a region with texture data.
 */
bool
glx_render(session_t *ps, const glx_texture_t *ptex,
    int x, int y, int dx, int dy, int width, int height, int z,
    double opacity, bool argb, bool neg,
    const region_t *reg_tgt, const glx_prog_main_t *pprogram
    ) {
  if (!ptex || !ptex->texture) {
    log_error("Missing texture.");
    return false;
  }

  argb = argb || (GLX_TEXTURE_FORMAT_RGBA_EXT ==
      ps->psglx->fbconfigs[ptex->depth]->texture_fmt);
  const bool has_prog = pprogram && pprogram->prog;
  bool dual_texture = false;

  // It's required by legacy versions of OpenGL to enable texture target
  // before specifying environment. Thanks to madsy for telling me.
  glEnable(ptex->target);

  // Enable blending if needed
  if (opacity < 1.0 || argb) {

    glEnable(GL_BLEND);

    // Needed for handling opacity of ARGB texture
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // This is all weird, but X Render is using premultiplied ARGB format, and
    // we need to use those things to correct it. Thanks to derhass for help.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(opacity, opacity, opacity, opacity);
  }

  if (!has_prog)
  {
    // The default, fixed-function path
    // Color negation
    if (neg) {
      // Simple color negation
      if (!glIsEnabled(GL_BLEND)) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(GL_COPY_INVERTED);
      }
      // ARGB texture color negation
      else if (argb) {
        dual_texture = true;

        // Use two texture stages because the calculation is too complicated,
        // thanks to madsy for providing code
        // Texture stage 0
        glActiveTexture(GL_TEXTURE0);

        // Negation for premultiplied color: color = A - C
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

        // Pass texture alpha through
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

        // Texture stage 1
        glActiveTexture(GL_TEXTURE1);
        glEnable(ptex->target);
        glBindTexture(ptex->target, ptex->texture);

        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

        // Modulation with constant factor
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_ALPHA);

        // Modulation with constant factor
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);

        glActiveTexture(GL_TEXTURE0);
      }
      // RGB blend color negation
      else {
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

        // Modulation with constant factor
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

        // Modulation with constant factor
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
      }
    }
  }
  else {
    // Programmable path
    assert(pprogram->prog);
    glUseProgram(pprogram->prog);
    if (pprogram->unifm_opacity >= 0)
      glUniform1f(pprogram->unifm_opacity, opacity);
    if (pprogram->unifm_invert_color >= 0)
      glUniform1i(pprogram->unifm_invert_color, neg);
    if (pprogram->unifm_tex >= 0)
      glUniform1i(pprogram->unifm_tex, 0);
  }

  //log_trace("Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d", x, y, width, height,
  //          dx, dy, ptex->width, ptex->height, z);

  // Bind texture
  glBindTexture(ptex->target, ptex->texture);
  if (dual_texture) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(ptex->target, ptex->texture);
    glActiveTexture(GL_TEXTURE0);
  }

  // Painting
  {
    P_PAINTREG_START(crect) {
      // XXX explain these variables
      GLfloat rx = (double) (crect.x1 - dx + x);
      GLfloat ry = (double) (crect.y1 - dy + y);
      GLfloat rxe = rx + (double) (crect.x2 - crect.x1);
      GLfloat rye = ry + (double) (crect.y2 - crect.y1);
      // Rectangle textures have [0-w] [0-h] while 2D texture has [0-1] [0-1]
      // Thanks to amonakov for pointing out!
      if (GL_TEXTURE_2D == ptex->target) {
        rx = rx / ptex->width;
        ry = ry / ptex->height;
        rxe = rxe / ptex->width;
        rye = rye / ptex->height;
      }
      GLint rdx = crect.x1;
      GLint rdy = ps->root_height - crect.y1;
      GLint rdxe = rdx + (crect.x2 - crect.x1);
      GLint rdye = rdy - (crect.y2 - crect.y1);

      // Invert Y if needed, this may not work as expected, though. I don't
      // have such a FBConfig to test with.
      if (!ptex->y_inverted) {
        ry = 1.0 - ry;
        rye = 1.0 - rye;
      }

      //log_trace("Rect %d: %f, %f, %f, %f -> %d, %d, %d, %d", ri, rx, ry, rxe, rye,
      //          rdx, rdy, rdxe, rdye);

#define P_TEXCOORD(cx, cy) { \
  if (dual_texture) { \
    glMultiTexCoord2f(GL_TEXTURE0, cx, cy); \
    glMultiTexCoord2f(GL_TEXTURE1, cx, cy); \
  } \
  else glTexCoord2f(cx, cy); \
}
      P_TEXCOORD(rx, ry);
      glVertex3i(rdx, rdy, z);

      P_TEXCOORD(rxe, ry);
      glVertex3i(rdxe, rdy, z);

      P_TEXCOORD(rxe, rye);
      glVertex3i(rdxe, rdye, z);

      P_TEXCOORD(rx, rye);
      glVertex3i(rdx, rdye, z);
    } P_PAINTREG_END();
  }

  // Cleanup
  glBindTexture(ptex->target, 0);
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glDisable(GL_BLEND);
  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(ptex->target);

  if (dual_texture) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(ptex->target, 0);
    glDisable(ptex->target);
    glActiveTexture(GL_TEXTURE0);
  }

  if (has_prog)
    glUseProgram(0);

  glx_check_err(ps);

  return true;
}

/**
 * @brief Get tightly packed RGB888 data from GL front buffer.
 *
 * Don't expect any sort of decent performance.
 *
 * @returns tightly packed RGB888 data of the size of the screen,
 *          to be freed with `free()`
 */
unsigned char *
glx_take_screenshot(session_t *ps, int *out_length) {
  int length = 3 * ps->root_width * ps->root_height;
  GLint unpack_align_old = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align_old);
  assert(unpack_align_old > 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  auto buf = ccalloc(length, unsigned char);
  glReadBuffer(GL_FRONT);
  glReadPixels(0, 0, ps->root_width, ps->root_height, GL_RGB,
      GL_UNSIGNED_BYTE, buf);
  glReadBuffer(GL_BACK);
  glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_align_old);
  if (out_length)
    *out_length = sizeof(unsigned char) * length;
  return buf;
}

GLuint
glx_create_shader(GLenum shader_type, const char *shader_str) {
  log_trace("glx_create_shader(): ===\n%s\n===", shader_str);

  bool success = false;
  GLuint shader = glCreateShader(shader_type);
  if (!shader) {
    log_error("Failed to create shader with type %#x.", shader_type);
    goto glx_create_shader_end;
  }
  glShaderSource(shader, 1, &shader_str, NULL);
  glCompileShader(shader);

  // Get shader status
  {
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (GL_FALSE == status) {
      GLint log_len = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
      if (log_len) {
        char log[log_len + 1];
        glGetShaderInfoLog(shader, log_len, NULL, log);
        log_error("Failed to compile shader with type %d: %s", shader_type, log);
      }
      goto glx_create_shader_end;
    }
  }

  success = true;

glx_create_shader_end:
  if (shader && !success) {
    glDeleteShader(shader);
    shader = 0;
  }

  return shader;
}

GLuint
glx_create_program(const GLuint * const shaders, int nshaders) {
  bool success = false;
  GLuint program = glCreateProgram();
  if (!program) {
    log_error("Failed to create program.");
    goto glx_create_program_end;
  }

  for (int i = 0; i < nshaders; ++i)
    glAttachShader(program, shaders[i]);
  glLinkProgram(program);

  // Get program status
  {
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (GL_FALSE == status) {
      GLint log_len = 0;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
      if (log_len) {
        char log[log_len + 1];
        glGetProgramInfoLog(program, log_len, NULL, log);
        log_error("Failed to link program: %s", log);
      }
      goto glx_create_program_end;
    }
  }
  success = true;

glx_create_program_end:
  if (program) {
    for (int i = 0; i < nshaders; ++i)
      glDetachShader(program, shaders[i]);
  }
  if (program && !success) {
    glDeleteProgram(program);
    program = 0;
  }

  return program;
}

/**
 * @brief Create a program from vertex and fragment shader strings.
 */
GLuint
glx_create_program_from_str(const char *vert_shader_str,
    const char *frag_shader_str) {
  GLuint vert_shader = 0;
  GLuint frag_shader = 0;
  GLuint prog = 0;

  if (vert_shader_str)
    vert_shader = glx_create_shader(GL_VERTEX_SHADER, vert_shader_str);
  if (frag_shader_str)
    frag_shader = glx_create_shader(GL_FRAGMENT_SHADER, frag_shader_str);

  GLuint shaders[2];
  unsigned int count = 0;
  if (vert_shader)
    shaders[count++] = vert_shader;
  if (frag_shader)
    shaders[count++] = frag_shader;
  assert(count <= sizeof(shaders) / sizeof(shaders[0]));
  if (count)
    prog = glx_create_program(shaders, count);

  if (vert_shader)
    glDeleteShader(vert_shader);
  if (frag_shader)
    glDeleteShader(frag_shader);

  return prog;
}
