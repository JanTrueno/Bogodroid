// opensl_stub.cpp -- Tier-1 OpenSL ES stub (no real audio output yet)
//
// libll1.so plays all of its audio through CRI ADX2, whose Android backend
// drives a standard OpenSL ES 1.0.1 buffer-queue player: an engine, an
// output mix, and a PCM buffer-queue player. Bogodroid's neo branch has no
// OpenSL ES implementation at all (only OpenAL thunks exist), so these
// imports are otherwise unresolved.
//
// The interface vtables below MUST keep the exact method order OpenSL ES
// 1.0.1 defines: the game calls methods by vtable slot offset, not by name
// (confirmed against reference/layton_nx-main/source/opensl.c, a working
// implementation of this exact slice of the API for this exact binary on
// the Switch homebrew port).
//
// This stub satisfies the object/interface model and keeps CRI's internal
// buffer-queue pump moving (every enqueued buffer is "consumed" on a short
// timer and the registered callback fires), but the audio data itself is
// discarded -- nothing reaches a speaker yet.

#include "opensl_stub.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

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
static const int iid_engine, iid_play, iid_bufferqueue, iid_volume, iid_androidcfg;
static const void *SL_IID_ENGINE_v = &iid_engine;
static const void *SL_IID_PLAY_v = &iid_play;
static const void *SL_IID_BUFFERQUEUE_v = &iid_bufferqueue;
static const void *SL_IID_VOLUME_v = &iid_volume;
static const void *SL_IID_ANDROIDCONFIGURATION_v = &iid_androidcfg;

typedef void (*slBufferQueueCallback)(void *bq, void *context);
typedef void (*slPlayCallback)(void *play, void *context, SLuint32 event);

struct Player {
    const void *playItf;
    const void *bqItf;
    const void *volItf;
    const void *cfgItf;

    int channels = 2;
    int rate = 48000;

    std::deque<SLuint32> queue; // pending buffer sizes, data discarded
    std::mutex q_lock;
    std::condition_variable q_cond;

    slBufferQueueCallback bq_cb = nullptr;
    void *bq_ctx = nullptr;

    volatile int state = SL_PLAYSTATE_STOPPED;
    volatile bool running = false;
    std::thread thread;
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

// --- worker: "consumes" queued buffers on a short timer instead of feeding real audio ---

static void player_thread(Player *p)
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

        SLuint32 size = p->queue.front();
        p->queue.pop_front();
        lock.unlock();

        // pace roughly like real playback would (48kHz stereo s16 default)
        const int bytes_per_sec = p->channels * p->rate * (int)sizeof(int16_t);
        const int ms = bytes_per_sec > 0 ? (int)((int64_t)size * 1000 / bytes_per_sec) : 5;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 1));

        if (p->bq_cb)
            p->bq_cb((void *)p->bqItf, p->bq_ctx);
    }
}

// --- SLBufferQueueItf ---

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size)
{
    (void)pBuffer;
    Player *p = SELF(self);
    {
        std::lock_guard<std::mutex> lock(p->q_lock);
        p->queue.push_back(size);
    }
    p->q_cond.notify_one();
    return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self)
{
    Player *p = SELF(self);
    std::lock_guard<std::mutex> lock(p->q_lock);
    p->queue.clear();
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
        p->running = false;
        p->q_cond.notify_one();
        if (p->thread.joinable())
            p->thread.join();
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
            p->rate = (int)(fmt->samplesPerSec / 1000); // millihertz -> hertz
            printf("opensl_stub: CreateAudioPlayer %d ch, %d Hz, %d bps (audio discarded)\n",
                   p->channels, p->rate, fmt->bitsPerSample);
        }
    }

    p->playItf = itf_new(play_vtbl, p);
    p->bqItf = itf_new(bq_vtbl, p);
    p->volItf = itf_new(vol_vtbl, p);
    p->cfgItf = itf_new(cfg_vtbl, p);

    p->running = true;
    p->thread = std::thread(player_thread, p);

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
