#ifndef __LAYTON3_H__
#define __LAYTON3_H__

#include "baron/baron.h"
#include "android.h"

#include <string>

// Software keyboard state, driven by the loader's event loop.
//
// On Android UI_StartEditText pops the IME and returns straight away; the
// game then polls UI_GetEditState every frame until it clears and reads the
// result with UI_GetEditText. (The Switch port blocks on a system applet
// instead -- reference/layton3_nx-main/source/jni.c.) There is no system
// IME here, so the loader feeds keystrokes in while editing is active.
namespace layton_ime
{
    bool editing();
    void append(const char *utf8);
    void backspace();
    void commit(); // accept: clears editing, keeps the text
    void cancel(); // dismiss: clears editing and the text
    const std::string &text();
    int type(); // 0 = free text, non-zero = numeric (engine's editType)
}

// libll3.so (Professor Layton and the Lost/Unwound Future HD, Level-5's
// custom engine -- same family as libll1.so/laytonloader) does not use the
// standard Android API surface. It resolves a fixed set of custom native
// methods by name on its own MainActivity class (movie playback, PNG decode,
// save directory, license checks, the software keyboard) instead of
// AAssetManager/ANativeActivity. This mirrors the method list found in the
// Switch homebrew port's jni.c dispatch table
// (reference/layton3_nx-main/source/jni.c) for the same binary.
//
// The method set is identical to LT1R's with one addition: OS_GetDeviceLanguage
// (present in LT2R/LT3R, not LT1R). See layton3.cpp.
namespace jnivm
{
    namespace com
    {
        namespace Level5
        {
            namespace LT3R
            {
                class MainActivity : public jnivm::android::app::Activity
                {
                public:
                    DEFINE_CLASS_NAME("com/Level5/LT3R/MainActivity")

                    // Movie (CRI ADX2 cutscene) playback
                    bool MO_PlayMovie(std::shared_ptr<FakeJni::JString> file);
                    bool MO_PlayMovieRegion(std::shared_ptr<FakeJni::JString> file, int offset, int size);
                    bool MO_GetState();
                    int MO_GetPosition();
                    void MO_PauseMovie(bool pause);
                    void MO_ReleaseMovie();
                    void MO_SetVolume(float volume);
                    std::shared_ptr<FakeJni::JFloatArray> MO_UpdateTexture();

                    // Software keyboard
                    bool UI_GetEditState();
                    std::shared_ptr<FakeJni::JString> UI_GetEditText();
                    void UI_StartEditText(std::shared_ptr<FakeJni::JString> initial, int editType);
                    void UI_SetIdleTimerDisabled(bool disabled);

                    // Save data / storage
                    std::shared_ptr<FakeJni::JString> CARD_GetFilesDirName();
                    std::shared_ptr<FakeJni::JString> CARD_GetExternalFilesDirName();
                    void CARD_CreateDirectory(std::shared_ptr<FakeJni::JString> dir);
                    long CARD_GetAvailableBytes();

                    bool L5iD_IsEndRequest();

                    int LVL_GetState();
                    int LSH_GetState();
                    int SBS_GetState();

                    // Misc
                    std::shared_ptr<FakeJni::JString> DL_GetFileName();
                    std::shared_ptr<FakeJni::JString> OS_GetAppVersion();

                    // The engine picks its assets/data-XX language folder from
                    // this; the Switch port maps to 1=EN,2=ES,3=FR,4=IT,5=DE
                    // (reference/layton2_nx-main/source/jni.c device_language(),
                    // unchanged in layton3_nx-main). Sourced from
                    // config["device"]["language"] here rather than a system
                    // API, since there is no Switch-style setGetSystemLanguage.
                    int OS_GetDeviceLanguage();

                    // static GL_LoadPNG(byte[]) -> int[]{ w, h, rgba... }
                    static std::shared_ptr<FakeJni::JIntArray> GL_LoadPNG(std::shared_ptr<FakeJni::JByteArray> data);
                };
            }
        }
    }
}
#endif
