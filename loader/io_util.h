#pragma once
#include "so_util.h"

bool load_so_from_file(so_module *mod, const char *filename, uintptr_t addr);

// Creates the parent directory chain for `path`, if missing. A real Android
// app's private storage comes with directories like "files/" and "cache/"
// already created by the OS; this loader's "cache/" working directory starts
// empty, so guest code that assumes a subdirectory already exists and only
// ever fopen()s/open()s into it -- never mkdir()s it itself -- fails to
// create files there. Called from the fopen/open thunks before any
// write-mode open.
void ensure_parent_dirs(const char *path);