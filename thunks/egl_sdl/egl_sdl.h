#pragma once
#include "glad_egl.h"

void sdl_initialize_gles();
void* getProc(const char* sym);

// Brings up SDL's window/context, captures the EGL handles behind it and
// resolves the EGL/GLES entry points. Normally reached through the game's own
// eglGetDisplay, but a loader must call it before loading a module whose GL
// imports need resolving -- so_resolve_link() patches anything unresolved to a
// crash stub at relocation time and never revisits it. Repeat calls are a
// no-op, so the game's later eglGetDisplay just gets the same display back.
EGLDisplay eglGetDisplay_impl(NativeDisplayType native_display);

// Make a GL context current on the calling thread: the loader's own if it is
// still free, otherwise a fresh one sharing objects with it. For engines that
// render from several threads without binding contexts themselves.
EGLBoolean egl_bind_thread_context();

// Real Android lets an app render into a smaller-than-window buffer via
// ANativeWindow_setBuffersGeometry and has SurfaceFlinger scale it up to the
// real display when compositing -- Limbo uses exactly this (renders at a
// fixed 1024x576 and asks the window for that buffer size) rather than
// scaling the image itself. We have no separate compositor to do that scale
// for us, so ANativeWindow_setBuffersGeometry (ndk.cpp) records the request
// here, and the render-thread GL hooks (gl_thread_bind.cpp) redirect the
// game's "framebuffer 0" to an offscreen FBO of that size; eglSwapBuffers_impl
// then blits it, scaled, into the real window framebuffer before presenting.
extern int g_ana_buffer_w;
extern int g_ana_buffer_h;

// Returns the offscreen FBO the game's framebuffer-0 should be redirected to,
// creating (or recreating, if the requested size changed) it on first use.
// Returns 0 if no buffer-geometry override is active (game gets the real
// window framebuffer as normal).
unsigned int get_virtual_present_fbo();

// Real pixel Y-coordinate where the "top" physical screen ends and the
// "bottom" one begins, when sdl_initialize_gles() combined two SDL displays
// into one stacked window (see egl_sdl.cpp). 0 when there is only one
// display -- callers must check this before treating it as a split point.
extern int dual_screen_split_y;
