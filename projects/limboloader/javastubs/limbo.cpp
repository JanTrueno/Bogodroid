#include "baron/baron.h"
#include "limbo.h"
#include "ainput.h"
#include <cstdio>
#include <cstring>
#include <climits>

// Set by config.cpp right after chdir() into the game's own directory (see
// the comment on this same declaration in thunks/ndk/asset_manager.c) -- used
// here so the save-state file lands next to the loader rather than wherever
// the process's cwd happens to be when a SaveGame_Set* call comes in.
extern "C" char g_loader_root[];

static void SaveState_Path(char *out, size_t out_sz)
{
    if (g_loader_root[0] != '\0')
        snprintf(out, out_sz, "%s/limbo_savegame_state.txt", g_loader_root);
    else
        snprintf(out, out_sz, "limbo_savegame_state.txt");
}

void jnivm::com::playdead::limbo::LimboActivity::SaveState_Load()
{
    char path[PATH_MAX];
    SaveState_Path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    int autoResumeInt = 0;
    char key[64];
    int value;
    while (fscanf(f, "%63[^=]=%d\n", key, &value) == 2)
    {
        if (!strcmp(key, "lastSavePoint")) lastSavePoint = value;
        else if (!strcmp(key, "savePointReached")) savePointReached = value;
        else if (!strcmp(key, "achievementBitfield")) achievementBitfield = value;
        else if (!strcmp(key, "autoResume")) autoResumeInt = value;
    }
    autoResume = autoResumeInt != 0;
    fclose(f);

    printf("[limbo] loaded persisted save state from %s: lastSavePoint=%d savePointReached=%d "
        "achievementBitfield=%d autoResume=%d\n",
        path, lastSavePoint, savePointReached, achievementBitfield, autoResume);
}

void jnivm::com::playdead::limbo::LimboActivity::SaveState_Save()
{
    char path[PATH_MAX];
    SaveState_Path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f)
    {
        printf("[limbo] failed to persist save state to %s\n", path);
        return;
    }
    fprintf(f, "lastSavePoint=%d\n", lastSavePoint);
    fprintf(f, "savePointReached=%d\n", savePointReached);
    fprintf(f, "achievementBitfield=%d\n", achievementBitfield);
    fprintf(f, "autoResume=%d\n", autoResume ? 1 : 0);
    fclose(f);
}

std::shared_ptr<FakeJni::JString> jnivm::com::playdead::limbo::LimboActivity::GetLimboCachedAssetsPath()
{
    auto context = std::make_shared<jnivm::android::content::Context>();
    return context->getCacheDir()->getPath();
}

// Publish the pad layout the loader emits. The engine reads these fields after
// native_DeviceAdded and builds its GameController from them; the button list
// and the axis lists (codes / sources / min / max, index-aligned) must describe
// exactly what main.cpp sends, or events arrive for inputs the engine does not
// believe the device has.
jnivm::com::playdead::limbo::LimboActivity::LimboActivity()
{
    // AKEYCODE_* values, matching controller_button_to_keycode().
    static const int buttons[] = {
        96,  // BUTTON_A
        97,  // BUTTON_B
        99,  // BUTTON_X
        100, // BUTTON_Y
        102, // BUTTON_L1
        103, // BUTTON_R1
        106, // BUTTON_THUMBL
        107, // BUTTON_THUMBR
        108, // BUTTON_START
        109, // BUTTON_SELECT
        110, // BUTTON_MODE
        19,  // DPAD_UP
        20,  // DPAD_DOWN
        21,  // DPAD_LEFT
        22,  // DPAD_RIGHT
    };

    // AMOTION_EVENT_AXIS_* values, matching sdl_axis_to_android() plus the hat
    // mirror the loader applies to the left stick.
    struct AxisDesc {
        int code;
        float min;
        float max;
    };
    static const AxisDesc axes[] = {
        { AMOTION_EVENT_AXIS_X,        -1.0f, 1.0f },
        { AMOTION_EVENT_AXIS_Y,        -1.0f, 1.0f },
        { AMOTION_EVENT_AXIS_Z,        -1.0f, 1.0f },
        { AMOTION_EVENT_AXIS_RZ,       -1.0f, 1.0f },
        { AMOTION_EVENT_AXIS_HAT_X,    -1.0f, 1.0f },
        { AMOTION_EVENT_AXIS_HAT_Y,    -1.0f, 1.0f },
        { AMOTION_EVENT_AXIS_LTRIGGER,  0.0f, 1.0f },
        { AMOTION_EVENT_AXIS_RTRIGGER,  0.0f, 1.0f },
    };

    const int nButtons = (int)(sizeof(buttons) / sizeof(*buttons));
    const int nAxes = (int)(sizeof(axes) / sizeof(*axes));

    gamepadButtonCodes = std::make_shared<FakeJni::JIntArray>(nButtons);
    for (int i = 0; i < nButtons; i++)
        (*gamepadButtonCodes)[i] = buttons[i];

    gamepadAxisCodes = std::make_shared<FakeJni::JIntArray>(nAxes);
    gamepadAxisSources = std::make_shared<FakeJni::JIntArray>(nAxes);
    gamepadAxisMinVals = std::make_shared<FakeJni::JFloatArray>(nAxes);
    gamepadAxisMaxVals = std::make_shared<FakeJni::JFloatArray>(nAxes);
    for (int i = 0; i < nAxes; i++)
    {
        (*gamepadAxisCodes)[i] = axes[i].code;
        // Every axis is reported on the joystick source, which is what the
        // loader tags its motion events with.
        (*gamepadAxisSources)[i] = AINPUT_SOURCE_JOYSTICK;
        (*gamepadAxisMinVals)[i] = axes[i].min;
        (*gamepadAxisMaxVals)[i] = axes[i].max;
    }

    SaveState_Load();
}

void jnivm::com::playdead::limbo::LimboActivity::SaveGame_SetLastSavePoint(int value)
{
    lastSavePoint = value;
    SaveState_Save();
}

int jnivm::com::playdead::limbo::LimboActivity::SaveGame_GetLastSavePoint()
{
    return lastSavePoint;
}

void jnivm::com::playdead::limbo::LimboActivity::SaveGame_SetSavePointReached(int value)
{
    savePointReached = value;
    SaveState_Save();
}

int jnivm::com::playdead::limbo::LimboActivity::SaveGame_GetSavePointReached()
{
    return savePointReached;
}

void jnivm::com::playdead::limbo::LimboActivity::SaveGame_SetAchievementBitfield(int value)
{
    achievementBitfield = value;
    SaveState_Save();
}

int jnivm::com::playdead::limbo::LimboActivity::SaveGame_GetAchievementBitfield()
{
    return achievementBitfield;
}

void jnivm::com::playdead::limbo::LimboActivity::SaveGame_SetAutoResume(bool value)
{
    autoResume = value;
    SaveState_Save();
}

bool jnivm::com::playdead::limbo::LimboActivity::SaveGame_GetAutoResume()
{
    return autoResume;
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::playdead::limbo::LimboActivity)
{ FakeJni::Constructor<LimboActivity> {} },
{ FakeJni::Function<&LimboActivity::GetLimboCachedAssetsPath> {}, "GetLimboCachedAssetsPath", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadDeviceId> {}, "gamepadDeviceId", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadVendorId> {}, "gamepadVendorId", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadProductId> {}, "gamepadProductId", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadButtonCodes> {}, "gamepadButtonCodes", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadAxisCodes> {}, "gamepadAxisCodes", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadAxisSources> {}, "gamepadAxisSources", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadAxisMinVals> {}, "gamepadAxisMinVals", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Field<&LimboActivity::gamepadAxisMaxVals> {}, "gamepadAxisMaxVals", FakeJni::JFieldID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_SetLastSavePoint> {}, "SaveGame_SetLastSavePoint", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_GetLastSavePoint> {}, "SaveGame_GetLastSavePoint", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_SetSavePointReached> {}, "SaveGame_SetSavePointReached", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_GetSavePointReached> {}, "SaveGame_GetSavePointReached", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_SetAchievementBitfield> {}, "SaveGame_SetAchievementBitfield", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_GetAchievementBitfield> {}, "SaveGame_GetAchievementBitfield", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_SetAutoResume> {}, "SaveGame_SetAutoResume", FakeJni::JMethodID::PUBLIC },
{ FakeJni::Function<&LimboActivity::SaveGame_GetAutoResume> {}, "SaveGame_GetAutoResume", FakeJni::JMethodID::PUBLIC },
END_NATIVE_DESCRIPTOR

// Report resolved so any engine code that calls this through the method id
// nativeInit cached cannot read "unresolved" and block on it.
bool jnivm::com::playdead::limbo::LimboAgeSignals::isAgeResolved()
{
    return true;
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::playdead::limbo::LimboAgeSignals)
{ FakeJni::Constructor<LimboAgeSignals> {} },
{ FakeJni::Function<&LimboAgeSignals::isAgeResolved> {}, "isAgeResolved", FakeJni::JMethodID::STATIC },
END_NATIVE_DESCRIPTOR
