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
    Baron::Jvm vm;
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

    int viewWidth = config["device"]["displayWidth"].value_or<int>(640);
    int viewHeight = config["device"]["displayHeight"].value_or<int>(480);

    printf("calling setViewSize(%d, %d)\n", viewWidth, viewHeight);
    setViewSize(env, nullptr, viewWidth, viewHeight);

    printf("calling resume\n");
    gameResume(env, nullptr);

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

        // frame_step=1, unused=0, touch_num=0, no touch points yet
        gameRender(env, nullptr, 1, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    printf("Exit.\n");
    return 0;
}
