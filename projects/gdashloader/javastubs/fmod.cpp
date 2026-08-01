#include "fmod.h"

#include <SDL2/SDL.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// SDL audio sink in push (queue) mode: the FMOD mixer thread is the sole
// producer and we apply backpressure so its write() paces like a blocking
// Android AudioTrack.
// ---------------------------------------------------------------------------

static SDL_AudioDeviceID s_dev;
static int s_inited;
static Uint32 s_queue_cap; // backpressure ceiling in bytes

static int nx_audio_init(int rate, int channels)
{
    if (s_inited)
        return 1;
    if (rate <= 0)
        rate = 48000;
    if (channels != 1 && channels != 2)
        channels = 2;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        return 0;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = 1024;
    want.callback = NULL; // push model via SDL_QueueAudio

    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev == 0)
        return 0;

    // ~140 ms of queued audio: low enough that the music stays in sync with
    // gameplay, deep enough to ride out a mixer hiccup. The drop deadline
    // (not the depth) is what prevents stutters.
    s_queue_cap = (have.size ? have.size : 4096u) * 6u;
    SDL_PauseAudioDevice(s_dev, 0);
    s_inited = 1;
    return 1;
}

static void nx_audio_write(const void *pcm, int n_shorts)
{
    if (!s_inited || !pcm || n_shorts <= 0)
        return;
    // Bounded backpressure: pace like a blocking AudioTrack, but never hang
    // forever -- only drop after a long stall (dead audio device), not on
    // ordinary queue jitter.
    const Uint32 deadline = SDL_GetTicks() + 200;
    while (SDL_GetQueuedAudioSize(s_dev) > s_queue_cap)
    {
        if ((Sint32)(SDL_GetTicks() - deadline) > 0)
            return; // stalled: drop this chunk
        usleep(500); // 0.5 ms
    }
    SDL_QueueAudio(s_dev, pcm, (Uint32)n_shorts * 2u);
}

// ---------------------------------------------------------------------------
// FMOD
// ---------------------------------------------------------------------------

bool jnivm::org::fmod::FMOD::checkInit()
{
    return true; // pretend org.fmod.FMOD.init(context) was called
}

bool jnivm::org::fmod::FMOD::supportsAAudio()
{
    return false; // no libaaudio.so -> FMOD uses its AudioTrack output
}

bool jnivm::org::fmod::FMOD::supportsLowLatency()
{
    return false;
}

bool jnivm::org::fmod::FMOD::lowLatencyFlag()
{
    return false;
}

bool jnivm::org::fmod::FMOD::isBluetoothOn()
{
    return false;
}

int jnivm::org::fmod::FMOD::getOutputSampleRate()
{
    return 48000;
}

int jnivm::org::fmod::FMOD::getOutputBlockSize()
{
    return 1024;
}

// AudioDevice.init(channels, sampleRate, bufferSize, numBuffers): FMOD's
// AudioTrack output opens the sink here. Detect rate/channels by value
// (robust to arg order).
bool jnivm::org::fmod::AudioDevice::init(int a, int b, int c, int d)
{
    int vals[4] = { a, b, c, d };
    int rate = 48000, ch = 2;
    for (int i = 0; i < 4; i++)
        if (vals[i] == 8000 || vals[i] == 11025 || vals[i] == 16000 || vals[i] == 22050 ||
            vals[i] == 24000 || vals[i] == 32000 || vals[i] == 44100 || vals[i] == 48000)
            rate = vals[i];
    for (int i = 0; i < 4; i++)
        if (vals[i] == 1 || vals[i] == 2)
        {
            ch = vals[i];
            break;
        }
    return nx_audio_init(rate, ch) != 0;
}

void jnivm::org::fmod::AudioDevice::close()
{
    if (s_inited)
    {
        SDL_CloseAudioDevice(s_dev);
        s_inited = 0;
    }
}

void jnivm::org::fmod::AudioDevice::write(std::shared_ptr<FakeJni::JShortArray> data, int lengthInShorts)
{
    if (data)
        nx_audio_write(data->getArray(), lengthInShorts);
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::org::fmod::FMOD)
    { FakeJni::Constructor<FMOD> {} },
    { FakeJni::Function<&FMOD::checkInit> {}, "checkInit", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&FMOD::supportsAAudio> {}, "supportsAAudio", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&FMOD::supportsLowLatency> {}, "supportsLowLatency", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&FMOD::lowLatencyFlag> {}, "lowLatencyFlag", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&FMOD::isBluetoothOn> {}, "isBluetoothOn", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&FMOD::getOutputSampleRate> {}, "getOutputSampleRate", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&FMOD::getOutputBlockSize> {}, "getOutputBlockSize", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::org::fmod::AudioDevice)
    { FakeJni::Constructor<AudioDevice> {} },
    { FakeJni::Function<&AudioDevice::init> {}, "init", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AudioDevice::close> {}, "close", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AudioDevice::write> {}, "write", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR
