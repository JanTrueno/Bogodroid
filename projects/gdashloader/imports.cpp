/* imports.cpp -- libcocos2dcpp.so + libfmod.so import resolution (Linux)
 *
 * Every undefined dynamic symbol of the two game libs is bound here, on top
 * of the Bogodroid bionic-libc thunks (symtable_libc) and the GLES2 glad
 * loader (symtable_gles2) -- this table is registered FIRST, so its entries
 * take precedence. The FMOD C/C++ API cross-resolves from the loaded
 * libfmod.so automatically; only createSound/createStream are overridden for
 * asset-path rewriting. GL goes through the glad loader (the game imports no
 * EGL at all); threads/fs through the bionic shims; the rest to glibc.
 *
 * Differences from the Switch port's imports.c (gdash_nx):
 *  - no bionic<->newlib stdio/fs shims: glibc IS the Linux ABI. The whole
 *    stdio family is pinned to glibc here (instead of the bionic FILE
 *    wrappers) so FILE* stays consistent end to end, with fix_path() on the
 *    path-taking entry points (the game hardcodes /data/data/... in a few
 *    places; those get rewritten onto the save dir).
 *  - no bionic<->BSD socket conversion: glibc sockets ARE bionic sockets.
 *  - setjmp/longjmp/sigsetjmp/siglongjmp go through a registry: glibc's
 *    jmp_buf is bigger than bionic's, so binding glibc directly would
 *    overflow the game's buffers.
 *  - __errno is a function returning &errno, matching bionic (whose `errno`
 *    macro is *__errno()) rather than a data symbol.
 *  - OpenSL ES entry points fail gracefully: Bogodroid's dlopen returns
 *    "loaded" for libOpenSLES.so, FMOD then dlsym()s slCreateEngine and
 *    calls it; returning FEATURE_UNSUPPORTED makes FMOD fall back to its
 *    AudioTrack output (org.fmod.AudioDevice -> audio.cpp).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <cctype>
#include <climits>
#include <cmath>
#include <csignal>
#include <csetjmp>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wctype.h>
#include <wchar.h>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>

#include "imports.h"
#include "so_util.h"

// The game asks FMOD for "file:///android_asset/<rel>"; the createSound/
// createStream wrappers rewrite that prefix to the loose assets dir (relative
// to the game files dir, which init_config() chdir'd into).
#define ANDROID_ASSET_URI     "file:///android_asset/"
#define ANDROID_ASSET_URI_LEN 22

// The game hardcodes its Android data dir in a few places; fix_path() maps it
// onto the save dir.
#define ANDROID_DATA_PREFIX "/data/data/com.robtopx.geometryjump/"
// glibc does not export __sF as a dynamic symbol; provide our own storage.
// The game imports __sF (not stdin/stdout/stderr) and *does* operate on these
// FILEs -- its logging and libc++ write to &__sF[1]/&__sF[2]. Those are not
// glibc FILEs, so handing them to glibc stdio trips its vtable check
// ("invalid stdio handle"); the wrappers below intercept them instead.
// Slots are 0x100 apart so the region covers &__sF[i] at bionic's stride
// (sizeof(bionic FILE) is smaller) without needing to match it exactly.
static uint8_t gdash_sF[3][0x100];

static bool is_fake_file(const void *f)
{
  auto p = (const uint8_t *)f;
  return p >= &gdash_sF[0][0] && p < &gdash_sF[0][0] + sizeof(gdash_sF);
}

// ---------------------------------------------------------------------------
// data shims
// ---------------------------------------------------------------------------

static int noop0() { return 0; }

// bionic's __errno is a function returning a pointer to the thread's errno
// (its `errno` macro is *__errno()), the same shape as glibc's
// __errno_location. Both guests import it as FUNC, so it must be called, not
// read.
static int *errno_gd() { return &errno; }

// ---------------------------------------------------------------------------
// path remapping: the game hardcodes its Android data dir in a few places;
// fix_path() maps it onto the save dir. Safe on any path; returns either the
// input or a rotating buffer.
// ---------------------------------------------------------------------------

static char fixbuf[4][PATH_MAX];
static int fixidx = 0;

// ---------------------------------------------------------------------------
// TLS CA bundle: the game's embedded OpenSSL (1.1.0c) was built on the
// original developer's machine and retained that machine's absolute
// OPENSSLDIR, so its default CA lookup always misses on any other system.
// gdash_nx hits the exact same issue on the Switch -- which has no such path
// at all -- and fixes it by exporting Horizon's own trust store to a file and
// redirecting the hardcoded lookup onto it. On Linux there's usually nothing
// to export: the hardcoded path already matches where a real CA bundle
// normally lives, so just point at whichever one actually exists here.
// ---------------------------------------------------------------------------

static const char *ca_bundle_path() {
  static const char *candidates[] = {
    "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu and derivatives
    "/etc/ssl/cert.pem",                  // Alpine and some embedded distros
    "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL family
    "/etc/ca-certificates/ca-bundle.crt",
  };
  static const char *resolved = nullptr;
  static bool checked = false;
  if (!checked) {
    checked = true;
    for (const char *c : candidates) {
      struct stat st;
      if (stat(c, &st) == 0 && st.st_size > 0) {
        resolved = c;
        break;
      }
    }
    if (resolved)
      printf("[gdash] TLS CA bundle: %s\n", resolved);
    else
      printf("[gdash] TLS CA bundle: none of the usual system paths exist -- "
             "HTTPS certificate verification will stay disabled\n");
  }
  return resolved;
}

static const char *fix_path(const char *path) {
  if (!path)
    return path;

  // The game's embedded OpenSSL fopen()s one of these exact suffixes looking
  // for its build-machine CA bundle; redirect to whichever real one exists on
  // this system (same suffix set gdash_nx matches on the Switch).
  static const char *ca_suffixes[] = {
    "/ssl/cert.pem",
    "/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
  };
  const size_t path_len = strlen(path);
  for (const char *suffix : ca_suffixes) {
    const size_t suffix_len = strlen(suffix);
    if (path_len >= suffix_len &&
        strcmp(path + path_len - suffix_len, suffix) == 0) {
      const char *real = ca_bundle_path();
      return real ? real : path;
    }
  }

  const size_t prefix_len = sizeof(ANDROID_DATA_PREFIX) - 1;
  if (strncmp(path, ANDROID_DATA_PREFIX, prefix_len) != 0)
    return path;
  char *buf = fixbuf[fixidx++ & 3];
  snprintf(buf, PATH_MAX, "%s/%s", "save", path + prefix_len);
  return buf;
}

// ---------------------------------------------------------------------------
// fs (glibc + fix_path on the path-taking ones)
// ---------------------------------------------------------------------------

static int open_gd(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & (O_CREAT | O_TMPFILE)) {
    va_list va;
    va_start(va, flags);
    mode = (mode_t)va_arg(va, int);
    va_end(va);
  }
  return open(fix_path(path), flags, mode);
}

// bionic __open_2 (fortify): no mode argument
static int open2_gd(const char *path, int flags) {
  return open(fix_path(path), flags);
}

static FILE *fopen_gd(const char *path, const char *mode) {
  return fopen(fix_path(path), mode);
}

static int stat_gd(const char *path, struct stat *st) {
  return stat(fix_path(path), st);
}

static int lstat_gd(const char *path, struct stat *st) {
  return lstat(fix_path(path), st);
}

static int access_gd(const char *path, int mode) {
  return access(fix_path(path), mode);
}

static int mkdir_gd(const char *path, unsigned int mode) {
  return mkdir(fix_path(path), mode);
}

static int chmod_gd(const char *path, unsigned int mode) {
  return chmod(fix_path(path), mode);
}

static int rename_gd(const char *from, const char *to) {
  return rename(fix_path(from), fix_path(to));
}

static int remove_gd(const char *path) {
  return remove(fix_path(path));
}

static DIR *opendir_gd(const char *path) {
  return opendir(fix_path(path));
}

// ---------------------------------------------------------------------------
// setjmp/longjmp registry
//
// The game's binaries are bionic-compiled: their jmp_buf is 256 bytes, while
// glibc's (used by the symtable_libc THUNK_DIRECT(setjmp)) is larger. Running
// glibc's setjmp on a bionic-sized buffer overflows it, so both ends of the
// pair are funneled through our own storage, keyed by the game's buffer
// address (which is otherwise opaque). sigsetjmp/siglongjmp use the same
// storage: glibc's setjmp and sigsetjmp write the same layout.
// ---------------------------------------------------------------------------

static std::mutex g_jmp_mtx;
struct JmpEntry { sigjmp_buf buf; };
static std::map<void *, JmpEntry> g_jmp_map;

static sigjmp_buf *jmp_lookup(void *b) {
  std::lock_guard<std::mutex> lk(g_jmp_mtx);
  return &g_jmp_map[b].buf;
}

static sigjmp_buf *jmp_find(void *b) {
  std::lock_guard<std::mutex> lk(g_jmp_mtx);
  auto it = g_jmp_map.find(b);
  return (it != g_jmp_map.end()) ? &it->second.buf : nullptr;
}

static int setjmp_gd(void *b) {
  sigjmp_buf *buf = jmp_lookup(b);
  return sigsetjmp(*buf, 0);
}

static int sigsetjmp_gd(void *b, int savemask) {
  sigjmp_buf *buf = jmp_lookup(b);
  return sigsetjmp(*buf, savemask);
}

static void longjmp_gd(void *b, int val) {
  sigjmp_buf *buf = jmp_find(b);
  if (!buf) {
    fatal_error("longjmp with no matching setjmp (b=%p)", b);
  }
  siglongjmp(*buf, val);
}

static void siglongjmp_gd(void *b, int val) {
  longjmp_gd(b, val);
}

// ---------------------------------------------------------------------------
// bionic C++ ABI / misc shims missing from the libc thunks
// ---------------------------------------------------------------------------

static pthread_mutex_t cxa_guard_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

// Itanium C++ ABI guard: acquire returns 1 with the lock HELD, the caller
// runs the static initializer, then release/abort unlock. Holding the lock
// across the initializer is what serializes concurrent first-use from
// multiple threads (e.g. FMOD's mixer thread racing the main thread when a
// level starts); releasing early double-initializes and corrupts the mutex.
// Recursive, because a static initializer may itself initialize another
// guarded static.
static int __cxa_guard_acquire_gd(uint64_t *guard) {
    pthread_mutex_lock(&cxa_guard_lock);
    if (*(uint8_t *)guard) {
        pthread_mutex_unlock(&cxa_guard_lock);
        return 0; // already initialized
    }
    return 1; // lock held; caller runs the initializer, then calls release
}

static void __cxa_guard_release_gd(uint64_t *guard) {
    *(uint8_t *)guard = 1;
    pthread_mutex_unlock(&cxa_guard_lock);
}

static void __cxa_guard_abort_gd(uint64_t *guard) {
    (void)guard;
    pthread_mutex_unlock(&cxa_guard_lock);
}

static void __cxa_pure_virtual_gd(void) {
  fatal_error("Pure virtual function called!");
}

static void operator_delete_gd(void *p) {
  free(p);
}

static void __assert2_gd(const char *file, int line, const char *func, const char *expr) {
  fatal_error("Assertion failed: %s at %s:%d (%s)", expr, file, line, func);
}

static int __signbit_gd(double x) {
  return std::signbit(x) ? 1 : 0;
}

// bionic semantics for __fpclassifyd: 0 normal, 1 zero, 2 subnormal, 3 inf, 4 nan
static int __fpclassifyd_gd(double d) {
  switch (std::fpclassify(d)) {
    case FP_NAN:       return 4;
    case FP_INFINITE:  return 3;
    case FP_SUBNORMAL: return 2;
    case FP_ZERO:      return 1;
    default:           return 0;
  }
}

static int android_set_abort_message_gd(const char *msg) {
  (void)msg;
  return 0;
}

// ---------------------------------------------------------------------------
// FMOD createSound/createStream: rewrite "file:///android_asset/<rel>" to the
// loose assets dir (and the hardcoded Android data dir onto the save dir);
// everything else passes through. The import table binds the game's calls to
// these wrappers, which tail into the real libfmod exports.
// ---------------------------------------------------------------------------

#define FMOD_OPENMEMORY        0x00000800
#define FMOD_OPENMEMORY_POINT  0x10000000

typedef int (*FmodCreateFn)(void *sys, const char *name, unsigned mode, void *exinfo, void **out);
static FmodCreateFn real_fmod_createSound;
static FmodCreateFn real_fmod_createStream;

static const char *fmod_fix_path(const char *name, unsigned mode, char *buf, size_t buflen) {
  if (!name || (mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT)))
    return name;
  if (strncmp(name, ANDROID_ASSET_URI, ANDROID_ASSET_URI_LEN) == 0) {
    snprintf(buf, buflen, "%s/%s", "assets", name + ANDROID_ASSET_URI_LEN);
    return buf;
  }
  if (strncmp(name, ANDROID_DATA_PREFIX, sizeof(ANDROID_DATA_PREFIX) - 1) == 0) {
    snprintf(buf, buflen, "%s/%s", "save", name + sizeof(ANDROID_DATA_PREFIX) - 1);
    return buf;
  }
  return name;
}

static int fmod_createSound_hook(void *sys, const char *name, unsigned mode, void *exinfo, void **out) {
  char buf[1024];
  return real_fmod_createSound(sys, fmod_fix_path(name, mode, buf, sizeof(buf)), mode, exinfo, out);
}

static int fmod_createStream_hook(void *sys, const char *name, unsigned mode, void *exinfo, void **out) {
  char buf[1024];
  return real_fmod_createStream(sys, fmod_fix_path(name, mode, buf, sizeof(buf)), mode, exinfo, out);
}

#define FMOD_SYM_CREATESOUND  "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"
#define FMOD_SYM_CREATESTREAM "_ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE"

void fmod_hooks_init(so_module *fmod) {
  real_fmod_createSound = (FmodCreateFn)so_symbol(fmod, FMOD_SYM_CREATESOUND);
  real_fmod_createStream = (FmodCreateFn)so_symbol(fmod, FMOD_SYM_CREATESTREAM);
}

// ---------------------------------------------------------------------------
// OpenSL ES: Bogodroid's dlopen() reports libOpenSLES.so as loaded (0xDEAD),
// so libfmod.so dlsym()s these and calls them. Failing the engine creation
// makes FMOD fall back to its AudioTrack output (org.fmod.AudioDevice).
// ---------------------------------------------------------------------------

#define SL_RESULT_FEATURE_UNSUPPORTED 12

static uint32_t slCreateEngine_gd(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                                  uint32_t numInterfaces, const void *pInterfaceIds,
                                  const uint8_t *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pEngine)
    *pEngine = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// stdio over the fake __sF: forward writes to our own stdout so the game's
// logging stays visible, and report reads as EOF. Everything else is a real
// glibc FILE and passes straight through.
// ---------------------------------------------------------------------------

static size_t fwrite_gd(const void *p, size_t sz, size_t n, FILE *f) {
  if (is_fake_file(f))
    return fwrite(p, sz, n, stdout);
  return fwrite(p, sz, n, f);
}

static size_t fread_gd(void *p, size_t sz, size_t n, FILE *f) {
  return is_fake_file(f) ? 0 : fread(p, sz, n, f);
}

static int fputc_gd(int c, FILE *f) {
  return fputc(c, is_fake_file(f) ? stdout : f);
}

static int putc_gd(int c, FILE *f) {
  return fputc(c, is_fake_file(f) ? stdout : f);
}

static int fputs_gd(const char *s, FILE *f) {
  return fputs(s, is_fake_file(f) ? stdout : f);
}

static int fprintf_gd(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = vfprintf(is_fake_file(f) ? stdout : f, fmt, va);
  va_end(va);
  return r;
}

static int vfprintf_gd(FILE *f, const char *fmt, va_list va) {
  return vfprintf(is_fake_file(f) ? stdout : f, fmt, va);
}

static int fflush_gd(FILE *f) {
  if (!f)
    return fflush(NULL);
  return fflush(is_fake_file(f) ? stdout : f);
}

static int fclose_gd(FILE *f) {
  return is_fake_file(f) ? 0 : fclose(f);
}

static int feof_gd(FILE *f) {
  return is_fake_file(f) ? 1 : feof(f);
}

static int ferror_gd(FILE *f) {
  return is_fake_file(f) ? 0 : ferror(f);
}

static int fileno_gd(FILE *f) {
  if (is_fake_file(f))
    return (int)(((const uint8_t *)f - &gdash_sF[0][0]) / 0x100);
  return fileno(f);
}

static char *fgets_gd(char *s, int n, FILE *f) {
  return is_fake_file(f) ? NULL : fgets(s, n, f);
}

static int getc_gd(FILE *f) {
  return is_fake_file(f) ? EOF : getc(f);
}

static int ungetc_gd(int c, FILE *f) {
  return is_fake_file(f) ? EOF : ungetc(c, f);
}

static wint_t getwc_gd(FILE *f) {
  return is_fake_file(f) ? WEOF : getwc(f);
}

static wint_t putwc_gd(wchar_t c, FILE *f) {
  return is_fake_file(f) ? (wint_t)c : putwc(c, f);
}

static wint_t ungetwc_gd(wint_t c, FILE *f) {
  return is_fake_file(f) ? WEOF : ungetwc(c, f);
}

static int fseek_gd(FILE *f, long off, int whence) {
  return is_fake_file(f) ? -1 : fseek(f, off, whence);
}

static int fseeko_gd(FILE *f, off_t off, int whence) {
  return is_fake_file(f) ? -1 : fseeko(f, off, whence);
}

static long ftell_gd(FILE *f) {
  return is_fake_file(f) ? -1 : ftell(f);
}

static off_t ftello_gd(FILE *f) {
  return is_fake_file(f) ? -1 : ftello(f);
}

static void setbuf_gd(FILE *f, char *buf) {
  if (!is_fake_file(f))
    setbuf(f, buf);
}

static int setvbuf_gd(FILE *f, char *buf, int mode, size_t size) {
  return is_fake_file(f) ? 0 : setvbuf(f, buf, mode, size);
}

// ---------------------------------------------------------------------------
// network: the game retries its (now dead) server forever when connections
// hang -- the main thread spins through DNS/TLS/retry for minutes. Fail every
// socket operation instantly, like the Switch port's offline mode (net_up=0):
// the game handles connection errors gracefully and plays fully offline.
// ---------------------------------------------------------------------------

static int socket_none(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    errno = EBADF;
    return -1;
}

static int connect_none(int fd, const void *addr, unsigned addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    errno = ECONNREFUSED;
    return -1;
}

static long send_none(int fd, const void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    errno = EBADF;
    return -1;
}

static long recv_none(int fd, void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)len; (void)flags;
    errno = EBADF;
    return -1;
}

static long sendto_none(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
    errno = EBADF;
    return -1;
}

static long recvfrom_none(int fd, void *buf, size_t len, int flags, void *addr, unsigned *addrlen) {
    (void)fd; (void)buf; (void)len; (void)flags; (void)addr; (void)addrlen;
    errno = EBADF;
    return -1;
}

static int accept_none(int fd, void *addr, unsigned *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    errno = EBADF;
    return -1;
}

static int listen_none(int fd, int backlog) {
    (void)fd; (void)backlog;
    errno = EBADF;
    return -1;
}

static int shutdown_none(int fd, int how) {
    (void)fd; (void)how;
    errno = EBADF;
    return -1;
}

static int getaddrinfo_none(const char *node, const char *service, const void *hints, void **res) {
    (void)node; (void)service; (void)hints;
    if (res)
        *res = NULL;
    return EAI_FAIL;
}

static void freeaddrinfo_none(void *res) {
    (void)res;
}

static void *gethostbyname_none(const char *name) {
    (void)name;
    return NULL;
}

// The remaining entry points are not reachable after socket()/getaddrinfo()
// fail, but they must NOT fall through to glibc: gethostbyname_r would do a
// real DNS lookup (with the resolver's retry/timeout behaviour -- exactly the
// "spins through DNS for minutes" hang the stubs exist to prevent), and a
// real socketpair() would hand back fds that select()/poll() would then wait
// on forever. Fail them all the same way.
static int gethostbyname_r_none(const char *name, void *ret, char *buf, size_t buflen,
                                void **result, int *h_errnop) {
    (void)name; (void)ret; (void)buf; (void)buflen;
    if (result)
        *result = NULL;
    if (h_errnop)
        *h_errnop = 1; // HOST_NOT_FOUND
    errno = EBADF;
    return -1;
}

static int getnameinfo_none(const void *sa, unsigned salen, char *host, unsigned hostlen,
                            char *serv, unsigned servlen, int flags) {
    (void)sa; (void)salen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags;
    return EAI_FAIL;
}

static int socketpair_none(int domain, int type, int protocol, int sv[2]) {
    (void)domain; (void)type; (void)protocol; (void)sv;
    errno = EBADF;
    return -1;
}

static int bind_none(int fd, const void *addr, unsigned addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    errno = EBADF;
    return -1;
}

static int getsockname_none(int fd, void *addr, unsigned *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    errno = EBADF;
    return -1;
}

static int getpeername_none(int fd, void *addr, unsigned *addrlen) {
    (void)fd; (void)addr; (void)addrlen;
    errno = EBADF;
    return -1;
}

// ---------------------------------------------------------------------------
// network on/off switch
//
// glibc IS the bionic socket ABI on this platform (same sockaddr layout, same
// errno values, same fd table), and symtable_libc already THUNK_DIRECTs the
// whole socket family to glibc -- the _none stubs above only exist to shadow
// it. This is the Bogodroid equivalent of the Switch port's net_shim.c: real
// sockets end to end, minus the bionic<->BSD conversion. The table is walked
// before either game lib is loaded, so relocation binds whichever side is
// selected for every socket import.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// connect with a bounded timeout
//
// To a dead server the kernel's TCP connect keeps re-sending SYNs for ~2
// minutes (tcp_syn_retries) -- exactly the "spins for minutes" stall the
// offline stubs existed to prevent. When networking is enabled the connect
// import is bound to this wrapper instead of plain glibc: the socket goes
// non-blocking, connect() is started, and poll() bounds the wait to
// connect_timeout_ms ([network] connect_timeout_ms in the config). A dead
// server then fails in seconds, the game reports "server offline" and plays
// on; a live one connects normally with the socket returned to blocking mode
// either way.
// ---------------------------------------------------------------------------

static int g_connect_timeout_ms = 4000;

static int socket_gd(int domain, int type, int protocol) {
    // Devices whose kernel has no IPv6 at all (no /proc/sys/net/ipv6 -- not
    // disabled, never built in) fail a real AF_INET6 socket() with
    // EAFNOSUPPORT, and the game treats that capability probe as fatal for
    // the request rather than falling back to IPv4. Hand back an AF_INET
    // socket so the probe succeeds; getaddrinfo_gd() already guarantees no
    // IPv6 address ever reaches curl, so nothing tries to connect it.
    if (domain == AF_INET6)
        domain = AF_INET;
    int fd = socket(domain, type, protocol);
    if (fd < 0)
        printf("[gdash] socket(domain=%d, type=%d) failed: %s\n", domain, type, strerror(errno));
    return fd;
}

// glibc lays out struct addrinfo as {..., ai_addr, ai_canonname, ai_next};
// bionic, like the BSD headers it derives from, uses {..., ai_canonname,
// ai_addr, ai_next}. Identical size, identical offsets for every other field
// -- only those two pointers trade places, so one in-place swap converts a
// chain between the two ABIs (and the swap is its own inverse).
//
// The game's curl was compiled against bionic, so it reads a raw glibc result
// with those two fields transposed: it takes ai_canonname (NULL, since we
// never pass AI_CANONNAME) as the address. Every resolved entry therefore
// looks addressless, curl finds nothing to connect to, and abandons the
// request without ever calling socket() or connect() -- the exact "DNS
// resolves fine, nothing ever goes out" symptom. gdash_nx never hit this
// because libnx/newlib is BSD-ordered too, matching bionic.
static void addrinfo_bionic_swap(struct addrinfo *ai) {
    for (; ai; ai = ai->ai_next) {
        char *canonname = ai->ai_canonname;
        ai->ai_canonname = (char *)ai->ai_addr;
        ai->ai_addr = (struct sockaddr *)canonname;
    }
}

// glibc's freeaddrinfo must see its own layout again, so undo the swap first.
static void freeaddrinfo_gd(struct addrinfo *res) {
    if (!res)
        return;
    addrinfo_bionic_swap(res);
    freeaddrinfo(res);
}

static int getaddrinfo_gd(const char *node, const char *service, const struct addrinfo *hints,
                           struct addrinfo **res) {
    // Force IPv4-only: some of these devices have no IPv6 in the kernel at
    // all, and an IPv6 result the game then can't create a socket for stalls
    // the request (see socket_gd).
    struct addrinfo forced_hints;
    if (hints && hints->ai_family == AF_UNSPEC) {
        forced_hints = *hints;
        forced_hints.ai_family = AF_INET;
        hints = &forced_hints;
    } else if (!hints) {
        memset(&forced_hints, 0, sizeof(forced_hints));
        forced_hints.ai_family = AF_INET;
        hints = &forced_hints;
    }

    // Retry transient resolver failures. On wifi these devices intermittently
    // lose a DNS query and glibc reports EAI_NONAME/EAI_AGAIN for a host that
    // resolves fine moments later; the game turns that single miss into a
    // visible "download failed" even though it would succeed on its own retry.
    // Only failures pay the delay, and only for the two retryable codes.
    int r = getaddrinfo(node, service, hints, res);
    for (int attempt = 0; r != 0 && attempt < 2 && (r == EAI_NONAME || r == EAI_AGAIN); attempt++) {
        usleep(150 * 1000);
        r = getaddrinfo(node, service, hints, res);
    }

    if (r != 0)
        printf("[gdash] getaddrinfo(%s:%s) failed: %s\n",
            node ? node : "(null)", service ? service : "(null)", gai_strerror(r));
    else if (res && *res)
        addrinfo_bionic_swap(*res); // hand the game a bionic-shaped chain
    return r;
}

static int connect_gd(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && !(flags & O_NONBLOCK))
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, addr, addrlen);
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        r = poll(&pfd, 1, g_connect_timeout_ms);
        if (r > 0) {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0)
                r = 0; // connected
            else {
                errno = soerr ? soerr : ETIMEDOUT;
                r = -1;
            }
        } else if (r == 0) {
            errno = ETIMEDOUT;
            r = -1;
        }
    }
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags); // restore blocking mode either way
    if (r != 0) {
        char dest[64] = "?";
        if (addr && addr->sa_family == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
            snprintf(dest, sizeof(dest), "%s:%u", ip, ntohs(in->sin_port));
        }
        printf("[gdash] connect(%s) failed: %s\n", dest, strerror(errno));
    }
    return r;
}

struct NetSym { const char *symbol; uintptr_t stub; uintptr_t real; };

static const NetSym g_net_syms[] = {
    { "socket",          (uintptr_t)&socket_none,          (uintptr_t)&socket_gd },
    { "connect",         (uintptr_t)&connect_none,         (uintptr_t)&connect_gd },
    { "send",            (uintptr_t)&send_none,            (uintptr_t)&send },
    { "recv",            (uintptr_t)&recv_none,            (uintptr_t)&recv },
    { "sendto",          (uintptr_t)&sendto_none,          (uintptr_t)&sendto },
    { "recvfrom",        (uintptr_t)&recvfrom_none,        (uintptr_t)&recvfrom },
    { "accept",          (uintptr_t)&accept_none,          (uintptr_t)&accept },
    { "listen",          (uintptr_t)&listen_none,          (uintptr_t)&listen },
    { "shutdown",        (uintptr_t)&shutdown_none,        (uintptr_t)&shutdown },
    { "bind",            (uintptr_t)&bind_none,            (uintptr_t)&bind },
    { "getpeername",     (uintptr_t)&getpeername_none,     (uintptr_t)&getpeername },
    { "getsockname",     (uintptr_t)&getsockname_none,     (uintptr_t)&getsockname },
    { "socketpair",      (uintptr_t)&socketpair_none,      (uintptr_t)&socketpair },
    { "getaddrinfo",     (uintptr_t)&getaddrinfo_none,     (uintptr_t)&getaddrinfo_gd },
    { "freeaddrinfo",    (uintptr_t)&freeaddrinfo_none,    (uintptr_t)&freeaddrinfo_gd },
    { "getnameinfo",     (uintptr_t)&getnameinfo_none,     (uintptr_t)&getnameinfo },
    { "gethostbyname",   (uintptr_t)&gethostbyname_none,   (uintptr_t)&gethostbyname },
    { "gethostbyname_r", (uintptr_t)&gethostbyname_r_none, (uintptr_t)&gethostbyname_r },
};

void gdash_network_init(bool enabled, int connect_timeout_ms) {
    if (connect_timeout_ms > 0)
        g_connect_timeout_ms = connect_timeout_ms;
    for (size_t i = 0; symtable_gdash[i].symbol; i++) {
        for (size_t j = 0; j < sizeof(g_net_syms) / sizeof(g_net_syms[0]); j++) {
            if (strcmp(symtable_gdash[i].symbol, g_net_syms[j].symbol) == 0) {
                symtable_gdash[i].func = enabled ? g_net_syms[j].real : g_net_syms[j].stub;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction symtable_gdash[] = {
  // --- bionic data symbols ------------------------------------------------
  { "__errno", (uintptr_t)&errno_gd },

  // --- stdio: glibc end to end (fix_path on fopen). This table is searched
  // before symtable_libc, so FILE* never mixes bionic emulation and glibc.
  // The FILE*-taking entries route through the __sF wrappers above.
  { "__sF", (uintptr_t)&gdash_sF[0] },
  { "stdin", (uintptr_t)&stdin },
  { "stdout", (uintptr_t)&stdout },
  { "stderr", (uintptr_t)&stderr },
  { "fopen", (uintptr_t)&fopen_gd },
  { "fclose", (uintptr_t)&fclose_gd },
  { "fdopen", (uintptr_t)&fdopen },
  { "feof", (uintptr_t)&feof_gd },
  { "ferror", (uintptr_t)&ferror_gd },
  { "fflush", (uintptr_t)&fflush_gd },
  { "fgets", (uintptr_t)&fgets_gd },
  { "fileno", (uintptr_t)&fileno_gd },
  { "fprintf", (uintptr_t)&fprintf_gd },
  { "fputc", (uintptr_t)&fputc_gd },
  { "fputs", (uintptr_t)&fputs_gd },
  { "fread", (uintptr_t)&fread_gd },
  { "fseek", (uintptr_t)&fseek_gd },
  { "fseeko", (uintptr_t)&fseeko_gd },
  { "ftell", (uintptr_t)&ftell_gd },
  { "ftello", (uintptr_t)&ftello_gd },
  { "fwrite", (uintptr_t)&fwrite_gd },
  { "getc", (uintptr_t)&getc_gd },
  { "getwc", (uintptr_t)&getwc_gd },
  { "printf", (uintptr_t)&printf },
  { "putc", (uintptr_t)&putc_gd },
  { "putwc", (uintptr_t)&putwc_gd },
  { "setbuf", (uintptr_t)&setbuf_gd },
  { "setvbuf", (uintptr_t)&setvbuf_gd },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "swprintf", (uintptr_t)&swprintf },
  { "ungetc", (uintptr_t)&ungetc_gd },
  { "ungetwc", (uintptr_t)&ungetwc_gd },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_gd },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vsscanf", (uintptr_t)&vsscanf },

  // --- fs (fix_path where paths are involved) ------------------------------
  { "access", (uintptr_t)&access_gd },
  { "chmod", (uintptr_t)&chmod_gd },
  { "mkdir", (uintptr_t)&mkdir_gd },
  { "open", (uintptr_t)&open_gd },
  { "__open_2", (uintptr_t)&open2_gd },
  { "opendir", (uintptr_t)&opendir_gd },
  { "remove", (uintptr_t)&remove_gd },
  { "rename", (uintptr_t)&rename_gd },
  { "stat", (uintptr_t)&stat_gd },
  { "lstat", (uintptr_t)&lstat_gd },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir },
  { "readdir64", (uintptr_t)&readdir64 },

  // --- setjmp family (registry; see above) ---------------------------------
  { "setjmp", (uintptr_t)&setjmp_gd },
  { "longjmp", (uintptr_t)&longjmp_gd },
  { "sigsetjmp", (uintptr_t)&sigsetjmp_gd },
  { "siglongjmp", (uintptr_t)&siglongjmp_gd },

  // --- bionic C++ ABI / misc (missing from symtable_libc) -------------------
  { "_ZdlPv", (uintptr_t)&operator_delete_gd },
  { "__assert2", (uintptr_t)&__assert2_gd },
  { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire_gd },
  { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release_gd },
  { "__cxa_guard_abort", (uintptr_t)&__cxa_guard_abort_gd },
  { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual_gd },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&noop0 },
  { "__google_potentially_blocking_region_end", (uintptr_t)&noop0 },
  { "__signbit", (uintptr_t)&__signbit_gd },
  { "__fpclassifyd", (uintptr_t)&__fpclassifyd_gd },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_gd },
  { "if_nametoindex", (uintptr_t)&if_nametoindex },

  // --- math that lives in libm rather than libc.so.6 ------------------------
  { "asinh", (uintptr_t)&asinh },
  { "asinhf", (uintptr_t)&asinhf },
  { "tanhf", (uintptr_t)&tanhf },
  { "fmax", (uintptr_t)&fmax },
  { "fmin", (uintptr_t)&fmin },
  { "frexp", (uintptr_t)&frexp },
  { "lround", (uintptr_t)&lround },

  // --- time / process / signals / locale the libc table doesn't emit -------
  { "clock", (uintptr_t)&clock },
  { "getpid", (uintptr_t)&getpid },
  { "fork", (uintptr_t)&fork },
  { "execl", (uintptr_t)&execl },
  { "waitpid", (uintptr_t)&waitpid },
  { "kill", (uintptr_t)&kill },
  { "alarm", (uintptr_t)&alarm },
  { "signal", (uintptr_t)&signal },
  { "sigaction", (uintptr_t)&sigaction },
  { "sigprocmask", (uintptr_t)&sigprocmask },
  { "sigemptyset", (uintptr_t)&sigemptyset },
  { "sigfillset", (uintptr_t)&sigfillset },
  { "sigaddset", (uintptr_t)&sigaddset },
  { "sigdelset", (uintptr_t)&sigdelset },
  { "getuid", (uintptr_t)&getuid },
  { "geteuid", (uintptr_t)&geteuid },
  { "getgid", (uintptr_t)&getgid },
  { "getegid", (uintptr_t)&getegid },
  { "setuid", (uintptr_t)&setuid },
  { "setgid", (uintptr_t)&setgid },
  { "umask", (uintptr_t)&umask },
  { "initgroups", (uintptr_t)&initgroups },
  { "getpwuid", (uintptr_t)&getpwuid },
  { "getpwuid_r", (uintptr_t)&getpwuid_r },
  { "tcgetattr", (uintptr_t)&tcgetattr },
  { "tcsetattr", (uintptr_t)&tcsetattr },
  { "setpriority", (uintptr_t)&setpriority },
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale },
  { "freelocale", (uintptr_t)&freelocale },
  { "uselocale", (uintptr_t)&uselocale },
  { "strtold_l", (uintptr_t)&strtold_l },
  { "strtoll_l", (uintptr_t)&strtoll_l },
  { "strtoull_l", (uintptr_t)&strtoull_l },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },

  // --- FMOD path rewrite (everything else FMOD resolves cross-module) -------
  { FMOD_SYM_CREATESOUND, (uintptr_t)&fmod_createSound_hook },
  { FMOD_SYM_CREATESTREAM, (uintptr_t)&fmod_createStream_hook },

  // --- network (fail instantly; the game plays offline) ----------------------
  { "socket", (uintptr_t)&socket_none },
  { "connect", (uintptr_t)&connect_none },
  { "send", (uintptr_t)&send_none },
  { "recv", (uintptr_t)&recv_none },
  { "sendto", (uintptr_t)&sendto_none },
  { "recvfrom", (uintptr_t)&recvfrom_none },
  { "accept", (uintptr_t)&accept_none },
  { "listen", (uintptr_t)&listen_none },
  { "shutdown", (uintptr_t)&shutdown_none },
  { "bind", (uintptr_t)&bind_none },
  { "getpeername", (uintptr_t)&getpeername_none },
  { "getsockname", (uintptr_t)&getsockname_none },
  { "socketpair", (uintptr_t)&socketpair_none },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_none },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_none },
  { "getnameinfo", (uintptr_t)&getnameinfo_none },
  { "gethostbyname", (uintptr_t)&gethostbyname_none },
  { "gethostbyname_r", (uintptr_t)&gethostbyname_r_none },

  // --- OpenSL ES (libfmod probes it; fail gracefully -> AudioTrack) ---------
  { "slCreateEngine", (uintptr_t)&slCreateEngine_gd },

  { NULL, (uintptr_t)NULL }
};

// ---------------------------------------------------------------------------
// server compat: ported from gdash_nx's game_compat.c. These patches target
// the game's own compiled machine code in libcocos2dcpp.so -- the same
// original Android build on both ports, since neither recompiles it -- so
// the same byte patterns apply here unmodified. Each is independently
// no-op-safe if its pattern isn't found (logs and moves on, same principle
// as the Limbo backbuffer-cap patch).
// ---------------------------------------------------------------------------

static int hex_digit_value(unsigned char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

// Geometry Dash 2.2.147's ZipUtils::hexToChar uses a C++ stringstream for
// each percent-escaped byte in online-level song metadata. Its cached
// std::ctype table isn't compatible with this runtime and crashes -- a
// general old-GNU-string-ABI-vs-runtime-std::ctype mismatch, not anything
// Switch-specific. The affected build uses the old GNU std::string ABI: a
// const reference points to an object whose first word is the character
// data pointer.
static unsigned char hex_to_char_compat(const void *string_ref) {
  if (!string_ref)
    return 0;
  const unsigned char *data = nullptr;
  memcpy(&data, string_ref, sizeof(data));
  if (!data)
    return 0;
  const int high = hex_digit_value(data[0]);
  if (high < 0)
    return 0;
  const int low = hex_digit_value(data[1]);
  if (low < 0)
    return (unsigned char)high;
  return (unsigned char)((high << 4) | low);
}

static void patch_url_decoder(so_module *mod) {
  uintptr_t address = so_symbol(mod, "_ZN7cocos2d8ZipUtils9hexToCharERKSs");
  if (!address) {
    printf("[gdash] hexToChar symbol not found -- online-level metadata "
           "parsing may crash on percent-escaped bytes\n");
    return;
  }
  hook_address(mod, address, (uintptr_t)&hex_to_char_compat);
  printf("[gdash] patched ZipUtils::hexToChar (online-level metadata crash fix)\n");
}

// On a fresh/anonymous profile the stock client tries updateUserScore()
// before its first getGJScores20 request. Boomlings rejects that upload
// without an account, and the failure callback never starts the public
// leaderboard download. Skip only that decision branch; the existing path
// immediately below still performs the normal cached/network lookup.
static void patch_leaderboard_bootstrap(so_module *mod) {
  uintptr_t address = so_symbol(mod,
      "_ZN17LeaderboardsLayer17selectLeaderboardE15LeaderboardType15LeaderboardStat");
  if (!address) {
    printf("[gdash] LeaderboardsLayer::selectLeaderboard symbol not found -- "
           "skipping fresh-profile leaderboard fix\n");
    return;
  }

  uint32_t *code = (uint32_t *)address;
  const uint32_t ldrb_score_uploaded = 0x3948a000u; // ldrb w0, [x0, #552]
  const uint32_t cbz_w0_mask = 0x7f00001fu;
  const uint32_t cbz_w0 = 0x34000000u;
  const uint32_t nop = 0xd503201fu;
  for (unsigned i = 0; i + 1 < 96; i++) {
    if (code[i] != ldrb_score_uploaded ||
        (code[i + 1] & cbz_w0_mask) != cbz_w0)
      continue;
    code[i + 1] = nop;
    printf("[gdash] patched public-leaderboard bootstrap (fresh-profile fix)\n");
    return;
  }
  printf("[gdash] leaderboard-bootstrap patch site not found -- fresh profiles "
         "may not see public leaderboards until they submit a score\n");
}

static int is_bl_gd(uint32_t instruction) {
  return (instruction & 0xfc000000u) == 0x94000000u;
}

static uintptr_t bl_target_gd(uintptr_t pc, uint32_t instruction) {
  int64_t immediate = instruction & 0x03ffffffu;
  if (immediate & 0x02000000)
    immediate -= 0x04000000;
  return (uintptr_t)((int64_t)pc + immediate * 4);
}

// CCHttpClient sets libcurl options 64 (SSL_VERIFYPEER) and 81
// (SSL_VERIFYHOST) to zero in every supported GD variant's arm64 build, and
// never sets CURLOPT_CAINFO -- meaning HTTPS requests already work (Boomlings
// doesn't require client certs), just without verifying the server's
// certificate at all. This replaces that with real verification pointed at a
// real CA bundle: matches the complete option sequence and shared
// curl_easy_setopt call target (both must resolve to the very same function)
// to stay constrained to this specific routine, then replaces the
// nonessential TCP-keepalive/DNS-cache-timeout setters with one CAINFO
// setter -- an 8-instruction slot, using a literal load so the CA path can
// live anywhere in the module's address space, with a branch that jumps over
// the embedded 64-bit pointer into the original epilogue. Ported as-is from
// gdash_nx's game_compat.c, which validated this exact byte sequence across
// every supported GD variant.
static void patch_curl_tls_verify(so_module *mod) {
  const char *ca_path_str = ca_bundle_path();
  if (!ca_path_str)
    return; // no valid system CA bundle; leave verification off rather than
            // point curl at a path that would just fail every request

  const uint32_t mov_x0_x19 = 0xaa1303e0u;
  const uint32_t mov_w1_verify_peer = 0x52800801u; // mov w1, #64
  const uint32_t mov_w1_verify_host = 0x52800a21u; // mov w1, #81
  const uint32_t mov_w1_tcp_keepalive = 0x52801aa1u; // mov w1, #213
  const uint32_t mov_w1_dns_cache_timeout = 0x52800b81u; // mov w1, #92
  const uint32_t mov_w1_cainfo = 0x5284ea21u; // mov w1, #10065
  const uint32_t mov_x2_zero = 0xd2800002u;
  const uint32_t mov_x2_one = 0xd2800022u;
  const uint32_t mov_x2_two = 0xd2800042u;
  const uint32_t mov_x2_sixty = 0xd2800782u;
  const uint32_t ldr_x2_literal_16 = 0x58000082u; // ldr x2, PC + 16
  const uint32_t branch_forward_16 = 0x14000004u; // b PC + 16
  const uint32_t nop = 0xd503201fu;
  const uintptr_t ca_path = (uintptr_t)ca_path_str;
  int patched = 0;

  // Scan every executable PT_LOAD segment. Don't take so_module's
  // text_base/text_size as a shortcut for "the .text segment" -- those track
  // this loader's own patch/code-cave arena (see the struct comment in
  // so_util.h), not the guest's code.
  for (int seg = 0; seg < mod->ehdr->e_phnum; seg++) {
    const Elf_Phdr *p = &mod->phdr[seg];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || p->p_filesz < 24)
      continue;
    // p_vaddr is already the absolute runtime address here (so_load()
    // normalizes it; segment 0's p_vaddr == mod->base), not the file-relative
    // offset most PIE ELFs use. Adding mod->base again lands at 2x base, in
    // unmapped memory.
    uint32_t *code = (uint32_t *)(uintptr_t)p->p_vaddr;
    // Guard in case that ever stops holding: skip rather than deref a wild
    // pointer.
    if ((uintptr_t)code < mod->base || (uintptr_t)code > mod->base + 0x10000000) {
      printf("[gdash] TLS patch: segment %d code=%p looks wrong "
             "(mod->base=%p) -- skipping\n", seg, (void *)code, (void *)mod->base);
      continue;
    }
    const size_t words = p->p_filesz / sizeof(*code);
    for (size_t i = 0; i + 5 < words; i++) {
      if (code[i] != mov_w1_verify_peer || code[i + 1] != mov_x2_zero ||
          !is_bl_gd(code[i + 2]))
        continue;
      const uintptr_t peer_target =
          bl_target_gd((uintptr_t)&code[i + 2], code[i + 2]);
      const size_t limit = i + 12 < words ? i + 12 : words - 2;
      for (size_t j = i + 3; j < limit; j++) {
        if (code[j] != mov_w1_verify_host || code[j + 1] != mov_x2_zero ||
            !is_bl_gd(code[j + 2]))
          continue;
        if (bl_target_gd((uintptr_t)&code[j + 2], code[j + 2]) != peer_target)
          continue;

        for (size_t k = j + 3; k < j + 24 && k + 7 < words; k++) {
          if (code[k] != mov_x0_x19 ||
              code[k + 1] != mov_w1_tcp_keepalive ||
              code[k + 2] != mov_x2_one || !is_bl_gd(code[k + 3]) ||
              code[k + 4] != mov_x0_x19 ||
              code[k + 5] != mov_w1_dns_cache_timeout ||
              code[k + 6] != mov_x2_sixty || !is_bl_gd(code[k + 7]))
            continue;
          if (bl_target_gd((uintptr_t)&code[k + 3], code[k + 3]) != peer_target ||
              bl_target_gd((uintptr_t)&code[k + 7], code[k + 7]) != peer_target)
            continue;

          code[i + 1] = mov_x2_one;
          code[j + 1] = mov_x2_two;
          code[k + 1] = mov_w1_cainfo;
          code[k + 2] = ldr_x2_literal_16;
          // code[k + 3] remains the original curl_easy_setopt BL.
          code[k + 4] = branch_forward_16;
          code[k + 5] = nop;
          memcpy(&code[k + 6], &ca_path, sizeof(ca_path));
          patched++;
          break;
        }
        break;
      }
    }
  }

  if (patched)
    printf("[gdash] enabled TLS certificate verification (%d site%s), CAINFO=%s\n",
        patched, patched == 1 ? "" : "s", ca_path_str);
  else
    printf("[gdash] TLS verification patch site not found -- HTTPS requests "
           "will work but without certificate verification\n");
}

void gdash_server_compat_init(so_module *game_mod) {
  patch_url_decoder(game_mod);
  patch_leaderboard_bootstrap(game_mod);
  patch_curl_tls_verify(game_mod);
}
