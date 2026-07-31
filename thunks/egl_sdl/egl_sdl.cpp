#include "toml++/toml.hpp"
extern toml::table config;
#include "egl_sdl.h"
#include "SDL2/SDL.h"
#include "glad_egl.h"
#include "gles2.h"
#include "logging.h"
#include "platform.h"
#include "so_util.h"
#include "thunk_gen.h"
#include <chrono>
#include <cstring>
#include <inttypes.h>
#include <memory>
#include <dlfcn.h>

SDL_Window* sdl_win;
SDL_GLContext sdl_ctx;
EGLDisplay egl_display;
EGLContext egl_context;
EGLSurface egl_surface;

// A surface handle that is neither NULL/EGL_NO_SURFACE (which
// eglCreatePbufferSurface's contract defines as "failed", and Limbo's own
// InitEGL treats exactly that way) nor a real driver handle. Handed out by
// eglCreatePbufferSurface_impl when there is no real pbuffer support; see the
// comment there. eglMakeCurrent_impl, eglGetCurrentSurface_impl,
// eglQuerySurface_impl, and eglSwapBuffers_impl all special-case it.
#define FAKE_PBUFFER_SURFACE ((EGLSurface)0x1)
static EGLint fake_pbuffer_width = 1;
static EGLint fake_pbuffer_height = 1;

// See the comment on get_virtual_present_fbo() in egl_sdl.h.
int g_ana_buffer_w = 0;
int g_ana_buffer_h = 0;

static GLuint virtual_fbo = 0;
static GLuint virtual_fbo_color_tex = 0;
static GLuint virtual_fbo_depth_rb = 0;
static int virtual_fbo_w = 0;
static int virtual_fbo_h = 0;

unsigned int get_virtual_present_fbo()
{
    if (g_ana_buffer_w <= 0 || g_ana_buffer_h <= 0)
        return 0;

    // Nothing to scale -- let the game draw straight into the real window
    // framebuffer instead of paying for an extra offscreen render + blit.
    int real_w = 0, real_h = 0;
    SDL_GL_GetDrawableSize(sdl_win, &real_w, &real_h);
    if (g_ana_buffer_w == real_w && g_ana_buffer_h == real_h)
        return 0;

    if (virtual_fbo != 0 && virtual_fbo_w == g_ana_buffer_w && virtual_fbo_h == g_ana_buffer_h)
        return virtual_fbo;

    if (virtual_fbo != 0)
    {
        glad_glDeleteFramebuffers(1, &virtual_fbo);
        glad_glDeleteTextures(1, &virtual_fbo_color_tex);
        glad_glDeleteRenderbuffers(1, &virtual_fbo_depth_rb);
        virtual_fbo = 0;
    }

    GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0;
    glad_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glad_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glad_glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);

    glad_glGenTextures(1, &virtual_fbo_color_tex);
    glad_glBindTexture(GL_TEXTURE_2D, virtual_fbo_color_tex);
    glad_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_ana_buffer_w, g_ana_buffer_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glad_glGenRenderbuffers(1, &virtual_fbo_depth_rb);
    glad_glBindRenderbuffer(GL_RENDERBUFFER, virtual_fbo_depth_rb);
    glad_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_ana_buffer_w, g_ana_buffer_h);

    glad_glGenFramebuffers(1, &virtual_fbo);
    glad_glBindFramebuffer(GL_FRAMEBUFFER, virtual_fbo);
    glad_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, virtual_fbo_color_tex, 0);
    glad_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, virtual_fbo_depth_rb);

    GLenum status = glad_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[egl] virtual present FBO incomplete: status=0x%x, size %dx%d\n", status, g_ana_buffer_w, g_ana_buffer_h);
    else
        printf("[egl] virtual present FBO created: %dx%d (game asked for this via ANativeWindow_setBuffersGeometry)\n",
            g_ana_buffer_w, g_ana_buffer_h);

    virtual_fbo_w = g_ana_buffer_w;
    virtual_fbo_h = g_ana_buffer_h;

    glad_glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glad_glBindTexture(GL_TEXTURE_2D, prev_tex);
    glad_glBindRenderbuffer(GL_RENDERBUFFER, prev_rb);

    return virtual_fbo;
}

namespace jnivm::android::view {
class Choreographer;

class Choreographer {
public:
    static std::shared_ptr<Choreographer> getInstance();
    void signalVSync();
};
}

void* getProc(const char* sym)
{
    // First try SDL
    void* proc=SDL_GL_GetProcAddress(sym);
    if(proc)
        return proc;

    // Some platforms don't expose EGL over SDL, so try dynamic linking
    static void* libEGL_handle=dlopen("libEGL.so", RTLD_NOW);
    proc=dlsym(libEGL_handle, sym);
    if(proc)
       return proc;

    return NULL;
}

EGLBoolean eglSwapBuffers_impl(EGLDisplay display,
    EGLSurface surface)
{
    // Not SDL_GL_MakeCurrent(sdl_win, sdl_ctx) + SDL_GL_SwapWindow: that used
    // to force sdl_ctx current before swapping, back when there was only ever
    // one context in play. Now the render thread has its own real context,
    // bound once via the engine's own eglMakeCurrent (see eglMakeCurrent_impl,
    // called directly against the driver, not through SDL) and never touched
    // again. Forcing sdl_ctx current here silently replaced the render
    // thread's actual context on the very first swap and never gave it back,
    // so every draw call after that landed in sdl_ctx's state instead -- the
    // window kept presenting whatever was last drawn correctly.
    //
    // Also not plain SDL_GL_SwapWindow(sdl_win) with whatever context happens
    // to already be current: SDL_GL_SwapWindow decides how to swap from SDL's
    // OWN per-thread bookkeeping of "the current context", which is only ever
    // updated by SDL_GL_MakeCurrent. The render thread's context was bound by
    // the engine calling the real eglMakeCurrent directly, bypassing SDL
    // entirely, so SDL never learned a context was current on this thread and
    // silently did nothing on swap -- hence the black screen (and eglSwapBuffers
    // returning near-instantly, which is why FPS read in the thousands).
    //
    // Go straight to the real eglSwapBuffers with the display/surface the
    // engine itself passed in: it only needs a context current on the calling
    // thread bound to that surface, which is already true, and it does not
    // care about SDL's bookkeeping at all.
    // If the game asked for a smaller-than-window buffer (see
    // get_virtual_present_fbo()), its draws all landed in our offscreen FBO
    // instead of the real window framebuffer (glBindFramebuffer(0, ...) was
    // redirected there -- see gl_thread_bind.cpp). Stretch that onto the real
    // window here, the same scaling step SurfaceFlinger would normally do,
    // before the real present.
    if (virtual_fbo != 0)
    {
        int real_w = 0, real_h = 0;
        SDL_GL_GetDrawableSize(sdl_win, &real_w, &real_h);

        GLint prev_read_fbo = 0, prev_draw_fbo = 0;
        glad_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
        glad_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);

        glad_glBindFramebuffer(GL_READ_FRAMEBUFFER, virtual_fbo);
        glad_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glad_glBlitFramebuffer(0, 0, virtual_fbo_w, virtual_fbo_h,
            0, 0, real_w, real_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);

        glad_glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
        glad_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw_fbo);
    }

    static auto real_swap_buffers = (EGLBoolean (*)(EGLDisplay, EGLSurface))getProc("eglSwapBuffers");
    if (real_swap_buffers)
        real_swap_buffers(display, surface == FAKE_PBUFFER_SURFACE ? EGL_NO_SURFACE : surface);

    using namespace std::chrono;

    // Persist across calls
    static int frameCount = 0;
    static auto lastTime = high_resolution_clock::now();
    static float fps = 0.0f;

    frameCount++;
    auto now = high_resolution_clock::now();
    duration<float> elapsed = now - lastTime;

    if (elapsed.count() >= 1.0f) {
        fps = frameCount / elapsed.count();
        frameCount = 0;
        lastTime = now;
        warning("FPS: %f\n", fps);
    }
    // verbose("EGL_SDL", "[Thread: %" PRIxPTR "] eglSwapBuffers about to call getInstance().", (uintptr_t)pthread_self());

    auto choreographer = jnivm::android::view::Choreographer::getInstance();
    if (choreographer) {
        // choreographer->dispatchFrameCallbacks(true);
        choreographer->signalVSync();
    }
    return EGL_TRUE;
}

// Just return the current display
EGLDisplay eglGetDisplay_impl(NativeDisplayType native_display)
{
    printf("[NATIVE] eglGetDisplay\n");
    if (egl_display)
        return egl_display;

    // Initialize SDL with video, audio, joystick, and controller support
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fatal_error("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        // return -1;
    }

    // SDL synthesizes a fake mouse down/motion/up out of every real touch by
    // default, for apps that only handle mouse input. main.cpp handles both
    // real finger events and real mouse input, so without this a single tap
    // arrives as two interleaved, conflicting event streams -- that is what
    // was producing Limbo's own "Began arrived after Move -- missing Ended"
    // touch-state errors.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // This "Teapot"/640x480/non-fullscreen window was a bring-up placeholder
    // that never got replaced with the real, config-driven sizing below (this
    // is the window limboloader actually uses -- see the comment in main.cpp
    // on why eglGetDisplay_impl is called instead of sdl_initialize_gles).
    // Besides never being fullscreen, its small, wrong-aspect-ratio size is
    // also why touch coordinates (which main.cpp scales by this window's
    // real size) were landing far outside the game's own [0-1024, 0-576]
    // bounds.
    int win_width = config["device"]["displayWidth"].value_or<int>(1280);
    int win_height = config["device"]["displayHeight"].value_or<int>(720);
    const bool want_fullscreen = config["device"]["fullscreen"].value_or<bool>(false);

    const char *video_driver = SDL_GetCurrentVideoDriver();
    printf("SDL video driver: %s, config wants %dx%d fullscreen=%s\n",
        video_driver ? video_driver : "(none)", win_width, win_height,
        want_fullscreen ? "true" : "false");

    Uint32 win_flags = SDL_WINDOW_OPENGL;
    if (want_fullscreen) {
        // Size the window to the display up front. On kmsdrm the window is the
        // whole display anyway; under X11/XWayland a window already at the
        // display size still covers the screen even if the compositor refuses
        // the fullscreen request.
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            win_width = dm.w;
            win_height = dm.h;
        } else {
            printf("SDL_GetDesktopDisplayMode failed (%s) -- using the configured size\n", SDL_GetError());
        }
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    sdl_win = SDL_CreateWindow("Loader", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        win_width, win_height, win_flags);
    if (sdl_win == NULL) {
        fatal_error("Failed to create SDL Window: %s\n", SDL_GetError());
        // return -1;
    }

    if (want_fullscreen) {
        // Re-assert after creation: this is what actually takes effect on
        // window managers that ignored the creation flag (common on XWayland
        // without a compositor managing the surface). Harmless on kmsdrm,
        // where it is already fullscreen.
        if (SDL_SetWindowFullscreen(sdl_win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
            printf("SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());

        int got_w = 0, got_h = 0;
        SDL_GetWindowSize(sdl_win, &got_w, &got_h);
        if (got_w < win_width || got_h < win_height) {
            // Still not covering the display -- drop the decorations and pin it
            // to the top-left at the display size.
            printf("window is %dx%d, wanted %dx%d -- falling back to borderless\n",
                   got_w, got_h, win_width, win_height);
            SDL_SetWindowFullscreen(sdl_win, 0);
            SDL_SetWindowBordered(sdl_win, SDL_FALSE);
            SDL_SetWindowSize(sdl_win, win_width, win_height);
            SDL_SetWindowPosition(sdl_win, 0, 0);
        }
    }

    {
        int final_w = 0, final_h = 0;
        SDL_GetWindowSize(sdl_win, &final_w, &final_h);
        printf("window is %dx%d, flags 0x%x\n", final_w, final_h, SDL_GetWindowFlags(sdl_win));
    }

    // Basic OpenGL ES 2.x setup
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    sdl_ctx = SDL_GL_CreateContext(sdl_win);
    if (sdl_ctx == NULL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        sdl_ctx = SDL_GL_CreateContext(sdl_win);
        if (sdl_ctx == NULL) {
            fatal_error("Failed to create OpenGL Context: %s\n", SDL_GetError());
        }
        // return -1;
    }
    SDL_GL_MakeCurrent(sdl_win, sdl_ctx);

    //

    egl_display = ((EGLDisplay (*)())getProc("eglGetCurrentDisplay"))();
    egl_context = ((EGLDisplay (*)())getProc("eglGetCurrentContext"))();
    egl_surface = ((EGLSurface (*)(EGLint))getProc("eglGetCurrentSurface"))(EGL_DRAW);

    {
        int drawable_w = 0, drawable_h = 0;
        SDL_GL_GetDrawableSize(sdl_win, &drawable_w, &drawable_h);
        EGLint surf_w = -1, surf_h = -1;
        auto real_query_surface = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*))
            getProc("eglQuerySurface");
        if (real_query_surface && egl_surface) {
            real_query_surface(egl_display, egl_surface, EGL_WIDTH, &surf_w);
            real_query_surface(egl_display, egl_surface, EGL_HEIGHT, &surf_h);
        }
        printf("[egl] drawable size %dx%d, real egl surface size %dx%d (surface=%p)\n",
            drawable_w, drawable_h, surf_w, surf_h, egl_surface);
    }

    load_egl_funcs();
    load_gles2_funcs();

    // Print OpenGL information
    const char* glVersion = (const char*)glad_glGetString(GL_VERSION);
    const char* glVendor = (const char*)glad_glGetString(GL_VENDOR);
    const char* glRenderer = (const char*)glad_glGetString(GL_RENDERER);
    const char* glExtensions = (const char*)glad_glGetString(GL_EXTENSIONS);

    if (glVersion) {
        printf("OpenGL Version: %s\n", glVersion);
    } else {
        fatal_error("Failed to retrieve OpenGL version.\n");
    }

    if (glVendor) {
        printf("OpenGL Vendor: %s\n", glVendor);
    } else {
        fatal_error("Failed to retrieve OpenGL vendor.\n");
    }

    if (glRenderer) {
        printf("OpenGL Renderer: %s\n", glRenderer);
    } else {
        fatal_error("Failed to retrieve OpenGL renderer.\n");
    }

    if (glExtensions) {
        printf("OpenGL Extensions: %s\n", glExtensions);
    } else {
        fatal_error("Failed to retrieve OpenGL extensions.\n");
    }

    // Just for good measure
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);

    return egl_display;
}

// Do not actually initialize, just return the EGL version number.
EGLBoolean eglInitialize_impl(EGLDisplay display, int* major, int* minor)
{
    verbose("EGL_SDL", "eglInitialize\n");
#ifdef FAKE_EGL
    if (major != NULL)
        *major = 1;
    if (minor != NULL)
        *minor = 4;
    return EGL_TRUE;
#endif

    if (!egl_display)
        eglGetDisplay_impl(NULL);

    int temp_major = 0, temp_minor = 0;
    const char* versionString = ((const char* (*)(EGLDisplay, EGLint))getProc("eglQueryString"))(display, EGL_VERSION);
    if (!versionString) {
        fatal_error("Failed to retrieve EGL version string.\n");
        return EGL_FALSE;
    }

    if (sscanf(versionString, "%d.%d", &temp_major, &temp_minor) != 2) {
        fatal_error("Failed to parse EGL version string: %s\n", versionString);
        return EGL_FALSE;
    }

    if (major != NULL)
        *major = temp_major;
    if (minor != NULL)
        *minor = temp_minor;

    return EGL_TRUE;
}

// Do not actually search for configs. Just always return the config that the current context uses
EGLBoolean eglChooseConfig_impl(EGLDisplay display, const EGLint* attribList, EGLConfig* configs, EGLint configSize, EGLint* numConfigs)
{
    verbose("EGL_SDL", "eglChooseConfig\n");
#ifdef FAKE_EGL
    *configs = malloc(1 * sizeof(EGLConfig));
    *numConfigs = 1;
    return EGL_TRUE;
#endif

    // Inline fetching of eglGetCurrentContext
    EGLContext context = egl_context;
    if (context == EGL_NO_CONTEXT) {
        fatal_error("Failed to get current EGLContext.\n");
        return EGL_FALSE;
    }

    EGLint configID;
    if (!((EGLBoolean (*)(EGLDisplay, EGLContext, EGLint, EGLint*))getProc("eglQueryContext"))(display, context, EGL_CONFIG_ID, &configID)) {
        fatal_error("Failed to query EGL_CONFIG_ID.\n");
        return EGL_FALSE;
    }

    EGLint totalConfigs;
    if (!((EGLBoolean (*)(EGLDisplay, EGLConfig*, EGLint, EGLint*))getProc("eglGetConfigs"))(display, NULL, 0, &totalConfigs)) {
        fatal_error("Failed to get the number of EGLConfigs.\n");
        return EGL_FALSE;
    }

    EGLConfig* allConfigs = (EGLConfig*)malloc(totalConfigs * sizeof(EGLConfig));
    if (!((EGLBoolean (*)(EGLDisplay, EGLConfig*, EGLint, EGLint*))getProc("eglGetConfigs"))(display, allConfigs, totalConfigs, &totalConfigs)) {
        fatal_error("Failed to retrieve EGLConfigs.\n");
        free(allConfigs);
        return EGL_FALSE;
    }

    // eglGetConfigAttrib to find the matching config
    EGLConfig matchingConfig = NULL;
    for (EGLint i = 0; i < totalConfigs; i++) {
        EGLint id;
        if (((EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*))getProc("eglGetConfigAttrib"))(display, allConfigs[i], EGL_CONFIG_ID, &id) && id == configID) {
            matchingConfig = allConfigs[i];
            break;
        }
    }
    free(allConfigs);

    if (!matchingConfig) {
        fatal_error("Failed to find a matching EGLConfig.\n");
        return EGL_FALSE;
    }

    // Populate the results
    if (configs && configSize > 0) {
        configs[0] = matchingConfig;
    }
    if (numConfigs) {
        *numConfigs = 1; // Always return exactly 1 config
    }

    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface_impl(EGLDisplay display, EGLConfig config, NativeWindowType native_window, EGLint const* attrib_list)
{
    verbose("EGL_SDL", "eglCreateWindowSurface\n");
#ifdef FAKE_EGL
    return (EGLSurface)0xDEAD;
#endif
    // Log the real, driver-reported size of the surface we're handing back --
    // this is what the compositor will actually present, as opposed to
    // whatever size we've told the game its ANativeWindow is (see
    // ANativeWindow_getWidth/Height in thunks/ndk/ndk.cpp) or what the game
    // computes as its internal render/backbuffer size. Comparing these three
    // is the way to tell whether a scaling mismatch is us reporting the wrong
    // window size, or the game's own upscale-to-window step not doing what it
    // should with a correct one.
    static auto real_query_surface = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*))
        getProc("eglQuerySurface");
    if (real_query_surface)
    {
        EGLint w = -1, h = -1;
        real_query_surface(display, egl_surface, EGL_WIDTH, &w);
        real_query_surface(display, egl_surface, EGL_HEIGHT, &h);
        printf("[egl] eglCreateWindowSurface: real driver surface size %dx%d\n", w, h);
    }
    return egl_surface;
}

// Pbuffer (offscreen) surfaces.
//
// Engines ask for a pbuffer so a context can be kept current on a worker
// thread while the window surface is bound to a different context on another
// thread (Limbo does exactly this: an "Offscreen context" on its game thread
// runs alongside the real render context on its render thread). EGL requires
// a surface be current in at most one context at a time -- handing back the
// window surface here works only as long as a single thread is ever bound to
// anything, and breaks the moment two threads are genuinely concurrent.
//
// On real hardware (confirmed on Panfrost/Mesa's GBM/DRM platform, no X11)
// there is no EGLConfig anywhere on the display advertising EGL_PBUFFER_BIT --
// eglCreatePbufferSurface fails with EGL_BAD_MATCH on the window's config, and
// an eglChooseConfig search across every config for one that supports
// pbuffers comes back with zero matches. GBM has no native pbuffer buffer
// type, so this driver just doesn't implement them; no config attribute
// combination will conjure one.
//
// EGL_KHR_surfaceless_context (which this display does advertise) is the real
// answer -- a context can go current with no surface at all -- but it only
// helps at the eglMakeCurrent end. Returning EGL_NO_SURFACE from *this*
// function instead means "I failed", by spec and by Limbo's own error check,
// so the engine aborts the offscreen setup instead of ever reaching that
// eglMakeCurrent. Hand back a fake, distinguishable, non-EGL_NO_SURFACE
// handle here so the engine believes creation succeeded, and translate it
// (and only it) back to EGL_NO_SURFACE in eglMakeCurrent_impl.
EGLSurface eglCreatePbufferSurface_impl(EGLDisplay display, EGLConfig config, EGLint const* attrib_list)
{
    verbose("EGL_SDL", "eglCreatePbufferSurface\n");
#ifdef FAKE_EGL
    return (EGLSurface)0xDEAD;
#endif
    static auto real_create_pbuffer = (EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*))
        getProc("eglCreatePbufferSurface");
    if (real_create_pbuffer)
    {
        EGLSurface pbuf = real_create_pbuffer(display, config, attrib_list);
        if (pbuf != EGL_NO_SURFACE)
            return pbuf;
    }

    if (attrib_list)
    {
        for (int i = 0; attrib_list[i] != EGL_NONE; i += 2)
        {
            if (attrib_list[i] == EGL_WIDTH)
                fake_pbuffer_width = attrib_list[i + 1];
            else if (attrib_list[i] == EGL_HEIGHT)
                fake_pbuffer_height = attrib_list[i + 1];
        }
    }

    static auto real_query_string = (char const* (*)(EGLDisplay, EGLint))getProc("eglQueryString");
    const char *extensions = real_query_string ? real_query_string(display, EGL_EXTENSIONS) : NULL;
    if (extensions && strstr(extensions, "EGL_KHR_surfaceless_context"))
    {
        printf("eglCreatePbufferSurface: no real pbuffer support -- handing back a fake surface, "
               "will go current as EGL_NO_SURFACE (EGL_KHR_surfaceless_context)\n");
        return FAKE_PBUFFER_SURFACE;
    }

    // Last resort: share the window surface. Only tolerates one thread being
    // current at a time, same limitation as before EGL_KHR_surfaceless_context
    // was tried.
    printf("eglCreatePbufferSurface: no pbuffer support and no EGL_KHR_surfaceless_context -- "
           "falling back to the window surface\n");
    return egl_surface;
}

EGLBoolean eglQuerySurface_impl(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value)
{
    verbose("EGL_SDL", "eglQuerySurface\n");
#ifdef FAKE_EGL
    if (attribute == EGL_WIDTH)
        *value = 640;
    if (attribute == EGL_HEIGHT)
        *value = 480;
    return EGL_TRUE;
#endif
    if (surface == FAKE_PBUFFER_SURFACE)
    {
        if (attribute == EGL_WIDTH)
            *value = fake_pbuffer_width;
        else if (attribute == EGL_HEIGHT)
            *value = fake_pbuffer_height;
        else
            *value = 0;
        if (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)
            printf("[egl] eglQuerySurface(FAKE_PBUFFER, %s) = %d\n",
                attribute == EGL_WIDTH ? "EGL_WIDTH" : "EGL_HEIGHT", *value);
        return EGL_TRUE;
    }
    EGLBoolean ok = ((EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*))getProc("eglQuerySurface"))(display, surface, attribute, value);
    if (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)
        printf("[egl] eglQuerySurface(surface=%p, %s) = %d (ok=%d)\n", surface,
            attribute == EGL_WIDTH ? "EGL_WIDTH" : "EGL_HEIGHT", *value, ok);
    return ok;
}

EGLContext eglCreateContext_impl(EGLDisplay display,
    EGLConfig config,
    EGLContext share_context,
    EGLint const* attrib_list)
{
    verbose("EGL_SDL", "eglCreateContext\n");
#ifdef FAKE_EGL
    return (EGLContext)0xDEAD;
#endif

    // Hand back a real context that shares objects with ours, rather than the
    // same one every time. An engine with more than one GL thread (Limbo has a
    // "game" and a "render" thread) asks for a context per thread, and a single
    // context cannot be current on two threads at once -- returning the shared
    // one makes the second thread's eglMakeCurrent fail with EGL_BAD_ACCESS and
    // its render targets never get created. Sharing means textures, buffers and
    // programs still live in one namespace, which is what the engine expects.
    static auto real_create = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))
        getProc("eglCreateContext");
    if (real_create && egl_display && egl_context)
    {
        EGLContext shared = real_create(egl_display, config, egl_context, attrib_list);
        if (shared)
        {
            printf("[NATIVE] eglCreateContext -> %p (shared with %p)\n", shared, egl_context);
            return shared;
        }
        printf("[NATIVE] eglCreateContext: could not create a shared context, reusing the main one\n");
    }
    return egl_context;
}

// Give the calling thread a usable GL context.
//
// Engines that render from more than one thread (Limbo has a "game" and a
// "render" thread) need a context each, since one cannot be current on two
// threads at once. The main context is handed to whichever thread asks first;
// everyone after that gets their own, sharing objects with it so textures,
// buffers and programs stay in a single namespace.
EGLBoolean egl_bind_thread_context()
{
    static auto real_make_current = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))
        getProc("eglMakeCurrent");
    if (!real_make_current || !egl_display || !egl_context)
        return EGL_FALSE;

    // first thread in takes the context the loader already made
    if (real_make_current(egl_display, egl_surface, egl_surface, egl_context))
        return EGL_TRUE;

    // Taken: find the config behind the main context so the new one matches it.
    EGLint configID = 0;
    if (!((EGLBoolean (*)(EGLDisplay, EGLContext, EGLint, EGLint *))getProc("eglQueryContext"))(
            egl_display, egl_context, EGL_CONFIG_ID, &configID))
        return EGL_FALSE;

    EGLint total = 0;
    auto get_configs = (EGLBoolean (*)(EGLDisplay, EGLConfig *, EGLint, EGLint *))getProc("eglGetConfigs");
    if (!get_configs || !get_configs(egl_display, NULL, 0, &total) || total <= 0)
        return EGL_FALSE;

    EGLConfig *all = (EGLConfig *)malloc(total * sizeof(EGLConfig));
    if (!all || !get_configs(egl_display, all, total, &total))
    {
        free(all);
        return EGL_FALSE;
    }

    auto get_attrib = (EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint *))getProc("eglGetConfigAttrib");
    EGLConfig match = NULL;
    for (EGLint i = 0; i < total; i++)
    {
        EGLint id = 0;
        if (get_attrib(egl_display, all[i], EGL_CONFIG_ID, &id) && id == configID)
        {
            match = all[i];
            break;
        }
    }
    free(all);
    if (!match)
        return EGL_FALSE;

    const EGLint attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext shared = ((EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))
        getProc("eglCreateContext"))(egl_display, match, egl_context, attribs);
    if (!shared)
        return EGL_FALSE;

    if (!real_make_current(egl_display, egl_surface, egl_surface, shared))
        return EGL_FALSE;

    printf("[NATIVE] gave this thread its own context %p (shared with %p)\n", shared, egl_context);
    fflush(stdout);
    return EGL_TRUE;
}

EGLBoolean eglDestroyContext_impl(EGLDisplay display,
    EGLContext context)
{
    return EGL_TRUE;
}

EGLBoolean eglDestroySurface_impl(EGLDisplay display,
    EGLSurface surface)
{
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent_impl(EGLDisplay display,
    EGLSurface draw,
    EGLSurface read,
    EGLContext context)
{
    static auto cached_eglMakeCurrent = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))getProc("eglMakeCurrent");
    verbose("EGL_SDL", "eglMakeCurrent\n");
    // eglCreatePbufferSurface_impl hands out FAKE_PBUFFER_SURFACE when there is
    // no real pbuffer support -- translate it to EGL_NO_SURFACE here, which is
    // what actually makes a context current with nothing backing it (relies on
    // EGL_KHR_surfaceless_context, already confirmed present on this display).
    if (draw == FAKE_PBUFFER_SURFACE)
        draw = EGL_NO_SURFACE;
    if (read == FAKE_PBUFFER_SURFACE)
        read = EGL_NO_SURFACE;
    return cached_eglMakeCurrent(display, draw, read, context);
}

EGLint eglGetError_impl()
{
    return EGL_SUCCESS; // TRULY AWFUL
}

EGLBoolean eglGetConfigAttrib_impl(EGLDisplay display,
    EGLConfig config,
    EGLint attribute,
    EGLint* value)
{
    return ((EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*))getProc("eglGetConfigAttrib"))(display, config, attribute, value);
}

char const* eglQueryString_impl(EGLDisplay display,
    EGLint name)
{
    verbose("EGL_SDL", "eglQueryString %d\n", name);
    return ((char const* (*)(EGLDisplay, EGLint))getProc("eglQueryString"))(display, name);
}

EGLDisplay eglGetCurrentDisplay_impl()
{
    return egl_display;
}

// These used to just return the cached startup handles, which was fine when
// only one context/surface ever existed. Now that the game and render
// threads each hold their own real, distinct context (see eglCreateContext_impl
// and egl_bind_thread_context), that stopped being true: the engine calls
// eglGetCurrentContext() every frame to check whether its context is still
// bound before re-touching GL state, and a hard-coded return of the original
// context made every such check fail -- it re-established the render context
// and re-logged GPU info every single frame, which is what the flicker was.
// Query the real per-thread state instead.
EGLContext eglGetCurrentContext_impl()
{
    static auto real_get_current_context = (EGLContext (*)())getProc("eglGetCurrentContext");
    return real_get_current_context ? real_get_current_context() : egl_context;
}

EGLSurface eglGetCurrentSurface_impl(EGLint readdraw)
{
    static auto real_get_current_context = (EGLContext (*)())getProc("eglGetCurrentContext");
    static auto real_get_current_surface = (EGLSurface (*)(EGLint))getProc("eglGetCurrentSurface");
    EGLSurface surface = real_get_current_surface ? real_get_current_surface(readdraw) : EGL_NO_SURFACE;
    // The fake pbuffer handle only ever exists in the game's own bookkeeping
    // (eglCreatePbufferSurface_impl's return value); the driver itself is
    // bound with EGL_NO_SURFACE for that context (see eglMakeCurrent_impl).
    // Translate back the same way eglMakeCurrent_impl translates forward, but
    // only when a context is genuinely current -- EGL_NO_SURFACE with no
    // current context just means nothing is bound on this thread at all.
    if (surface == EGL_NO_SURFACE && real_get_current_context && real_get_current_context() != EGL_NO_CONTEXT)
        return FAKE_PBUFFER_SURFACE;
    return surface;
}

EGLBoolean eglSwapInterval_impl(EGLDisplay display,
    EGLint interval)
{
    return EGL_FALSE; // Generally can't set swap interval on these platforms.
}

// Actually implemented in egl.cpp
ABI_ATTR __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress_impl(const char* procname);

DynLibFunction symtable_egl_sdl[] = {
    NO_THUNK("eglSwapBuffers", (uintptr_t)&eglSwapBuffers_impl),
    NO_THUNK("eglGetDisplay", (uintptr_t)&eglGetDisplay_impl),
    NO_THUNK("eglInitialize", (uintptr_t)&eglInitialize_impl),
    NO_THUNK("eglChooseConfig", (uintptr_t)&eglChooseConfig_impl),
    NO_THUNK("eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface_impl),
    NO_THUNK("eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface_impl),
    NO_THUNK("eglQuerySurface", (uintptr_t)&eglQuerySurface_impl),
    NO_THUNK("eglCreateContext", (uintptr_t)&eglCreateContext_impl),
    NO_THUNK("eglMakeCurrent", (uintptr_t)&eglMakeCurrent_impl),
    NO_THUNK("eglGetError", (uintptr_t)&eglGetError_impl),
    NO_THUNK("eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib_impl),
    NO_THUNK("eglDestroyContext", (uintptr_t)&eglDestroyContext_impl),
    NO_THUNK("eglDestroySurface", (uintptr_t)&eglDestroySurface_impl),
    NO_THUNK("eglQueryString", (uintptr_t)&eglQueryString_impl),
    NO_THUNK("eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay_impl),
    NO_THUNK("eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext_impl),
    NO_THUNK("eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface_impl),
    NO_THUNK("eglSwapInterval", (uintptr_t)&eglSwapInterval_impl),
    NO_THUNK("eglGetProcAddress", (uintptr_t)&eglGetProcAddress_impl),
    { NULL, (uintptr_t)NULL }
};

// Internal use, do not put these in the symtable

void sdl_initialize_gles()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fatal_error("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    }
    int win_width = config["device"]["displayWidth"].value_or<int>(640);
    int win_height = config["device"]["displayHeight"].value_or<int>(480);
    const bool want_fullscreen = config["device"]["fullscreen"].value_or<bool>(false);

    const char *driver = SDL_GetCurrentVideoDriver();
    printf("SDL video driver: %s\n", driver ? driver : "(none)");

    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
    if (want_fullscreen) {
        // Size the window to the display up front. On kmsdrm the window is the
        // whole display anyway; under X11/XWayland a window already at the
        // display size still covers the screen even if the compositor refuses
        // the fullscreen request.
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            win_width = dm.w;
            win_height = dm.h;
        } else {
            printf("SDL_GetDesktopDisplayMode failed (%s) -- using the configured size\n", SDL_GetError());
        }
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    sdl_win = SDL_CreateWindow("Loader", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_width, win_height, win_flags);
    if (sdl_win == NULL) {
        fatal_error("Failed to create SDL Window: %s\n", SDL_GetError());
    }

    if (want_fullscreen) {
        // Re-assert after creation: this is what actually takes effect on
        // window managers that ignored the creation flag (common on XWayland
        // without a compositor managing the surface). Harmless on kmsdrm,
        // where it is already fullscreen.
        if (SDL_SetWindowFullscreen(sdl_win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
            printf("SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());

        int got_w = 0, got_h = 0;
        SDL_GetWindowSize(sdl_win, &got_w, &got_h);
        if (got_w < win_width || got_h < win_height) {
            // Still not covering the display -- drop the decorations and pin it
            // to the top-left at the display size.
            printf("window is %dx%d, wanted %dx%d -- falling back to borderless\n",
                   got_w, got_h, win_width, win_height);
            SDL_SetWindowFullscreen(sdl_win, 0);
            SDL_SetWindowBordered(sdl_win, SDL_FALSE);
            SDL_SetWindowSize(sdl_win, win_width, win_height);
            SDL_SetWindowPosition(sdl_win, 0, 0);
        }
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    sdl_ctx = SDL_GL_CreateContext(sdl_win);
    if (sdl_ctx == NULL) {
        fatal_error("Failed to create OpenGL Context: %s\n", SDL_GetError());
    }

    // SDL_GL_DeleteContext(sdl_ctx);
    // SDL_DestroyWindow(sdl_win);
    // SDL_Quit();
}