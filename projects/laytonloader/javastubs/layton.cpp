#include "baron/baron.h"
#include "layton.h"
#include "../movie.h"
#include <cstring>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

using namespace jnivm::com::Level5::LT1R;

// ---------------------------------------------------------------------------
// Movie (CRI ADX2 cutscene) playback -- not implemented yet (Milestone 1).
// Reporting "no movie" lets the game skip cutscenes instead of blocking.
// ---------------------------------------------------------------------------

bool MainActivity::MO_PlayMovie(std::shared_ptr<FakeJni::JString> file)
{
    if (!file)
        return false;
    return movie_open(file->c_str(), 0, 0);
}

// Some cutscenes live as a region inside a larger file
bool MainActivity::MO_PlayMovieRegion(std::shared_ptr<FakeJni::JString> file, int offset, int size)
{
    if (!file)
        return false;
    return movie_open(file->c_str(), offset, size);
}

bool MainActivity::MO_GetState()
{
    return movie_active();
}

int MainActivity::MO_GetPosition()
{
    return movie_position_ms();
}

void MainActivity::MO_PauseMovie(bool pause)
{
    movie_pause(pause);
}

void MainActivity::MO_ReleaseMovie()
{
    movie_close();
}

void MainActivity::MO_SetVolume(float volume)
{
    movie_set_volume(volume);
}

std::shared_ptr<FakeJni::JFloatArray> MainActivity::MO_UpdateTexture()
{
    // SurfaceTexture transform matrix; standard Android flip matrix so any
    // consumer of this (before real movie playback exists) gets a sane UV
    // quad instead of an identity/garbage one.
    static const float surfaceMat[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
    };
    auto arr = std::make_shared<FakeJni::JFloatArray>(16);
    for (int i = 0; i < 16; i++)
        (*arr)[i] = surfaceMat[i];
    return arr;
}

// ---------------------------------------------------------------------------
// Software keyboard -- not implemented yet
// ---------------------------------------------------------------------------

namespace layton_ime
{
    static std::string s_text;
    static bool s_editing = false;
    static int s_type = 0;

    static const size_t TEXT_MAX = 511; // matches the Switch port's ime_text

    bool editing() { return s_editing; }
    const std::string &text() { return s_text; }
    int type() { return s_type; }

    void append(const char *utf8)
    {
        if (!s_editing || !utf8)
            return;
        // editType != 0 is the engine's numeric keypad (puzzle answers)
        if (s_type != 0)
        {
            for (const char *c = utf8; *c; c++)
                if (*c < '0' || *c > '9')
                    return;
        }
        if (s_text.size() + strlen(utf8) <= TEXT_MAX)
            s_text += utf8;
    }

    void backspace()
    {
        if (!s_editing || s_text.empty())
            return;
        // step back over a whole UTF-8 sequence, not one byte
        size_t i = s_text.size() - 1;
        while (i > 0 && (s_text[i] & 0xC0) == 0x80)
            i--;
        s_text.erase(i);
    }

    void commit() { s_editing = false; }

    void cancel()
    {
        s_editing = false;
        s_text.clear();
    }

    static void begin(const char *initial, int editType)
    {
        s_text = initial ? initial : "";
        s_type = editType;
        s_editing = true;
    }
}

bool MainActivity::UI_GetEditState()
{
    return layton_ime::editing();
}

std::shared_ptr<FakeJni::JString> MainActivity::UI_GetEditText()
{
    return std::make_shared<FakeJni::JString>(layton_ime::text().c_str());
}

void MainActivity::UI_StartEditText(std::shared_ptr<FakeJni::JString> initial, int editType)
{
    layton_ime::begin(initial ? initial->c_str() : "", editType);
    printf("UI_StartEditText(\"%s\", %d) -- type to edit, Enter accepts, Esc cancels\n",
           initial ? initial->c_str() : "", editType);
    fflush(stdout);
}

void MainActivity::UI_SetIdleTimerDisabled(bool disabled)
{
    (void)disabled;
}

// ---------------------------------------------------------------------------
// Save data / storage
// ---------------------------------------------------------------------------

std::shared_ptr<FakeJni::JString> MainActivity::CARD_GetFilesDirName()
{
    return std::make_shared<FakeJni::JString>(".");
}

std::shared_ptr<FakeJni::JString> MainActivity::CARD_GetExternalFilesDirName()
{
    return std::make_shared<FakeJni::JString>(".");
}

void MainActivity::CARD_CreateDirectory(std::shared_ptr<FakeJni::JString> dir)
{
    const std::string path = dir ? dir->asStdString() : "";
    printf("CARD_CreateDirectory(%s)\n", path.c_str());
    if (!path.empty())
        std::filesystem::create_directories(path);
}

long MainActivity::CARD_GetAvailableBytes()
{
    return 1ll << 30; // report 1 GB free, same as the Switch port
}

// ---------------------------------------------------------------------------
// Level-5 ID: an OPTIONAL cross-game rewards/save-sync account, not DRM and
// not required to play (see level5-id.com/guide/layton-fushigi-app/
// registration.html). Reporting "declined" just matches a real device where
// the player skipped linking.
// ---------------------------------------------------------------------------

bool MainActivity::L5iD_IsEndRequest()
{
    return true;
}

int MainActivity::LVL_GetState() { return 2; }
int MainActivity::LSH_GetState() { return 2; }
int MainActivity::SBS_GetState() { return 2; }

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

std::shared_ptr<FakeJni::JString> MainActivity::DL_GetFileName()
{
    return std::make_shared<FakeJni::JString>("");
}

std::shared_ptr<FakeJni::JString> MainActivity::OS_GetAppVersion()
{
    return std::make_shared<FakeJni::JString>("1.0.8");
}

// static GL_LoadPNG(byte[]) -> int[]{ w, h, rgba... }
std::shared_ptr<FakeJni::JIntArray> MainActivity::GL_LoadPNG(std::shared_ptr<FakeJni::JByteArray> data)
{
    if (!data)
        return nullptr;

    int w = 0, h = 0;
    uint8_t *px = stbi_load_from_memory((const uint8_t *)data->getArray(), data->getSize(), &w, &h, NULL, 4);
    if (!px)
    {
        printf("GL_LoadPNG: decode failed (%s)\n", stbi_failure_reason());
        return nullptr;
    }

    // the engine uploads these as BGRA, so swap R/B (matches the Vita/Switch ports)
    for (int i = 0; i < w * h; i++) {
        uint8_t t = px[i * 4 + 0];
        px[i * 4 + 0] = px[i * 4 + 2];
        px[i * 4 + 2] = t;
    }

    auto result = std::make_shared<FakeJni::JIntArray>(w * h + 2);
    (*result)[0] = w;
    (*result)[1] = h;
    memcpy(&result->getArray()[2], px, (size_t)w * h * 4);
    stbi_image_free(px);
    return result;
}

BEGIN_NATIVE_DESCRIPTOR(MainActivity){FakeJni::Constructor<MainActivity>{}},
    {FakeJni::Function<&MainActivity::MO_PlayMovie>{}, "MO_PlayMovie", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_PlayMovieRegion>{}, "MO_PlayMovie", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_GetState>{}, "MO_GetState", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_GetPosition>{}, "MO_GetPosition", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_PauseMovie>{}, "MO_PauseMovie", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_ReleaseMovie>{}, "MO_ReleaseMovie", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_SetVolume>{}, "MO_SetVolume", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::MO_UpdateTexture>{}, "MO_UpdateTexture", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::UI_GetEditState>{}, "UI_GetEditState", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::UI_GetEditText>{}, "UI_GetEditText", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::UI_StartEditText>{}, "UI_StartEditText", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::UI_SetIdleTimerDisabled>{}, "UI_SetIdleTimerDisabled", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::CARD_GetFilesDirName>{}, "CARD_GetFilesDirName", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::CARD_GetExternalFilesDirName>{}, "CARD_GetExternalFilesDirName", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::CARD_CreateDirectory>{}, "CARD_CreateDirectory", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::CARD_GetAvailableBytes>{}, "CARD_GetAvailableBytes", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::L5iD_IsEndRequest>{}, "L5iD_IsEndRequest", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::LVL_GetState>{}, "LVL_GetState", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::LSH_GetState>{}, "LSH_GetState", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::SBS_GetState>{}, "SBS_GetState", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::DL_GetFileName>{}, "DL_GetFileName", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::OS_GetAppVersion>{}, "OS_GetAppVersion", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&MainActivity::GL_LoadPNG>{}, "GL_LoadPNG", FakeJni::JMethodID::STATIC},
END_NATIVE_DESCRIPTOR
