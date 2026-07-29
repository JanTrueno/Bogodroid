#ifndef AINPUT_H
#define AINPUT_H

// Native input event queue (NDK android/input.h).
//
// Apps built on ANativeActivity never see Java KeyEvent/MotionEvent objects:
// the framework hands them an AInputQueue, they attach it to their thread's
// ALooper, and pull AInputEvents out of it as the looper wakes. libLimbo.so
// takes this path exclusively -- it imports AInputQueue_getEvent /
// AKeyEvent_getKeyCode / AMotionEvent_getAxisValue and registers no JNI input
// methods at all, so platform/common/input_backend.cpp (which builds Java-side
// jnivm KeyEvent/MotionEvent objects) cannot drive it.
//
// The queue here is fed by the loader instead of by the system: the loader
// translates its host events (SDL) and calls the AInputQueue_push* helpers at
// the bottom of this header. Delivery is a pipe so the engine's
// ALooper_pollOnce blocks and wakes exactly like it would on Android.
//
// Android's AKEYCODE_* values are numerically identical to the Java
// KeyEvent.KEYCODE_* constants, so a caller may reuse the mapping tables in
// platform/common/input_backend.cpp for the keyCode argument.

#include <stdint.h>
#include "alooper.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- event types ----
enum {
    AINPUT_EVENT_TYPE_KEY = 1,
    AINPUT_EVENT_TYPE_MOTION = 2,
};

// ---- key actions ----
enum {
    AKEY_EVENT_ACTION_DOWN = 0,
    AKEY_EVENT_ACTION_UP = 1,
    AKEY_EVENT_ACTION_MULTIPLE = 2,
};

// ---- motion actions ----
enum {
    AMOTION_EVENT_ACTION_MASK = 0xff,
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK = 0xff00,
    AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT = 8,
    AMOTION_EVENT_ACTION_DOWN = 0,
    AMOTION_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_MOVE = 2,
    AMOTION_EVENT_ACTION_CANCEL = 3,
    AMOTION_EVENT_ACTION_OUTSIDE = 4,
    AMOTION_EVENT_ACTION_POINTER_DOWN = 5,
    AMOTION_EVENT_ACTION_POINTER_UP = 6,
    AMOTION_EVENT_ACTION_HOVER_MOVE = 7,
    AMOTION_EVENT_ACTION_SCROLL = 8,
};

// ---- input sources ----
enum {
    AINPUT_SOURCE_UNKNOWN = 0x00000000,
    AINPUT_SOURCE_KEYBOARD = 0x00000101,
    AINPUT_SOURCE_DPAD = 0x00000201,
    AINPUT_SOURCE_GAMEPAD = 0x00000401,
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001002,
    AINPUT_SOURCE_MOUSE = 0x00002002,
    AINPUT_SOURCE_JOYSTICK = 0x01000010,
};

// ---- motion axes ----
enum {
    AMOTION_EVENT_AXIS_X = 0,
    AMOTION_EVENT_AXIS_Y = 1,
    AMOTION_EVENT_AXIS_PRESSURE = 2,
    AMOTION_EVENT_AXIS_SIZE = 3,
    AMOTION_EVENT_AXIS_Z = 11,
    AMOTION_EVENT_AXIS_RX = 12,
    AMOTION_EVENT_AXIS_RY = 13,
    AMOTION_EVENT_AXIS_RZ = 14,
    AMOTION_EVENT_AXIS_HAT_X = 15,
    AMOTION_EVENT_AXIS_HAT_Y = 16,
    AMOTION_EVENT_AXIS_LTRIGGER = 17,
    AMOTION_EVENT_AXIS_RTRIGGER = 18,
    AMOTION_EVENT_AXIS_GAS = 22,
    AMOTION_EVENT_AXIS_BRAKE = 23,
};

#define AINPUT_AXIS_COUNT 48    // axis ids 0..47 (NDK defines up to AXIS_GENERIC_16 == 47)
#define AINPUT_MAX_POINTERS 8

typedef struct AInputEvent AInputEvent;
typedef struct AInputQueue AInputQueue;

// ---- the API libLimbo.so imports ----
int32_t AInputEvent_getType(const AInputEvent* event);
int32_t AInputEvent_getDeviceId(const AInputEvent* event);
int32_t AInputEvent_getSource(const AInputEvent* event);

int32_t AKeyEvent_getAction(const AInputEvent* event);
int32_t AKeyEvent_getKeyCode(const AInputEvent* event);
int32_t AKeyEvent_getRepeatCount(const AInputEvent* event);
int32_t AKeyEvent_getFlags(const AInputEvent* event);
int32_t AKeyEvent_getMetaState(const AInputEvent* event);

int32_t AMotionEvent_getAction(const AInputEvent* event);
int32_t AMotionEvent_getFlags(const AInputEvent* event);
size_t AMotionEvent_getPointerCount(const AInputEvent* event);
int32_t AMotionEvent_getPointerId(const AInputEvent* event, size_t pointer_index);
float AMotionEvent_getX(const AInputEvent* event, size_t pointer_index);
float AMotionEvent_getY(const AInputEvent* event, size_t pointer_index);
float AMotionEvent_getAxisValue(const AInputEvent* event, int32_t axis, size_t pointer_index);

void AInputQueue_attachLooper(AInputQueue* queue, ALooper* looper, int ident,
    ALooper_callbackFunc callback, void* data);
void AInputQueue_detachLooper(AInputQueue* queue);
int32_t AInputQueue_hasEvents(AInputQueue* queue);
int32_t AInputQueue_getEvent(AInputQueue* queue, AInputEvent** outEvent);
int32_t AInputQueue_preDispatchEvent(AInputQueue* queue, AInputEvent* event);
void AInputQueue_finishEvent(AInputQueue* queue, AInputEvent* event, int handled);

// ---- loader-side API (not part of the NDK) ----

// Create/destroy the queue the loader hands to onInputQueueCreated.
AInputQueue* AInputQueue_create(void);
void AInputQueue_destroy(AInputQueue* queue);

// Enqueue events from the host. Safe to call from the loader's thread while
// the engine polls from its own.
void AInputQueue_pushKeyEvent(AInputQueue* queue, int32_t deviceId, int32_t source,
    int32_t action, int32_t keyCode, int32_t repeatCount);

// Absolute-position motion (touch/mouse): x/y are in view pixels.
void AInputQueue_pushMotionEvent(AInputQueue* queue, int32_t deviceId, int32_t source,
    int32_t action, float x, float y);

// Joystick/gamepad axis update. `axes` is an AINPUT_AXIS_COUNT-entry table
// indexed by AMOTION_EVENT_AXIS_*; the whole axis state is sent every time,
// matching how Android reports joystick ACTION_MOVE.
void AInputQueue_pushJoystickEvent(AInputQueue* queue, int32_t deviceId, int32_t source,
    const float* axes);

#ifdef __cplusplus
}
#endif

#endif // AINPUT_H
