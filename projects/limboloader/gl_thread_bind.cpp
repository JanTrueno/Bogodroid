// gl_thread_bind.cpp -- give Limbo's render thread a GL context.
//
// Limbo runs its renderer on its own "LIMBO game" thread but never calls
// eglMakeCurrent there (verified: the EGL shim logs no such call). On Android
// that works because the context is bound for it; here a GL context is
// per-thread, so every GL call from that thread runs with none current.
// glGetString then returns NULL, and the engine strstr()s GL_EXTENSIONS
// straight into a segfault.
//
// These hooks bind the loader's context the first time a thread touches GL.
// They are registered ahead of symtable_gles2 in so_dynamic_libraries, so the
// game resolves them instead of the real entry points; after the first call on
// a thread the context stays current there and the check costs a thread-local
// read.

#include <stdio.h>
#include <stdint.h>

#include <SDL2/SDL.h>

#include "glad.h"
#include "so_util.h"
#include "egl_sdl.h"

static thread_local bool tls_bound = false;

static void ensure_current()
{
    if (tls_bound)
        return;
    tls_bound = true; // only try once per thread, however it goes

    // Limbo binds its own context on both its "game" and "render" threads
    // (confirmed: it logs "eglMakeCurrent -> ... bound!" for each, and now that
    // eglCreatePbufferSurface_impl hands out a real separate surface instead of
    // sharing the window one, those calls succeed on their own). So check for
    // a context already current before doing anything -- forcing our own bind
    // on top of one the engine already set up correctly would replace it with
    // a mismatched context/surface pair and reintroduce the exact bug this
    // file was written to work around. This is now purely a fallback for a
    // thread that touches GL without ever calling eglMakeCurrent itself.
    static auto real_get_current = (EGLContext (*)())getProc("eglGetCurrentContext");
    if (real_get_current && real_get_current() != EGL_NO_CONTEXT)
        return;

    if (!egl_bind_thread_context())
    {
        printf("limbo: could not bind a GL context on thread %p\n", (void *)SDL_ThreadID());
        fflush(stdout);
        return;
    }
    printf("limbo: GL context bound on thread %p (fallback -- engine did not bind one itself)\n",
           (void *)SDL_ThreadID());
    fflush(stdout);
}

// The engine's first GL calls on the render thread: whichever runs first binds
// the context, and the rest follow on the same thread.
static const GLubyte *glGetString_hook(GLenum name)
{
    ensure_current();
    return glGetString(name);
}

static void glGetIntegerv_hook(GLenum pname, GLint *data)
{
    ensure_current();
    glGetIntegerv(pname, data);
}

static void glViewport_hook(GLint x, GLint y, GLsizei w, GLsizei h)
{
    ensure_current();
    glViewport(x, y, w, h);
}

static void glClear_hook(GLbitfield mask)
{
    ensure_current();
    glClear(mask);
}

// Redirects the game's notion of "the window framebuffer" (id 0) to our
// offscreen present FBO when the game asked for a smaller buffer via
// ANativeWindow_setBuffersGeometry (see get_virtual_present_fbo() in
// egl_sdl.h). Its own FBOs (any nonzero id, e.g. for its blur/blit passes)
// are untouched.
static void glBindFramebuffer_hook(GLenum target, GLuint framebuffer)
{
    ensure_current();
    if (framebuffer == 0)
    {
        GLuint virtual_fbo = get_virtual_present_fbo();
        if (virtual_fbo != 0)
        {
            glBindFramebuffer(target, virtual_fbo);
            return;
        }
    }
    glBindFramebuffer(target, framebuffer);
}

static GLuint glCreateShader_hook(GLenum type)
{
    ensure_current();
    return glCreateShader(type);
}

static void glGenTextures_hook(GLsizei n, GLuint *textures)
{
    ensure_current();
    glGenTextures(n, textures);
}

static void glTexImage2D_hook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
    GLint border, GLenum format, GLenum type, const void *pixels)
{
    ensure_current();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

DynLibFunction symtable_limbo_gl[] = {
    {"glGetString", (uintptr_t)&glGetString_hook},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv_hook},
    {"glViewport", (uintptr_t)&glViewport_hook},
    {"glClear", (uintptr_t)&glClear_hook},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer_hook},
    {"glCreateShader", (uintptr_t)&glCreateShader_hook},
    {"glGenTextures", (uintptr_t)&glGenTextures_hook},
    {"glTexImage2D", (uintptr_t)&glTexImage2D_hook},
    {NULL, (uintptr_t)NULL},
};
