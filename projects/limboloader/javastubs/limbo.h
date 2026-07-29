#ifndef __LIMBO_H__
#define __LIMBO_H__

#include "baron/baron.h"
#include "android.h"

// libLimbo.so is a standard NDK ANativeActivity app: the engine lives entirely
// in native code, gets its window/input/assets through the NDK C API, and only
// reaches back into Java for the handful of things the NDK cannot do.
//
// The native entry points it exports (verified against the shipped arm64-v8a
// binary's .dynsym) are all on LimboActivity, and are called *by Java into
// native*, not the other way round:
//
//   native_ReportVSyncCallEvent      <- Choreographer frame callback (frame pump)
//   native_DeviceAdded               <- InputManager.onInputDeviceAdded
//   native_DeviceRemoved             <- InputManager.onInputDeviceRemoved
//   native_ReportIsPlayable          <- licensing/"can we play" gate
//   native_ReportGameServicesState   <- Play Games sign-in state
//
// The loader calls those directly (see projects/limboloader/main.cpp), so the
// class here only has to exist and satisfy whatever the engine looks up on it.
namespace jnivm
{
    namespace com
    {
        namespace playdead
        {
            namespace limbo
            {
                class LimboActivity : public jnivm::android::app::NativeActivity
                {
                public:
                    DEFINE_CLASS_NAME("com/playdead/limbo/LimboActivity")

                    // The engine asks Java where it may unpack the .pkg assets
                    // it streams from; hand back the cache dir the Context shim
                    // already manages.
                    std::shared_ptr<FakeJni::JString> GetLimboCachedAssetsPath();
                };

                // Age-gate helper (GDPR/COPPA signals). nativeInit is exported by
                // libLimbo.so and registers this class's natives; with no Play
                // Services present the object just has to exist.
                class LimboAgeSignals : public FakeJni::JObject
                {
                public:
                    DEFINE_CLASS_NAME("com/playdead/limbo/LimboAgeSignals")
                };
            }
        }
    }
}
#endif
