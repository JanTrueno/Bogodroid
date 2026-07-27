#pragma once
#include "so_util.h"

// Tier-1 stub: implements just enough of OpenSL ES 1.0.1 (the slice CRI
// ADX2's Android SLES backend uses -- engine, output mix, PCM buffer-queue
// player) for libll1.so's imports to resolve and its init sequence to
// proceed. Buffers the game enqueues are discarded rather than played --
// no real audio output yet. See projects/laytonloader/opensl_stub.cpp.
extern DynLibFunction symtable_opensl[];
