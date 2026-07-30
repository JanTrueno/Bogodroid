// opensl_stub.cpp -- OpenSL ES 1.0.1 buffer-queue playback over SDL audio.
//
// libll3.so plays all of its audio through CRI ADX2, whose Android backend
// drives a standard OpenSL ES 1.0.1 buffer-queue player: an engine, an
// output mix, and a PCM buffer-queue player. Bogodroid's neo branch has no
// OpenSL ES implementation at all (only OpenAL thunks exist), so these
// imports are otherwise unresolved.
//
// The interface vtables below MUST keep the exact method order OpenSL ES
// 1.0.1 defines: the game calls methods by vtable slot offset, not by name
// (the ordering was validated against reference/layton3_nx-main/source/opensl.c,
// a working implementation of this exact slice of the API on the Switch
// homebrew port).
//
// Taken from projects/laytonloader/opensl_stub.cpp, which is the tested copy;
// this is now duplicated across three ports (laytonloader, limboloader, this
// one) and probably wants to move to thunks/ if a fourth needs it.
//
// This satisfies the object/interface model and keeps CRI's internal
// buffer-queue pump moving: each enqueued buffer is copied into a private
// slot (CRI recycles its own buffer as soon as Enqueue returns) and played
// out through a real SDL audio device, resampled to the device's rate.

#include "opensl_stub.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>

#include <SDL2/SDL.h>

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PARAMETER_INVALID 13
#define SL_RESULT_FEATURE_UNSUPPORTED 12
#define SL_RESULT_MEMORY_FAILURE 2

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3

#define SL_DATAFORMAT_PCM 2

typedef uint32_t SLuint32;
typedef uint8_t SLboolean;
typedef uint32_t SLresult;

typedef struct {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec; // millihertz
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
    void *pLocator;
    void *pFormat;
} SLDataSource, SLDataSink;

// interface ids: opaque, compared by pointer identity
static const int iid_engine = 0, iid_play = 0, iid_bufferqueue = 0, iid_volume = 0, iid_androidcfg = 0;
static const void *SL_IID_ENGINE_v = &iid_engine;
static const void *SL_IID_PLAY_v = &iid_play;
static const void *SL_IID_BUFFERQUEUE_v = &iid_bufferqueue;
static const void *SL_IID_VOLUME_v = &iid_volume;
static const void *SL_IID_ANDROIDCONFIGURATION_v = &iid_androidcfg;

typedef void (*slBufferQueueCallback)(void *bq, void *context);
typedef void (*slPlayCallback)(void *play, void *context, SLuint32 event);

// One enqueued buffer. OpenSL ES lets the app keep ownership of the memory
// until the completion callback fires, but CRI reuses its buffers promptly
// and the audio thread reads them asynchronously, so take a copy instead of
// racing the engine for the original.
struct Buffer {
    std::vector<uint8_t> data;
    size_t pos = 0; // bytes already handed to the device
};

struct Player {
    const void *playItf;
    const void *bqItf;
    const void *volItf;
    const void *cfgItf;

    int channels = 2;
    int rate = 48000;
    int bits = 16;

    std::deque<Buffer> queue;
    size_t queued_bytes = 0; // unplayed bytes across the whole queue
    int owed = 0;            // buffers accepted but not yet acknowledged
    std::mutex q_lock;
    std::condition_variable q_cond;

    slBufferQueueCallback bq_cb = nullptr;
    void *bq_ctx = nullptr;

    SDL_AudioDeviceID dev = 0; // 0 when no device could be opened
    float volume = 1.0f;

    // Completion callbacks run on their own thread, never on SDL's audio
    // thread: the engine decodes the next chunk inside the callback, and
    // that work must not sit in the device's realtime path.
    int pending_completions = 0;
    std::mutex cb_lock;
    std::condition_variable cb_cond;
    std::thread cb_thread;

    volatile int state = SL_PLAYSTATE_STOPPED;
    volatile bool running = false;
    std::thread thread; // only used as the no-device fallback pump
};

// each interface pointer the game holds is a pointer to one of these slots;
// GetInterface returns the slot address so (*itf)->Method(itf, ...) dispatches
struct Itf {
    const void *vtbl;
    Player *self;
};

static Itf *itf_new(const void *vtbl, Player *p)
{
    Itf *i = (Itf *)calloc(1, sizeof(*i));
    i->vtbl = vtbl;
    i->self = p;
    return i;
}

#define SELF(itf) (((Itf *)(itf))->self)

// The engine hands over ~1024-byte chunks and keeps only one in flight: it
// enqueues, waits for the completion, then enqueues the next. Acknowledging a
// buffer only once the device had played it therefore left at most 5 ms
// queued, while SDL asks for 21 ms per callback -- three quarters of every
// callback came out as silence.
//
// A buffer is copied on Enqueue, so it can be acknowledged as soon as that
// copy is made rather than when it is played. The engine then runs ahead and
// builds a backlog. HIGH_WATER bounds how far, which keeps the engine's
// audio-paced logic honest and stops the queue growing without limit.
static const size_t HIGH_WATER = 48000 * 2 * 2 / 10; // ~100 ms of stereo s16

// Call with q_lock held. Returns how many completions may now be signalled.
static int take_releases_locked(Player *p)
{
    int n = 0;
    while (p->owed > 0 && p->queued_bytes <= HIGH_WATER)
    {
        p->owed--;
        n++;
    }
    return n;
}

// Signal completions. Must NOT be called with q_lock held -- the engine
// enqueues from inside its callback, which takes q_lock again.
static void signal_completions(Player *p, int n)
{
    if (n <= 0)
        return;
    {
        std::lock_guard<std::mutex> lock(p->cb_lock);
        p->pending_completions += n;
    }
    p->cb_cond.notify_one();
}

// --- SDL audio device: drains the buffer queue into the sound card ---

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    Player *p = (Player *)userdata;
    SDL_memset(stream, 0, len); // silence wherever the queue runs dry

    int release = 0;
    {
        std::lock_guard<std::mutex> lock(p->q_lock);
        if (p->state == SL_PLAYSTATE_PLAYING)
        {
            int off = 0;
            while (off < len && !p->queue.empty())
            {
                Buffer &b = p->queue.front();
                const size_t avail = b.data.size() - b.pos;
                const size_t want = (size_t)(len - off);
                const size_t take = avail < want ? avail : want;

                memcpy(stream + off, b.data.data() + b.pos, take);
                b.pos += take;
                off += (int)take;
                p->queued_bytes -= take;

                if (b.pos >= b.data.size())
                    p->queue.pop_front();
            }
        }
        // playing frees space, which may let throttled buffers be released
        release = take_releases_locked(p);
    }
    signal_completions(p, release);
}

// Runs the engine's buffer-completion callbacks off the audio thread. This is
// also where the next Enqueue happens (the engine calls it from inside the
// callback), so it must not hold q_lock.
static void completion_thread(Player *p)
{
    while (p->running)
    {
        int n = 0;
        {
            std::unique_lock<std::mutex> lock(p->cb_lock);
            p->cb_cond.wait(lock, [&] { return !p->running || p->pending_completions > 0; });
            if (!p->running)
                return;
            n = p->pending_completions;
            p->pending_completions = 0;
        }
        for (int i = 0; i < n && p->running; i++)
        {
            if (p->bq_cb)
                p->bq_cb((void *)p->bqItf, p->bq_ctx);
        }
    }
}

// Fallback when no audio device could be opened: retire buffers on a timer so
// the engine's audio pump keeps turning and the game does not stall waiting
// for completions that would never come.
static void fallback_thread(Player *p)
{
    while (p->running)
    {
        std::unique_lock<std::mutex> lock(p->q_lock);
        p->q_cond.wait_for(lock, std::chrono::milliseconds(5), [&] {
            return !p->running || (p->state == SL_PLAYSTATE_PLAYING && !p->queue.empty());
        });
        if (!p->running)
            return;
        if (p->state != SL_PLAYSTATE_PLAYING || p->queue.empty())
            continue;

        const Buffer &front = p->queue.front();
        const size_t size = front.data.size();
        p->queued_bytes -= (size - front.pos);
        if (p->owed > 0)
            p->owed--;
        p->queue.pop_front();
        lock.unlock();

        // pace roughly like real playback would
        const int bytes_per_sec = p->channels * p->rate * (p->bits / 8);
        const int ms = bytes_per_sec > 0 ? (int)((int64_t)size * 1000 / bytes_per_sec) : 5;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 1));

        if (p->bq_cb)
            p->bq_cb((void *)p->bqItf, p->bq_ctx);
    }
}

// --- SLBufferQueueItf ---

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size)
{
    Player *p = SELF(self);
    if (!pBuffer || size == 0)
        return SL_RESULT_PARAMETER_INVALID;

    int release = 0;
    {
        std::lock_guard<std::mutex> lock(p->q_lock);
        Buffer b;
        b.data.assign((const uint8_t *)pBuffer, (const uint8_t *)pBuffer + size);
        p->queue.push_back(std::move(b));
        p->queued_bytes += size;
        p->owed++;
        // With a device attached the copy is enough to hand the buffer back.
        // The fallback pump has no device, so it acknowledges on its own
        // schedule instead and must not double-count here.
        if (p->dev)
            release = take_releases_locked(p);
    }
    signal_completions(p, release);
    p->q_cond.notify_one();
    return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self)
{
    Player *p = SELF(self);
    int release = 0;
    {
        std::lock_guard<std::mutex> lock(p->q_lock);
        p->queue.clear();
        p->queued_bytes = 0;
        // dropping the backlog frees the whole window, so anything still
        // owed can be handed back rather than stranding the engine
        release = take_releases_locked(p);
    }
    signal_completions(p, release);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *pState)
{
    Player *p = SELF(self);
    if (pState)
    {
        SLuint32 *s = (SLuint32 *)pState;
        std::lock_guard<std::mutex> lock(p->q_lock);
        s[0] = (SLuint32)p->queue.size();
        s[1] = 0; // playIndex
    }
    return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx)
{
    Player *p = SELF(self);
    p->bq_cb = cb;
    p->bq_ctx = ctx;
    return SL_RESULT_SUCCESS;
}

static const void *bq_vtbl[] = {
    (void *)bq_Enqueue,
    (void *)bq_Clear,
    (void *)bq_GetState,
    (void *)bq_RegisterCallback,
};

// --- SLPlayItf ---

static SLresult play_SetPlayState(void *self, SLuint32 state)
{
    Player *p = SELF(self);
    p->state = state;
    if (p->dev)
        SDL_PauseAudioDevice(p->dev, state == SL_PLAYSTATE_PLAYING ? 0 : 1);
    if (state == SL_PLAYSTATE_PLAYING)
        p->q_cond.notify_one();
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState)
{
    if (pState)
        *pState = SELF(self)->state;
    return SL_RESULT_SUCCESS;
}
static SLresult play_GetDuration(void *self, SLuint32 *pMsec) { (void)self; if (pMsec) *pMsec = 0; return SL_RESULT_SUCCESS; }
static SLresult play_GetPosition(void *self, SLuint32 *pMsec) { (void)self; if (pMsec) *pMsec = 0; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, slPlayCallback cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult play_ret(void *self) { (void)self; return SL_RESULT_SUCCESS; }

static const void *play_vtbl[] = {
    (void *)play_SetPlayState,
    (void *)play_GetPlayState,
    (void *)play_GetDuration,
    (void *)play_GetPosition,
    (void *)play_RegisterCallback,
    (void *)play_ret, // SetCallbackEventsMask
    (void *)play_ret, // GetCallbackEventsMask
    (void *)play_ret, // SetMarkerPosition
    (void *)play_ret, // ClearMarkerPosition
    (void *)play_ret, // GetMarkerPosition
    (void *)play_ret, // SetPositionUpdatePeriod
    (void *)play_ret, // GetPositionUpdatePeriod
};

// --- SLVolumeItf ---

static SLresult vol_ret(void *self) { (void)self; return SL_RESULT_SUCCESS; }

static const void *vol_vtbl[] = {
    (void *)vol_ret, // SetVolumeLevel
    (void *)vol_ret, // GetVolumeLevel
    (void *)vol_ret, // GetMaxVolumeLevel
    (void *)vol_ret, // SetMute
    (void *)vol_ret, // GetMute
    (void *)vol_ret, // EnableStereoPosition
    (void *)vol_ret, // IsEnabledStereoPosition
    (void *)vol_ret, // SetStereoPosition
    (void *)vol_ret, // GetStereoPosition
};

// --- SLAndroidConfigurationItf ---

static SLresult cfg_ret(void *self) { (void)self; return SL_RESULT_SUCCESS; }

static const void *cfg_vtbl[] = {
    (void *)cfg_ret, // SetConfiguration
    (void *)cfg_ret, // GetConfiguration
    (void *)cfg_ret, // AcquireJavaProxy
    (void *)cfg_ret, // ReleaseJavaProxy
};

// --- SLObjectItf (shared by engine, output mix, player) ---

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = 2 /*REALIZED*/; return SL_RESULT_SUCCESS; }
static SLresult obj_GetInterface(void *self, const void *iid, void *pInterface);
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static void obj_AbortAsyncOperation(void *self) { (void)self; }
static void obj_Destroy(void *self);
static SLresult obj_ret(void *self) { (void)self; return SL_RESULT_SUCCESS; }

static const void *obj_vtbl[] = {
    (void *)obj_Realize,
    (void *)obj_Resume,
    (void *)obj_GetState,
    (void *)obj_GetInterface,
    (void *)obj_RegisterCallback,
    (void *)obj_AbortAsyncOperation,
    (void *)obj_Destroy,
    (void *)obj_ret, // SetPriority
    (void *)obj_ret, // GetPriority
    (void *)obj_ret, // SetLossOfControlInterfaces
};

// --- engine object ---

static SLresult eng_CreateAudioPlayer(void *self, void **pPlayer, SLDataSource *pSrc, SLDataSink *pSnk,
                                       SLuint32 numIfaces, const void *iids, const SLboolean *req);
static SLresult eng_CreateOutputMix(void *self, void **pMix, SLuint32 numIfaces, const void *iids, const SLboolean *req);
static SLresult eng_ret(void *self) { (void)self; return SL_RESULT_FEATURE_UNSUPPORTED; }

static const void *eng_vtbl[] = {
    (void *)eng_ret, // CreateLEDDevice
    (void *)eng_ret, // CreateVibraDevice
    (void *)eng_CreateAudioPlayer,
    (void *)eng_ret, // CreateAudioRecorder
    (void *)eng_ret, // CreateMidiPlayer
    (void *)eng_ret, // CreateListener
    (void *)eng_ret, // Create3DGroup
    (void *)eng_CreateOutputMix,
    (void *)eng_ret, // CreateMetadataExtractor
    (void *)eng_ret, // CreateExtensionObject
    (void *)eng_ret, // QueryNumSupportedInterfaces
    (void *)eng_ret, // QuerySupportedInterfaces
    (void *)eng_ret, // QueryNumSupportedExtensions
    (void *)eng_ret, // QuerySupportedExtension
    (void *)eng_ret, // IsExtensionSupported
};

enum { OBJ_ENGINE, OBJ_MIX, OBJ_PLAYER };

struct Object {
    const void *objVtbl; // must be first: this is the SLObjectItf the game holds
    int kind;
    const void *engVtbl; // for OBJ_ENGINE
    Player *player;       // for OBJ_PLAYER
};

static SLresult obj_GetInterface(void *self, const void *iid, void *pInterface)
{
    Object *o = (Object *)self;
    if (!pInterface)
        return SL_RESULT_PARAMETER_INVALID;
    void **out = (void **)pInterface;

    if (o->kind == OBJ_ENGINE && iid == SL_IID_ENGINE_v)
    {
        *out = &o->engVtbl;
        return SL_RESULT_SUCCESS;
    }
    if (o->kind == OBJ_PLAYER && o->player)
    {
        Player *p = o->player;
        if (iid == SL_IID_PLAY_v) { *out = (void *)p->playItf; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_BUFFERQUEUE_v) { *out = (void *)p->bqItf; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_VOLUME_v) { *out = (void *)p->volItf; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDCONFIGURATION_v) { *out = (void *)p->cfgItf; return SL_RESULT_SUCCESS; }
    }
    printf("opensl_stub: GetInterface unsupported iid %p (kind %d)\n", iid, o ? o->kind : -1);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void obj_Destroy(void *self)
{
    Object *o = (Object *)self;
    if (o->kind == OBJ_PLAYER && o->player)
    {
        Player *p = o->player;
        // Close the device first: the callback runs on SDL's audio thread and
        // dereferences this Player, so it has to be stopped before anything
        // here is torn down.
        if (p->dev)
        {
            SDL_CloseAudioDevice(p->dev);
            p->dev = 0;
        }
        p->running = false;
        p->q_cond.notify_one();
        p->cb_cond.notify_one();
        if (p->thread.joinable())
            p->thread.join();
        if (p->cb_thread.joinable())
            p->cb_thread.join();
        free((void *)p->playItf);
        free((void *)p->bqItf);
        free((void *)p->volItf);
        free((void *)p->cfgItf);
        delete p;
    }
    free(o);
}

static SLresult eng_CreateOutputMix(void *self, void **pMix, SLuint32 numIfaces, const void *iids, const SLboolean *req)
{
    (void)self; (void)numIfaces; (void)iids; (void)req;
    Object *o = (Object *)calloc(1, sizeof(*o));
    o->objVtbl = obj_vtbl;
    o->kind = OBJ_MIX;
    *pMix = o;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateAudioPlayer(void *self, void **pPlayer, SLDataSource *pSrc, SLDataSink *pSnk,
                                       SLuint32 numIfaces, const void *iids, const SLboolean *req)
{
    (void)self; (void)pSnk; (void)numIfaces; (void)iids; (void)req;

    Player *p = new Player();

    if (pSrc && pSrc->pFormat)
    {
        const SLDataFormat_PCM *fmt = (const SLDataFormat_PCM *)pSrc->pFormat;
        if (fmt->formatType == SL_DATAFORMAT_PCM)
        {
            p->channels = (int)fmt->numChannels;
            p->bits = (int)fmt->bitsPerSample;

            // OpenSL ES specifies samplesPerSec in millihertz, but not every
            // implementation honours that -- take the value as-is when it is
            // already a plausible rate, so a Hz value does not become 48.
            const SLuint32 sps = fmt->samplesPerSec;
            p->rate = sps >= 1000000 ? (int)(sps / 1000) : (int)sps;
        }
    }

    p->playItf = itf_new(play_vtbl, p);
    p->bqItf = itf_new(bq_vtbl, p);
    p->volItf = itf_new(vol_vtbl, p);
    p->cfgItf = itf_new(cfg_vtbl, p);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        printf("opensl: SDL audio init failed: %s\n", SDL_GetError());

    SDL_AudioSpec want = {}, have = {};
    want.freq = p->rate;
    want.format = p->bits == 8 ? AUDIO_U8 : AUDIO_S16SYS;
    want.channels = (Uint8)p->channels;
    want.samples = 1024; // ~21 ms at 48 kHz
    want.callback = audio_callback;
    want.userdata = p;

    // No format conversion: the engine's buffers must land in the device
    // exactly as queued, so ask SDL to match rather than resample.
    p->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    p->running = true;
    if (p->dev)
    {
        printf("opensl: %d Hz, %d ch, %d-bit\n", p->rate, p->channels, p->bits);
        p->cb_thread = std::thread(completion_thread, p);
    }
    else
    {
        printf("opensl: no audio device (%s) -- running silent\n", SDL_GetError());
        p->thread = std::thread(fallback_thread, p);
    }

    Object *o = (Object *)calloc(1, sizeof(*o));
    o->objVtbl = obj_vtbl;
    o->kind = OBJ_PLAYER;
    o->player = p;
    *pPlayer = o;
    return SL_RESULT_SUCCESS;
}

static uint32_t slCreateEngine_stub(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                                     uint32_t numInterfaces, const void *pInterfaceIds, const uint8_t *pInterfaceRequired)
{
    (void)numOptions; (void)pEngineOptions; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    if (!pEngine)
        return SL_RESULT_PARAMETER_INVALID;
    Object *o = (Object *)calloc(1, sizeof(*o));
    o->objVtbl = obj_vtbl;
    o->kind = OBJ_ENGINE;
    o->engVtbl = eng_vtbl;
    *pEngine = o;
    return SL_RESULT_SUCCESS;
}

DynLibFunction symtable_opensl[] = {
    {"slCreateEngine", (uintptr_t)&slCreateEngine_stub},
    {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_v},
    {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_v},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE_v},
    {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_v},
    {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION_v},
    {NULL, 0},
};
