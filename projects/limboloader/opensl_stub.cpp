// opensl_stub.cpp -- OpenSL ES 1.0.1 buffer-queue playback over SDL audio.
//
// libLimbo.so lists libOpenSLES.so in its DT_NEEDED and imports slCreateEngine
// plus SL_IID_ENGINE/PLAY/BUFFERQUEUE/ANDROIDCONFIGURATION, i.e. the standard
// engine + output mix + PCM buffer-queue player setup. Bogodroid's neo branch
// has no OpenSL ES implementation of its own (only OpenAL thunks exist), so
// these imports are otherwise unresolved.
//
// The interface vtables below MUST keep the exact method order OpenSL ES
// 1.0.1 defines: the game calls methods by vtable slot offset, not by name
// (the ordering was validated against reference/layton_nx-main/source/opensl.c,
// a working implementation of this exact slice of the API on the Switch
// homebrew port).
//
// Taken verbatim from projects/laytonloader/opensl_stub.cpp, which is the
// tested copy. This is now duplicated across two ports -- if a third needs it,
// move it to thunks/ rather than copying it again.

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
#include <chrono>

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
    std::mutex q_lock;
    std::condition_variable q_cond;

    slBufferQueueCallback bq_cb = nullptr;
    void *bq_ctx = nullptr;

    SDL_AudioDeviceID dev = 0; // snapshot at creation: 0 if no device backs this player
    float volume = 1.0f;

    // Drives bq_cb: never on SDL's audio thread, since the engine decodes
    // the next chunk inside the callback and that work must not sit in the
    // device's realtime path. See pump_thread for why this polls on a
    // steady clock rather than waiting for a downstream-drain signal.
    std::thread cb_thread;

    volatile int state = SL_PLAYSTATE_STOPPED;
    volatile bool running = false;
    std::thread thread; // only used as the no-device fallback drain
};

// The game tears down and rebuilds its *entire* OpenSL engine/output-mix/
// player triple far more often than once -- observed on nearly every menu
// transition during actual gameplay, not just at boot. Whether that mirrors
// real Android's own audio-session pause/resume lifecycle or is a quirk of
// this engine, the effect on a naive per-Player SDL device is the same: a
// fresh device opens paused, and if the game destroys this Player again
// before ever calling SetPlayState(PLAYING) on it, nothing was ever heard.
//
// So the real SDL audio device is process-lifetime and shared, independent
// of how many logical Player objects come and go. A new Player only opens
// (or reopens, if the format actually changed) the device on demand; it
// becomes "the" audio source the moment it's told to play, and destroying a
// Player just detaches it if it was the active one -- the physical device
// stays open and ready for whatever Player replaces it next. This assumes
// only one Player is ever meant to be audible at a time, which matches what
// is actually observed here: exactly one engine/mix/player cycle at a time,
// never several overlapping (this engine mixes its own voices internally
// and hands the platform exactly one composited PCM stream, same as most
// licensed audio middlewares' OpenSL ES backends).
static std::mutex g_dev_lock;
static SDL_AudioDeviceID g_dev = 0;
static int g_dev_rate = 0, g_dev_channels = 0, g_dev_bits = 0;
static Player *g_active_player = nullptr;

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

// --- SDL audio device: drains the buffer queue into the sound card ---

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata; // the device is shared -- see g_active_player, not a fixed Player
    SDL_memset(stream, 0, len); // silence wherever the queue runs dry

    Player *p;
    {
        std::lock_guard<std::mutex> lock(g_dev_lock);
        p = g_active_player;
    }
    if (!p)
        return;

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
}

// Reference: a confirmed-working Wwise-over-OpenSL-ES-over-SDL port (Sonic,
// see conversation/commit context) drives its buffer queue from a dedicated
// thread that actively calls the game's registered callback on a steady
// ~4ms clock whenever buffered content drops below a target, independent of
// whatever the SDL/downstream side happens to be draining. That is what real
// Android's OpenSL ES does too: the platform calls the buffer-queue callback
// on its own hardware-clocked cadence, not reactively once the app's own
// consumer drains something.
//
// Our previous design only ever asked for more data *reactively*, gated by
// our own downstream queue crossing a byte watermark (HIGH_WATER) -- which,
// against this specific engine, essentially never asked for data often or
// regularly enough for its own mixer/decode step to get ahead of real-time.
// That produced exactly the symptom traced at length: the engine's ring
// buffer never accumulated real samples, and every callback found on the
// engine's internal mixer paused (a debugger breakpoint, or a large enough
// sleep) happened to buy it the wall-clock time it needed. Actively pumping
// removes the need for any of that: the callback is now invoked frequently
// and predictably enough that the mixer has a real, steady opportunity to
// produce ahead of playback, the same as it would on real hardware.
static void pump_thread(Player *p)
{
    while (p->running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        if (!p->running)
            return;
        if (p->state != SL_PLAYSTATE_PLAYING || !p->bq_cb)
            continue;

        const size_t bytes_per_sec = (size_t)p->channels * p->rate * (p->bits / 8);
        const size_t refill_target = bytes_per_sec / 10; // ~100 ms buffered ahead

        // Each call typically gets one small chunk enqueued (this engine's
        // chunks were observed at ~256 bytes), so filling a real target from
        // empty can take many calls -- cap generously rather than tightly,
        // matching the reference's "call repeatedly until topped up" shape
        // rather than its exact per-tick call count (tuned for a different
        // engine's much larger chunk size).
        for (int calls = 0; calls < 64 && p->running; calls++)
        {
            size_t queued;
            {
                std::lock_guard<std::mutex> lock(p->q_lock);
                queued = p->queued_bytes;
            }
            if (queued >= refill_target)
                break;
            p->bq_cb((void *)p->bqItf, p->bq_ctx);
        }
    }
}

// Fallback when no audio device could be opened: drain buffers on a timer so
// the queue does not grow without bound while pump_thread keeps soliciting
// more data. Nothing is audible on this path regardless.
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
        p->queue.pop_front();
    }
}

// --- SLBufferQueueItf ---

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size)
{
    Player *p = SELF(self);
    if (!pBuffer || size == 0)
        return SL_RESULT_PARAMETER_INVALID;

    // Rate-print rather than per-call: buffers arrive every few ms during
    // real playback and per-call logging would either flood or (capped)
    // stop telling us anything after the first burst. Also track whether
    // the data itself is non-silent, to rule out the game feeding valid-
    // looking but all-zero buffers (e.g. a missing decoder upstream).
    {
        static size_t bytes_since = 0, silent_bytes_since = 0;
        static auto last_print = std::chrono::steady_clock::now();
        static auto last_dump = std::chrono::steady_clock::time_point{};
        bool all_zero = true;
        for (SLuint32 i = 0; i < size; i++)
        {
            if (((const uint8_t *)pBuffer)[i] != 0) { all_zero = false; break; }
        }
        bytes_since += size;
        if (all_zero)
            silent_bytes_since += size;
        auto now = std::chrono::steady_clock::now();

        // Whenever real (non-zero) content shows up, dump it -- rate-limited
        // since a real stream would otherwise flood this every callback.
        // Printed as signed 16-bit samples (matches the negotiated format)
        // plus a peak-amplitude stat: plausible PCM has a peak well above 0
        // and below the full-scale ~32767; near-zero peak on "non-silent"
        // data would mean sub-audible noise, not real audio.
        if (!all_zero && now - last_dump >= std::chrono::seconds(2))
        {
            last_dump = now;
            const int16_t *samples = (const int16_t *)pBuffer;
            SLuint32 nsamples = size / 2;
            int16_t peak = 0;
            for (SLuint32 i = 0; i < nsamples; i++)
            {
                int16_t v = samples[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            printf("opensl: NON-SILENT buffer! size=%u peak_sample=%d first16=", size, peak);
            for (SLuint32 i = 0; i < 16 && i < nsamples; i++)
                printf("%d,", samples[i]);
            printf("\n");
        }

        if (now - last_print >= std::chrono::seconds(1))
        {
            printf("opensl: Enqueue rate: %zu bytes/s (%zu silent) player=%p state=%d is_active=%d\n",
                bytes_since, silent_bytes_since, (void *)p, p->state, g_active_player == p);
            bytes_since = 0;
            silent_bytes_since = 0;
            last_print = now;
        }
    }

    // No acknowledgment bookkeeping here anymore: pump_thread is the sole
    // driver of bq_cb calls now (see its comment), so Enqueue just has to
    // store the data.
    {
        std::lock_guard<std::mutex> lock(p->q_lock);
        Buffer b;
        b.data.assign((const uint8_t *)pBuffer, (const uint8_t *)pBuffer + size);
        p->queue.push_back(std::move(b));
        p->queued_bytes += size;
    }
    p->q_cond.notify_one();
    return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self)
{
    Player *p = SELF(self);
    std::lock_guard<std::mutex> lock(p->q_lock);
    p->queue.clear();
    p->queued_bytes = 0;
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
    printf("opensl: SetPlayState player=%p state=%u (1=stopped,2=paused,3=playing)\n", (void *)p, (unsigned)state);

    // Becoming the audible source (or stepping down from it) is decided here,
    // against the one shared device -- see the comment above g_dev.
    {
        std::lock_guard<std::mutex> lock(g_dev_lock);
        if (state == SL_PLAYSTATE_PLAYING)
        {
            g_active_player = p;
            if (g_dev)
                SDL_PauseAudioDevice(g_dev, 0);
        }
        else if (g_active_player == p && g_dev)
        {
            SDL_PauseAudioDevice(g_dev, 1);
        }
    }
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

// Declared here rather than below the engine vtable so the tracing in
// obj_Realize can name which object is being realized.
enum { OBJ_ENGINE, OBJ_MIX, OBJ_PLAYER };

struct Object {
    const void *objVtbl; // must be first: this is the SLObjectItf the game holds
    int kind;
    const void *engVtbl; // for OBJ_ENGINE
    Player *player;      // for OBJ_PLAYER
};

static SLresult obj_Realize(void *self, SLboolean async)
{
    // Traced: Wwise realizes the engine, then the output mix, then the player.
    // Seeing where the sequence stops localises an audio-init failure that
    // otherwise produces no output at all.
    Object *o = (Object *)self;
    printf("opensl: Realize(kind=%d)\n", o ? o->kind : -1);
    fflush(stdout);
    (void)async;
    return SL_RESULT_SUCCESS;
}
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
        // Detach from the shared device rather than closing it (see the
        // comment above g_dev): the next Player created may reuse it
        // directly. SDL_LockAudioDevice blocks until the callback -- which
        // may be mid-read of g_active_player right now -- isn't running, so
        // clearing the pointer here can't race a callback invocation that
        // already grabbed the old p.
        {
            std::lock_guard<std::mutex> lock(g_dev_lock);
            if (g_dev)
                SDL_LockAudioDevice(g_dev);
            if (g_active_player == p)
                g_active_player = nullptr;
            if (g_dev)
                SDL_UnlockAudioDevice(g_dev);
        }
        p->running = false;
        p->q_cond.notify_one();
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
    printf("opensl: CreateOutputMix\n");
    fflush(stdout);
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

    // Reuse the shared device (see the comment above g_dev) unless it does
    // not exist yet or the game asked for a genuinely different format.
    {
        std::lock_guard<std::mutex> lock(g_dev_lock);
        bool need_open = (g_dev == 0 || g_dev_rate != p->rate ||
                          g_dev_channels != p->channels || g_dev_bits != p->bits);
        if (need_open)
        {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
                printf("opensl: SDL audio init failed: %s\n", SDL_GetError());

            SDL_AudioSpec want = {}, have = {};
            want.freq = p->rate;
            want.format = p->bits == 8 ? AUDIO_U8 : AUDIO_S16SYS;
            want.channels = (Uint8)p->channels;
            want.samples = 1024; // ~21 ms at 48 kHz
            want.callback = audio_callback;
            want.userdata = nullptr; // shared device -- see g_active_player

            if (g_dev)
            {
                SDL_CloseAudioDevice(g_dev);
                g_dev = 0;
                g_active_player = nullptr;
            }

            // No format conversion: the engine's buffers must land in the
            // device exactly as queued, so ask SDL to match rather than resample.
            g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
            if (g_dev)
            {
                g_dev_rate = p->rate;
                g_dev_channels = p->channels;
                g_dev_bits = p->bits;
                printf("opensl: opened shared device: %d Hz, %d ch, %d-bit -- SDL driver=%s, "
                    "negotiated freq=%d format=0x%x channels=%d samples=%d\n",
                    p->rate, p->channels, p->bits, SDL_GetCurrentAudioDriver(),
                    have.freq, have.format, have.channels, have.samples);
            }
            else
            {
                printf("opensl: no audio device (%s) -- running silent\n", SDL_GetError());
            }
        }
        else
        {
            printf("opensl: reusing shared device for new player (%d Hz, %d ch, %d-bit)\n",
                p->rate, p->channels, p->bits);
        }
        p->dev = g_dev;
    }

    p->running = true;
    // pump_thread solicits data regardless of whether a real device backs
    // this player; fallback_thread additionally drains the queue when there
    // is no real device to do that via audio_callback.
    p->cb_thread = std::thread(pump_thread, p);
    if (!p->dev)
        p->thread = std::thread(fallback_thread, p);

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
    // First call the audio middleware makes. If this never prints, the failure
    // is upstream of OpenSL entirely and no audio backend was even attempted.
    printf("opensl: slCreateEngine\n");
    fflush(stdout);
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
