#include "ainput.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

struct AInputEvent {
    int32_t type;
    int32_t deviceId;
    int32_t source;
    int32_t action;
    int32_t flags;
    int32_t metaState;

    // key
    int32_t keyCode;
    int32_t repeatCount;

    // motion
    size_t pointerCount;
    int32_t pointerId[AINPUT_MAX_POINTERS];
    float axes[AINPUT_MAX_POINTERS][AINPUT_AXIS_COUNT];

    struct AInputEvent* next;
};

struct AInputQueue {
    // One byte is written per queued event and read per dequeued event, so a
    // level-triggered poll on read_fd stays readable exactly while events
    // remain -- that is what makes ALooper_pollOnce behave like Android's.
    int read_fd;
    int write_fd;

    ALooper* looper;
    int ident;
    ALooper_callbackFunc callback;
    void* data;

    pthread_mutex_t mutex;
    AInputEvent* head;
    AInputEvent* tail;
};

// ---- helpers ----

static AInputEvent* event_new(int32_t type, int32_t deviceId, int32_t source, int32_t action)
{
    AInputEvent* e = (AInputEvent*)calloc(1, sizeof(AInputEvent));
    if (!e)
        return NULL;
    e->type = type;
    e->deviceId = deviceId;
    e->source = source;
    e->action = action;
    return e;
}

// Append to the queue and make the fd readable so a polling looper wakes.
static void queue_push(AInputQueue* queue, AInputEvent* e)
{
    if (!queue || !e)
        return;

    pthread_mutex_lock(&queue->mutex);
    if (queue->tail)
        queue->tail->next = e;
    else
        queue->head = e;
    queue->tail = e;
    pthread_mutex_unlock(&queue->mutex);

    const uint8_t token = 1;
    (void)!write(queue->write_fd, &token, 1);
}

// ---- lifecycle ----

AInputQueue* AInputQueue_create(void)
{
    AInputQueue* queue = (AInputQueue*)calloc(1, sizeof(AInputQueue));
    if (!queue)
        return NULL;

    int fds[2];
    if (pipe(fds) != 0) {
        free(queue);
        return NULL;
    }
    // Non-blocking so a spurious wake never parks the engine inside getEvent.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

    queue->read_fd = fds[0];
    queue->write_fd = fds[1];
    pthread_mutex_init(&queue->mutex, NULL);
    return queue;
}

void AInputQueue_destroy(AInputQueue* queue)
{
    if (!queue)
        return;

    AInputQueue_detachLooper(queue);

    pthread_mutex_lock(&queue->mutex);
    AInputEvent* e = queue->head;
    while (e) {
        AInputEvent* next = e->next;
        free(e);
        e = next;
    }
    queue->head = queue->tail = NULL;
    pthread_mutex_unlock(&queue->mutex);

    close(queue->read_fd);
    close(queue->write_fd);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

// ---- looper attachment ----

void AInputQueue_attachLooper(AInputQueue* queue, ALooper* looper, int ident,
    ALooper_callbackFunc callback, void* data)
{
    if (!queue || !looper)
        return;

    queue->looper = looper;
    queue->ident = ident;
    queue->callback = callback;
    queue->data = data;

    ALooper_addFd(looper, queue->read_fd, ident, ALOOPER_EVENT_INPUT, callback, data);
}

void AInputQueue_detachLooper(AInputQueue* queue)
{
    if (!queue || !queue->looper)
        return;

    ALooper_removeFd(queue->looper, queue->read_fd);
    queue->looper = NULL;
    queue->callback = NULL;
    queue->data = NULL;
}

// ---- consumption ----

int32_t AInputQueue_hasEvents(AInputQueue* queue)
{
    if (!queue)
        return 0;
    pthread_mutex_lock(&queue->mutex);
    const int has = queue->head != NULL;
    pthread_mutex_unlock(&queue->mutex);
    return has;
}

int32_t AInputQueue_getEvent(AInputQueue* queue, AInputEvent** outEvent)
{
    if (!queue || !outEvent)
        return -1;

    pthread_mutex_lock(&queue->mutex);
    AInputEvent* e = queue->head;
    if (e) {
        queue->head = e->next;
        if (!queue->head)
            queue->tail = NULL;
        e->next = NULL;
    }
    pthread_mutex_unlock(&queue->mutex);

    if (!e) {
        *outEvent = NULL;
        return -1;    // NDK: negative means "no events available"
    }

    // Consume the matching wake token so the fd stops signalling once drained.
    uint8_t token;
    (void)!read(queue->read_fd, &token, 1);

    *outEvent = e;
    return 0;
}

// Android returns non-zero when it consumed the event itself (IME pre-dispatch).
// There is no IME here, so the app always gets to handle it.
int32_t AInputQueue_preDispatchEvent(AInputQueue* queue, AInputEvent* event)
{
    (void)queue;
    (void)event;
    return 0;
}

void AInputQueue_finishEvent(AInputQueue* queue, AInputEvent* event, int handled)
{
    (void)queue;
    (void)handled;
    free(event);
}

// ---- event accessors ----

int32_t AInputEvent_getType(const AInputEvent* event) { return event ? event->type : 0; }
int32_t AInputEvent_getDeviceId(const AInputEvent* event) { return event ? event->deviceId : 0; }
int32_t AInputEvent_getSource(const AInputEvent* event) { return event ? event->source : AINPUT_SOURCE_UNKNOWN; }

int32_t AKeyEvent_getAction(const AInputEvent* event) { return event ? event->action : 0; }
int32_t AKeyEvent_getKeyCode(const AInputEvent* event) { return event ? event->keyCode : 0; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent* event) { return event ? event->repeatCount : 0; }
int32_t AKeyEvent_getFlags(const AInputEvent* event) { return event ? event->flags : 0; }
int32_t AKeyEvent_getMetaState(const AInputEvent* event) { return event ? event->metaState : 0; }

int32_t AMotionEvent_getAction(const AInputEvent* event) { return event ? event->action : 0; }
int32_t AMotionEvent_getFlags(const AInputEvent* event) { return event ? event->flags : 0; }
size_t AMotionEvent_getPointerCount(const AInputEvent* event) { return event ? event->pointerCount : 0; }

int32_t AMotionEvent_getPointerId(const AInputEvent* event, size_t pointer_index)
{
    if (!event || pointer_index >= event->pointerCount)
        return -1;
    return event->pointerId[pointer_index];
}

float AMotionEvent_getAxisValue(const AInputEvent* event, int32_t axis, size_t pointer_index)
{
    if (!event || pointer_index >= event->pointerCount)
        return 0.0f;
    if (axis < 0 || axis >= AINPUT_AXIS_COUNT)
        return 0.0f;
    return event->axes[pointer_index][axis];
}

float AMotionEvent_getX(const AInputEvent* event, size_t pointer_index)
{
    return AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, pointer_index);
}

float AMotionEvent_getY(const AInputEvent* event, size_t pointer_index)
{
    return AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, pointer_index);
}

// ---- loader-side producers ----

void AInputQueue_pushKeyEvent(AInputQueue* queue, int32_t deviceId, int32_t source,
    int32_t action, int32_t keyCode, int32_t repeatCount)
{
    AInputEvent* e = event_new(AINPUT_EVENT_TYPE_KEY, deviceId, source, action);
    if (!e)
        return;
    e->keyCode = keyCode;
    e->repeatCount = repeatCount;
    queue_push(queue, e);
}

void AInputQueue_pushMotionEvent(AInputQueue* queue, int32_t deviceId, int32_t source,
    int32_t action, float x, float y)
{
    AInputEvent* e = event_new(AINPUT_EVENT_TYPE_MOTION, deviceId, source, action);
    if (!e)
        return;
    e->pointerCount = 1;
    e->pointerId[0] = 0;
    e->axes[0][AMOTION_EVENT_AXIS_X] = x;
    e->axes[0][AMOTION_EVENT_AXIS_Y] = y;
    queue_push(queue, e);
}

void AInputQueue_pushJoystickEvent(AInputQueue* queue, int32_t deviceId, int32_t source,
    const float* axes)
{
    AInputEvent* e = event_new(AINPUT_EVENT_TYPE_MOTION, deviceId, source,
        AMOTION_EVENT_ACTION_MOVE);
    if (!e)
        return;
    e->pointerCount = 1;
    e->pointerId[0] = 0;
    if (axes)
        memcpy(e->axes[0], axes, sizeof(float) * AINPUT_AXIS_COUNT);
    queue_push(queue, e);
}
