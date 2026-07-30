#pragma once
#include "so_util.h"

// Real OpenSL ES 1.0.1 playback (engine, output mix, PCM buffer-queue player)
// over SDL audio, backing libll2.so's imports for CRI ADX2's Android SLES
// output. See projects/layton2loader/opensl_stub.cpp.
extern DynLibFunction symtable_opensl[];
