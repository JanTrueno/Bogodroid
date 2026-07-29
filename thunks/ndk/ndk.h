#ifndef __NDK_BRIDGE_H__
#define __NDK_BRIDGE_H__

#include "platform.h"
#include "baron/baron.h"
#include "android.h"
#include <vector>

typedef struct
{
   // Number of bytes in this structure.
    uint32_t size;
} AConfiguration;

typedef struct
{

} ARect;

typedef struct
{

} ANativeWindow;

// AInputQueue / AInputEvent are real types now -- see thunks/ndk/ainput.h
#include "ainput.h"


ABI_ATTR extern AConfiguration* AConfiguration_new();

// Defined in thunks/ndk/ndk.cpp. Returns a shared, non-NULL placeholder window:
// the EGL shim ignores its contents (eglCreateWindowSurface_impl hands back the
// SDL surface), but ANativeActivity apps null-check the pointer and read
// width/height off it, so a loader must pass this rather than NULL to
// onNativeWindowCreated.
ABI_ATTR extern ANativeWindow* ANativeWindow_fromSurface(void*, void*);
ABI_ATTR extern int32_t ANativeWindow_getWidth(ANativeWindow* window);
ABI_ATTR extern int32_t ANativeWindow_getHeight(ANativeWindow* window);


#endif /* __NDK_BRIDGE_H__ */