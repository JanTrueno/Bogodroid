#ifndef __GDASH_ROBTOP_H__
#define __GDASH_ROBTOP_H__

#include "baron/baron.h"
#include "android.h"

// Geometry Dash's own activity class. The game reaches into it through
// JniHelper for the handful of RobTop-specific helpers that are not part of
// stock cocos2d-x (the rest of the surface lives on Cocos2dxHelper).
//
// android.os.Build$VERSION (with SDK_INT) is already provided by the shared
// javastubs (android_misc.cpp) and registered by InitJNIAndroidClasses.
namespace jnivm
{
    namespace com
    {
        namespace customRobTop
        {
            namespace base
            {
                class BaseRobTopActivity : public FakeJni::JObject
                {
                public:
                    DEFINE_CLASS_NAME("com/customRobTop/BaseRobTopActivity")

                    static bool gameServicesIsSignedIn();
                    static int getAPILevel();
                    static long uptimeMillis();
                    static float getDeviceRefreshRate();
                    static std::shared_ptr<FakeJni::JString> loadAndDecryptFileToString(std::shared_ptr<FakeJni::JString> file);
                    static void loadingFinished();
                };
            }
        }
    }
}

#endif
