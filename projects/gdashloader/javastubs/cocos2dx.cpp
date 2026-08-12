#include "cocos2dx.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// ---------------------------------------------------------------------------
// SharedPreferences emulation, persisted to prefs.txt beside the game files.
// ---------------------------------------------------------------------------

#define MAX_PREFS 128
#define KEY_LEN 96
#define VAL_LEN 256

static struct
{
    char key[KEY_LEN];
    char val[VAL_LEN];
} prefs[MAX_PREFS];
static int pref_count = 0;
static bool prefs_loaded = false;

static void prefs_load()
{
    prefs_loaded = true;
    FILE *f = fopen("prefs.txt", "r");
    if (!f)
        return;
    char line[KEY_LEN + VAL_LEN + 8];
    while (fgets(line, sizeof(line), f) && pref_count < MAX_PREFS)
    {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        char *val = tab + 1;
        char *nl = strchr(val, '\n');
        if (nl)
            *nl = '\0';
        auto p = &prefs[pref_count++];
        snprintf(p->key, sizeof(p->key), "%s", line);
        snprintf(p->val, sizeof(p->val), "%s", val);
    }
    fclose(f);
}

static const char *prefs_get(const char *key, const char *def)
{
    if (!prefs_loaded)
        prefs_load();
    for (int i = 0; i < pref_count; i++)
        if (!strcmp(prefs[i].key, key))
            return prefs[i].val;
    return def;
}

static void prefs_save()
{
    FILE *f = fopen("prefs.txt", "w");
    if (!f)
        return;
    for (int i = 0; i < pref_count; i++)
        fprintf(f, "%s\t%s\n", prefs[i].key, prefs[i].val);
    fclose(f);
}

static void prefs_set_raw(const char *key, const char *val)
{
    if (!prefs_loaded)
        prefs_load();
    if (!val)
        val = "";
    for (int i = 0; i < pref_count; i++)
    {
        if (!strcmp(prefs[i].key, key))
        {
            if (!strcmp(prefs[i].val, val))
                return; // unchanged: skip the file rewrite (callers can be per-frame)
            snprintf(prefs[i].val, sizeof(prefs[i].val), "%s", val);
            prefs_save();
            return;
        }
    }
    if (pref_count >= MAX_PREFS)
        return;
    auto p = &prefs[pref_count++];
    snprintf(p->key, sizeof(p->key), "%s", key);
    snprintf(p->val, sizeof(p->val), "%s", val);
    prefs_save();
}

// ---------------------------------------------------------------------------
// Cocos2dxHelper
// ---------------------------------------------------------------------------

static std::string &writable_path()
{
    static std::string p = std::filesystem::absolute("save").string();
    return p;
}

std::shared_ptr<FakeJni::JString> jnivm::org::cocos2dx::lib::Cocos2dxHelper::getCocos2dxWritablePath()
{
    return std::make_shared<FakeJni::JString>(writable_path());
}

std::shared_ptr<FakeJni::JString> jnivm::org::cocos2dx::lib::Cocos2dxHelper::getCocos2dxPackageName()
{
    return std::make_shared<FakeJni::JString>(GD_PACKAGE_NAME);
}

std::shared_ptr<FakeJni::JString> jnivm::org::cocos2dx::lib::Cocos2dxHelper::getCurrentLanguage()
{
    const char *lang = getenv("LANG");
    if (lang)
    {
        if      (strncmp(lang, "fr", 2) == 0) return std::make_shared<FakeJni::JString>("fr");
        else if (strncmp(lang, "de", 2) == 0) return std::make_shared<FakeJni::JString>("de");
        else if (strncmp(lang, "it", 2) == 0) return std::make_shared<FakeJni::JString>("it");
        else if (strncmp(lang, "es", 2) == 0) return std::make_shared<FakeJni::JString>("es");
        else if (strncmp(lang, "nl", 2) == 0) return std::make_shared<FakeJni::JString>("nl");
        else if (strncmp(lang, "pt", 2) == 0) return std::make_shared<FakeJni::JString>("pt");
        else if (strncmp(lang, "ru", 2) == 0) return std::make_shared<FakeJni::JString>("ru");
        else if (strncmp(lang, "ja", 2) == 0) return std::make_shared<FakeJni::JString>("ja");
        else if (strncmp(lang, "ko", 2) == 0) return std::make_shared<FakeJni::JString>("ko");
        else if (strncmp(lang, "zh", 2) == 0) return std::make_shared<FakeJni::JString>("zh");
    }
    return std::make_shared<FakeJni::JString>("en");
}

// A stable per-install anonymous device ID, generated once and persisted --
// same approach gdash_nx uses on the Switch (there via the hardware RNG;
// here via /dev/urandom). The game sends this with its server requests; a
// fixed "0" for every install/session is at best a minor server-side
// annoyance and at worst something the server could use to conflate
// unrelated players.
static bool valid_device_id(const char *s)
{
    if (strlen(s) != 16)
        return false;
    bool any_nonzero = false;
    for (int i = 0; i < 16; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
            return false;
        any_nonzero |= s[i] != '0';
    }
    return any_nonzero;
}

std::shared_ptr<FakeJni::JString> jnivm::org::cocos2dx::lib::Cocos2dxHelper::getUserID()
{
    static std::string id;
    if (!id.empty())
        return std::make_shared<FakeJni::JString>(id);

    const char *saved = prefs_get("__gdash_device_id", "");
    if (valid_device_id(saved))
    {
        id = saved;
        return std::make_shared<FakeJni::JString>(id);
    }

    // A short read (or no /dev/urandom at all) just leaves some/all of
    // random[] zeroed; valid_device_id()'s any_nonzero check below catches a
    // fully-zeroed result and falls back to the fixed placeholder ID.
    unsigned char random[8] = {0};
    FILE *f = fopen("/dev/urandom", "rb");
    if (f)
    {
        size_t got = fread(random, 1, sizeof(random), f);
        (void)got;
        fclose(f);
    }
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 0; i < 8; i++)
    {
        buf[i * 2] = hex[random[i] >> 4];
        buf[i * 2 + 1] = hex[random[i] & 15];
    }
    buf[16] = '\0';
    id = valid_device_id(buf) ? buf : "0123456789abcdef";
    prefs_set_raw("__gdash_device_id", id.c_str());
    return std::make_shared<FakeJni::JString>(id);
}

std::shared_ptr<FakeJni::JString> jnivm::org::cocos2dx::lib::Cocos2dxHelper::getStringForKey(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> def)
{
    const char *k = key ? key->c_str() : "";
    const char *d = def ? def->c_str() : "";
    return std::make_shared<FakeJni::JString>(prefs_get(k, d));
}

bool jnivm::org::cocos2dx::lib::Cocos2dxHelper::getBoolForKey(std::shared_ptr<FakeJni::JString> key, bool def)
{
    return atoi(prefs_get(key ? key->c_str() : "", def ? "1" : "0")) != 0;
}

int jnivm::org::cocos2dx::lib::Cocos2dxHelper::getIntegerForKey(std::shared_ptr<FakeJni::JString> key, int def)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", def);
    return atoi(prefs_get(key ? key->c_str() : "", buf));
}

float jnivm::org::cocos2dx::lib::Cocos2dxHelper::getFloatForKey(std::shared_ptr<FakeJni::JString> key, float def)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%g", (double)def);
    return strtof(prefs_get(key ? key->c_str() : "", buf), NULL);
}

double jnivm::org::cocos2dx::lib::Cocos2dxHelper::getDoubleForKey(std::shared_ptr<FakeJni::JString> key, double def)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", def);
    return strtod(prefs_get(key ? key->c_str() : "", buf), NULL);
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setStringForKey(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> value)
{
    prefs_set_raw(key ? key->c_str() : "", value ? value->c_str() : "");
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setBoolForKey(std::shared_ptr<FakeJni::JString> key, bool value)
{
    prefs_set_raw(key ? key->c_str() : "", value ? "1" : "0");
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setIntegerForKey(std::shared_ptr<FakeJni::JString> key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    prefs_set_raw(key ? key->c_str() : "", buf);
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setFloatForKey(std::shared_ptr<FakeJni::JString> key, float value)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%g", (double)value);
    prefs_set_raw(key ? key->c_str() : "", buf);
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setDoubleForKey(std::shared_ptr<FakeJni::JString> key, double value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    prefs_set_raw(key ? key->c_str() : "", buf);
}

int jnivm::org::cocos2dx::lib::Cocos2dxHelper::getDPI()
{
    return 236; // 6.2" 1280x720 handheld panel
}

int jnivm::org::cocos2dx::lib::Cocos2dxHelper::getFontSizeAccordingHeight(int h)
{
    return h > 0 ? h * 3 / 4 : 12;
}

float jnivm::org::cocos2dx::lib::Cocos2dxHelper::getDeviceRefreshRate()
{
    return 60.0f;
}

bool jnivm::org::cocos2dx::lib::Cocos2dxHelper::isNetworkAvailable()
{
    return true;
}

std::shared_ptr<FakeJni::JString> jnivm::org::cocos2dx::lib::Cocos2dxHelper::getStringWithEllipsis(std::shared_ptr<FakeJni::JString> in, int width, int fontSize)
{
    (void)width;
    (void)fontSize;
    return in; // return the input unmodified
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::showEditTextDialog(std::shared_ptr<FakeJni::JString> title, std::shared_ptr<FakeJni::JString> msg, int inputMode, int inputFlag, int returnType, int maxlen)
{
    (void)inputMode;
    (void)inputFlag;
    (void)returnType;
    gdash_show_edittext_dialog(title ? title->c_str() : "", msg ? msg->c_str() : "", maxlen);
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::openIMEKeyboard()
{
    gdash_open_ime_keyboard();
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setBlockBackButton(bool value)
{
    gdash_block_back_button = value ? 1 : 0;
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::terminateProcess()
{
    gdash_request_quit();
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::setAnimationInterval(float interval)
{
    (void)interval;
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::loadingFinished()
{
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::openURL(std::shared_ptr<FakeJni::JString> url)
{
    (void)url;
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::closeIMEKeyboard()
{
}

void jnivm::org::cocos2dx::lib::Cocos2dxHelper::copyToClipboard(std::shared_ptr<FakeJni::JString> text)
{
    (void)text;
}

void jnivm::org::cocos2dx::lib::Cocos2dxRenderer::setAnimationInterval(double interval)
{
    (void)interval;
}

void jnivm::org::cocos2dx::lib::Cocos2dxBitmap::createTextBitmapShadowStroke(std::shared_ptr<FakeJni::JString> text, std::shared_ptr<FakeJni::JString> fontFile, int fontSize, float red, float green, float blue, int alignment, int width, int height, bool shadow, float shadowRed, float shadowGreen, float shadowBlue, bool stroke, float strokeRed, float strokeGreen, float strokeBlue)
{
    (void)text; (void)fontFile; (void)fontSize; (void)red; (void)green; (void)blue;
    (void)alignment; (void)width; (void)height; (void)shadow;
    (void)shadowRed; (void)shadowGreen; (void)shadowBlue; (void)stroke;
    (void)strokeRed; (void)strokeGreen; (void)strokeBlue;
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::org::cocos2dx::lib::Cocos2dxHelper)
    { FakeJni::Constructor<Cocos2dxHelper> {} },
    { FakeJni::Function<&Cocos2dxHelper::getCocos2dxWritablePath> {}, "getCocos2dxWritablePath", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getCocos2dxPackageName> {}, "getCocos2dxPackageName", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getCurrentLanguage> {}, "getCurrentLanguage", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getUserID> {}, "getUserID", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getStringForKey> {}, "getStringForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getBoolForKey> {}, "getBoolForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getIntegerForKey> {}, "getIntegerForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getFloatForKey> {}, "getFloatForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getDoubleForKey> {}, "getDoubleForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setStringForKey> {}, "setStringForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setBoolForKey> {}, "setBoolForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setIntegerForKey> {}, "setIntegerForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setFloatForKey> {}, "setFloatForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setDoubleForKey> {}, "setDoubleForKey", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getDPI> {}, "getDPI", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getFontSizeAccordingHeight> {}, "getFontSizeAccordingHeight", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getDeviceRefreshRate> {}, "getDeviceRefreshRate", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::isNetworkAvailable> {}, "isNetworkAvailable", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::getStringWithEllipsis> {}, "getStringWithEllipsis", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::showEditTextDialog> {}, "showEditTextDialog", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::openIMEKeyboard> {}, "openIMEKeyboard", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setBlockBackButton> {}, "setBlockBackButton", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::terminateProcess> {}, "terminateProcess", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::setAnimationInterval> {}, "setAnimationInterval", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::loadingFinished> {}, "loadingFinished", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::openURL> {}, "openURL", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::closeIMEKeyboard> {}, "closeIMEKeyboard", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Cocos2dxHelper::copyToClipboard> {}, "copyToClipboard", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::org::cocos2dx::lib::Cocos2dxRenderer)
    { FakeJni::Constructor<Cocos2dxRenderer> {} },
    { FakeJni::Function<&Cocos2dxRenderer::setAnimationInterval> {}, "setAnimationInterval", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::org::cocos2dx::lib::Cocos2dxBitmap)
    { FakeJni::Constructor<Cocos2dxBitmap> {} },
    { FakeJni::Function<&Cocos2dxBitmap::createTextBitmapShadowStroke> {}, "createTextBitmapShadowStroke", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR
