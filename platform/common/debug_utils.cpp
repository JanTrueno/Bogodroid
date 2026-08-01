#include "debug_utils.h"

#include "gles2.h"
#include "glad.h"
#include "glad_egl.h"

#include <iostream>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>

void print_native_callbacks(ANativeActivity nActivity)
{
    printf("onStart: %p\n", (void*)nActivity.callbacks->onStart);
    printf("onResume: %p\n", (void*)nActivity.callbacks->onResume);
    printf("onSaveInstanceState: %p\n", (void*)nActivity.callbacks->onSaveInstanceState);
    printf("onPause: %p\n", (void*)nActivity.callbacks->onPause);
    printf("onStop: %p\n", (void*)nActivity.callbacks->onStop);
    printf("onDestroy: %p\n", (void*)nActivity.callbacks->onDestroy);
    printf("onWindowFocusChanged: %p\n", (void*)nActivity.callbacks->onWindowFocusChanged);
    printf("onNativeWindowCreated: %p\n", (void*)nActivity.callbacks->onNativeWindowCreated);
    printf("onNativeWindowResized: %p\n", (void*)nActivity.callbacks->onNativeWindowResized);
    printf("onNativeWindowRedrawNeeded: %p\n", (void*)nActivity.callbacks->onNativeWindowRedrawNeeded);
    printf("onNativeWindowDestroyed: %p\n", (void*)nActivity.callbacks->onNativeWindowDestroyed);
    printf("onInputQueueCreated: %p\n", (void*)nActivity.callbacks->onInputQueueCreated);
    printf("onInputQueueDestroyed: %p\n", (void*)nActivity.callbacks->onInputQueueDestroyed);
    printf("onContentRectChanged: %p\n", (void*)nActivity.callbacks->onContentRectChanged);
    printf("onConfigurationChanged: %p\n", (void*)nActivity.callbacks->onConfigurationChanged);
    printf("onLowMemory: %p\n", (void*)nActivity.callbacks->onLowMemory);
}

void segfault_handler(int signal) {
    void *array[50];  // Array to store stack trace addresses
    size_t size;

    // Get the stack trace
    size = backtrace(array, 50);

    // Print the stack trace
    std::cerr << "Error: signal " << signal << ":\n";
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    // exit() is not async-signal-safe: it runs atexit handlers and static
    // destructors, which can deadlock if the crash happened while a lock
    // they need was held (e.g. mid GPU-driver-call) -- turning a clean crash
    // into a process that's technically alive but permanently stuck,
    // requiring a hard reboot. _exit() skips all that and terminates now.
    _exit(1);
}

// Print the faulting address and program counter (arm64) alongside the
// backtrace -- the PC pinpoints the crashing instruction, which the plain
// backtrace() chain only implies. Registered by print_backtrace_on_segfault.
void segfault_handler_si(int signal, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t pc = 0;
#ifdef __aarch64__
    pc = uc->uc_mcontext.pc;
    uintptr_t sp = uc->uc_mcontext.sp;
    uintptr_t lr = uc->uc_mcontext.regs[30];
#else
    pc = uc->uc_mcontext.gregs[REG_PC];
    uintptr_t sp = uc->uc_mcontext.gregs[REG_SP];
    uintptr_t lr = uc->uc_mcontext.gregs[REG_LR];
#endif
    fprintf(stderr, "Error: signal %d (fault addr %p, pc %p, lr %p, sp %p):\n",
            signal, si ? si->si_addr : 0, (void *)pc, (void *)lr, (void *)sp);

    void *array[50];
    size_t size = backtrace(array, 50);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    _exit(1);
}

void exit_handler(int signal) {
    printf("Caught signal %d, exiting...\n", signal);
    exit(0);
}

void print_backtrace_on_segfault()
{
    struct sigaction sa;
    sa.sa_sigaction = segfault_handler_si;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

void exit_on_signals()
{
    struct sigaction sa;
    sa.sa_handler = exit_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}