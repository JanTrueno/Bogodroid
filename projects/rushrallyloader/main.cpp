// main.cpp -- Rush Rally 3 (Brownmonster) Bogodroid NEO loader. Milestone 1:
// minimal boot only -- load the .so, drive it through the standard
// ANativeActivity lifecycle far enough to see it render, nothing else yet.
//
// libRushRally3.so is a plain NDK ANativeActivity app built on Brownmonster's
// in-house "RuSDK" engine (shared across Rush Rally 2/3/Origins), statically
// linking libc++/OpenSSL/curl/zlib/vorbis into the one .so -- no separate
// libc++_shared.so ships in the APK, unlike Limbo.
//
// Verified against the shipped arm64-v8a binary's ELF tables:
//   DT_NEEDED : libEGL, libGLESv2, libOpenSLES, libandroid, liblog, libm, libc
//   exports   : ANativeActivity_onCreate, JNI_OnLoad, plus 14 JNI natives
//               under brownmonster.rusdk.* (Play Games sign-in/friends/
//               snapshots, IAP, text input) -- none required to boot/render,
//               deliberately not wired up this pass.
//   imports   : AInputQueue_*/AInputEvent_* (native input queue, not JNI),
//               AAssetManager_*/AAsset_*, AConfiguration_*, ALooper_*,
//               android_main/android_app_pre_exec_cmd/post_exec_cmd (a
//               statically-linked android_native_app_glue), egl*, gl*
//
// OpenSL ES is imported but intentionally left unresolved this pass (no
// symtable_opensl linked in so_dynamic_libraries below) -- see docs plan for
// why, and what to do if that turns out to be on the boot-critical path.

#include <execinfo.h>
#include <iostream>
#include <cstdlib>

#include "toml++/toml.hpp"
toml::table config;
#include "config.h"

#include <unistd.h>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <fcntl.h>
#include <stdlib.h>

#include <baron/baron.h>
#include "javastubs/binding.h"
#include "javastubs/rushrally.h"

#include "platform.h"
#include "so_util.h"
#include "io_util.h"
#include "logging.h"

#include "ndk.h"
#include "ainput.h"
#include "anative_activity.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_hints.h>
#include "gles2.h"
#include "glad.h"
#include "glad_egl.h"
#include "egl_sdl.h"
#include "debug_utils.h"

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
    NULL};

Baron::Jvm vm;

int main(int argc, char *argv[])
{
    print_backtrace_on_segfault();
    exit_on_signals();

    if (argc < 2)
    {
        fatal_error("Usage: %s <config file>\n", argv[0]);
        return -1;
    }

    // init_config also chdir()s into paths.game_files.
    //
    // eglGetDisplay_impl (not sdl_initialize_gles()) both creates the window
    // and captures the EGL handles the game's own EGL calls get answered
    // from -- has to happen before libRushRally3 is loaded, since
    // so_resolve_link() patches unresolved imports to a crash stub during
    // relocation and never looks again, so the GLES/EGL entry points must
    // exist by then.
    init_config(argv[1]);
    eglGetDisplay_impl(nullptr);

    // That left the context current on this thread; the game renders from
    // its own thread (same shape as Limbo), so free it up here rather than
    // fighting over it later.
    SDL_GL_MakeCurrent(sdl_win, NULL);

    InitJNIBinding(&vm);

    printf("Loading libRushRally3\n");
    so_module lmain = {};
    uintptr_t addr_lmain = 0x50000000;
    const char *path_lmain = "lib/arm64-v8a/libRushRally3.so";
    if (!load_so_from_file(&lmain, path_lmain, addr_lmain))
    {
        printf("Failed to load %s\n", path_lmain);
        return 1;
    }

    FakeJni::LocalFrame frame(vm);
    JNIEnv *env = &frame.getJniEnv();

    printf("calling JNI_OnLoad from libRushRally3.so\n");
    auto jniOnLoad = (jint(*)(JavaVM *, void *))(so_symbol(&lmain, "JNI_OnLoad"));
    if (jniOnLoad)
        jniOnLoad(&vm, nullptr);

    // ANativeActivity_create()'s make_shared bypasses jnivm's class factory,
    // leaving clazz with no usable class (GetObjectClass -> "Invalid", every
    // method lookup silently fails) -- same fix Limbo/Layton both needed.
    ANativeActivity nActivity =
        ANativeActivity_create<jnivm::brownmonster::app::game::rushrally3::RushRally3Activity>(
            &vm, env, "assets");

    auto activityEnv = jnivm::ENV::FromJNIEnv(env);
    auto activityClass = vm.findClass("brownmonster/app/game/rushrally3/RushRally3Activity");
    auto activity = activityClass->Instantiate(activityEnv);
    nActivity.clazz =
        std::dynamic_pointer_cast<jnivm::android::app::NativeActivity>(activity);

    // The engine writes saves/unpacked assets here; keep them beside the
    // game files rather than in the host's home dir.
    static std::string internalPath = std::filesystem::absolute("files").string();
    static std::string externalPath = std::filesystem::absolute("files").string();
    static std::string obbPath = std::filesystem::absolute("obb").string();
    std::filesystem::create_directories(internalPath);
    std::filesystem::create_directories(obbPath);

    nActivity.internalDataPath = internalPath.c_str();
    nActivity.externalDataPath = externalPath.c_str();
    nActivity.obbPath = obbPath.c_str();
    nActivity.sdkVersion = 25;

    printf("calling ANativeActivity_onCreate from libRushRally3.so\n");
    auto onCreate = (void (*)(ANativeActivity *, void *, size_t))(
        so_symbol(&lmain, "ANativeActivity_onCreate"));
    if (!onCreate)
    {
        printf("ANativeActivity_onCreate not found.\n");
        return 1;
    }
    onCreate(&nActivity, nullptr, 0);

    // onCreate spawns the engine's own android_native_app_glue thread, which
    // prepares its ALooper and blocks waiting for a window. Everything below
    // runs on this thread, standing in for Android's UI thread.

    int winWidth = config["device"]["displayWidth"].value_or<int>(1280);
    int winHeight = config["device"]["displayHeight"].value_or<int>(720);
    if (sdl_win)
        SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);
    printf("window %dx%d\n", winWidth, winHeight);

    // A real (non-NULL) ANativeWindow. eglCreateWindowSurface_impl ignores
    // its contents and hands back the SDL surface, but the engine
    // null-checks the pointer.
    ANativeWindow *window = ANativeWindow_fromSurface(nullptr, nullptr);

    AInputQueue *inputQueue = AInputQueue_create();
    if (!inputQueue)
    {
        printf("Could not create the native input queue.\n");
        return 1;
    }

    // Lifecycle order matches Android's: started/resumed first, then the
    // input queue and window surface arrive.
    if (nActivity.callbacks->onStart)
    {
        printf("calling onStart\n");
        nActivity.callbacks->onStart(&nActivity);
    }

    if (nActivity.callbacks->onResume)
    {
        printf("calling onResume\n");
        nActivity.callbacks->onResume(&nActivity);
    }

    if (nActivity.callbacks->onInputQueueCreated)
    {
        printf("calling onInputQueueCreated\n");
        nActivity.callbacks->onInputQueueCreated(&nActivity, inputQueue);
    }

    if (nActivity.callbacks->onNativeWindowCreated)
    {
        printf("calling onNativeWindowCreated\n");
        nActivity.callbacks->onNativeWindowCreated(&nActivity, window);
    }

    if (sdl_win)
        SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);

    if (nActivity.callbacks->onWindowFocusChanged)
    {
        printf("calling onWindowFocusChanged(1)\n");
        nActivity.callbacks->onWindowFocusChanged(&nActivity, 1);
    }

    printf("Entering minimal event loop (quit/resize/focus only -- no input, "
           "no frame pump yet)\n");

    bool running = true;
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_RESIZED)
                {
                    SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);
                    if (nActivity.callbacks->onNativeWindowResized)
                        nActivity.callbacks->onNativeWindowResized(&nActivity, window);
                }
                else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                         ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                {
                    if (nActivity.callbacks->onWindowFocusChanged)
                        nActivity.callbacks->onWindowFocusChanged(&nActivity,
                            ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? 1 : 0);
                }
                break;

            default:
                break;
            }
        }

        SDL_Delay(1);
    }

    printf("Shutting down\n");

    if (nActivity.callbacks->onPause)
        nActivity.callbacks->onPause(&nActivity);
    if (nActivity.callbacks->onStop)
        nActivity.callbacks->onStop(&nActivity);
    if (nActivity.callbacks->onNativeWindowDestroyed)
        nActivity.callbacks->onNativeWindowDestroyed(&nActivity, window);
    if (nActivity.callbacks->onInputQueueDestroyed)
        nActivity.callbacks->onInputQueueDestroyed(&nActivity, inputQueue);
    if (nActivity.callbacks->onDestroy)
        nActivity.callbacks->onDestroy(&nActivity);

    AInputQueue_destroy(inputQueue);

    printf("Exit.\n");
    return 0;
}
