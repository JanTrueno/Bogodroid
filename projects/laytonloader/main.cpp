#include <execinfo.h>
#include <iostream>
#include <cstdlib>

#include "toml++/toml.hpp"
toml::table config;
#include "config.h"

#include <unistd.h>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <baron/baron.h>
#include "javastubs/binding.h"
#include "javastubs/layton.h"
#include <fstream>
#include <fcntl.h>
#include <stdlib.h>
#include "platform.h"
#include "so_util.h"
#include "io_util.h"
#include "logging.h"

#include "ndk.h"
#include "anative_activity.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_hints.h>
#include "gles2.h"
#include "glad.h"
#include "glad_egl.h"
#include "egl_sdl.h"
#include "debug_utils.h"
#include "opensl_stub.h"

using namespace FakeJni;

extern DynLibFunction symtable_libc[];
extern DynLibFunction symtable_ndk[];
extern DynLibFunction symtable_gles2[];
extern DynLibFunction symtable_egl_sdl[];

extern SDL_Window *sdl_win;

DynLibFunction *so_static_patches[32] = {
    NULL,
};

DynLibFunction *so_dynamic_libraries[32] = {
    symtable_libc,
    symtable_ndk,
    symtable_gles2,
    symtable_egl_sdl,
    symtable_opensl,
    NULL
};

// libll1.so's native entry points, resolved by JNI naming convention
// (Java_com_Level5_LT1R_MainActivity_<method>) -- same symbols the Switch
// homebrew port (reference/layton_nx-main) calls directly by address.
typedef void (*setViewSize_t)(JNIEnv *, jobject, jint, jint);
typedef void (*resume_t)(JNIEnv *, jobject);
typedef void (*render_t)(JNIEnv *, jobject, jint, jint, jint, jfloat, jfloat, jfloat, jfloat);

// ---------------------------------------------------------------------------
// game patches -- the engine's own C++ file-loading functions, hooked
// directly (not via JNI). Both the Switch and Vita homebrew ports of this
// binary patch these two exported symbols; without them the engine cannot
// load a single asset. Mangled names are for FS_LoadFile(char*, char const*,
// int, int) and FS_GetLength(char const*) respectively -- verified against
// reference/layton_nx-main/source/main.c, which hooks the identical symbols
// on the identical (arm64) binary.
//
// The engine passes fname relative to the APK's assets root (e.g.
// "data/ani/akira.png", "data-en/etext/...", confirmed by the real extracted
// APK layout under gamefiles/layton/assets/). init_config() chdir's into
// gamefiles/layton/, so these prepend "assets/" -- same as the Switch/Vita
// ports' asset_path() helper.
// ---------------------------------------------------------------------------

static std::string asset_path(const char *rel)
{
    return std::string("assets/") + rel;
}

static uint8_t FS_LoadFile(char *buf, const char *fname, int pos, int size)
{
    FILE *f = fopen(asset_path(fname).c_str(), "rb");
    if (!f)
    {
        printf("FS_LoadFile: missing %s\n", fname);
        return 0;
    }
    fseek(f, pos, SEEK_SET);
    fread(buf, 1, size, f);
    fclose(f);
    return 1;
}

static int FS_GetLength(const char *fname)
{
    struct stat st;
    if (stat(asset_path(fname).c_str(), &st) >= 0)
        return (int)st.st_size;
    return 0;
}

static void criErr_Notify(int unk, const char *err)
{
    (void)unk;
    printf("criErr: %s\n", err ? err : "(null)");
}

static void patch_game(so_module *mod)
{
    hook_symbol(mod, "_Z11FS_LoadFilePcPKcii", (uintptr_t)FS_LoadFile, 0);
    hook_symbol(mod, "_Z12FS_GetLengthPKc", (uintptr_t)FS_GetLength, 0);
    hook_symbol(mod, "criErr_Notify", (uintptr_t)criErr_Notify, 1);
}

// ---------------------------------------------------------------------------
// rotation -- the DS layout is portrait (two screens stacked), so on a
// widescreen panel the game renders into an offscreen FBO at the portrait
// view size and that texture is blitted rotated onto the window. Ported from
// reference/layton_nx-main/source/main.c (rot_init / rot_blit), which does
// exactly this for the same binary on the Switch.
//
// device.displayRotation: 0 = none (render straight to the window),
// 90 / 180 / 270 = degrees counter-clockwise.
// ---------------------------------------------------------------------------

static struct
{
    GLuint fbo, tex, depth;
    GLuint prog;
    GLint loc_pos, loc_uv, loc_tex;
} rot;

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src)
{
    const GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void rot_init(int view_w, int view_h)
{
    glGenTextures(1, &rot.tex);
    glBindTexture(GL_TEXTURE_2D, rot.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, view_w, view_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenRenderbuffers(1, &rot.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, rot.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /* GL_DEPTH24_STENCIL8_OES */, view_w, view_h);

    glGenFramebuffers(1, &rot.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rot.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rot.tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rot.depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rot.depth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        // retry with a plain 16-bit depth buffer
        glBindRenderbuffer(GL_RENDERBUFFER, rot.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, view_w, view_h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fatal_error("Could not create the rotation framebuffer.\n");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    rot.prog = link_program(
        "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;"
        "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }",
        "precision mediump float; uniform sampler2D tex; varying vec2 vUV;"
        "void main() { gl_FragColor = texture2D(tex, vUV); }");
    if (!rot.prog)
        fatal_error("Could not build the rotation blit shader.\n");
    rot.loc_pos = glGetAttribLocation(rot.prog, "aPos");
    rot.loc_uv = glGetAttribLocation(rot.prog, "aUV");
    rot.loc_tex = glGetUniformLocation(rot.prog, "tex");
}

// draw the FBO rotated onto the window (strip order: BL BR TL TR)
static void rot_blit(int rotation, int win_w, int win_h)
{
    static const GLfloat pos[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    static const GLfloat uv_90[8] = {0, 1, 0, 0, 1, 1, 1, 0};
    static const GLfloat uv_180[8] = {1, 1, 0, 1, 1, 0, 0, 0};
    static const GLfloat uv_270[8] = {1, 0, 1, 1, 0, 0, 0, 1};
    const GLfloat *uv = rotation == 180 ? uv_180 : (rotation == 270 ? uv_270 : uv_90);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glUseProgram(rot.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rot.tex);
    glUniform1i(rot.loc_tex, 0);
    glEnableVertexAttribArray(rot.loc_pos);
    glEnableVertexAttribArray(rot.loc_uv);
    glVertexAttribPointer(rot.loc_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(rot.loc_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(rot.loc_pos);
    glDisableVertexAttribArray(rot.loc_uv);
}

Baron::Jvm vm;

int main(int argc, char *argv[])
{
    print_backtrace_on_segfault(); // Registers a signal handler to print backtrace on segfaults
    exit_on_signals();             // Exits when CTRL-C is pressed (or SIGINT or SIGTERM is received)

    if (argc < 2)
    {
        fatal_error("Usage: %s <config file>\n", argv[0]);
        return -1;
    }

    // Init config (also chdir's into paths.game_files), GLES pointers, JNI VM and bindings
    init_config(argv[1]);
    sdl_initialize_gles();
    load_gles2_funcs();
    InitJNIBinding(&vm);

    // Load the main so file
    printf("Loading libll1\n");
    so_module lmain = {};
    uintptr_t addr_lmain = 0x50000000;
    const char *path_lmain = "lib/arm64-v8a/libll1.so";
    if (!load_so_from_file(&lmain, path_lmain, addr_lmain))
    {
        printf("Failed to load libll1.so.\n");
        return 1;
    }

    patch_game(&lmain);

    FakeJni::LocalFrame frame(vm);
    JNIEnv *env = &frame.getJniEnv();

    // The engine looks up its Java statics (notably GL_LoadPNG, the PNG
    // decoder) by calling GetObjectClass on the activity it is handed -- it
    // never FindClass'es the name, so there is no class-name string in
    // libll1.so. Passing null makes jnivm hand back the "Invalid" class
    // (see libjnivm/src/jnivm/vm.cpp:252), the lookup fails silently and no
    // image is ever decoded. Instantiate through the class factory so the
    // object carries its clazz.
    auto activityClass = vm.findClass("com/Level5/LT1R/MainActivity");
    auto activityEnv = jnivm::ENV::FromJNIEnv(env);
    auto activity = activityClass->Instantiate(activityEnv);
    jobject activityObj =
        jnivm::JNITypes<std::shared_ptr<jnivm::Object>>::ToJNIType(activityEnv, activity);
    printf("MainActivity instance=%p class=%s\n", (void *)activityObj,
           activity && activity->getClassInternal(activityEnv)
               ? activity->getClassInternal(activityEnv)->getName().c_str()
               : "(none)");

    printf("calling JNI_OnLoad from libll1.so\n");
    auto jniOnLoad = (jint(*)(JavaVM *, void *))(so_symbol(&lmain, "JNI_OnLoad"));
    if (jniOnLoad)
        jniOnLoad(&vm, nullptr);

    auto setViewSize = (setViewSize_t)(so_symbol(&lmain, "Java_com_Level5_LT1R_MainActivity_setViewSize"));
    auto gameResume = (resume_t)(so_symbol(&lmain, "Java_com_Level5_LT1R_MainActivity_resume"));
    auto gameRender = (render_t)(so_symbol(&lmain, "Java_com_Level5_LT1R_MainActivity_render"));

    printf("setViewSize=%p resume=%p render=%p\n", (void *)setViewSize, (void *)gameResume, (void *)gameRender);
    if (!setViewSize || !gameResume || !gameRender)
    {
        printf("Missing one or more MainActivity entry points -- check the exported symbol names above.\n");
        return 1;
    }

    // The window is the real drawable (it may be fullscreen, so ask SDL rather
    // than trusting the configured size). At 90/270 the game's view is the
    // window with its axes swapped -- the portrait DS layout that then gets
    // rotated onto the widescreen panel.
    int winWidth = 0, winHeight = 0;
    SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);

    int rotation = config["device"]["displayRotation"].value_or<int>(0);
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
    {
        printf("displayRotation=%d is not one of 0/90/180/270 -- ignoring\n", rotation);
        rotation = 0;
    }
    const bool swapAxes = (rotation == 90 || rotation == 270);

    int viewWidth = swapAxes ? winHeight : winWidth;
    int viewHeight = swapAxes ? winWidth : winHeight;

    printf("window %dx%d, rotation %d -> view %dx%d\n",
           winWidth, winHeight, rotation, viewWidth, viewHeight);
    if (rotation != 0)
        rot_init(viewWidth, viewHeight);

    printf("calling setViewSize(%d, %d)\n", viewWidth, viewHeight);
    setViewSize(env, activityObj, viewWidth, viewHeight);

    printf("calling resume\n");
    gameResume(env, activityObj);

    printf("Entering render loop\n");
    bool running = true;
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                running = false;
        }

        if (rotation != 0)
            glBindFramebuffer(GL_FRAMEBUFFER, rot.fbo);
        glViewport(0, 0, viewWidth, viewHeight);

        // frame_step=1, unused=0, touch_num=0, no touch points yet
        gameRender(env, activityObj, 1, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);

        if (rotation != 0)
            rot_blit(rotation, winWidth, winHeight);

        // The game only draws into the back buffer -- on Android the
        // GLSurfaceView owned the swap, so the loader has to do it here
        // (same split as reference/layton_nx-main/source/main.c).
        SDL_GL_SwapWindow(sdl_win);
    }

    printf("Exit.\n");
    return 0;
}
