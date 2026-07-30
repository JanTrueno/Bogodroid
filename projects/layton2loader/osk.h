#ifndef __OSK_H__
#define __OSK_H__

// Minimal on-screen keyboard for the engine's text fields.
//
// The game asks for text through UI_StartEditText and then polls
// UI_GetEditState until it clears (see javastubs/layton.h). On Android the
// system IME handled that; here the loader draws its own, so the port is
// usable without a physical keyboard.
//
// Everything is in game-view coordinates, the same space the engine's touch
// points use, so it lines up with both the touchscreen and the stick pointer
// and rotates with the display for free.

// True while the engine wants text, i.e. while the keyboard should be shown.
bool osk_active();

// Start button: show/hide it by hand, for when the engine's field is already
// open or you just want to check it.
void osk_toggle();

// Feed the pointer once per frame, whether or not the keyboard is shown --
// it has to see the release that ends a press. `pressed` is the current
// touch/click state; presses are edge-detected, so holding does not repeat.
void osk_update(bool pressed, float x, float y, int view_w, int view_h);

// True while a press that began on the keyboard is still held. The caller must
// keep that input away from the game until it clears, otherwise closing the
// keyboard with OK hands the still-held touch straight to whatever is behind.
bool osk_consuming();

// Draw into the current render target; call after the game's frame.
void osk_draw(int view_w, int view_h);

void osk_deinit();

#endif
