#ifndef __GDASH_COCOS2DX_H__
#define __GDASH_COCOS2DX_H__

#include "baron/baron.h"
#include "android.h"

#include <string>

// Loader-side callbacks the JNI surface needs, implemented in main.cpp:
// software keyboard, quit, and the BACK-button gate.
void gdash_open_ime_keyboard();
void gdash_show_edittext_dialog(const char *title, const char *msg, int maxlen);
void gdash_request_quit();
extern volatile int gdash_block_back_button;

#define GD_PACKAGE_NAME "com.robtopx.geometryjump"

// libcocos2dcpp.so (RobTop's cocos2d-x 2.2 fork) reaches back into Java
// through cocos2d's JniHelper: static methods on org.cocos2dx.lib.
// Cocos2dxHelper. The method list and signatures are from the public
// cocos2d-x 2.2 sources, cross-checked against the gdash_nx Switch port's
// name-dispatched JNI table for this exact binary.
namespace jnivm
{
    namespace org
    {
        namespace cocos2dx
        {
            namespace lib
            {
                class Cocos2dxHelper : public FakeJni::JObject
                {
                public:
                    DEFINE_CLASS_NAME("org/cocos2dx/lib/Cocos2dxHelper")
                    // paths / identity
                    static std::shared_ptr<FakeJni::JString> getCocos2dxWritablePath();
                    static std::shared_ptr<FakeJni::JString> getCocos2dxPackageName();
                    static std::shared_ptr<FakeJni::JString> getCurrentLanguage();
                    static std::shared_ptr<FakeJni::JString> getUserID();

                    // SharedPreferences
                    static std::shared_ptr<FakeJni::JString> getStringForKey(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> def);
                    static bool getBoolForKey(std::shared_ptr<FakeJni::JString> key, bool def);
                    static int getIntegerForKey(std::shared_ptr<FakeJni::JString> key, int def);
                    static float getFloatForKey(std::shared_ptr<FakeJni::JString> key, float def);
                    static double getDoubleForKey(std::shared_ptr<FakeJni::JString> key, double def);
                    static void setStringForKey(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> value);
                    static void setBoolForKey(std::shared_ptr<FakeJni::JString> key, bool value);
                    static void setIntegerForKey(std::shared_ptr<FakeJni::JString> key, int value);
                    static void setFloatForKey(std::shared_ptr<FakeJni::JString> key, float value);
                    static void setDoubleForKey(std::shared_ptr<FakeJni::JString> key, double value);

                    // device info
                    static int getDPI();
                    static int getFontSizeAccordingHeight(int h);
                    static float getDeviceRefreshRate();
                    static bool isNetworkAvailable();
                    static std::shared_ptr<FakeJni::JString> getStringWithEllipsis(std::shared_ptr<FakeJni::JString> in, int width, int fontSize);

                    // input / lifecycle
                    static void showEditTextDialog(std::shared_ptr<FakeJni::JString> title, std::shared_ptr<FakeJni::JString> msg, int inputMode, int inputFlag, int returnType, int maxlen);
                    static void openIMEKeyboard();
                    static void setBlockBackButton(bool value);
                    static void terminateProcess();
                    static void setAnimationInterval(float interval);
                    static void loadingFinished();
                    static void openURL(std::shared_ptr<FakeJni::JString> url);
                    static void closeIMEKeyboard();
                    static void copyToClipboard(std::shared_ptr<FakeJni::JString> text);
                };

                // The GLSurfaceView renderer class; the engine sets the frame
                // interval on it at startup.
                class Cocos2dxRenderer : public FakeJni::JObject
                {
                public:
                    DEFINE_CLASS_NAME("org/cocos2dx/lib/Cocos2dxRenderer")

                    static void setAnimationInterval(double interval);
                };

                // Text bitmaps: the engine renders labels (level name, %
                // loaders) through this; a no-op means missing text but no
                // crash.
                class Cocos2dxBitmap : public FakeJni::JObject
                {
                public:
                    DEFINE_CLASS_NAME("org/cocos2dx/lib/Cocos2dxBitmap")

                    static void createTextBitmapShadowStroke(std::shared_ptr<FakeJni::JString> text, std::shared_ptr<FakeJni::JString> fontFile, int fontSize, float red, float green, float blue, int alignment, int width, int height, bool shadow, float shadowRed, float shadowGreen, float shadowBlue, bool stroke, float strokeRed, float strokeGreen, float strokeBlue);
                };
            }
        }
    }
}

#endif
