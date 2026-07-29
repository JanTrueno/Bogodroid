#pragma once
#include "so_util.h"

// OpenSL ES 1.0.1 buffer-queue playback over SDL audio: engine, output mix and
// PCM buffer-queue player -- the slice libLimbo.so imports from libOpenSLES.so.
// See projects/limboloader/opensl_stub.cpp.
extern DynLibFunction symtable_opensl[];
