// AAssetManager_fromJava override, specific to Limbo.
//
// init_config() finishes its own chdir before the game gets a chance to
// chdir() again on its own (see config.cpp). Limbo does exactly that -- it
// chdirs into its private "cache/" dir after startup, which is harmless on
// real Android (the real AAssetManager talks to the APK's asset store
// directly and isn't affected by the process's current directory at all),
// but here it broke the plain relative "assets" path the shared NDK thunk
// (thunks/ndk/asset_manager.c) normally hands out: any AAssetManager_open
// call made after that chdir was resolving "assets/" against "cache/"
// instead, which does not exist there, and looked indistinguishable from the
// file genuinely not existing. Route through g_loader_root instead so the
// path stays correct regardless of the game's own chdir calls.
//
// Registered ahead of symtable_ndk in so_dynamic_libraries (see main.cpp) so
// the game resolves this instead of the shared implementation.

#include <cstdio>
#include <climits>

#include "so_util.h"
#include "asset_manager.h"

extern "C" char g_loader_root[];

static AAssetManager *AAssetManager_fromJava_limbo(void *env, void *obj)
{
    if (g_loader_root[0] != '\0')
    {
        char abs_path[PATH_MAX];
        snprintf(abs_path, sizeof(abs_path), "%s/assets", g_loader_root);
        return AAssetManager_create(abs_path);
    }
    // g_loader_root not set (init_config() failed or hasn't run) -- fall back
    // to the plain relative behavior rather than handing out a manager with
    // an empty path.
    return AAssetManager_create("assets");
}

DynLibFunction symtable_limbo_assets[] = {
    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_limbo},
    {NULL, (uintptr_t)NULL},
};
