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

                    // ---- gamepad description ----
                    //
                    // Limbo does NOT learn about a gamepad from the input events
                    // themselves. On Android the Java activity inspects the
                    // InputDevice and publishes its layout into these fields;
                    // native_DeviceAdded then makes the engine read them back
                    // (GameController_Android.cpp) and build a GameController.
                    //
                    // Until they exist, GetFieldID fails, no controller is built
                    // and onInputEvent discards everything with "Game controller
                    // is not present" -- so the pad appears dead even though the
                    // events are being delivered correctly.
                    //
                    // Contents must agree with what the loader actually emits;
                    // see controller_button_to_keycode / sdl_axis_to_android in
                    // projects/limboloader/main.cpp.
                    int gamepadDeviceId = 2;        // == LIMBO_DEVICE_GAMEPAD
                    int gamepadVendorId = 0x045E;   // Microsoft
                    int gamepadProductId = 0x028E;  // Xbox 360 pad -- a layout the
                                                    // engine's controller DB knows
                    std::shared_ptr<FakeJni::JIntArray> gamepadButtonCodes;
                    std::shared_ptr<FakeJni::JIntArray> gamepadAxisCodes;
                    std::shared_ptr<FakeJni::JIntArray> gamepadAxisSources;
                    std::shared_ptr<FakeJni::JFloatArray> gamepadAxisMinVals;
                    std::shared_ptr<FakeJni::JFloatArray> gamepadAxisMaxVals;

                    LimboActivity();
                };

                // Age-gate helper (GDPR/COPPA signals). nativeInit is exported by
                // libLimbo.so and registers this class's natives; with no Play
                // Services present the object just has to exist.
                //
                // isAgeResolved appears in the binary's string table, so the
                // engine reads it -- report resolved so a startup gate waiting on
                // an age check cannot stall.
                class LimboAgeSignals : public FakeJni::JObject
                {
                public:
                    DEFINE_CLASS_NAME("com/playdead/limbo/LimboAgeSignals")
                    bool isAgeResolved = true;
                };
            }
        }
    }
}
#endif
