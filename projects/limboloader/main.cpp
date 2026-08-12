// main.cpp -- LIMBO (Playdead) Bogodroid NEO loader.
//
// libLimbo.so is a plain NDK ANativeActivity app, not a Unity/engine-runtime
// port: the whole game is native, and the Java side only ever
//   - creates the activity and forwards the ANativeActivity lifecycle,
//   - pumps Choreographer frame callbacks into native_ReportVSyncCallEvent,
//   - reports input-device add/remove.
// Everything else (window, input, assets, audio, GL) goes through the NDK C
// API. That is what this loader reproduces.
//
// Verified against the shipped arm64-v8a binary's ELF tables:
//   DT_NEEDED : libOpenSLES, libGLESv2, libEGL, libandroid, liblog, libm,
//               libc++_shared, libdl, libc
//   exports   : ANativeActivity_onCreate, JNI_OnLoad,
//               Java_com_playdead_limbo_LimboActivity_native_1{ReportVSyncCallEvent,
//               DeviceAdded, DeviceRemoved, ReportIsPlayable,
//               ReportGameServicesState},
//               Java_com_playdead_limbo_LimboAgeSignals_nativeInit
//   imports   : AInputQueue_*/AInputEvent_*/AKeyEvent_*/AMotionEvent_* (input
//               arrives through the native queue, NOT through JNI), AAsset*,
//               AConfiguration_*, ALooper_*, ANativeWindow_*, egl*, gl*
//
// This replaces the pre-NEO revision, which could not get past the splash for
// three structural reasons, all fixed here:
//   1. onNativeWindowCreated was handed a NULL window,
//   2. the ANativeActivity was memset to zero and never given an AssetManager,
//   3. the "render loop" called the vsync native exactly once and returned.

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
#include <chrono>

#include <baron/baron.h>
#include "javastubs/binding.h"
#include "javastubs/limbo.h"
#include "input_backend.h"

#include "platform.h"
#include "so_util.h"
#include "io_util.h"
#include "logging.h"

#include "ndk.h"
#include "ainput.h"
#include "alooper.h"
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
extern DynLibFunction symtable_limbo_gl[];
extern DynLibFunction symtable_limbo_assets[];

extern SDL_Window *sdl_win;

DynLibFunction *so_static_patches[32] = {
    NULL,
};

DynLibFunction *so_dynamic_libraries[32] = {
    symtable_libc,
    // must precede symtable_ndk: resolution takes the first match, and this
    // overrides AAssetManager_fromJava for Limbo's own chdir behavior (see
    // asset_manager_override.cpp)
    symtable_limbo_assets,
    symtable_ndk,
    // must precede symtable_gles2: resolution takes the first match, and these
    // bind the GL context on Limbo's render thread (see gl_thread_bind.cpp)
    symtable_limbo_gl,
    symtable_gles2,
    symtable_egl_sdl,
    symtable_opensl,
    NULL};

// ---------------------------------------------------------------------------
// native entry points
//
// These are JNI natives, so they take (JNIEnv*, jobject) before their declared
// arguments -- the pre-NEO loader called native_ReportVSyncCallEvent as
// `void(long)`, which put the frame time where JNIEnv* belongs.
// ---------------------------------------------------------------------------

typedef void (*reportVSync_t)(JNIEnv *, jobject, jlong frameTimeNanos);
typedef void (*deviceChanged_t)(JNIEnv *, jobject, jint deviceId);
typedef void (*reportIsPlayable_t)(JNIEnv *, jobject, jboolean playable);
typedef void (*reportGameServices_t)(JNIEnv *, jobject, jint state);

// ---------------------------------------------------------------------------
// input
//
// Limbo reads input from the native AInputQueue (thunks/ndk/ainput.c), so the
// loader translates SDL events into AInputEvents rather than into jnivm
// KeyEvent/MotionEvent objects the way platform/common/input_backend.cpp does.
//
// Android's AKEYCODE_* values are numerically identical to Java's
// KeyEvent.KEYCODE_*, so the mapping below matches input_backend.cpp's.
// ---------------------------------------------------------------------------

// Device ids reported to native_DeviceAdded and carried on every event. These
// deliberately match platform/common/input_backend.cpp's INPUT_ID_* values so a
// game that also asks Java about a device by id gets a consistent answer.
#define LIMBO_DEVICE_KEYBOARD 1
#define LIMBO_DEVICE_GAMEPAD 2
#define LIMBO_DEVICE_MOUSE 3

// AKEYCODE_* (android/keycodes.h)
enum {
    AKEYCODE_UNKNOWN = 0,
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_DPAD_CENTER = 23,
    AKEYCODE_ENTER = 66,
    AKEYCODE_SPACE = 62,
    AKEYCODE_ESCAPE = 111,
    AKEYCODE_MENU = 82,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_L2 = 104,
    AKEYCODE_BUTTON_R2 = 105,
    AKEYCODE_BUTTON_THUMBL = 106,
    AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
    AKEYCODE_BUTTON_MODE = 110,
    AKEYCODE_W = 51,
    AKEYCODE_A = 29,
    AKEYCODE_S = 47,
    AKEYCODE_D = 32,
};

static int32_t controller_button_to_keycode(uint8_t button)
{
    switch (button)
    {
    // SDL's A/B and X/Y are swapped relative to Android's gamepad layout --
    // same swap input_backend.cpp applies.
    case SDL_CONTROLLER_BUTTON_A:             return AKEYCODE_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B:             return AKEYCODE_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X:             return AKEYCODE_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y:             return AKEYCODE_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_BACK:          return AKEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_GUIDE:         return AKEYCODE_BUTTON_MODE;
    case SDL_CONTROLLER_BUTTON_START:         return AKEYCODE_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return AKEYCODE_BUTTON_THUMBL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return AKEYCODE_BUTTON_THUMBR;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return AKEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return AKEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return AKEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return AKEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return AKEYCODE_DPAD_RIGHT;
    default:                                  return AKEYCODE_UNKNOWN;
    }
}

// Keyboard fallback so the game is playable without a pad -- Limbo only needs
// left/right/up/down plus one action button.
static int32_t scancode_to_keycode(SDL_Scancode sc)
{
    switch (sc)
    {
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_A:      return AKEYCODE_DPAD_LEFT;
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_D:      return AKEYCODE_DPAD_RIGHT;
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_W:      return AKEYCODE_DPAD_UP;
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_S:      return AKEYCODE_DPAD_DOWN;
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_RETURN: return AKEYCODE_BUTTON_A;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return AKEYCODE_BUTTON_B;
    case SDL_SCANCODE_ESCAPE: return AKEYCODE_BACK;
    default:                  return AKEYCODE_UNKNOWN;
    }
}

// Whole-gamepad axis state. Android reports a joystick ACTION_MOVE with every
// axis populated, not one axis per event, so the state is kept here and resent
// on each change.
static float gamepad_axes[AINPUT_AXIS_COUNT];

static void push_axis_state(AInputQueue *queue)
{
    AInputQueue_pushJoystickEvent(queue, LIMBO_DEVICE_GAMEPAD,
        AINPUT_SOURCE_JOYSTICK | AINPUT_SOURCE_GAMEPAD, gamepad_axes);
}

static bool sdl_axis_to_android(uint8_t sdl_axis, int32_t *out_axis, bool *is_trigger)
{
    switch (sdl_axis)
    {
    case SDL_CONTROLLER_AXIS_LEFTX:        *out_axis = AMOTION_EVENT_AXIS_X;  *is_trigger = false; return true;
    case SDL_CONTROLLER_AXIS_LEFTY:        *out_axis = AMOTION_EVENT_AXIS_Y;  *is_trigger = false; return true;
    case SDL_CONTROLLER_AXIS_RIGHTX:       *out_axis = AMOTION_EVENT_AXIS_Z;  *is_trigger = false; return true;
    case SDL_CONTROLLER_AXIS_RIGHTY:       *out_axis = AMOTION_EVENT_AXIS_RZ; *is_trigger = false; return true;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  *out_axis = AMOTION_EVENT_AXIS_LTRIGGER; *is_trigger = true; return true;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: *out_axis = AMOTION_EVENT_AXIS_RTRIGGER; *is_trigger = true; return true;
    default: return false;
    }
}

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

    // Init config (also chdir's into paths.game_files) and the JNI VM.
    //
    // GL setup goes through eglGetDisplay_impl rather than sdl_initialize_gles():
    // both create the window and context, but only the former also captures the
    // EGL handles the game's EGL calls are answered from. Using
    // sdl_initialize_gles() here left egl_display unset, so the game's own
    // InitEGL ran the whole path a second time and replaced the window and
    // context out from under everything.
    //
    // It has to happen before libLimbo is loaded: so_resolve_link() patches
    // unresolved imports to a crash stub during relocation and never looks
    // again, so the GLES entry points must exist by then.
    init_config(argv[1]);
    eglGetDisplay_impl(nullptr);

    // That left the context current on this thread. Limbo renders from its own
    // "LIMBO game" thread and never calls eglMakeCurrent, so gl_thread_bind.cpp
    // binds it there on the first GL call -- which fails with EGL_BAD_ACCESS
    // while another thread still owns it. This thread does no GL of its own
    // (the game swaps its own buffers), so hand it over.
    SDL_GL_MakeCurrent(sdl_win, NULL);

    InitJNIBinding(&vm);

    // Gamepad support. SDL video comes up later, inside the game's eglGetDisplay.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
        printf("SDL gamecontroller init failed (%s) -- keyboard only\n", SDL_GetError());

    // ---- load the C++ runtime first ----
    //
    // libLimbo.so links against libc++_shared.so (it is in its DT_NEEDED) and
    // imports 36 symbols from it -- std::string/ofstream methods, operator
    // new/delete, __cxa_guard_*. Nothing in thunks/ provides those, so they can
    // only be resolved out of the shipped runtime .so.
    //
    // Order matters twice over. so_load() relocates a module against every
    // module already in the chain and then runs its init_array, so libc++ has
    // to be in place before libLimbo is relocated, and its static constructors
    // (which build std::cout/cerr and the locale tables) have to run before the
    // game's. Loading it first gets both for free. Same sequence the ct_nx
    // Switch port uses for libchrono.so.
    printf("Loading libc++_shared\n");
    so_module lcpp = {};
    const char *path_lcpp = "lib/arm64-v8a/libc++_shared.so";
    if (!load_so_from_file(&lcpp, path_lcpp, 0x40000000))
    {
        printf("Failed to load %s -- libLimbo's std:: imports cannot resolve.\n", path_lcpp);
        return 1;
    }

    printf("Loading libLimbo\n");
    so_module lmain = {};
    uintptr_t addr_lmain = 0x50000000;
    const char *path_lmain = "lib/arm64-v8a/libLimbo.so";
    if (!load_so_from_file(&lmain, path_lmain, addr_lmain))
    {
        printf("Failed to load %s\n", path_lmain);
        return 1;
    }

    // Remove the engine's hardcoded 1024px backbuffer-width cap.
    //
    // Note: this exposes a second, still-unlocated hardcoded-1024 assumption
    // elsewhere in libLimbo.so -- confirmed by A/B testing at 1024x576 vs
    // native res -- that corrupts certain scenes (horizontal stripes / black
    // squares, wedge-shaped toward one edge, consistent with a row-stride
    // mismatch). Re-enabled here for another look; if the artifact is a
    // dealbreaker again, comment the byte-patch block below back out.
    //
    // CreateWindowToGameBinding (decompiled via Ghidra; real/file address
    // 0x241cb8, using this project's established Ghidra-VA-minus-0x100000
    // convention, cross-checked exactly against the real ELF address of
    // native_ReportVSyncCallEvent) computes its render target from the real
    // ANativeWindow size, then unconditionally clamps it:
    //
    //   iVar11 = <the real display width, computed either directly or via
    //             an aspect-ratio roundtrip that reduces to the same value>;
    //   if (0x3ff < iVar11) iVar11 = 0x400;   // hardcoded clamp to 1024
    //
    // which is why the game always renders at 1024x576 regardless of the
    // real screen size, and regardless of anything in assets/settings.txt --
    // that file's backbufferheight/per-GPU platform table is parsed (see
    // FUN_0034a84c) but never consulted by this code path at all.
    //
    // The clamp compiles to (disassembly, real/file-relative addresses):
    //   241cac  cmp   w8, #0x400
    //   241cb0  mov   w9, #0x400
    //   241cb8  csel  w20, w8, w9, lt   ; w20 = (w8 < 0x400) ? w8 : w9
    //
    // Flipping the csel's condition nibble from LT (0xb) to AL (0xe) makes
    // it always select w8 -- the pre-clamp value, i.e. the real display
    // width -- with no hardcoded replacement number of our own, so it keeps
    // tracking whatever size we report via config/ANativeWindow forever.
    //
    // Different builds of libLimbo.so (Play Store vs Epic Games Store, etc.)
    // compile this to different addresses -- each entry here is the same
    // csel's condition byte in one known build. Try each until one matches.
    static const uintptr_t backbuffer_cap_offsets[] = {
        0x241cb9, // Play Store build (SM8250/Adreno device)
        0x229bcd, // Epic Games Store build (RK3566/Mali device)
    };
    uint8_t *csel_cond_byte = nullptr;
    for (uintptr_t off : backbuffer_cap_offsets)
    {
        uint8_t *candidate = (uint8_t *)(lmain.base + off);
        if (*candidate == 0xb1)
        {
            csel_cond_byte = candidate;
            break;
        }
    }
    if (csel_cond_byte)
    {
        *csel_cond_byte = 0xe1;
        __builtin___clear_cache((char *)csel_cond_byte, (char *)csel_cond_byte + 1);
        printf("[patch] removed libLimbo's hardcoded 1024px backbuffer-width cap\n");
    }
    else
    {
        printf("[patch] WARNING: backbuffer-cap patch site not found in any known build "
               "-- skipping; game will render at a fixed low resolution\n");
    }

    FakeJni::LocalFrame frame(vm);
    JNIEnv *env = &frame.getJniEnv();

    printf("calling JNI_OnLoad from libLimbo.so\n");
    auto jniOnLoad = (jint(*)(JavaVM *, void *))(so_symbol(&lmain, "JNI_OnLoad"));
    if (jniOnLoad)
        jniOnLoad(&vm, nullptr);

    // On real Android, MainActivity's onCreate calls LimboAgeSignals.nativeInit()
    // once at startup -- Java calling into native, the opposite direction from
    // every native_Report*/native_Device* export below, so it never gets called
    // by anything the engine itself drives. Decompiling it (Java_com_playdead_
    // limbo_LimboAgeSignals_nativeInit) shows it does NewGlobalRef(env, class)
    // then GetStaticMethodID(env, classRef, "isAgeResolved", ...) and caches
    // both; skip this and any later isAgeResolved() call goes through a null
    // class ref / method id, reading (or crashing into) "never resolved" --
    // exactly the shape of a stuck-at-title-with-no-progress symptom. The
    // loader has to make this call itself since there is no real Java side to.
    {
        auto ageSignalsEnv = jnivm::ENV::FromJNIEnv(env);
        auto ageSignalsClass = vm.findClass("com/playdead/limbo/LimboAgeSignals");
        jclass ageSignalsClassObj =
            jnivm::JNITypes<std::shared_ptr<jnivm::Class>>::ToJNIType(ageSignalsEnv, ageSignalsClass);
        auto ageSignalsInit = (void (*)(JNIEnv *, jclass))so_symbol(&lmain,
            "Java_com_playdead_limbo_LimboAgeSignals_nativeInit");
        printf("LimboAgeSignals.nativeInit=%p\n", (void *)ageSignalsInit);
        if (ageSignalsInit)
            ageSignalsInit(env, ageSignalsClassObj);
    }

    auto reportVSync = (reportVSync_t)so_symbol(&lmain,
        "Java_com_playdead_limbo_LimboActivity_native_1ReportVSyncCallEvent");
    auto deviceAdded = (deviceChanged_t)so_symbol(&lmain,
        "Java_com_playdead_limbo_LimboActivity_native_1DeviceAdded");
    auto deviceRemoved = (deviceChanged_t)so_symbol(&lmain,
        "Java_com_playdead_limbo_LimboActivity_native_1DeviceRemoved");
    auto reportIsPlayable = (reportIsPlayable_t)so_symbol(&lmain,
        "Java_com_playdead_limbo_LimboActivity_native_1ReportIsPlayable");
    auto reportGameServices = (reportGameServices_t)so_symbol(&lmain,
        "Java_com_playdead_limbo_LimboActivity_native_1ReportGameServicesState");

    printf("reportVSync=%p deviceAdded=%p deviceRemoved=%p isPlayable=%p gameServices=%p\n",
        (void *)reportVSync, (void *)deviceAdded, (void *)deviceRemoved,
        (void *)reportIsPlayable, (void *)reportGameServices);

    if (!reportVSync)
    {
        printf("native_ReportVSyncCallEvent is missing -- the game cannot be stepped.\n");
        return 1;
    }

    // ---- ANativeActivity ----
    //
    // ANativeActivity_create() initialises the struct, allocates the callback
    // table and builds the AAssetManager, which the pre-NEO loader never did
    // (it left assetManager NULL, so nothing past the splash could load).
    // `env` comes from this function's LocalFrame, which outlives the activity.
    ANativeActivity nActivity =
        ANativeActivity_create<jnivm::com::playdead::limbo::LimboActivity>(&vm, env, "assets");

    // The helper builds the activity with make_shared, which bypasses jnivm's
    // class factory: the resulting object carries no clazz, so any
    // GetObjectClass on it yields the "Invalid" class and every method lookup
    // silently fails (see libjnivm/src/jnivm/vm.cpp:252). NDK apps reach back
    // into Java through exactly that call on activity->clazz, so instantiate
    // properly and replace it -- same fix laytonloader needed.
    auto activityEnv = jnivm::ENV::FromJNIEnv(env);
    auto activityClass = vm.findClass("com/playdead/limbo/LimboActivity");
    auto activity = activityClass->Instantiate(activityEnv);
    nActivity.clazz =
        std::dynamic_pointer_cast<jnivm::android::app::NativeActivity>(activity);

    jobject activityObj =
        jnivm::JNITypes<std::shared_ptr<jnivm::Object>>::ToJNIType(activityEnv, activity);
    printf("LimboActivity instance=%p class=%s\n", (void *)activityObj,
        activity && activity->getClassInternal(activityEnv)
            ? activity->getClassInternal(activityEnv)->getName().c_str()
            : "(none)");

    // Just valid-looking paths for the ANativeActivity struct fields -- Limbo
    // doesn't actually read or write through them (its real save state goes
    // through the SaveGame_* JNI stubs in javastubs/limbo.cpp instead, next
    // to the binary), so there's nothing to create on disk here.
    static std::string internalPath = std::filesystem::absolute("files").string();
    static std::string externalPath = std::filesystem::absolute("files").string();
    static std::string obbPath = std::filesystem::absolute("obb").string();

    nActivity.internalDataPath = internalPath.c_str();
    nActivity.externalDataPath = externalPath.c_str();
    nActivity.obbPath = obbPath.c_str();
    nActivity.sdkVersion = 25;

    printf("calling ANativeActivity_onCreate from libLimbo.so\n");
    auto onCreate = (void (*)(ANativeActivity *, void *, size_t))(
        so_symbol(&lmain, "ANativeActivity_onCreate"));
    if (!onCreate)
    {
        printf("ANativeActivity_onCreate not found.\n");
        return 1;
    }
    // (activity, savedState, savedStateSize) -- the pre-NEO loader passed only
    // the activity, leaving the saved-state pointer as whatever was in x1.
    onCreate(&nActivity, nullptr, 0);

    // onCreate spawns the engine's own thread, which prepares its ALooper and
    // then blocks waiting for a window. Everything below runs on this thread,
    // standing in for Android's UI thread.

    // The window does not exist yet -- the game creates it during InitEGL,
    // which happens below in onNativeWindowCreated -- so report the configured
    // size for now and re-read it once it is real.
    int winWidth = config["device"]["displayWidth"].value_or<int>(1280);
    int winHeight = config["device"]["displayHeight"].value_or<int>(720);
    if (sdl_win)
        SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);
    printf("window %dx%d\n", winWidth, winHeight);

    // ANativeWindow_getWidth/getHeight answer out of config (thunks/ndk/ndk.cpp),
    // so if the real drawable differs from what the config asked for -- a
    // compositor forcing fullscreen, say -- the engine lays out and hit-tests
    // against the wrong size and every touch lands in the wrong place. Publish
    // the size we actually got.
    if (auto *dev = config["device"].as_table())
    {
        dev->insert_or_assign("displayWidth", winWidth);
        dev->insert_or_assign("displayHeight", winHeight);
    }

    // A real (non-NULL) ANativeWindow. eglCreateWindowSurface_impl ignores its
    // contents and hands back the SDL surface, but the engine null-checks the
    // pointer and reads width/height off it.
    ANativeWindow *window = ANativeWindow_fromSurface(nullptr, nullptr);

    AInputQueue *inputQueue = AInputQueue_create();
    if (!inputQueue)
    {
        printf("Could not create the native input queue.\n");
        return 1;
    }

    // Lifecycle order matches Android's: the activity is started and resumed
    // first, and only then does the window/input surface arrive. (The pre-NEO
    // loader created the window before onStart, which is the reverse.)
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

    // InitEGL runs inside that callback and is what actually creates sdl_win
    // (see eglGetDisplay_impl); pick up the real size now that it exists.
    if (sdl_win)
        SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);

    if (nActivity.callbacks->onWindowFocusChanged)
    {
        printf("calling onWindowFocusChanged(1)\n");
        nActivity.callbacks->onWindowFocusChanged(&nActivity, 1);
    }

    // Licensing / Play Games gates. Both are fire-and-forget reports from Java;
    // with no Play Services the engine otherwise waits on them forever.
    if (reportIsPlayable)
    {
        printf("reporting isPlayable=true\n");
        reportIsPlayable(env, activityObj, JNI_TRUE);
    }
    if (reportGameServices)
    {
        // 0 == signed out / unavailable.
        printf("reporting game services state=0\n");
        reportGameServices(env, activityObj, 0);
    }

    // The Java InputManager/InputDevice plumbing below is a real, independent
    // fix (registerInputDeviceListener used to be a no-op that never stored
    // the listener, so android.hardware.input.InputManager.InputDeviceListener
    // could never fire) -- but it turned out NOT to be what gates Limbo's own
    // touch-vs-press prompt. That check (BoyInput_UsingGameController(),
    // reverse engineered from the shipped .so) just reads bit 8 of a
    // per-player flags struct that the engine's own input processing sets the
    // first time it sees a real gamepad-sourced AKeyEvent/AMotionEvent come
    // through AInputQueue. ChooseStartLabelBasedOnInputType runs once, right
    // as the Press Start screen first appears (a second or two into boot) --
    // long before the player has had any chance to press anything -- so that
    // bit is still 0 at decision time no matter how correctly later button
    // presses are reported. See the push_axis_state() call right below: it
    // exists specifically to get one real gamepad-sourced event processed
    // before that screen ever asks.
    InputBackend::instance();

    // Announce the input devices the game can expect.
    if (deviceAdded)
    {
        deviceAdded(env, activityObj, LIMBO_DEVICE_KEYBOARD);
        // Limbo's press-start screen defaults to its touch prompt
        // (touchStartEntry) rather than the button prompt, and nothing below
        // ever sent it a touch/pointer event to satisfy that -- SDL_FINGER*/
        // SDL_MOUSEBUTTON* were never handled at all, so neither a real
        // touchscreen tap nor a synthesized one ever reached the engine.
        deviceAdded(env, activityObj, LIMBO_DEVICE_MOUSE);
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
        {
            if (SDL_IsGameController(i))
            {
                SDL_GameControllerOpen(i);
                printf("opened game controller: %s\n", SDL_GameControllerNameForIndex(i));
                deviceAdded(env, activityObj, LIMBO_DEVICE_GAMEPAD);
                jnivm::android::hardware::input::InputManager::NotifyDeviceAdded(LIMBO_DEVICE_GAMEPAD);
                // A synthetic axis/motion event here did NOT trigger "setting
                // current game controller" -- that only ever fires on a real
                // key press (BOYINPUT_JUMP/UP/LEFT/RIGHT/DOWN/ACTION are all
                // discrete buttons, not analog motion), so the flag this is
                // meant to pre-empt is evidently set from key events
                // specifically. AKEYCODE_BUTTON_SELECT ("back") is not bound
                // to any real gameplay action, and at this point in startup
                // no scene/player exists yet to react to it anyway.
                AInputQueue_pushKeyEvent(inputQueue, LIMBO_DEVICE_GAMEPAD,
                    AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK,
                    AKEY_EVENT_ACTION_DOWN, AKEYCODE_BUTTON_SELECT, 0);
                AInputQueue_pushKeyEvent(inputQueue, LIMBO_DEVICE_GAMEPAD,
                    AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK,
                    AKEY_EVENT_ACTION_UP, AKEYCODE_BUTTON_SELECT, 0);
                break;
            }
        }
    }

    printf("Entering frame loop\n");

    // native_ReportVSyncCallEvent takes the Choreographer's frameTimeNanos: a
    // monotonic nanosecond clock. It was previously called once, with 100000,
    // which is why the game never advanced past its first frame.
    const auto startTime = std::chrono::steady_clock::now();
    bool running = true;
    // Whether to accept pointer events as touch at all. Decided up front from
    // whether SDL sees a real touch device, rather than latched on the first
    // finger event: this panel reports as both, and waiting for a finger left
    // a window at startup where a stray pointer motion still got forwarded --
    // the engine saw a MOVE with no preceding DOWN (at a degenerate edge
    // position) and rejected the touch that followed it.
    const bool have_touch_device = SDL_GetNumTouchDevices() > 0;
    printf("touch devices: %d (pointer %s treated as touch)\n",
        SDL_GetNumTouchDevices(), have_touch_device ? "will NOT be" : "will be");
    // The one finger being forwarded to the engine, if any.
    constexpr SDL_FingerID NO_FINGER = -1;
    SDL_FingerID active_finger = NO_FINGER;
    // Limbo's own internal backbuffer/touch space, confirmed fixed at 1024x576
    // regardless of the ANativeWindow size we report (1280x720 -> 1024x576 at
    // 0.8x, 1920x1080 -> 1024x576 at 0.533x -- always the same target). Touch
    // coordinates need to land in THIS space, not the reported window size:
    // scaling by ANativeWindow_getWidth/Height only happened to look right for
    // taps near the center, since e.g. 0.5 normalized * 1920 ~= 960, still
    // under 1024 by luck -- a tap further out would compute a value over the
    // bound the engine actually checks against.
    constexpr float LIMBO_TOUCH_WIDTH = 1024.0f;
    constexpr float LIMBO_TOUCH_HEIGHT = 576.0f;

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

            case SDL_KEYDOWN:
            case SDL_KEYUP:
            {
                if (ev.key.repeat && ev.type == SDL_KEYDOWN)
                    break;    // repeats are reported via repeatCount, not new events
                const int32_t keyCode = scancode_to_keycode(ev.key.keysym.scancode);
                if (keyCode == AKEYCODE_UNKNOWN)
                    break;
                AInputQueue_pushKeyEvent(inputQueue, LIMBO_DEVICE_KEYBOARD,
                    AINPUT_SOURCE_KEYBOARD,
                    ev.type == SDL_KEYDOWN ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP,
                    keyCode, 0);
                break;
            }

            // Touch has to be reported in the coordinate space the engine
            // believes its window to be, which is what ANativeWindow_getWidth/
            // Height hand it -- the configured displayWidth/Height, NOT the
            // host window's actual pixel size. Those were the same thing while
            // the window was a fixed 640x480, but once it goes fullscreen the
            // drawable is whatever the panel is, and scaling by that put every
            // touch in the wrong space: the engine applies its own
            // window->game transform on top (1280x720 -> its 1024x576 touch
            // bounds, letterboxing included), so the mismatch came back out as
            // the negative and past-the-bottom coordinates it rejects.
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
            {
                // Follow one finger at a time. AInputQueue_pushMotionEvent
                // carries a single pointer (pointerCount 1, pointerId 0), so
                // every finger was being flattened onto the same pointer id:
                // a second finger interleaved its own DOWN/MOVE into the
                // first one's stream, which is what the engine was reporting
                // as "Began arrived after Move - missing Ended" (the tell was
                // a MOVE and a DOWN microseconds apart at unrelated
                // positions). Limbo only needs a single touch point, so track
                // the first finger down and ignore the rest until it lifts.
                if (ev.type == SDL_FINGERDOWN)
                {
                    if (active_finger != NO_FINGER)
                    {
                        printf("finger DOWN ignored: already following finger %lld\n",
                            (long long)active_finger);
                        break;              // already following another finger
                    }
                    active_finger = ev.tfinger.fingerId;
                }
                else if (ev.tfinger.fingerId != active_finger)
                    break;                  // a finger we are not following

                const int32_t action =
                    ev.type == SDL_FINGERDOWN ? AMOTION_EVENT_ACTION_DOWN
                    : ev.type == SDL_FINGERUP ? AMOTION_EVENT_ACTION_UP
                                              : AMOTION_EVENT_ACTION_MOVE;
                const float x = ev.tfinger.x * LIMBO_TOUCH_WIDTH;
                const float y = ev.tfinger.y * LIMBO_TOUCH_HEIGHT;
                if (ev.type != SDL_FINGERMOTION)
                    printf("finger %s: raw=(%.3f,%.3f) engine-space=(%.1f,%.1f)\n",
                        ev.type == SDL_FINGERDOWN ? "DOWN" : "UP",
                        ev.tfinger.x, ev.tfinger.y, x, y);
                AInputQueue_pushMotionEvent(inputQueue, LIMBO_DEVICE_MOUSE,
                    AINPUT_SOURCE_TOUCHSCREEN, action, x, y);

                if (ev.type == SDL_FINGERUP)
                    active_finger = NO_FINGER;
                break;
            }

            // Fallback for touchscreens whose driver surfaces taps as a mouse
            // rather than through SDL's finger API (common on embedded
            // kmsdrm/evdev setups without native multitouch support). These
            // arrive in real window pixels, so normalise before rescaling into
            // the engine's space the same way finger events are.
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (!have_touch_device && ev.button.button == SDL_BUTTON_LEFT)
                {
                    const float x = (float)ev.button.x / winWidth * LIMBO_TOUCH_WIDTH;
                    const float y = (float)ev.button.y / winHeight * LIMBO_TOUCH_HEIGHT;
                    printf("mouse %s: raw=(%d,%d) engine-space=(%.1f,%.1f)\n",
                        ev.type == SDL_MOUSEBUTTONDOWN ? "DOWN" : "UP",
                        ev.button.x, ev.button.y, x, y);
                    AInputQueue_pushMotionEvent(inputQueue, LIMBO_DEVICE_MOUSE,
                        AINPUT_SOURCE_TOUCHSCREEN,
                        ev.type == SDL_MOUSEBUTTONDOWN ? AMOTION_EVENT_ACTION_DOWN
                                                        : AMOTION_EVENT_ACTION_UP,
                        x, y);
                }
                break;

            case SDL_MOUSEMOTION:
                if (!have_touch_device && (ev.motion.state & SDL_BUTTON_LMASK))
                    AInputQueue_pushMotionEvent(inputQueue, LIMBO_DEVICE_MOUSE,
                        AINPUT_SOURCE_TOUCHSCREEN, AMOTION_EVENT_ACTION_MOVE,
                        (float)ev.motion.x / winWidth * LIMBO_TOUCH_WIDTH,
                        (float)ev.motion.y / winHeight * LIMBO_TOUCH_HEIGHT);
                break;

            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_IsGameController(ev.cdevice.which))
                {
                    SDL_GameControllerOpen(ev.cdevice.which);
                    printf("controller connected: %s\n",
                        SDL_GameControllerNameForIndex(ev.cdevice.which));
                    if (deviceAdded)
                        deviceAdded(env, activityObj, LIMBO_DEVICE_GAMEPAD);
                    jnivm::android::hardware::input::InputManager::NotifyDeviceAdded(LIMBO_DEVICE_GAMEPAD);
                }
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                printf("controller disconnected\n");
                if (deviceRemoved)
                    deviceRemoved(env, activityObj, LIMBO_DEVICE_GAMEPAD);
                break;

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            {
                const int32_t keyCode = controller_button_to_keycode(ev.cbutton.button);
                if (ev.type == SDL_CONTROLLERBUTTONDOWN)
                {
                    const char *buttonName = SDL_GameControllerGetStringForButton(
                        (SDL_GameControllerButton)ev.cbutton.button);
                    printf("controller button pressed: raw=%d (%s) -> keycode=%d%s\n",
                        ev.cbutton.button, buttonName ? buttonName : "?", keyCode,
                        keyCode == AKEYCODE_UNKNOWN ? " (unmapped)" : "");
                    fflush(stdout);
                }
                if (keyCode == AKEYCODE_UNKNOWN)
                    break;
                // Matches push_axis_state below: a real Android gamepad reports
                // GAMEPAD | JOYSTICK on all of its input, not just its axes.
                // Limbo's own "am I looking at joystick input?" check (the one
                // that switches its press-start prompt away from the touch
                // label) tests for the JOYSTICK source class, which plain
                // AINPUT_SOURCE_GAMEPAD alone does not carry -- button events
                // still registered and read as valid keycodes (hence the DOWN
                // events showing up in the logs) but were invisible to that
                // check, so the prompt never left touch mode no matter what
                // was pressed.
                AInputQueue_pushKeyEvent(inputQueue, LIMBO_DEVICE_GAMEPAD,
                    AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK,
                    ev.type == SDL_CONTROLLERBUTTONDOWN ? AKEY_EVENT_ACTION_DOWN
                                                        : AKEY_EVENT_ACTION_UP,
                    keyCode, 0);
                break;
            }

            case SDL_CONTROLLERAXISMOTION:
            {
                int32_t axis = 0;
                bool isTrigger = false;
                if (!sdl_axis_to_android(ev.caxis.axis, &axis, &isTrigger))
                    break;
                // Triggers are 0..1, sticks are -1..1 (SDL's negative range is
                // one larger, hence the split divisor).
                const float value = isTrigger
                    ? ev.caxis.value / 32767.0f
                    : (ev.caxis.value < 0 ? ev.caxis.value / 32768.0f
                                          : ev.caxis.value / 32767.0f);
                gamepad_axes[axis] = value;
                // Mirror the left stick onto the hat axes: Limbo's controls are
                // digital, and some NDK titles only read HAT_X/HAT_Y.
                if (axis == AMOTION_EVENT_AXIS_X)
                    gamepad_axes[AMOTION_EVENT_AXIS_HAT_X] = value;
                else if (axis == AMOTION_EVENT_AXIS_Y)
                    gamepad_axes[AMOTION_EVENT_AXIS_HAT_Y] = value;
                push_axis_state(inputQueue);
                break;
            }

            default:
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const int64_t frameTimeNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - startTime).count();

        // Re-report playability/services once a second, capped at 3 attempts:
        // on Android these arrive from Java after license/sign-in resolve,
        // which can be after the engine starts waiting, so a single report at
        // startup can race and be missed (stuck title screen, no audio).
        // Each call is treated as a fresh "just became playable" transition
        // though -- it recreates the OpenSL engine and resets the animation/
        // audio clock baseline -- so repeats must stop once the race window
        // has passed, not run for the whole session.
        {
            static int attemptsLeft = 3;
            static int64_t lastGateNs = 0;
            if (attemptsLeft > 0 && frameTimeNanos - lastGateNs > 1000000000LL)
            {
                lastGateNs = frameTimeNanos;
                --attemptsLeft;
                if (reportIsPlayable)
                    reportIsPlayable(env, activityObj, JNI_TRUE);
                if (reportGameServices)
                    reportGameServices(env, activityObj, 0);
            }
        }

        // Stands in for the Choreographer frame callback that drives the
        // game. Without it, reportVSync would fire uncapped (500-1000+/s) --
        // the only other throttle in this loop is the 1ms SDL_Delay below.
        //
        // The engine only signals its render thread on every OTHER call at a
        // normal (<=20ms) cadence; the rest just update bookkeeping. So a
        // report rate of N Hz yields N/2 rendered frames/s. It measures real
        // elapsed time per rendered frame itself (no fixed 1/30s timestep
        // constant), so that should hold at 1x speed for any N -- confirmed
        // true at 60 (30 FPS). 120 (60 FPS) measures the FPS counter right
        // but still runs gameplay at 2x speed, not yet root-caused (possibly
        // the engine's own adaptive "OnVSyncEvent ... adjusting to NHz
        // display" logic misreading the rate) -- so [video] vsync_hz stays
        // at 60 until that's understood.
        //
        // The deadline advances by a fixed interval rather than resetting to
        // the check time, so the report cadence is exact regardless of how
        // late a given SDL_Delay(1) poll lands; any backlog is flushed with
        // on-time deltas so the engine never sees a spurious "slow vsync".
        {
            static const int64_t vsyncIntervalNs =
                1'000'000'000LL / config["video"]["vsync_hz"].value_or<int>(60);
            static int64_t nextVsyncNs = -1;
            static int64_t lastCheckNs = 0;
            static bool haveLast = false;

            // A >20ms gap here is a stall in our own loop (we're driven by
            // the SDL_Delay(1) below), not the engine's concern.
            if (haveLast && frameTimeNanos - lastCheckNs > 20'000'000LL)
                printf("[vsync] %lld ns (%.1fms) since last check -- stall in our own loop\n",
                    (long long)(frameTimeNanos - lastCheckNs),
                    (frameTimeNanos - lastCheckNs) / 1e6);
            lastCheckNs = frameTimeNanos;

            if (nextVsyncNs < 0)
                nextVsyncNs = frameTimeNanos;
            while (frameTimeNanos >= nextVsyncNs)
            {
                reportVSync(env, activityObj, (jlong)nextVsyncNs);
                nextVsyncNs += vsyncIntervalNs;
            }
            haveLast = true;
        }

        // The engine renders and swaps on its own thread through our EGL shim
        // (eglSwapBuffers_impl -> SDL_GL_SwapWindow), so the loader must not
        // swap here as well -- unlike laytonloader, where the game only draws
        // into the back buffer and the loader owns the swap.

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
