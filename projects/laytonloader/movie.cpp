// movie.cpp -- cutscene video playback (MO_* native methods)
//
// A worker thread demuxes the .mp4 with FFmpeg, decodes the video stream and
// scales each frame to RGBA into a small ring; audio is decoded to 48 kHz
// stereo and played through its own SDL device.
//
// Rendering is done BY THE GAME: MO_CreateTexture makes the texture and the
// engine's MO_Render draws it, so aspect fitting and UI layering keep working.
// The loader only uploads the frame that is due (movie_gl_tick).
//
// Ported from reference/layton_nx-main/source/movie.c. The Switch version
// mixes movie audio into the single audout stream; here the movie gets its own
// SDL audio device instead, so playback does not depend on CRI's player being
// in the right state during a cutscene.

#include "movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <SDL2/SDL.h>

#include "glad.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define NUM_VFRAMES 12
#define MOVIE_RATE 48000 // output rate for the movie's audio device

namespace
{

struct VideoFrame {
    uint8_t *rgba = nullptr;
    double pts = 0.0; // seconds
};

struct Movie {
    std::atomic<bool> active{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> stop_req{false};
    std::atomic<bool> decode_eof{false};

    std::thread thread;

    // the selected file region, held in memory and fed to FFmpeg through AVIO
    uint8_t *src = nullptr;
    int64_t src_size = 0;
    int64_t src_pos = 0;
    AVIOContext *avio = nullptr;

    AVFormatContext *fmt = nullptr;
    AVCodecContext *vctx = nullptr;
    SwsContext *sws = nullptr;
    int vstream = -1;
    double video_tb = 0.0;

    int width = 0, height = 0;

    VideoFrame frames[NUM_VFRAMES];
    int frame_read = 0, frame_write = 0, frame_count = 0;
    std::mutex lock;
    std::condition_variable can_produce;

    // audio: decoded to 48 kHz stereo s16 into a ring drained by the SDL
    // audio callback
    AVCodecContext *actx = nullptr;
    SwrContext *swr = nullptr;
    int astream = -1;
    bool has_audio = false;
    std::atomic<float> volume{1.0f};
    int16_t *aring = nullptr;
    int a_cap = 0, a_r = 0, a_w = 0; // in sample frames
    std::mutex a_lock;
    SDL_AudioDeviceID adev = 0;

    // the texture is created by the game's MO_CreateTexture export and owned
    // by the engine; pending_create asks the main loop to do that on the GL
    // thread, since only it has the context current
    std::atomic<bool> pending_create{false};
    unsigned int game_tex = 0;
    bool tex_alloc = false;

    // playback clock, anchored to the first presented frame
    uint64_t start_tick = 0;
    uint64_t paused_ticks = 0;
    uint64_t pause_tick = 0;
    double pts_base = 0.0;
    double cur_pts = 0.0;

    bool frame_shown = false;
};

Movie mp;

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------

uint64_t now_ticks() { return SDL_GetPerformanceCounter(); }

double movie_clock()
{
    const uint64_t now = mp.paused ? mp.pause_tick : now_ticks();
    return mp.pts_base +
           (double)(now - mp.start_tick - mp.paused_ticks) / (double)SDL_GetPerformanceFrequency();
}

// ---------------------------------------------------------------------------
// in-memory AVIO over the selected file region
// ---------------------------------------------------------------------------

int mem_read(void *, uint8_t *buf, int size)
{
    int64_t remain = mp.src_size - mp.src_pos;
    if (remain <= 0)
        return AVERROR_EOF;
    if (size > remain)
        size = (int)remain;
    memcpy(buf, mp.src + mp.src_pos, size);
    mp.src_pos += size;
    return size;
}

int64_t mem_seek(void *, int64_t off, int whence)
{
    if (whence == AVSEEK_SIZE)
        return mp.src_size;
    int64_t base = 0;
    if (whence == SEEK_CUR)
        base = mp.src_pos;
    else if (whence == SEEK_END)
        base = mp.src_size;
    mp.src_pos = base + off;
    if (mp.src_pos < 0)
        mp.src_pos = 0;
    if (mp.src_pos > mp.src_size)
        mp.src_pos = mp.src_size;
    return mp.src_pos;
}

// ---------------------------------------------------------------------------
// audio output
// ---------------------------------------------------------------------------

void audio_callback(void *, Uint8 *stream, int len)
{
    SDL_memset(stream, 0, len);
    const int frames = len / (2 * (int)sizeof(int16_t));
    int16_t *dst = (int16_t *)stream;

    std::lock_guard<std::mutex> lk(mp.a_lock);
    // gate on frame_shown so pre-rolled audio starts with the first frame
    if (!mp.active || !mp.has_audio || !mp.frame_shown || mp.paused)
        return;

    const float vol = mp.volume;
    for (int i = 0; i < frames && mp.a_r != mp.a_w; i++)
    {
        dst[i * 2 + 0] = (int16_t)((float)mp.aring[mp.a_r * 2 + 0] * vol);
        dst[i * 2 + 1] = (int16_t)((float)mp.aring[mp.a_r * 2 + 1] * vol);
        mp.a_r = (mp.a_r + 1) % mp.a_cap;
    }
}

// ---------------------------------------------------------------------------
// decoder thread
// ---------------------------------------------------------------------------

void push_video_frame(AVFrame *frm)
{
    std::unique_lock<std::mutex> lk(mp.lock);
    mp.can_produce.wait(lk, [] { return mp.frame_count < NUM_VFRAMES || mp.stop_req; });
    if (mp.stop_req)
        return;

    VideoFrame *slot = &mp.frames[mp.frame_write];
    uint8_t *dst[1] = {slot->rgba};
    int dst_stride[1] = {mp.width * 4};
    sws_scale(mp.sws, (const uint8_t *const *)frm->data, frm->linesize, 0, mp.height, dst, dst_stride);

    int64_t ts = frm->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE)
        ts = frm->pts;
    slot->pts = (ts == AV_NOPTS_VALUE) ? 0.0 : (double)ts * mp.video_tb;

    mp.frame_write = (mp.frame_write + 1) % NUM_VFRAMES;
    mp.frame_count++;
}

// The video ring (~0.5 s) backpressures the demuxer, so the 4 s audio ring
// practically never fills; overflow drops samples rather than blocking.
void push_audio_frame(const AVFrame *frm)
{
    static int16_t tmp[8192 * 2]; // decoder thread only
    uint8_t *outp[1] = {(uint8_t *)tmp};
    const int out = swr_convert(mp.swr, outp, 8192, (const uint8_t **)frm->data, frm->nb_samples);
    if (out <= 0)
        return;

    std::lock_guard<std::mutex> lk(mp.a_lock);
    for (int i = 0; i < out; i++)
    {
        const int next = (mp.a_w + 1) % mp.a_cap;
        if (next == mp.a_r)
            break; // ring full
        mp.aring[mp.a_w * 2 + 0] = tmp[i * 2 + 0];
        mp.aring[mp.a_w * 2 + 1] = tmp[i * 2 + 1];
        mp.a_w = next;
    }
}

void drain_codec(AVCodecContext *ctx, AVFrame *frm, bool is_video)
{
    while (avcodec_receive_frame(ctx, frm) == 0)
    {
        if (mp.stop_req)
            return;
        if (is_video)
            push_video_frame(frm);
        else
            push_audio_frame(frm);
    }
}

void decoder_main()
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();

    while (!mp.stop_req)
    {
        if (mp.paused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (av_read_frame(mp.fmt, pkt) < 0)
            break;

        if (pkt->stream_index == mp.vstream)
        {
            if (avcodec_send_packet(mp.vctx, pkt) == 0)
                drain_codec(mp.vctx, frm, true);
        }
        else if (mp.has_audio && pkt->stream_index == mp.astream)
        {
            if (avcodec_send_packet(mp.actx, pkt) == 0)
                drain_codec(mp.actx, frm, false);
        }
        av_packet_unref(pkt);
    }

    if (!mp.stop_req)
    {
        avcodec_send_packet(mp.vctx, NULL);
        drain_codec(mp.vctx, frm, true);
        if (mp.has_audio)
        {
            avcodec_send_packet(mp.actx, NULL);
            drain_codec(mp.actx, frm, false);
        }
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    mp.decode_eof = true;
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

std::string asset_path(const char *rel) { return std::string("assets/") + rel; }

int open_video_codec()
{
    AVStream *stream = mp.fmt->streams[mp.vstream];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return -1;
    mp.vctx = avcodec_alloc_context3(codec);
    if (!mp.vctx)
        return -1;
    if (avcodec_parameters_to_context(mp.vctx, stream->codecpar) < 0)
        return -1;
    mp.vctx->thread_count = 3;
    if (avcodec_open2(mp.vctx, codec, NULL) < 0)
        return -1;
    return 0;
}

// audio is optional; on any failure the movie just plays silent
void open_audio()
{
    mp.astream = av_find_best_stream(mp.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (mp.astream < 0)
        return;

    AVStream *as = mp.fmt->streams[mp.astream];
    const AVCodec *ac = avcodec_find_decoder(as->codecpar->codec_id);
    if (!ac)
        return;
    mp.actx = avcodec_alloc_context3(ac);
    if (!mp.actx)
        return;
    if (avcodec_parameters_to_context(mp.actx, as->codecpar) < 0 ||
        avcodec_open2(mp.actx, ac, NULL) < 0)
        return;

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&mp.swr, &out_layout, AV_SAMPLE_FMT_S16, MOVIE_RATE,
                            &mp.actx->ch_layout, mp.actx->sample_fmt, mp.actx->sample_rate,
                            0, NULL) < 0)
        mp.swr = NULL;
    if (!mp.swr || swr_init(mp.swr) != 0)
    {
        printf("movie: audio resampler setup failed, playing silent\n");
        return;
    }

    mp.a_cap = MOVIE_RATE * 4; // 4 s ring
    mp.aring = (int16_t *)malloc((size_t)mp.a_cap * 2 * sizeof(int16_t));
    if (!mp.aring)
        return;

    SDL_AudioSpec want = {}, have = {};
    want.freq = MOVIE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    mp.adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!mp.adev)
    {
        printf("movie: no audio device (%s), playing silent\n", SDL_GetError());
        free(mp.aring);
        mp.aring = NULL;
        return;
    }

    mp.has_audio = true;
    SDL_PauseAudioDevice(mp.adev, 0);
}

void free_session()
{
    if (mp.adev)
    {
        SDL_CloseAudioDevice(mp.adev); // stops the callback before the ring goes
        mp.adev = 0;
    }
    for (int i = 0; i < NUM_VFRAMES; i++)
    {
        free(mp.frames[i].rgba);
        mp.frames[i].rgba = nullptr;
    }
    if (mp.sws)
        sws_freeContext(mp.sws);
    if (mp.vctx)
        avcodec_free_context(&mp.vctx);
    if (mp.swr)
        swr_free(&mp.swr);
    if (mp.actx)
        avcodec_free_context(&mp.actx);
    free(mp.aring);
    if (mp.fmt)
        avformat_close_input(&mp.fmt);
    if (mp.avio)
    {
        av_freep(&mp.avio->buffer);
        avio_context_free(&mp.avio);
    }
    free(mp.src);

    // reset the plain fields; the atomics and sync objects are reused as-is
    mp.src = nullptr;
    mp.src_size = mp.src_pos = 0;
    mp.avio = nullptr;
    mp.fmt = nullptr;
    mp.vctx = nullptr;
    mp.sws = nullptr;
    mp.vstream = -1;
    mp.video_tb = 0.0;
    mp.width = mp.height = 0;
    mp.frame_read = mp.frame_write = mp.frame_count = 0;
    mp.actx = nullptr;
    mp.swr = nullptr;
    mp.astream = -1;
    mp.has_audio = false;
    mp.aring = nullptr;
    mp.a_cap = mp.a_r = mp.a_w = 0;
    mp.game_tex = 0;
    mp.tex_alloc = false;
    mp.start_tick = mp.paused_ticks = mp.pause_tick = 0;
    mp.pts_base = mp.cur_pts = 0.0;
    mp.frame_shown = false;

    mp.active = false;
    mp.paused = false;
    mp.stop_req = false;
    mp.decode_eof = false;
    mp.pending_create = false;
    mp.volume = 1.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

bool movie_open(const char *rel_path, int64_t offs, int64_t size)
{
    if (mp.active)
        movie_close();

    mp.volume = 1.0f;

    FILE *f = fopen(asset_path(rel_path).c_str(), "rb");
    if (!f)
    {
        printf("movie: cannot open %s\n", rel_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long total = ftell(f);
    if (size <= 0)
        size = total - offs;
    if (offs < 0 || offs + size > total)
    {
        fclose(f);
        printf("movie: bad region offs=%lld size=%lld total=%ld\n",
               (long long)offs, (long long)size, total);
        return false;
    }
    mp.src = (uint8_t *)malloc(size);
    if (!mp.src)
    {
        fclose(f);
        return false;
    }
    fseek(f, offs, SEEK_SET);
    const size_t got = fread(mp.src, 1, size, f);
    fclose(f);
    if (got != (size_t)size)
    {
        printf("movie: short read on %s\n", rel_path);
        goto fail;
    }
    mp.src_size = size;
    mp.src_pos = 0;

    {
        const int avio_bufsz = 64 * 1024;
        uint8_t *avio_buf = (uint8_t *)av_malloc(avio_bufsz);
        mp.avio = avio_alloc_context(avio_buf, avio_bufsz, 0, NULL, mem_read, NULL, mem_seek);
        if (!mp.avio)
            goto fail;

        mp.fmt = avformat_alloc_context();
        mp.fmt->pb = mp.avio;
        if (avformat_open_input(&mp.fmt, NULL, NULL, NULL) < 0)
        {
            printf("movie: avformat_open_input failed\n");
            mp.fmt = NULL;
            goto fail;
        }
        if (avformat_find_stream_info(mp.fmt, NULL) < 0)
            goto fail;

        mp.vstream = av_find_best_stream(mp.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (mp.vstream < 0 || open_video_codec() < 0)
            goto fail;

        mp.width = mp.vctx->width;
        mp.height = mp.vctx->height;
        mp.video_tb = av_q2d(mp.fmt->streams[mp.vstream]->time_base);
        if (mp.width <= 0 || mp.height <= 0)
            goto fail;

        mp.sws = sws_getContext(mp.width, mp.height, mp.vctx->pix_fmt,
                                mp.width, mp.height, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, NULL, NULL, NULL);
        if (!mp.sws)
            goto fail;

        for (int i = 0; i < NUM_VFRAMES; i++)
        {
            mp.frames[i].rgba = (uint8_t *)malloc((size_t)mp.width * mp.height * 4);
            if (!mp.frames[i].rgba)
                goto fail;
        }

        open_audio();

        mp.pending_create = true; // the GL thread makes the game-side texture
        mp.start_tick = now_ticks();
        mp.active = true;
        mp.thread = std::thread(decoder_main);

        printf("movie: playing %s (%dx%d, audio=%d)\n",
               rel_path, mp.width, mp.height, (int)mp.has_audio);
        return true;
    }

fail:
    printf("movie: failed to start %s\n", rel_path ? rel_path : "(null)");
    mp.stop_req = true;
    free_session();
    return false;
}

void movie_close()
{
    if (!mp.active && !mp.thread.joinable())
        return;

    mp.stop_req = true;
    {
        std::lock_guard<std::mutex> lk(mp.lock);
        mp.can_produce.notify_all();
    }
    if (mp.thread.joinable())
        mp.thread.join();
    free_session();
    printf("movie: closed\n");
}

bool movie_active()
{
    if (!mp.active)
        return false;
    // finished once decoding hit EOF and the ring drained
    if (mp.decode_eof && mp.frame_count == 0 && mp.frame_shown)
    {
        movie_close();
        return false;
    }
    return true;
}

int movie_position_ms() { return mp.active ? (int)(mp.cur_pts * 1000.0) : 0; }

void movie_pause(bool paused)
{
    if (!mp.active || mp.paused == paused)
        return;
    if (paused)
    {
        mp.pause_tick = now_ticks();
        mp.paused = true;
    }
    else
    {
        mp.paused_ticks += now_ticks() - mp.pause_tick;
        mp.paused = false;
    }
    if (mp.adev)
        SDL_PauseAudioDevice(mp.adev, paused ? 1 : 0);
}

void movie_set_volume(float vol)
{
    if (vol < 0.0f)
        vol = 0.0f;
    if (vol > 1.0f)
        vol = 1.0f;
    mp.volume = vol;
}

bool movie_pending_texture(int *w, int *h)
{
    if (!mp.active || !mp.pending_create)
        return false;
    mp.pending_create = false;
    *w = mp.width;
    *h = mp.height;
    return true;
}

void movie_set_texture(unsigned int tex)
{
    mp.game_tex = tex;
    mp.tex_alloc = false;
}

void movie_gl_tick()
{
    if (!mp.active)
        return;
    if (!mp.game_tex)
    {
        // the game never handed back a texture -- frames decode into nothing
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            printf("movie: no game texture, frames are being dropped\n");
        }
        return;
    }

    // Adopt the newest frame due by the clock, and always consume something
    // when one is due so the decoder -- which blocks on a full ring -- keeps
    // moving.
    int adopted = -1;
    std::unique_lock<std::mutex> lk(mp.lock);

    if (!mp.frame_shown)
    {
        if (mp.frame_count > 0)
        {
            adopted = mp.frame_read;
            mp.frame_read = (mp.frame_read + 1) % NUM_VFRAMES;
            mp.frame_count--;
            // anchor the clock to the first presented frame, so decoder
            // warm-up does not count as the clock running ahead
            mp.pts_base = mp.frames[adopted].pts;
            mp.start_tick = now_ticks();
            mp.paused_ticks = 0;
        }
    }
    else
    {
        double now = movie_clock();
        // if the process was stalled (window unmapped, scheduler hiccup) the
        // clock has jumped; re-anchor rather than fast-forwarding the gap
        if (now > mp.cur_pts + 2.0)
        {
            mp.pts_base = mp.cur_pts;
            mp.start_tick = now_ticks();
            mp.paused_ticks = 0;
            now = movie_clock();
        }
        while (mp.frame_count > 0 && mp.frames[mp.frame_read].pts <= now)
        {
            adopted = mp.frame_read;
            mp.frame_read = (mp.frame_read + 1) % NUM_VFRAMES;
            mp.frame_count--;
            if (mp.frame_count == 0 || mp.frames[mp.frame_read].pts > now)
                break;
        }
    }

    if (adopted >= 0)
    {
        mp.cur_pts = mp.frames[adopted].pts;

        GLint prev_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(GL_TEXTURE_2D, mp.game_tex);
        if (!mp.tex_alloc)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mp.width, mp.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, mp.frames[adopted].rgba);
            mp.tex_alloc = true;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mp.width, mp.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, mp.frames[adopted].rgba);
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);

        mp.frame_shown = true;
        mp.can_produce.notify_one();
    }
}
