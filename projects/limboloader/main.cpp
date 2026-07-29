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

    // Init config (also chdir's into paths.game_files), GLES pointers, JNI VM
    init_config(argv[1]);
    sdl_initialize_gles();
    load_gles2_funcs();
    InitJNIBinding(&vm);

    // Gamepad support. sdl_initialize_gles() only brings up SDL_INIT_VIDEO.
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
    const char *path_lcpp = "arm64-v8a/libc++_shared.so";
    if (!load_so_from_file(&lcpp, path_lcpp, 0x40000000))
    {
        printf("Failed to load %s -- libLimbo's std:: imports cannot resolve.\n", path_lcpp);
        return 1;
    }

    printf("Loading libLimbo\n");
    so_module lmain = {};
    uintptr_t addr_lmain = 0x50000000;
    const char *path_lmain = "arm64-v8a/libLimbo.so";
    if (!load_so_from_file(&lmain, path_lmain, addr_lmain))
    {
        printf("Failed to load %s\n", path_lmain);
        return 1;
    }

    FakeJni::LocalFrame frame(vm);
    JNIEnv *env = &frame.getJniEnv();

    printf("calling JNI_OnLoad from libLimbo.so\n");
    auto jniOnLoad = (jint(*)(JavaVM *, void *))(so_symbol(&lmain, "JNI_OnLoad"));
    if (jniOnLoad)
        jniOnLoad(&vm, nullptr);

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

    // The engine writes saves/unpacked assets here; keep them beside the game
    // files rather than in the host's home dir.
    static std::string internalPath = std::filesystem::absolute("files").string();
    static std::string externalPath = std::filesystem::absolute("files").string();
    static std::string obbPath = std::filesystem::absolute("obb").string();
    std::filesystem::create_directories(internalPath);
    std::filesystem::create_directories(obbPath);

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

    int winWidth = 0, winHeight = 0;
    SDL_GL_GetDrawableSize(sdl_win, &winWidth, &winHeight);
    printf("window %dx%d\n", winWidth, winHeight);

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

    // Announce the input devices the game can expect.
    if (deviceAdded)
    {
        deviceAdded(env, activityObj, LIMBO_DEVICE_KEYBOARD);
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
        {
            if (SDL_IsGameController(i))
            {
                SDL_GameControllerOpen(i);
                printf("opened game controller: %s\n", SDL_GameControllerNameForIndex(i));
                deviceAdded(env, activityObj, LIMBO_DEVICE_GAMEPAD);
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

            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_IsGameController(ev.cdevice.which))
                {
                    SDL_GameControllerOpen(ev.cdevice.which);
                    printf("controller connected: %s\n",
                        SDL_GameControllerNameForIndex(ev.cdevice.which));
                    if (deviceAdded)
                        deviceAdded(env, activityObj, LIMBO_DEVICE_GAMEPAD);
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
                if (keyCode == AKEYCODE_UNKNOWN)
                    break;
                AInputQueue_pushKeyEvent(inputQueue, LIMBO_DEVICE_GAMEPAD,
                    AINPUT_SOURCE_GAMEPAD,
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

        // Stands in for the Choreographer frame callback that drives the game.
        reportVSync(env, activityObj, (jlong)frameTimeNanos);

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
