#ifndef __GDASH_FMOD_H__
#define __GDASH_FMOD_H__

#include "baron/baron.h"
#include "android.h"

// libfmod.so (stock Android) caches the JavaVM from JNI_OnLoad and queries
// org.fmod.FMOD for output-backend support; with AAudio/OpenSL refused it
// uses its AudioTrack output, whose org.fmod.AudioDevice.init/write drive
// the SDL audio sink (fmod.cpp).
namespace jnivm
{
    namespace org
    {
        namespace fmod
        {
            class FMOD : public FakeJni::JObject
            {
            public:
                DEFINE_CLASS_NAME("org/fmod/FMOD")

                static bool checkInit();
                static bool supportsAAudio();
                static bool supportsLowLatency();
                static bool lowLatencyFlag();
                static bool isBluetoothOn();
                static int getOutputSampleRate();
                static int getOutputBlockSize();
            };

            class AudioDevice : public FakeJni::JObject
            {
            public:
                DEFINE_CLASS_NAME("org/fmod/AudioDevice")

                // FMOD instantiates org.fmod.AudioDevice and drives it as an
                // instance (init/close/write are instance methods on Android).
                bool init(int a, int b, int c, int d);
                void close();
                void write(std::shared_ptr<FakeJni::JShortArray> data, int lengthInShorts);
            };
        }
    }
}

#endif
