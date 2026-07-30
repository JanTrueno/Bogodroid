#ifndef __MOVIE_H__
#define __MOVIE_H__

// Cutscene playback for the MO_* native methods.
//
// The game asks the platform layer to play an .mp4 and then composites it
// itself: on Android MainActivity.MO_CreateTexture makes a GL texture that
// MediaPlayer streams into through a SurfaceTexture, and the engine's own
// MO_Render draws it -- keeping aspect fitting and UI layering intact. So the
// loader never draws the video. It decodes with FFmpeg on a worker thread and
// uploads the frame that is due into the texture the game created.
//
// Modelled on reference/layton_nx-main/source/movie.c, which does the same for
// this binary on the Switch. The differences here are the platform bits: SDL
// for the clock and for audio output, and std:: threading rather than C11.

#include <stdint.h>

// path is relative to the assets dir; offs/size select a sub-region of the
// file (whole file when size <= 0).
bool movie_open(const char *rel_path, int64_t offs, int64_t size);

// true while a movie is loaded (mirrors MO_GetState). Closes the movie once
// decoding has finished and the last frame has been shown.
bool movie_active();

int movie_position_ms(); // mirrors MO_GetPosition
void movie_pause(bool paused);
void movie_set_volume(float vol);
void movie_close(); // safe when nothing is playing

// True once per movie, when the game-side texture still has to be made. The
// main loop then calls the game's exported MO_CreateTexture(w, h) -- it has to
// happen on the GL thread -- and hands the id back with movie_set_texture.
bool movie_pending_texture(int *w, int *h);
void movie_set_texture(unsigned int tex);

// Upload the frame due by the playback clock into the game's texture. Call
// once per main-loop iteration on the GL thread; no-op when idle.
void movie_gl_tick();

#endif
