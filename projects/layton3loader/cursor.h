#ifndef __CURSOR_H__
#define __CURSOR_H__

// On-screen pointer for stick/d-pad control.
//
// Drawn in game-view coordinates into whatever target the game just rendered
// to (the rotation FBO, or the window when rotation is off), so it sits
// exactly on the touch point fed to the engine and rotates along with the
// game. Same approach as the Switch port's cursor_draw().

void cursor_init();
void cursor_deinit();

// x/y are in view space; view_w/view_h size that space. The dot scales with
// the view so it stays the same apparent size at any resolution.
void cursor_draw(float x, float y, int view_w, int view_h);

#endif
