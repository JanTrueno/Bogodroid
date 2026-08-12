/* main.cpp
 *
 * Geometry Dash (Android, arm64) on ARM Linux, via Bogodroid.
 *
 * The game is RobTop's cocos2d-x 2.2 fork inside libcocos2dcpp.so plus the
 * stock Android libfmod.so; both are ELF aarch64 and load natively through
 * Bogodroid's so_util (libfmod first, so the game's FMOD imports
 * cross-resolve from it). The Android Java layer is provided by the Baron VM
 * (javastubs/): the game's JniHelper calls land on Cocos2dxHelper /
 * BaseRobTopActivity / org.fmod.*. The GLSurfaceView render thread is this
 * main thread: bring up the SDL/EGL context (the game imports no EGL at all,
 * its GLES2 imports bind straight to the glad loader), call
 * Cocos2dxRenderer.nativeInit(w, h), then loop input dispatch ->
 * nativeRender -> swap.
 *
 * Assets are read loose from <gamefiles>/assets/ via the search path added
 * below (cocos2d fopen()s '/'-rooted search paths directly, so no .apk is
 * needed); FMOD reads music/sfx from the same folder via the
 * createSound/createStream path rewrite (imports.cpp). Save files and
 * prefs.txt land in <gamefiles>/save/.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <baron/baron.h>
#include <toml++/toml.hpp>

#include "egl_sdl.h"
#include "glad.h"
#include "gles2.h"

#include "so_util.h"
#include "io_util.h"

#include "debug_utils.h"

#include "globals.h"
#include "cocos2dx.h"
#include "robtop.h"
#include "imports.h"

toml::table config;

// platform/common/config.cpp: chdir into the game files dir
bool init_config(const char *config_path);

extern DynLibFunction symtable_gdash[];
extern DynLibFunction symtable_libc[];
extern DynLibFunction symtable_gles2[];
void InitJNIBinding(FakeJni::Jvm *vm);

Baron::Jvm vm;

// egl_sdl.cpp globals (the SDL window + the real EGL handles behind it)
extern SDL_Window *sdl_win;
extern SDL_GLContext sdl_ctx;
extern EGLDisplay egl_display;
extern EGLContext egl_context;
extern EGLSurface egl_surface;

// egl_sdl.cpp: swaps via the real eglSwapBuffers with the engine's own
// display/surface (it needs a context current on the calling thread, which
// is true here -- this is the thread that owns the context)
EGLBoolean eglSwapBuffers_impl(EGLDisplay display, EGLSurface surface);

so_module fmod_mod; // libfmod.so       (loaded first: exports feed the game)
so_module game_mod; // libcocos2dcpp.so

static volatile sig_atomic_t g_quit = 0;
volatile int gdash_block_back_button = 0;

// The JNI env handed to the game (a LocalFrame's env, alive for the whole
// run -- cocos2d's JniHelper caches it).
static JNIEnv *g_env = nullptr;

static int screen_width = 1280;
static int screen_height = 720;

static double cursor_speed = 900.0; // px/s at 720p

static void noop_void()
{
}

// ---------------------------------------------------------------------------
// Clamp CCDirector::calculateDeltaTime. After a long stall (starting a level,
// decoding assets -- seconds on this class of hardware), the raw frame delta
// makes GJBaseGameLayer::update run tens of thousands of catch-up 1/240 s
// physics steps inside a single frame -- the "freeze" (the engine only clamps
// the delta in debug builds). Replicating the original with a 1/30 s cap:
// m_fDeltaTime at +160, m_fSmoothFix at +164, m_bNextDeltaTimeZero at +304,
// m_pLastTime at +296, offsets read from the exported calculateDeltaTime.
// ---------------------------------------------------------------------------

struct cc_timeval
{
    long long tv_sec;
    int tv_usec;
};

#define DIR_DELTA 160
#define DIR_SMOOTH 164
#define DIR_NEXT_ZERO 304
#define DIR_LAST_TIME 296
#define MAX_FRAME_DELTA (1.0f / 30.0f)

static void (*real_applySmoothFix)(void *self);
static int (*real_gettimeofdayCocos2d)(struct cc_timeval *tv, void *tz);

static void calc_delta_time_hook(void *self)
{
    struct cc_timeval now;
    if (real_gettimeofdayCocos2d(&now, NULL) != 0)
    {
        *(float *)((char *)self + DIR_DELTA) = 0.f;
        *(float *)((char *)self + DIR_SMOOTH) = 0.f;
        return;
    }

    struct cc_timeval *last = *(struct cc_timeval **)((char *)self + DIR_LAST_TIME);

    if (*(uint8_t *)((char *)self + DIR_NEXT_ZERO))
    {
        *(uint8_t *)((char *)self + DIR_NEXT_ZERO) = 0;
        *(float *)((char *)self + DIR_DELTA) = 0.f;
        *(float *)((char *)self + DIR_SMOOTH) = 0.f;
    }
    else
    {
        float dt = (float)(now.tv_sec - last->tv_sec) +
                   (float)(now.tv_usec - last->tv_usec) / 1000000.0f;
        if (dt > MAX_FRAME_DELTA)
            dt = MAX_FRAME_DELTA; // clamp the catch-up
        if (dt > 0.f)
            *(float *)((char *)self + DIR_DELTA) = dt;
    }

    real_applySmoothFix(self);

    if (last)
    {
        last->tv_sec = now.tv_sec;
        last->tv_usec = now.tv_usec;
    }
}

static void install_delta_clamp()
{
    real_applySmoothFix = (void (*)(void *))so_symbol(
        &game_mod, "_ZN7cocos2d10CCDirector14applySmoothFixEv");
    real_gettimeofdayCocos2d = (int (*)(struct cc_timeval *, void *))so_symbol(
        &game_mod, "_ZN7cocos2d6CCTime19gettimeofdayCocos2dEPNS_10cc_timevalEPv");
    uintptr_t calc = so_symbol(&game_mod, "_ZN7cocos2d10CCDirector18calculateDeltaTimeEv");
    if (real_applySmoothFix && real_gettimeofdayCocos2d && calc)
    {
        hook_address(&game_mod, calc, (uintptr_t)&calc_delta_time_hook);
        printf("[patch] clamped CCDirector::calculateDeltaTime to 1/30 s\n");
        fflush(stdout);
    }
    else
    {
        printf("[patch] WARNING: calculateDeltaTime hook unavailable, level "
               "stalls may catch up in one frame\n");
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// file layout checks
// ---------------------------------------------------------------------------

static void check_data(void)
{
    struct stat st;
    if (stat("lib/arm64-v8a/libcocos2dcpp.so", &st) < 0)
        fatal_error("Could not find lib/arm64-v8a/libcocos2dcpp.so.\nExtract it from the APK's lib/arm64-v8a/ into the game files folder.");
    if (stat("lib/arm64-v8a/libfmod.so", &st) < 0)
        fatal_error("Could not find lib/arm64-v8a/libfmod.so.\nExtract it from the APK's lib/arm64-v8a/ into the game files folder.");
    if (stat("assets", &st) < 0)
        fatal_error("Could not find the assets folder.\nExtract the APK's assets/ into the game files folder.");
    // the assets folder must be the FULL apk assets/, not just the audio files
    char probe[600];
    snprintf(probe, sizeof(probe), "%s/GJ_GameSheet.plist", "assets");
    if (stat(probe, &st) < 0)
        fatal_error("The assets folder is incomplete.\n\nExtract the APK's ENTIRE assets/ folder\n"
                    "(sprites, fonts, plists -- not just audio)\ninto the game files folder.");
}

// The game opens its save files for reading before ever creating them; make
// sure they exist (empty is fine, the game treats empty as "new").
static void save_files_init(void)
{
    mkdir("save", 0777);
    static const char *names[] = {
        "CCGameManager.dat",
        "CCGameManager2.dat",
        "CCGameManager.dat.bak",
        "CCLocalLevels.dat",
        "CCLocalLevels.dat.bak",
        "CCLocalLevels2.dat",
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "save/%s", names[i]);
        FILE *f = fopen(path, "r");
        if (f)
        {
            fclose(f);
            continue;
        }
        f = fopen(path, "w");
        if (f)
            fclose(f);
    }
}

// ---------------------------------------------------------------------------
// cocos2d-x native entry points (resolved from libcocos2dcpp.so by name)
// ---------------------------------------------------------------------------

// 2.2 touch natives carry a trailing jdouble timestamp in SECONDS (the Java
// side passes MotionEvent.getEventTime()/1000.0).
static struct
{
    void *(*ccFileUtils)(void);                        // CCFileUtils::sharedFileUtils()
    void (*ccAddSearchPath)(void *self, const char *); // ::addSearchPath(const char*)
    void *(*gmSharedState)(void);                      // GameManager::sharedState()
    void (*gmDoQuickSave)(void *self);                 // GameManager::doQuickSave()
    void (*init)(JNIEnv *env, jobject thiz, int w, int h);
    void (*render)(JNIEnv *env, jobject thiz);
    void (*onPause)(JNIEnv *env, jobject thiz);
    void (*onResume)(JNIEnv *env, jobject thiz);
    void (*touchesBegin)(JNIEnv *env, jobject thiz, int id, float x, float y, double ts);
    void (*touchesEnd)(JNIEnv *env, jobject thiz, int id, float x, float y, double ts);
    void (*touchesMove)(JNIEnv *env, jobject thiz, jobject ids, jobject xs, jobject ys, double ts);
    void (*touchesCancel)(JNIEnv *env, jobject thiz, jobject ids, jobject xs, jobject ys, double ts);
    unsigned char (*keyDown)(JNIEnv *env, jobject thiz, int keycode);
    void (*insertText)(JNIEnv *env, jobject thiz, jobject jstr);
    void (*deleteBackward)(JNIEnv *env, jobject thiz);
    jobject (*getContentText)(JNIEnv *env, jobject thiz);
    void (*setEditTextDialogResult)(JNIEnv *env, jobject thiz, jobject jbytearr);
} gd;

// JavaVM*, not void*: Baron::Jvm's JavaVM base is not at offset 0 (jnivm::VM
// is polymorphic, so it takes the primary-base slot), and only a typed
// parameter makes the compiler apply the base-subobject adjustment.
static int (*fmod_JNI_OnLoad)(JavaVM *vm, void *reserved);
static int (*game_JNI_OnLoad)(JavaVM *vm, void *reserved);

#define RESOLVE(field, sym) \
    gd.field = (decltype(gd.field))so_symbol(&game_mod, sym)
#define RESOLVE_OPT(field, sym) \
    gd.field = (decltype(gd.field))so_symbol(&game_mod, sym)

static void resolve_gd_exports(void)
{
    RESOLVE(ccFileUtils, "_ZN7cocos2d11CCFileUtils15sharedFileUtilsEv");
    RESOLVE(ccAddSearchPath, "_ZN7cocos2d11CCFileUtils13addSearchPathEPKc");
    RESOLVE_OPT(gmSharedState, "_ZN11GameManager11sharedStateEv");
    RESOLVE_OPT(gmDoQuickSave, "_ZN11GameManager11doQuickSaveEv");
    RESOLVE(init, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
    RESOLVE(render, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
    RESOLVE(onPause, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
    RESOLVE(onResume, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
    RESOLVE(touchesBegin, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    RESOLVE(touchesEnd, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    RESOLVE(touchesMove, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
    RESOLVE_OPT(touchesCancel, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
    RESOLVE(keyDown, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
    RESOLVE_OPT(insertText, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
    RESOLVE_OPT(deleteBackward, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
    RESOLVE_OPT(getContentText, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText");
    RESOLVE_OPT(setEditTextDialogResult,
                "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetEditTextDialogResult");

    fmod_JNI_OnLoad = (int (*)(JavaVM *, void *))so_symbol(&fmod_mod, "JNI_OnLoad");
    game_JNI_OnLoad = (int (*)(JavaVM *, void *))so_symbol(&game_mod, "JNI_OnLoad");
}

// ---------------------------------------------------------------------------
// software keyboard (JNI callbacks from cocos2dx.cpp land here; reads from
// stdin -- run the loader from a terminal/SSH session)
// ---------------------------------------------------------------------------

// jnivm objects -> jobject (what the game's natives expect)
static jobject to_jobject(const std::shared_ptr<FakeJni::JString> &s)
{
    return jnivm::JNITypes<std::shared_ptr<FakeJni::JString>>::ToJNIType(jnivm::ENV::FromJNIEnv(g_env), s);
}

static int keyboard_get_text(const char *header, const char *initial,
                             char *out, size_t out_len)
{
    printf("\n=== %s ===\n", (header && header[0]) ? header : "Input");
    if (initial && initial[0])
        printf("(initial: %s)\n", initial);
    printf("> ");
    fflush(stdout);
    if (!fgets(out, (int)out_len, stdin))
        return 0;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return len > 0;
}

void gdash_open_ime_keyboard(void)
{
    if (!gd.insertText)
        return;
    char initial[256] = "";
    if (gd.getContentText)
    {
        jobject jstr = gd.getContentText(g_env, NULL);
        if (jstr)
        {
            auto s = reinterpret_cast<FakeJni::JString *>(jstr);
            snprintf(initial, sizeof(initial), "%s", s->c_str());
        }
    }

    char out[256] = "";
    if (!keyboard_get_text("", initial, out, sizeof(out)))
        return;

    // replace the current content: clear it, then insert the new text and a
    // newline (cocos2d text fields treat '\n' as end-of-editing)
    if (gd.deleteBackward)
    {
        for (size_t i = strlen(initial); i > 0; i--)
            gd.deleteBackward(g_env, NULL);
    }
    auto jstr = std::make_shared<FakeJni::JString>(out);
    gd.insertText(g_env, NULL, to_jobject(jstr));
    auto newline = std::make_shared<FakeJni::JString>("\n");
    gd.insertText(g_env, NULL, to_jobject(newline));
}

void gdash_show_edittext_dialog(const char *title, const char *msg, int maxlen)
{
    (void)maxlen;
    if (!gd.setEditTextDialogResult)
        return;
    char out[512] = "";
    if (!keyboard_get_text(title, msg, out, sizeof(out)))
        snprintf(out, sizeof(out), "%s", msg ? msg : "");
    auto jarr = std::make_shared<FakeJni::JByteArray>((jbyte *)out, (jsize)strlen(out));
    gd.setEditTextDialogResult(g_env, NULL,
        jnivm::JNITypes<std::shared_ptr<FakeJni::JByteArray>>::ToJNIType(jnivm::ENV::FromJNIEnv(g_env), jarr));
}

void gdash_request_quit(void)
{
    g_quit = 1;
}

// ---------------------------------------------------------------------------
// input pump: touch passthrough + gamepad/keyboard-to-touch synthesis + cursor
// ---------------------------------------------------------------------------

#define MAX_POINTERS 24 // 0..15 real fingers, 20+ virtual (buttons, cursor)

typedef struct
{
    int active;
    float x, y;
} Pointer;
static Pointer pcur[MAX_POINTERS]; // committed state
static Pointer pnew[MAX_POINTERS]; // desired state this frame

// virtual pointer ids
enum
{
    VPTR_JUMP = 20,  // A/Z/R -> tap (jump / menu select at bottom-right)
    VPTR_LEFT = 21,  // dpad/stick left  -> platformer left arrow zone
    VPTR_RIGHT = 22, // dpad/stick right -> platformer right arrow zone
    VPTR_CURSOR = 23 // A press at the stick-driven cursor (menu select)
};

// Android keycodes
#define AKEY_BACK 4

// stick-driven cursor for menu navigation (essential without a touchscreen)
static float cursor_x, cursor_y;
static double cursor_last_move_tick;
static int cursor_visible_flag = 0;

static SDL_GameController *ctr = NULL;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

// Frame-time based cursor movement: dt makes it independent of the (uncapped)
// render rate, and SDL sticks report up as negative -- unlike the Switch's
// hid, so the y term is added, not subtracted (stick up = cursor up).
static void cursor_move_by(float dx, float dy, float dt)
{
    const float speed = (float)cursor_speed * ((float)screen_height / 720.0f);
    if (dt > 0.1f)
        dt = 0.1f; // don't teleport after a stall
    cursor_x += dx * speed * dt;
    cursor_y += dy * speed * dt;
    if (cursor_x < 0.f)
        cursor_x = 0.f;
    if (cursor_y < 0.f)
        cursor_y = 0.f;
    if (cursor_x > (float)screen_width - 1.f)
        cursor_x = (float)screen_width - 1.f;
    if (cursor_y > (float)screen_height - 1.f)
        cursor_y = (float)screen_height - 1.f;
    cursor_last_move_tick = now_seconds();
    cursor_visible_flag = 1;
}

static int cursor_visible(void)
{
    if (!cursor_visible_flag)
        return 0;
    return (now_seconds() - cursor_last_move_tick) < 3.0; // 3 s after the last motion
}

// scissor-clear rectangle painter (no shaders, state saved/restored)
static void draw_rect(int x, int y, int w, int h, float r, float g, float b)
{
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void cursor_render(void)
{
    if (!cursor_visible())
        return;
    GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint old_box[4];
    GLfloat old_clear[4];
    glGetIntegerv(GL_SCISSOR_BOX, old_box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
    glEnable(GL_SCISSOR_TEST);

    // GL window coords: origin bottom-left; cursor_y is top-left based
    const int cx = (int)cursor_x;
    const int cy = screen_height - 1 - (int)cursor_y;
    const int s = screen_height / 90; // ~8 px at 720p
    draw_rect(cx - s / 2 - 1, cy - s / 2 - 1, s + 2, s + 2, 0.f, 0.f, 0.f);
    draw_rect(cx - s / 2, cy - s / 2, s, s, 1.f, 1.f, 1.f);

    glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
    glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    if (!had_scissor)
        glDisable(GL_SCISSOR_TEST);
}

static void build_virtual_pointers(void)
{
    const float w = (float)screen_width, h = (float)screen_height;

    // A is the menu click or the jump tap, never both -- raising VPTR_JUMP
    // during a click would also tap the bottom-right corner, pressing
    // whatever button sits there no matter where the cursor is.
    const int cursor_on = cursor_visible();

    int jump = 0, left = 0, right = 0;
    if (ctr)
    {
        if (!cursor_on)
            jump |= SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_A);
        left |= SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        right |= SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    }
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (keys[SDL_SCANCODE_Z])
        jump = 1;
    if (keys[SDL_SCANCODE_RETURN])
        jump = 1;
    if (keys[SDL_SCANCODE_LEFT])
        left = 1;
    if (keys[SDL_SCANCODE_RIGHT])
        right = 1;

    // jump: tap near the bottom-right corner
    if (jump)
    {
        pnew[VPTR_JUMP].active = 1;
        pnew[VPTR_JUMP].x = w - 4.0f;
        pnew[VPTR_JUMP].y = h - 4.0f;
    }

    // platformer move zones (positions ported from the Vita build: 95/225 x
    // 480 in 960x544 view space)
    if (left)
    {
        pnew[VPTR_LEFT].active = 1;
        pnew[VPTR_LEFT].x = (95.0f / 960.0f) * w;
        pnew[VPTR_LEFT].y = (480.0f / 544.0f) * h;
    }
    if (right)
    {
        pnew[VPTR_RIGHT].active = 1;
        pnew[VPTR_RIGHT].x = (225.0f / 960.0f) * w;
        pnew[VPTR_RIGHT].y = (480.0f / 544.0f) * h;
    }

    // cursor click on A -- but only while the cursor is on screen (i.e. a
    // stick was moved recently, meaning we're navigating a menu). In gameplay
    // the stick is idle, the cursor is hidden, and A is purely the jump tap.
    if (ctr && SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_A) && cursor_on)
    {
        pnew[VPTR_CURSOR].active = 1;
        pnew[VPTR_CURSOR].x = cursor_x;
        pnew[VPTR_CURSOR].y = cursor_y;
    }
}

// translate the per-frame pointer delta into the cocos touch protocol
static void dispatch_pointers(void)
{
    // reusable JNI arrays for the move batch
    static std::shared_ptr<FakeJni::JIntArray> move_ids;
    static std::shared_ptr<FakeJni::JFloatArray> move_xs, move_ys;
    if (!move_ids)
    {
        move_ids = std::make_shared<FakeJni::JIntArray>(MAX_POINTERS);
        move_xs = std::make_shared<FakeJni::JFloatArray>(MAX_POINTERS);
        move_ys = std::make_shared<FakeJni::JFloatArray>(MAX_POINTERS);
    }

    const double ts = now_seconds();

    // downs
    for (int i = 0; i < MAX_POINTERS; i++)
    {
        if (pnew[i].active && !pcur[i].active)
            gd.touchesBegin(g_env, NULL, i, pnew[i].x, pnew[i].y, ts);
    }

    // moves, batched like the Java GLSurfaceView does
    int nmove = 0;
    int ids[MAX_POINTERS];
    float xs[MAX_POINTERS], ys[MAX_POINTERS];
    for (int i = 0; i < MAX_POINTERS; i++)
    {
        if (pnew[i].active && pcur[i].active &&
            (pnew[i].x != pcur[i].x || pnew[i].y != pcur[i].y))
        {
            ids[nmove] = i;
            xs[nmove] = pnew[i].x;
            ys[nmove] = pnew[i].y;
            nmove++;
        }
    }
    if (nmove > 0)
    {
        for (int i = 0; i < nmove; i++)
        {
            (*move_ids)[i] = ids[i];
            (*move_xs)[i] = xs[i];
            (*move_ys)[i] = ys[i];
        }
        gd.touchesMove(g_env, NULL,
            jnivm::JNITypes<std::shared_ptr<FakeJni::JIntArray>>::ToJNIType(jnivm::ENV::FromJNIEnv(g_env), move_ids),
            jnivm::JNITypes<std::shared_ptr<FakeJni::JFloatArray>>::ToJNIType(jnivm::ENV::FromJNIEnv(g_env), move_xs),
            jnivm::JNITypes<std::shared_ptr<FakeJni::JFloatArray>>::ToJNIType(jnivm::ENV::FromJNIEnv(g_env), move_ys),
            ts);
    }

    // ups
    for (int i = 0; i < MAX_POINTERS; i++)
    {
        if (!pnew[i].active && pcur[i].active)
            gd.touchesEnd(g_env, NULL, i, pcur[i].x, pcur[i].y, ts);
    }

    memcpy(pcur, pnew, sizeof(pcur));
}

static void update_input(void)
{
    // stick-driven cursor (frame-time based, see cursor_move_by)
    float stick_dx = 0.f, stick_dy = 0.f;
    if (ctr)
    {
        const Sint16 rx = SDL_GameControllerGetAxis(ctr, SDL_CONTROLLER_AXIS_RIGHTX);
        const Sint16 ry = SDL_GameControllerGetAxis(ctr, SDL_CONTROLLER_AXIS_RIGHTY);
        const Sint16 lx = SDL_GameControllerGetAxis(ctr, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 ly = SDL_GameControllerGetAxis(ctr, SDL_CONTROLLER_AXIS_LEFTY);
        // either stick moves the cursor; right stick has priority
        float dx = (float)rx / 32767.0f, dy = (float)ry / 32767.0f;
        if (fabsf(dx) < 0.25f && fabsf(dy) < 0.25f)
        {
            dx = (float)lx / 32767.0f;
            dy = (float)ly / 32767.0f;
        }
        if (fabsf(dx) < 0.25f)
            dx = 0.f;
        if (fabsf(dy) < 0.25f)
            dy = 0.f;
        // the d-pad also drives the cursor (stick keeps priority); in
        // gameplay it still doubles as the platformer move zones
        if (dx == 0.f && dy == 0.f)
        {
            if (SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) dx = -1.f;
            if (SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx = 1.f;
            if (SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_DPAD_UP)) dy = -1.f;
            if (SDL_GameControllerGetButton(ctr, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) dy = 1.f;
        }
        stick_dx = dx;
        stick_dy = dy;
    }
    if (stick_dx != 0.f || stick_dy != 0.f)
    {
        static double last_cursor_t = 0.0;
        const double t = now_seconds();
        float dt = (float)(t - last_cursor_t);
        last_cursor_t = t;
        cursor_move_by(stick_dx, stick_dy, dt);
    }

    memset(pnew, 0, sizeof(pnew));
    build_virtual_pointers();
    dispatch_pointers();
}

// BACK (pause / dismiss): B, X or Escape, edge-triggered
static void back_press(void)
{
    if (!gdash_block_back_button && gd.keyDown)
        gd.keyDown(g_env, NULL, AKEY_BACK);
}

static int s_app_focused = 1;

// GD only saves on background/quit, which is unreliable; call its own
// quick-save directly. This serializes, compresses and writes the whole
// GameManager (a couple of MB once online songs/levels are in it) on the
// calling thread -- ~400 ms, so it must only ever run somewhere a dropped
// frame does not matter. Never call it from the render loop mid-level.
static void force_save(void)
{
    if (!gd.gmSharedState || !gd.gmDoQuickSave)
        return;
    void *gm = gd.gmSharedState();
    if (!gm)
        return;
    const double t0 = now_seconds();
    gd.gmDoQuickSave(gm);
    const double ms = (now_seconds() - t0) * 1000.0;
    if (ms > 50.0)
        printf("[gdash] save took %.0f ms\n", ms);
}

// ---------------------------------------------------------------------------
// Save point.
//
// GD itself only quick-saves from one place (EndLevelLayer::onMenu -- finishing
// a level and tapping Menu); on Android everything else rides on the activity
// lifecycle, which does not exist here. A timer cannot help landing its ~400 ms
// hitch in the middle of a run, so save on the one moment the player has just
// stopped playing: leaving a level. Pausing does not need its own save --
// quitting from the pause menu tears the PlayLayer down and lands here anyway,
// and resuming changes nothing worth persisting.
//
// onExit is virtual, so patch the vtable slot rather than using hook_address():
// hook_address() overwrites the function's entry with a branch, leaving no way
// to call the original. Swapping a vtable entry leaves the real function
// untouched, so the hook can run it first and then save.
static void (*orig_playlayer_onexit)(void *) = NULL;

static void playlayer_onexit_hook(void *self)
{
    orig_playlayer_onexit(self);
    force_save(); // left a level (quit or finished)
}

// Find the slot holding fn_sym in vtable_sym and point it at replacement.
// Matching on the resolved function address rather than a hardcoded index
// keeps this working if the layout shifts between GD builds; a miss is
// reported and simply leaves that save point inactive.
static void hook_vtable_slot(so_module *mod, const char *vtable_sym, const char *fn_sym,
                             void *replacement, void **out_orig)
{
    uintptr_t vtable = so_symbol(mod, vtable_sym);
    uintptr_t fn = so_symbol(mod, fn_sym);
    if (!vtable || !fn)
    {
        printf("[gdash] save point: %s / %s not found -- skipping\n", vtable_sym, fn_sym);
        return;
    }
    uintptr_t *slots = (uintptr_t *)vtable;
    for (int i = 0; i < 512; i++)
    {
        if (slots[i] != fn)
            continue;
        *out_orig = (void *)fn;
        slots[i] = (uintptr_t)replacement;
        return;
    }
    printf("[gdash] save point: %s not present in %s -- skipping\n", fn_sym, vtable_sym);
}

static void install_save_points(so_module *mod)
{
    hook_vtable_slot(mod, "_ZTV9PlayLayer", "_ZN9PlayLayer6onExitEv",
        (void *)&playlayer_onexit_hook, (void **)&orig_playlayer_onexit);
}

static void handle_event(const SDL_Event *e)
{
    switch (e->type)
    {
    case SDL_QUIT:
        g_quit = 1;
        break;

    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED && !s_app_focused)
        {
            s_app_focused = 1;
            gd.onResume(g_env, NULL);
        }
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST && s_app_focused)
        {
            s_app_focused = 0;
            gd.onPause(g_env, NULL);
            force_save();
        }
        break;

    case SDL_CONTROLLERDEVICEADDED:
        if (!ctr)
        {
            ctr = SDL_GameControllerOpen(e->cdevice.which);
            printf("Game controller opened: %s\n",
                   ctr ? SDL_GameControllerName(ctr) : "(failed)");
        }
        break;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (ctr && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctr)))
        {
            SDL_GameControllerClose(ctr);
            ctr = NULL;
        }
        break;

    case SDL_KEYDOWN:
        if (e->key.keysym.scancode == SDL_SCANCODE_X || e->key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            back_press();
        break;

    case SDL_CONTROLLERBUTTONDOWN:
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_B)
            back_press();
        break;

    case SDL_MOUSEMOTION:
    {
        // mouse drives the cursor (and the cursor is always "visible" for it)
        int win_w = screen_width, win_h = screen_height;
        if (sdl_win)
            SDL_GetWindowSize(sdl_win, &win_w, &win_h);
        cursor_x = (float)e->motion.x * (float)screen_width / (float)win_w;
        cursor_y = (float)e->motion.y * (float)screen_height / (float)win_h;
        if (cursor_x < 0.f)
            cursor_x = 0.f;
        if (cursor_y < 0.f)
            cursor_y = 0.f;
        if (cursor_x > (float)screen_width - 1.f)
            cursor_x = (float)screen_width - 1.f;
        if (cursor_y > (float)screen_height - 1.f)
            cursor_y = (float)screen_height - 1.f;
        cursor_visible_flag = 1;
        break;
    }

    case SDL_MOUSEBUTTONDOWN:
        // a mouse click is a direct touch tap (pointer 0)
        if (e->button.button == SDL_BUTTON_LEFT)
        {
            pnew[0].active = 1;
            pnew[0].x = cursor_x;
            pnew[0].y = cursor_y;
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT)
        {
            pnew[0].active = 0;
            pnew[0].x = cursor_x;
            pnew[0].y = cursor_y;
        }
        break;

    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
    {
        // real touchscreen (pointer id = 1 + finger)
        const int id = 1 + (int)e->tfinger.fingerId;
        if (id < 16)
        {
            pnew[id].x = e->tfinger.x * (float)screen_width;
            pnew[id].y = e->tfinger.y * (float)screen_height;
            pnew[id].active = (e->type != SDL_FINGERUP);
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------

DynLibFunction *so_static_patches[32] = {
    NULL,
};

DynLibFunction *so_dynamic_libraries[32] = {
    symtable_gdash,
    symtable_libc,
    symtable_gles2,
    NULL
};

int main(int argc, char *argv[])
{
    // Unbuffered stdout: jnivm's JNI_TRACE logs and game output must survive
    // crashes (_exit() skips normal flush).
    setvbuf(stdout, NULL, _IONBF, 0);

    print_backtrace_on_segfault(); // Registers a signal handler to print backtrace on segfaults
    exit_on_signals();             // Exits when CTRL-C is pressed (or SIGINT or SIGTERM is received)

    // ...then take SIGINT/SIGTERM back off it. The shared handler _exit(0)s on
    // the spot, which skips the quick-save at the end of main() -- and SIGTERM
    // is exactly how the frontend stops the game, so that is the normal way to
    // quit, not an edge case. Ask the render loop to finish instead so the
    // save runs. A second signal still hard-exits, keeping the escape hatch
    // for a wedged process that the shared handler exists to provide.
    {
        struct sigaction sa;
        sa.sa_handler = [](int) {
            if (g_quit)
                _exit(0);
            g_quit = 1;
        };
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    if (argc < 2)
    {
        fatal_error("Usage: %s <config file>\n", argv[0]);
        return -1;
    }

    // Init config (also chdir's into paths.game_files)
    init_config(argv[1]);

    // Real sockets (glibc is the bionic socket ABI; same as the Switch port's
    // net_shim.c but without the ABI conversion) unless the config turns the
    // fail-fast offline stubs back on. connect_timeout_ms bounds each TCP
    // connect attempt so a dead server fails in seconds instead of stalling
    // on kernel SYN retries. Must run before either game lib loads:
    // relocation reads the table once.
    gdash_network_init(config["network"]["enabled"].value_or<bool>(true),
                       config["network"]["connect_timeout_ms"].value_or<int>(4000));

    screen_width = config["device"]["displayWidth"].value_or<int>(1280);
    screen_height = config["device"]["displayHeight"].value_or<int>(720);
    cursor_speed = config["input"]["cursorSpeed"].value_or<double>(675.0);

    // GL up first: the game's gl* imports bind to the glad pointers during
    // relocation, so they must exist before the modules load.
    sdl_initialize_gles();
    load_gles2_funcs();

    InitJNIBinding(&vm);

    check_data();
    save_files_init();

    printf("Loading libfmod\n");
    if (!load_so_from_file(&fmod_mod, "lib/arm64-v8a/libfmod.so", 0x50000000))
        fatal_error("Could not load lib/arm64-v8a/libfmod.so.");

    printf("Loading libcocos2dcpp\n");
    // gdash_nx patches this OpenSSL constructor before Cocos2d's init_array
    // runs. Bogodroid normally initializes inside so_load(), so defer this
    // module's constructors until the patch is installed below.
    so_set_defer_init(1);
    if (!load_so_from_file(&game_mod, "lib/arm64-v8a/libcocos2dcpp.so", 0x51000000))
        fatal_error("Could not load lib/arm64-v8a/libcocos2dcpp.so.");
    so_set_defer_init(0);

    fmod_hooks_init(&fmod_mod);
    resolve_gd_exports();

    // Clamp the frame delta so level-load stalls don't trigger the physics
    // catch-up loop (the "freeze").
    install_delta_clamp();

    // OPENSSL_cpuid_setup probes the CPU with an Android signal/longjmp
    // harness during static initialization. Neutralize it as the NX port
    // does; the rest of Cocos2d's constructors can then run normally.
    {
        uintptr_t cpuid = so_symbol(&game_mod, "OPENSSL_cpuid_setup");
        if (cpuid)
            hook_address(&game_mod, cpuid, (uintptr_t)&noop_void);
    }

    // Keep the loose-assets search path alive: purgeFileUtils() (on a
    // texture-quality change) would drop it and the game never re-adds it.
    {
        uintptr_t purge = so_symbol(&game_mod, "_ZN7cocos2d11CCFileUtils14purgeFileUtilsEv");
        if (purge)
            hook_address(&game_mod, purge, (uintptr_t)&noop_void);
    }

    // Server compat (TLS verification/CA bundle, online-level metadata parser
    // crash, fresh-profile leaderboard bootstrap) -- see imports.cpp.
    gdash_server_compat_init(&game_mod);

    // Save on leaving a level (see force_save above).
    install_save_points(&game_mod);

    so_flush_caches(&game_mod, 1);
    so_initialize(&game_mod);

    // The game's JniHelper caches this env; the LocalFrame outlives the loop.
    FakeJni::LocalFrame frame(vm);
    g_env = &frame.getJniEnv();

    printf("calling JNI_OnLoad from libfmod.so\n");
    if (fmod_JNI_OnLoad)
        fmod_JNI_OnLoad(&vm, nullptr);
    printf("calling JNI_OnLoad from libcocos2dcpp.so\n");
    if (game_JNI_OnLoad)
        game_JNI_OnLoad(&vm, nullptr);

    // load assets loose from <base>/assets/: cocos2d fopen()s an absolute
    // ('/'-rooted) search path directly, so no .apk is needed
    static std::string assets_search = std::filesystem::absolute("assets").string() + "/";
    gd.ccAddSearchPath(gd.ccFileUtils(), assets_search.c_str());

    // input
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
        printf("SDL gamecontroller init failed (%s) -- keyboard only\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
    {
        if (SDL_IsGameController(i))
        {
            ctr = SDL_GameControllerOpen(i);
            printf("Game controller opened: %s\n", ctr ? SDL_GameControllerName(ctr) : "(failed)");
            break;
        }
    }
    cursor_x = screen_width / 2.0f;
    cursor_y = screen_height / 2.0f;
    SDL_GL_SetSwapInterval(1); // vsync

    // GL up, then boot the engine on this thread (context current)
    glad_glViewport(0, 0, screen_width, screen_height);
    {
        const char *r = (const char *)glad_glGetString(GL_RENDERER);
        const char *v = (const char *)glad_glGetString(GL_VERSION);
        printf("[GL] renderer: %s\n[GL] version: %s\n", r ? r : "(null)", v ? v : "(null)");
        fflush(stdout);
    }
    gd.init(g_env, NULL, screen_width, screen_height);

    unsigned frame_count = 0;
    double frame_prev = now_seconds();
    while (!g_quit)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            handle_event(&e);

        update_input();
        gd.render(g_env, NULL);
        cursor_render();
        eglSwapBuffers_impl(egl_display, egl_surface);

        // Hitch reporter: at 60 fps a frame is ~16.7 ms, so anything past
        // 100 ms is a visible stutter. Only outliers are printed, and the
        // save points are all outside gameplay, so this should stay silent
        // mid-level -- if it does not, something else is stalling the loop.
        const double frame_now = now_seconds();
        const double frame_ms = (frame_now - frame_prev) * 1000.0;
        frame_prev = frame_now;
        if (frame_ms > 100.0 && frame_count > 60)
            printf("[gdash] frame hitch: %.0f ms\n", frame_ms);
        frame_count++;
    }

    if (s_app_focused)
    { // clean in-game quit; background path already saved
        gd.onPause(g_env, NULL);
        force_save();
    }
    usleep(200000); // 200 ms for save writes on worker threads

    SDL_Quit();
    return 0;
}
