# Layton 1 (Curious Village HD) ARM Linux port — plan & research notes

Status: **research complete, implementation not started**. Working on the `neo` branch.
Last updated: 2026-07-27.

## Goal

Port *Professor Layton and the Curious Village HD* (Android release, native lib
`libll1.so`, a custom Level-5 engine — **not Unity**) to run on ARM Linux handhelds
using Bogodroid as the loader/shim framework, following the same approach as the
existing Switch homebrew port in `reference/layton_nx-main/`.

## Key decision: build on `neo`, not `main`

`main`'s loader (`platform/common/so_util.c`) is ELF32/armeabi-v7a only. The `neo`
branch (`upstream/neo`, now checked out locally) already has a working ELF64/aarch64
loader, cross-compile tooling, and a much cleaner per-project scaffold. **Use `neo`
as the base for the new `laytonloader` project.**

## Reference project: `reference/layton_nx-main/` (Switch homebrew port)

- Loads `libll1.so` (arm64 Elf64) via a from-scratch loader (`source/so_util.c`):
  parses PT_LOAD segments, handles `R_AARCH64_ABS64/RELATIVE/GLOB_DAT/JUMP_SLOT`
  plus packed RELR relocations, maps memory via Switch syscalls.
- `source/jni.c` — **hand-rolled name-dispatch fake JNIEnv**, not a generic Java
  object model. Builds a 233-slot JNIEnv vtable; `Call*Method` dispatch compares
  the resolved method **name string** against a hardcoded set of ~15 custom native
  methods the Level-5 engine calls: `MO_PlayMovie`, `MO_GetState`, `UI_GetEditState`,
  `L5iD_*`, `LVL_GetState`, `GL_LoadPNG`, `CARD_GetFilesDirName`,
  `UI_SetIdleTimerDisabled`, etc. Has a manual ref-counted "live object registry"
  (8192 slots) for fake String/Array/Object handles.
- `source/imports.c` — import table mapping every undefined symbol in `libll1.so`
  to newlib/libnx passthroughs, bionic-ABI shims, GLES2, or the mini OpenSL ES impl.
- `source/libc_shim.c` — bionic↔newlib ABI conversion (struct stat layout, `_chk`
  fortify wrappers, fake bionic `__sF` FILE* array for libc++ cout/cerr).
- `source/movie.c` (581 lines) — **FFmpeg**-based .mp4 cutscene decode on a worker
  thread, composited into a GL texture the game itself creates
  (`MO_CreateTexture`/`GL_DrawMovie` JNI calls intercepted).
- `source/opensl.c` (580 lines) — minimal **OpenSL ES 1.0.1** (engine/outputmix/
  buffer-queue player) implemented over libnx `audout`, because CRI ADX2's Android
  backend expects that API.
- `source/main.c` — entry sequence: EGL init → `so_load`+relocate+resolve →
  `patch_game()` hooks a few exported symbols → resolve `JNI_OnLoad`/
  `setViewSize`/`resume`/`render`/`MO_CreateTexture` **before** finalize (dynsym
  becomes unreadable after finalize remaps memory) → finalize+flush caches →
  `so_execute_init_array` → call `JNI_OnLoad` then `setViewSize`/`resume` →
  per-frame loop calling the render entry point with touch input.
- **Not Unity.** No il2cpp/mono, no generic reflection, no real `AAssetManager`/
  `ANativeActivity` lifecycle — engine credited to Level-5 (`OS_Run`, `NitroMain`
  refs — DS-ported engine). Audio via CRI ADX2 (needs OpenSL ES). Credits confirm
  lineage: TheOfficialFloW's original Android so-loader (gtasa_vita) →
  Rinnegatamante's Vita Layton port → fgsfds' Switch so-loader groundwork → this port.

## Bogodroid `neo` branch architecture (confirmed via `upstream/neo`)

### Loader — `loader/` (arch-generic, replaces main's ELF32-only `platform/common/so_util.c`)
- `loader/so_util.cpp`/`.h` — shared relocation/loading logic, handles both ARM32
  and AArch64 relocation types in one switch (`R_AARCH64_RELATIVE`, `_ABS64`,
  `_GLOB_DAT`, `_JUMP_SLOT` alongside `R_ARM_*`).
- `loader/so_util_arm32.cpp` / `loader/so_util_arm64.cpp` — per-arch hooking/trampolines.
- `loader/platform.h` — compile-time `Elf32_*`/`Elf64_*` type selection based on
  `__aarch64__`/`__x86_64`.
- `loader/leb128.h` — SLEB128 decode for Android packed relocations.
- **Caveat:** top-level `CMakeLists.txt` currently only compiles
  `loader/so_util_arm64.cpp` into the build. `so_util_arm32.cpp` exists but is not
  wired into CMake anywhere (dead code today) — the branch is effectively arm64-only
  right now, which is exactly what we need for Layton's `libll1.so`.

### Project scaffold — `projects/<name>/`
Each project is `main.cpp` + `javastubs/` (own JNI classes + `binding.cpp`),
auto-globbed by CMake via `-DPROJ=<name>`. Existing projects:
- **`teapotloader`** — best architectural template. Verified directly (read
  `projects/teapotloader/main.cpp` and `javastubs/teapot.h` on disk): loads
  Google's NDK "Teapot" NativeActivity sample (`lib/arm64-v8a/
  libTeapotNativeActivity.so`, class `com.sample.teapot.TeapotNativeActivity`).
  Sequence: init config/GLES → create fake `Baron::Jvm` → `InitJNIBinding` →
  `so_load` the game .so → build an `ANativeActivity` bound to a fake
  `TeapotNativeActivity` jnivm class → resolve+call `ANativeActivity_onCreate`
  from the loaded .so directly → manually drive `onNativeWindowCreated`/
  `onWindowFocusChanged`/`onResume`/`onStart` → idle loop. **No Unity/Mono
  anywhere in this path** — confirmed no UnityPlayer/mono references in
  `main.cpp` or `teapot.h`. (Note: initial research pass and a later offhand
  comment both floated it as possibly Unity-related — double-checked directly
  against the source and it is not.)
- **`limboloader`** — Playdead's *Limbo*, custom engine, closest precedent for a
  game with **bespoke non-Android native methods**: `LimboActivity` class declares
  `GetLimboCachedAassetsPath()` etc. as real registered methods; `main.cpp` also
  resolves and calls a specific exported symbol directly by name
  (`Java_com_playdead_limbo_LimboActivity_native_1ReportVSyncCallEvent`) rather
  than going through a generic Java dispatch — same pattern the Layton port needs
  for `MO_PlayMovie`/`GL_LoadPNG`/etc.
- `hexagonloader` — openFrameworks-based, less relevant.
- `unityloader` — Mono/IL2CPP + "LemonLoader" .NET CoreCLR host compat path, not
  relevant to Layton (not Unity).

### JNI model
Still real C++ class registration via vendored `libjnivm`/fake-jni
(`Baron::Jvm`, `DEFINE_CLASS_NAME`, `BEGIN_NATIVE_DESCRIPTOR`/
`FakeJni::Function<...>`), **not** the Switch port's name-dispatch hack. Base
hierarchy in shared `javastubs/android.h`: `Context → Activity → NativeActivity`.

**Plan for Layton's custom native surface:** define a `LaytonActivity` (or
similar) extending `jnivm::android::app::NativeActivity` with each of the ~15
custom native methods as real registered member functions
(`MO_PlayMovie`, `MO_GetState`, `UI_GetEditState`, `GL_LoadPNG`,
`CARD_GetFilesDirName`, `UI_SetIdleTimerDisabled`, `L5iD_*`, `LVL_GetState`, ...),
following `teapot.h`/`limbo.h`'s pattern exactly. No changes to jnivm/fake-jni core
needed.

### What's missing on `neo` that Layton needs (net-new work)
- **No FFmpeg / movie decode anywhere** on the branch (confirmed via full-tree
  grep — zero matches for ffmpeg/avcodec/avformat/movie). Need to port
  `reference/layton_nx-main/source/movie.c` logic: FFmpeg decode thread →
  upload frames into a GL texture the game creates, intercepted via the
  `MO_CreateTexture`/`GL_DrawMovie` native methods.
- **No OpenSL ES / CRI ADX2-compatible audio backend.** `neo` only has
  `thunks/openal/` (OpenAL-Soft thunk) and stub `android_media.cpp`
  (`AudioManager` stubs, hardcoded values). Need to port
  `reference/layton_nx-main/source/opensl.c`'s minimal OpenSL ES 1.0.1
  (engine/outputmix/buffer-queue player) onto whatever host audio Bogodroid uses
  on Linux (likely SDL2 audio, since `neo` already links SDL2 for EGL/window/input
  via `thunks/egl_sdl/`).
- **No `bridges/` content yet** — reserved include path in CMakeLists but empty;
  main's bridge concept doesn't exist on neo (folded into `thunks/`). PNG decode
  (`GL_LoadPNG`) can reuse `stb_image.h` (already vendored in the reference project,
  can be copied over) — check `thunks/` for an existing image decode dependency
  before adding a new one.
- **No `projects/laytonloader/`** — new project directory needed, following the
  `teapotloader` template.
- **No `configs/layton.toml`** — new config, same schema as existing
  `configs/teapot.toml` (`[paths]`, `[package]`, `[device]` tables).
- **No `gamefiles/layton/`** — needs `lib/arm64-v8a/libll1.so` + assets from the
  actual Android APK, which we do not currently have on disk.

## Progress log

**2026-07-27**: Switched to `neo` branch locally (`git checkout neo`, tracking
`upstream/neo`). User confirmed they have `libll1.so` + assets and copied them
to `gamefiles/layton/` (verified: `libll1.so` is arm64-v8a, NDK r23, Android 26
target — matches the reference Switch port's binary). Assets live directly under
`gamefiles/layton/data/...` (not an `assets/` APK-style folder), so no
AAssetManager involvement needed — matches the Switch port's direct-fopen
behavior.

Scaffolded `projects/laytonloader/` (Milestone 1: boot-to-render, no movies/audio):
- `main.cpp` — mirrors the Switch port's entry sequence but simplified for
  Bogodroid's thunk model (the guest .so drives its own EGL/GLES calls via
  `so_dynamic_libraries`, so the host doesn't need to manage the GL context
  per-frame like the Switch port did): `init_config` → `sdl_initialize_gles`
  → `so_load` `libll1.so` at `0x50000000` → resolve+call `JNI_OnLoad` →
  resolve `Java_com_Level5_LT1R_MainActivity_{setViewSize,resume,render}` by
  symbol name → call `setViewSize`/`resume` → loop calling `render(env, null,
  1, 0, 0, 0,0,0,0)` each frame with basic SDL event pump (quit on
  `SDL_QUIT`). No touch input, no movie/audio, no frame-timing yet.
- `javastubs/layton.h`/`.cpp` — `jnivm::com::Level5::LT1R::MainActivity`
  (extends `Activity`, not `NativeActivity`, since this engine calls exported
  `Java_com_Level5_LT1R_MainActivity_*` symbols directly rather than driving
  the `ANativeActivity` callback lifecycle) with all ~21 custom native methods
  from the Switch port's `jni.c` dispatch table registered as real jnivm
  methods, each stubbed to a safe default (movie playback returns
  false/not-playing, license checks return 2/licensed, `CARD_GetFilesDirName`
  returns ".", etc.) — same defaults the Switch/Vita ports use. `GL_LoadPNG`
  is fully implemented via vendored `stb_image.h` (copied from
  `reference/layton_nx-main/source/stb_image.h` into
  `projects/laytonloader/javastubs/`).
- `javastubs/binding.h`/`.cpp` — registers `MainActivity` plus the shared
  Android/Java stub classes, following `teapotloader`'s `InitJNIBinding`
  pattern exactly.
- `configs/layton.toml` — same schema as `configs/teapot.toml`;
  `paths.game_files = "../gamefiles/layton/"`, package
  `com.Level5.LT1R`, device display 960x544 (Vita/PS-Vita-era portrait
  resolution used by this game's other ports; adjust as needed).
- No changes needed to the top-level `CMakeLists.txt` — `PROJ_SOURCES` is a
  recursive glob over `projects/${PROJ}/`, so `-DPROJ=laytonloader` picks up
  the new files automatically, same as every other project.

**2026-07-27 (manual review pass, no build available)**: Without a compiler on
hand, did a careful static read-through of `main.cpp`/`layton.h`/`layton.cpp`
against the actual `loader/`, `libjnivm/include/`, and `thunks/` headers on
disk (not guessing from memory) and found/fixed one real bug, plus resolved
one open question:

- **Bug fixed**: `main.cpp` called the `fatal_error(...)` macro without
  including `platform/common/logging.h` (the only place it's defined). Traced
  the full include chain (`config.h`, `so_util.h`, `io_util.h`, `android.h`,
  `javac.h`, the PCH headers) and none of them pull it in — `projects/teapotloader/main.cpp`
  appears to have this same latent gap (it calls `fatal_error` too, with an
  identical include list, and nothing there reaches `logging.h` either).
  Added `#include "logging.h"` to `laytonloader/main.cpp` directly rather than
  relying on an unverified transitive include; worth flagging to upstream if
  `teapotloader` actually fails to build for the same reason.
- **Resolved (was flagged as unverified)**: the double `BEGIN_NATIVE_DESCRIPTOR`
  entry both named `"MO_PlayMovie"` (two overloads). Checked
  `libjnivm/include/jnivm/class.h`: `Class::methods` is a
  `std::vector<std::shared_ptr<Method>>`, not a name-keyed map, and
  `Descriptor::registre` just appends — so registering the same name twice
  with different C++ signatures is exactly how the framework models real JNI
  method overloading, not an edge case. No longer considered a risk.
- Also individually verified: `JNIEnv*`/`JavaVM*`/`jint`/`jobject`/`jfloat`
  are available transitively via `thunks/ndk/anative_activity.h` (already
  included, already uses these types) — no missing `<jni.h>` issue.
  `FakeJni::JByteArray`/`JIntArray`/`JFloatArray` and their
  `getArray()`/`getSize()` accessors, `jnivm::String`'s `c_str()` (inherited
  from `std::string`, confirmed in `libjnivm/include/jnivm/string.h`) and
  `asStdString()` all check out against their actual definitions. `stb_image.h`
  living in `projects/laytonloader/javastubs/` (not project root) is required
  for the include path to find it, since `CMakeLists.txt` only adds
  `${PROJ_SOURCE_DIR}/javastubs` to the per-project include dirs, not
  `${PROJ_SOURCE_DIR}` itself — confirmed this placement is correct.
- CMake itself needs no changes: `PROJ_SOURCES` is `GLOB_RECURSE` over
  `projects/${PROJ}/`, so `-DPROJ=laytonloader` will pick up `main.cpp` and
  everything under `javastubs/` automatically, same as every other project.

**Build/test still blocked on environment**: this machine (Windows, no WSL
distro, no local gcc/cmake) can't compile or run the ARM Linux target. User
will build on a separate Linux machine (with Docker) later — build/boot
verification (tasks #4/#5) deferred until then.

## Progress log continued: reviewed the Vita port, found a critical gap

**2026-07-27, later**: User pointed at
[Rinnegatamante/layton-vita](https://github.com/Rinnegatamante/layton-vita)
(the source of the `MethodIDs` enum they'd pasted earlier, confirming
`LVL_/LSH_/SBS_GetState` really are all treated identically -- return `2`,
no distinction between them, so left as-is with the DLC framing removed from
comments per their instruction). Read through the Vita port's `main.c`,
`player.c`, `dialog.c`, `config.h` in full and cross-checked against
`reference/layton_nx-main/source/main.c` locally. Found one critical gap in
`laytonloader` and one open risk:

**Fixed -- missing `FS_LoadFile`/`FS_GetLength` binary patches.** Both the
Vita and Switch ports patch two of the engine's own exported C++ functions
directly (`hook_symbol`/`hook_addr`, not JNI):
`_Z11FS_LoadFilePcPKcii` (`FS_LoadFile(char*, char const*, int, int)`) and
`_Z12FS_GetLengthPKc` (`FS_GetLength(char const*)`). This is the engine's
*entire* file-loading path for its "data/..." asset tree -- without this hook
nothing under `gamefiles/layton/data/` would ever load, regardless of how
correct the JNI stub layer is. `main.cpp` was missing this completely. Added
a `patch_game()` function (mirrors `reference/layton_nx-main/source/main.c`'s
`patch_game()`) that hooks both symbols plus `criErr_Notify` (CRI middleware's
error callback, hooked just for debug visibility, optional). Confirmed by
reading `loader/so_util.cpp`'s `so_load()` that this is safe to call any time
after `so_load()`/`load_so_from_file()` returns: `so_flush_caches(mod, 1)`
marks `[patch_base, text_end)` RWX and nothing ever revokes write access
afterward on this Linux loader (no W^X lockdown pass like the Switch port's
staged `so_finalize`), so there's no ordering constraint to worry about.

Path handling (updated -- see next log entry): the engine passes `fname`
relative to the APK assets root (e.g. `"data/ani/akira.png"`, confirmed by a
comment in the Switch port's `libc_shim.c`). `FS_LoadFile`/`FS_GetLength`
prepend `"assets/"` to match.

**Previously an open risk, now resolved by restoring the real APK layout**:
the Vita port also redirects `AAssetManager_open` onto the same asset path,
implying the engine may use the standard NDK `AAssetManager` API for some
files in addition to its own `FS_LoadFile`. This is no longer a concern --
see below.

**2026-07-27, later still**: user replaced the flattened `gamefiles/layton/`
layout with the real extracted-APK structure:
```
gamefiles/layton/lib/arm64-v8a/libll1.so
gamefiles/layton/assets/AVConfig.json
gamefiles/layton/assets/data/{ani,bg,debug,font,html,script,sound,video}
gamefiles/layton/assets/data-{en,de,es,fr,it,EU}/{ani,bg,etext,htext,itext,otext,qtext,room,script,stext,storytext}
gamefiles/layton/assets/dexopt
```
This directly confirms the "data/...", "data-en/..." naming the Switch port's
comment predicted, and means `gamefiles/layton/assets/` now exists for real --
so `thunks/ndk/asset_manager.c`'s hardcoded `"assets"` base path resolves
correctly with **no code changes needed there**, fixing the previously-flagged
open risk for free. Updated `main.cpp`:
- `path_lmain` changed from `"libll1.so"` to `"lib/arm64-v8a/libll1.so"`.
- Added an `asset_path()` helper (`"assets/" + rel`) and pointed
  `FS_LoadFile`/`FS_GetLength` through it, matching the Switch/Vita ports'
  own `asset_path()` pattern exactly now that the real folder structure is in
  place.

**Not ported (deferred, not needed for Milestone 1)**:
- Switch's landscape-mode `GL_DrawMovie` hook (fullscreen aspect-fit cutscene
  draw) -- irrelevant until movie playback exists.
- Switch's vsync "turnstile" mutex pairing
  (`register_turnstile_mutexes`/`TURNSTILE_A_OFF`/`TURNSTILE_B_OFF` in
  `reference/layton_nx-main/source/main.c`) -- works around libnx's mutexes
  forbidding cross-thread unlock for the engine's render/logic thread
  handoff. Whether Linux glibc pthreads need the same workaround is unknown
  (glibc is generally more permissive about this than libnx, but it's still
  technically undefined behavior per POSIX) -- **watch for a hang/deadlock
  once the game starts driving multiple threads**, not before.
- Vita's `stat_hook` (`.mp3` extension probing) and `fopen_hook`
  (write-blocking on the obb file) -- audio-format and OBB-specific,
  irrelevant to our loose-file layout and to Milestone 1 (no audio yet).

## Progress log continued: scanned classes.dex for the full native method surface

**2026-07-27, later still**: user provided the real APK's `classes.dex`/
`classes2.dex` (from the full APK dump, not just `lib/`+`assets/`). No
decompiler was available/used -- method names were extracted directly as raw
ASCII strings from the dex (`grep -a -o -E "(MO_|UI_|CARD_|LVL_|LSH_|SBS_|
L5iD_|GL_LoadPNG|DL_GetFileName|OS_GetAppVersion)[A-Za-z0-9_]*"`), and the
actual `.so`'s exported `Java_com_Level5_LT1R_MainActivity_*` symbols were
extracted the same way directly from `gamefiles/layton/lib/arm64-v8a/libll1.so`.

**Confirmed exported entry points in the .so** (only 5 total):
`render`, `resume`, `setViewSize`, `suspend`, `MO_1CreateTexture`. `suspend`
was not previously known/wired up -- likely the counterpart to `resume`
(pause/exit lifecycle). `MO_CreateTexture` already known, deferred to movie
work. Other dex-declared native methods (`MO_ReleaseTexture`, `MO_Resume`,
`MO_Suspend`) have **no matching exported symbol** in this build, meaning if
used at all it's via runtime `RegisterNatives()`, not the static naming
convention -- not something host code calls directly either way.

**Confirmed via `libjnivm/src/jnivm/internal/method.cpp`**: unregistered JNI
methods do not crash. `GetMethodID` fabricates a placeholder `Method` on a
lookup miss, and `Call*Method` on it returns `defaultVal<T>()` (`{}` --
0/false/null) rather than crashing or throwing. This is the same effective
behavior as the Switch port's manual `if (!strncmp(name, "L5iD_", 5)) return
0;` wildcard, just built into the framework for every unregistered
name/signature automatically.

**Full extra native-method surface found in classes.dex, not yet stubbed**
(confirmed safe to leave unregistered per the above -- listed here for
later, deliberately not implemented now per user instruction, "add to plan
for later, not now"):
- `UI_EndEditText`, `UI_ExitApp`, `UI_OpenBrowser`, `UI_ShowToast`,
  `UI_WebView` -- misc UI callbacks (`UI_ExitApp` is the one most worth doing
  first when this work is picked up: currently a no-op default means an
  in-game "exit" button does nothing instead of closing the app).
- `LSH_Start`/`LSH_End`, `LVL_Start`/`LVL_End`, `SBS_Start`/`SBS_End` -- pair
  with the already-stubbed `*_GetState` calls; likely kick off the
  license-check state machine that `GetState` polls. Since `GetState` already
  unconditionally returns success, these being no-ops should be harmless.
- Extended `L5iD_*` surface (17 methods total vs. the 1 currently stubbed):
  `L5iD_Init`, `L5iD_Login`, `L5iD_IsLinkedAccount`, `L5iD_CreateGdkey`,
  `L5iD_GetGdkey(sCount)`, `L5iD_GetUdkey(Count)`, `L5iD_SetUdkey`,
  `L5iD_DownloadCloudSave`, `L5iD_UploadCloudSave`, `L5iD_GetCloudSaveData`,
  `L5iD_StartAutoLinkDevice`, `L5iD_NeedSignature`, `L5iD_GetRequestVersion`,
  `L5iD_GetL5iDStatusCode`, `L5iD_GetLastStatusCode`,
  `L5iD_SetWebViewCloseButtonText`, `L5iD_HTTP`, `L5iD_L5iDStatus`.

**Not methods -- named constants found in the same scan, no implementation
needed**: `LVL_IDLE/WAIT/SUCCESS/ERROR/RETRY`, `SBS_IDLE/WAIT/ERROR/RETRY/
LICENSED/NOT_LICENSED/ITEM_DEBUG_PURCHASE_MODE`, `UI_MODE/UI_TYPE_DEFAULT/
UI_TYPE_POPUPWINDOW`, `CARD_UTF_8`. Useful confirmation that `SBS_GetState`
really is a binary licensed/not-licensed check (matches the earlier decision
to leave it returning the known-working value).

**Recommended next step once actually building**: build with the default
`Debug` `CMAKE_BUILD_TYPE` (already turns on `JNIVM_ENABLE_TRACE`,
`JNI_DEBUG`, `VERBOSE_LOG` per `CMakeLists.txt`) and read the
`"Constructed Unresolved symbol..."` log lines to see which of the methods
above the engine *actually* calls at runtime, instead of stubbing all ~25
speculatively. Implement only what real usage shows is needed.

## Progress log continued: diffed the Switch port's import table against neo's symtables

**2026-07-27, later still**: cross-referenced `reference/layton_nx-main/source/
imports.c` (the Switch port's complete, confirmed-working import table for
this exact binary -- 203 symbols) against everything `neo`'s
`symtable_libc`/`symtable_ndk`/`symtable_gles2`/`symtable_egl_sdl` actually
cover (`thunks/libc/symtab` -- 1728 generated entries -- plus
`thunks/libc/libc_table.cpp`'s manual additions, `thunks/ndk/ndk.cpp`,
`thunks/egl_sdl/egl_sdl.cpp`, and `thunks/khronos/gles2_funcs.hpp`'s ~900
GL functions). Found real, specific gaps instead of a vague "might be
missing libc++ symbols" guess:

- **`slCreateEngine`, `SL_IID_ENGINE`, `SL_IID_BUFFERQUEUE`, `SL_IID_PLAY`,
  `SL_IID_VOLUME`, `SL_IID_ANDROIDCONFIGURATION`** -- OpenSL ES, completely
  absent from `neo` (confirmed earlier: only `thunks/openal/` exists).
  **This is likely a bigger and EARLIER blocker than previously assessed.**
  The Switch port's `main.c` calls `game_resume()` right after `JNI_OnLoad`/
  `setViewSize`, with the comment "resume() starts CRI audio" -- meaning
  OpenSL ES symbol resolution is probably needed within the first few calls
  into the engine, likely *before* the first `render()` call, not safely
  deferred until "movie/audio work" the way Milestone 1 planning assumed.
  If these imports fail to resolve, either `so_resolve` logs a missing-symbol
  warning and leaves a null function pointer (which then crashes the moment
  the engine calls it, probably inside `resume()`), or -- depending on how
  critical the engine considers audio init -- it could crash immediately.
  **This may need to move up from "deferred" to "needed for Milestone 1."**
- **`AAsset_getLength64`, `AAsset_openFileDescriptor64`, `AAsset_seek`** --
  missing from `thunks/ndk/asset_manager.c`, which only implements
  `AAsset_getLength` (32-bit), `AAsset_seek64`, and no file-descriptor
  variant at all. Only matters if the engine actually calls these three
  specific entry points via `AAssetManager` (still unconfirmed either way --
  see the earlier `AAssetManager` note, now resolved for the *base path*
  question but not for *symbol coverage*).
- **Math functions** (`sin`, `cos`, `tan`, `pow`, `log`, `exp`, `fmod`,
  `atan2`, `asin`, `acos`, `modf`, `sincosf`, and their `f`-suffixed
  variants) -- **not actually a code gap.** Checked
  `thunks/libc/symtab_exclude` and every one of these is *deliberately*
  excluded from `symtable_libc`. Per `loader/so_util.cpp`'s header comment
  ("Can now load multiple android system libraries for better compatibility,
  e.g. libm, libc++, etc.") and `so_resolve_link`'s cross-module resolution
  (matching a loaded module's `DT_NEEDED`/SONAME), the intended fix is
  loading the real bionic `libm.so` (pulled from an Android NDK sysroot) as
  a second `so_module` alongside `libll1.so` -- the same pattern
  `unityloader` already uses for `libmonobdwgc-2.0.so` etc. -- not writing
  thunk implementations by hand. Low-effort once actually building (grab
  `libm.so` from any NDK's `sysroot/usr/lib/aarch64-linux-android/<api>/`,
  load it at a second fixed address before `libll1.so`).

**Net effect on plan**: audio (OpenSL ES) may need to be pulled forward from
"deferred, Milestone 2+" to "required for `laytonloader` to get past
`resume()` at all" -- worth revisiting once real build/run logs confirm
whether `resume()` actually dereferences these before the first render call.

## Progress log continued: added a Tier-1 OpenSL ES stub (no real audio yet)

**2026-07-27, later still**: per user decision ("stub it, no audio for
now"), added `projects/laytonloader/opensl_stub.h`/`.cpp`, a new
`symtable_opensl[]` covering exactly the 6 missing symbols found by the
import diff above (`slCreateEngine`, `SL_IID_ENGINE`, `SL_IID_PLAY`,
`SL_IID_BUFFERQUEUE`, `SL_IID_VOLUME`, `SL_IID_ANDROIDCONFIGURATION`), wired
into `main.cpp`'s `so_dynamic_libraries[]`. No CMake changes needed
(`PROJ_SOURCES` globs recursively).

Implementation mirrors `reference/layton_nx-main/source/opensl.c`'s object/
interface/vtable model exactly (same method slot ordering -- required, since
the game dispatches by vtable offset not name), since that's a proven-correct
implementation of this exact API slice against this exact binary. The only
real change: the Switch port's worker thread feeds real PCM into `audout`;
this stub's worker thread just paces a short delay (based on buffer size and
assumed 48kHz stereo s16) then fires the buffer-queue-consumed callback,
discarding the actual audio data. This keeps CRI ADX2's internal buffer-queue
pump moving (so it doesn't stall waiting for buffer space) without any real
audio output existing yet. Uses `std::thread`/`std::mutex`/
`std::condition_variable` instead of the reference's libnx-specific
`Thread`/`Mutex`/`CondVar` types, otherwise structurally identical.

**Not yet verified by a build**: whether the DynLibFunction data-symbol
resolution pattern used here (`{"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_v}`,
where the local static variable's *address* becomes the resolved symbol,
double-indirection preserved) works exactly like the existing precedent in
`thunks/libc/libc_table.cpp`'s `{"__sF", (uintptr_t)&__sF_fake}` -- structurally
identical pattern, reused rather than invented, but still compiler-unverified.

## Open items / blockers (current)

1. **Build/test environment not yet set up.** This dev machine is Windows with
   no WSL distro, no local gcc/cmake, no aarch64 cross toolchain. Resolved
   options: (a) install a WSL distro and build there, (b) use the armhf/aarch64
   chroot the README describes, (c) build+run directly on the target ARM Linux
   handheld, or (d) cross-compile via `cmake/aarch64-linux-gnu.cmake` from
   some other Linux host. **Need user input on which one to use** before Milestone
   1 can be verified.
2. **Overload registration unverified** — see "Known open question" in the
   progress log above (`MO_PlayMovie` double registration).
3. Task list (`TaskList` in this session) tracks remaining work:
   scaffold done (#1-#3), CMake glob verification and actual boot test (#4-#5)
   pending on the build environment above.

## Completed implementation steps (Milestone 1 scaffold)

1. ~~Scaffold `projects/laytonloader/`~~ — done, see progress log.
2. ~~Add `configs/layton.toml`~~ — done.
3. ~~Define JNI class with custom native methods as registered stubs~~ — done
   (`javastubs/layton.h`/`.cpp`), based on `reference/layton_nx-main/source/jni.c`.
4. ~~Wire up `so_load` of `libll1.so` + entry point resolution + call
   sequence~~ — done (`main.cpp`), simplified vs. `teapotloader`'s
   `ANativeActivity` pattern since this engine uses direct `Java_*` symbol
   exports instead.
5. **Get it booting to first render — blocked on build environment (open item #1).**

## Remaining work after Milestone 1 boots

6. Port PNG decode (`GL_LoadPNG`) via stb_image — already implemented, needs
   verification once building is possible.
7. Port movie playback (FFmpeg) — new `thunks/` or project-local module.
8. Port OpenSL-ES-equivalent audio backend onto SDL2 audio.
9. Touch/input wiring for `render()`'s touch args (currently always 0 touches).
10. Save directory / misc remaining JNI intercepts as they surface at runtime.

## Key file references

- Switch port JNI dispatch table (source of truth for method names): 
  `reference/layton_nx-main/source/jni.c`
- Switch port movie playback: `reference/layton_nx-main/source/movie.c`
- Switch port audio: `reference/layton_nx-main/source/opensl.c`
- Switch port import/symbol table: `reference/layton_nx-main/source/imports.c`
- Bogodroid neo loader: `loader/so_util.cpp`, `loader/so_util_arm64.cpp`
- Best template project: `projects/teapotloader/main.cpp`,
  `projects/teapotloader/javastubs/teapot.h`/`.cpp`
- Custom-native-method precedent: `projects/limboloader/javastubs/limbo.h`,
  `projects/limboloader/main.cpp`
- Config schema example: `configs/teapot.toml`
