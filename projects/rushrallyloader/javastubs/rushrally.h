#ifndef __RUSHRALLY_H__
#define __RUSHRALLY_H__

#include "baron/baron.h"
#include "android.h"

// libRushRally3.so is a standard NDK ANativeActivity app (Brownmonster's
// in-house "RuSDK" engine, statically linking libc++/OpenSSL/curl/zlib/vorbis
// into the one .so) -- window, input and assets all go through the NDK C API,
// not through this class.
//
// The .so exports 14 JNI natives, all under brownmonster.rusdk.* (Play Games
// sign-in/friends/snapshots, IAP, text input) rather than on this class --
// none of them are required to boot and render, so this milestone leaves
// RushRally3Activity with no native methods registered at all. It only needs
// to exist so ANativeActivity_create<RushRally3Activity>() has a real jnivm
// class to instantiate.
namespace jnivm
{
    namespace brownmonster
    {
        namespace app
        {
            namespace game
            {
                namespace rushrally3
                {
                    class RushRally3Activity : public jnivm::android::app::NativeActivity
                    {
                    public:
                        DEFINE_CLASS_NAME("brownmonster/app/game/rushrally3/RushRally3Activity")
                    };
                }
            }
        }
    }
}
#endif
