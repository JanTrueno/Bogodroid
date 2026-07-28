// osk.cpp -- minimal on-screen keyboard.
//
// Ortholinear grid, drawn as solid quads with a built-in 5x7 bitmap font, so
// there is no font library and no texture involved. Everything is in game-view
// coordinates, which means it lines up with the engine's touch points, works
// with the stick pointer, and rotates with the display without extra work.

#include "osk.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "glad.h"
#include "javastubs/layton.h"

namespace
{

// --- 5x7 font, one byte per row, low 5 bits used -------------------------
struct Glyph {
    char c;
    unsigned char rows[7];
};

const Glyph FONT[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
};

const Glyph *find_glyph(char c)
{
    for (const Glyph &g : FONT)
        if (g.c == c)
            return &g;
    return nullptr;
}

// --- keys ------------------------------------------------------------------

enum Action { ACT_CHAR, ACT_BACK, ACT_SPACE, ACT_OK, ACT_CANCEL };

struct Key {
    const char *label;
    Action action;
    char ch;
    float x, y, w, h; // view space
};

std::vector<Key> keys;
int laid_out_w = 0, laid_out_h = 0;
bool laid_out_numeric = false;

bool prev_pressed = false;
int pressed_key = -1; // highlighted while held

// A press that began on the keyboard belongs to the keyboard until it is
// released -- including after OK/ESC has closed it. Without this the still-held
// touch reaches the game on the very next frame and lands on whatever was
// behind the key, which is an easy way to hit a back button by accident.
bool swallow_until_release = false;

// The engine opening a field is what normally shows the keyboard. Start is an
// override on top of that, in whichever direction makes sense: dismiss it
// during a field, or call it up when there is no field at all.
bool forced_visible = false; // shown by hand with no field open
bool user_hidden = false;    // dismissed by hand during a field
bool prev_editing = false;

// Rows are plain strings; a '_' marks a wide/special key handled by index.
const char *TEXT_ROWS[] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL\x01", // \x01 = DEL
    "ZXCVBNM\x02\x03\x04", // SPC, OK, ESC
};
const char *NUM_ROWS[] = {
    "123",
    "456",
    "789",
    "0\x01\x03", // DEL, OK
};

void add_key(char c, float x, float y, float w, float h)
{
    Key k{};
    k.x = x; k.y = y; k.w = w; k.h = h;
    switch (c)
    {
    case '\x01': k.label = "DEL"; k.action = ACT_BACK; break;
    case '\x02': k.label = "SPC"; k.action = ACT_SPACE; break;
    case '\x03': k.label = "OK";  k.action = ACT_OK; break;
    case '\x04': k.label = "ESC"; k.action = ACT_CANCEL; break;
    default:
        k.action = ACT_CHAR;
        k.ch = c;
        k.label = nullptr;
        break;
    }
    keys.push_back(k);
}

// The keyboard sits across the bottom of the view. Ortholinear: every key is
// the same size, which keeps the layout and the hit test trivial.
void layout(int view_w, int view_h)
{
    const bool numeric = layton_ime::type() != 0;
    if (view_w == laid_out_w && view_h == laid_out_h && numeric == laid_out_numeric)
        return;
    laid_out_w = view_w;
    laid_out_h = view_h;
    laid_out_numeric = numeric;
    keys.clear();

    const char **rows = numeric ? NUM_ROWS : TEXT_ROWS;
    const int nrows = 4;
    int ncols = 0;
    for (int r = 0; r < nrows; r++)
        ncols = (int)strlen(rows[r]) > ncols ? (int)strlen(rows[r]) : ncols;

    // occupy the lower part of the view, centred horizontally
    const float pad = (float)view_w * 0.01f;
    const float boardW = (float)view_w * (numeric ? 0.5f : 0.94f);
    const float keyW = (boardW - pad * (ncols + 1)) / (float)ncols;
    const float keyH = keyW * 0.85f;
    const float boardH = keyH * nrows + pad * (nrows + 1);
    const float originX = ((float)view_w - boardW) * 0.5f;
    const float originY = (float)view_h - boardH - pad * 2.0f;

    for (int r = 0; r < nrows; r++)
    {
        const int len = (int)strlen(rows[r]);
        // centre short rows within the board
        const float rowW = keyW * len + pad * (len - 1);
        const float x0 = originX + (boardW - rowW) * 0.5f;
        for (int c = 0; c < len; c++)
        {
            add_key(rows[r][c],
                    x0 + c * (keyW + pad),
                    originY + pad + r * (keyH + pad),
                    keyW, keyH);
        }
    }
}

int key_at(float x, float y)
{
    for (size_t i = 0; i < keys.size(); i++)
    {
        const Key &k = keys[i];
        if (x >= k.x && x <= k.x + k.w && y >= k.y && y <= k.y + k.h)
            return (int)i;
    }
    return -1;
}

void activate(const Key &k)
{
    switch (k.action)
    {
    case ACT_CHAR: { const char s[2] = {k.ch, 0}; layton_ime::append(s); break; }
    case ACT_BACK: layton_ime::backspace(); break;
    case ACT_SPACE: layton_ime::append(" "); break;
    case ACT_OK: layton_ime::commit(); forced_visible = false; user_hidden = false; break;
    case ACT_CANCEL: layton_ime::cancel(); forced_visible = false; user_hidden = false; break;
    }
}

// --- drawing ---------------------------------------------------------------

struct {
    GLuint prog = 0;
    GLint loc_pos = -1;
    GLint loc_col = -1;
    bool tried = false;
} gl;

GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

void gl_init()
{
    if (gl.tried)
        return;
    gl.tried = true;
    const GLuint vs = compile(GL_VERTEX_SHADER,
        "attribute vec2 aPos;"
        "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }");
    const GLuint fs = compile(GL_FRAGMENT_SHADER,
        "precision mediump float; uniform vec4 uCol;"
        "void main(){ gl_FragColor = uCol; }");
    gl.prog = glCreateProgram();
    glAttachShader(gl.prog, vs);
    glAttachShader(gl.prog, fs);
    glLinkProgram(gl.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(gl.prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        printf("osk: shader link failed\n");
        glDeleteProgram(gl.prog);
        gl.prog = 0;
        return;
    }
    gl.loc_pos = glGetAttribLocation(gl.prog, "aPos");
    gl.loc_col = glGetUniformLocation(gl.prog, "uCol");
}

std::vector<GLfloat> verts;

// view-space rect -> two NDC triangles
void push_rect(float x, float y, float w, float h, int view_w, int view_h)
{
    const float x0 = (x / view_w) * 2.0f - 1.0f;
    const float x1 = ((x + w) / view_w) * 2.0f - 1.0f;
    const float y0 = 1.0f - (y / view_h) * 2.0f;
    const float y1 = 1.0f - ((y + h) / view_h) * 2.0f;
    const GLfloat q[12] = {x0, y0, x1, y0, x0, y1, x0, y1, x1, y0, x1, y1};
    verts.insert(verts.end(), q, q + 12);
}

void flush(float r, float g, float b, float a)
{
    if (verts.empty() || !gl.prog)
    {
        verts.clear();
        return;
    }
    glUseProgram(gl.prog);
    glUniform4f(gl.loc_col, r, g, b, a);
    glEnableVertexAttribArray(gl.loc_pos);
    glVertexAttribPointer(gl.loc_pos, 2, GL_FLOAT, GL_FALSE, 0, verts.data());
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 2));
    glDisableVertexAttribArray(gl.loc_pos);
    verts.clear();
}

// Largest pixel size that fits n glyphs inside box_w x box_h. A glyph is 5x7
// pixels with a 1-pixel gap between characters, so n of them span (6n-1)
// pixels wide and 7 tall -- sizing off key height alone (as this first did)
// makes single characters overflow the key and forces the three-letter labels
// to be shrunk to fit.
float fit_px(int n, float box_w, float box_h)
{
    const float by_w = box_w / (float)(6 * n - 1);
    const float by_h = box_h / 7.0f;
    return by_w < by_h ? by_w : by_h;
}

void push_text(const char *s, float cx, float cy, float px, int view_w, int view_h)
{
    const int n = (int)strlen(s);
    const float glyphW = 5 * px, glyphH = 7 * px, gap = px;
    const float totalW = n * glyphW + (n - 1) * gap;
    float x = cx - totalW * 0.5f;
    const float y = cy - glyphH * 0.5f;

    for (int i = 0; i < n; i++)
    {
        const Glyph *g = find_glyph(s[i]);
        if (g)
        {
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (g->rows[row] & (1 << (4 - col)))
                        push_rect(x + col * px, y + row * px, px, px, view_w, view_h);
        }
        x += glyphW + gap;
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool osk_active()
{
    const bool editing = layton_ime::editing();
    // a newly opened field starts visible again, whatever was dismissed before
    if (editing && !prev_editing)
    {
        user_hidden = false;
        forced_visible = false;
    }
    prev_editing = editing;

    return editing ? !user_hidden : forced_visible;
}

void osk_toggle()
{
    if (layton_ime::editing())
        user_hidden = !user_hidden; // dismiss/restore over the game's field
    else
        forced_visible = !forced_visible;
    pressed_key = -1;
}

bool osk_consuming() { return swallow_until_release; }

void osk_update(bool pressed, float x, float y, int view_w, int view_h)
{
    const bool active = osk_active();

    if (!pressed)
    {
        // release clears everything, including the swallow latch
        swallow_until_release = false;
        prev_pressed = false;
        pressed_key = -1;
        return;
    }

    if (!active)
    {
        // a press that began away from the keyboard is the game's to handle
        prev_pressed = true;
        pressed_key = -1;
        return;
    }

    layout(view_w, view_h);
    const int hit = key_at(x, y);
    pressed_key = hit;

    // Rising edge only, so holding does not repeat. Anything pressed while the
    // keyboard is up belongs to it -- including the panel between keys, which
    // would otherwise fall through to the game.
    if (!prev_pressed)
    {
        swallow_until_release = true;
        if (hit >= 0)
            activate(keys[hit]);
    }
    prev_pressed = true;
}

void osk_draw(int view_w, int view_h)
{
    if (!osk_active())
        return;
    gl_init();
    if (!gl.prog)
        return;
    layout(view_w, view_h);
    if (keys.empty())
        return;

    GLboolean had_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean had_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean had_blend = glIsEnabled(GL_BLEND);
    GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // backing panel
    const Key &first = keys.front();
    const Key &last = keys.back();
    const float pad = (float)view_w * 0.01f;
    push_rect(first.x - pad, first.y - pad,
              (last.x + last.w) - first.x + pad * 2.0f,
              (last.y + last.h) - first.y + pad * 2.0f, view_w, view_h);
    flush(0.05f, 0.05f, 0.07f, 0.88f);

    // the current text, above the keys
    const std::string &txt = layton_ime::text();
    if (!txt.empty())
    {
        // never wider than the board, however long the entry gets
        const float px = fit_px((int)txt.size(), (last.x + last.w) - first.x, first.h * 0.6f);
        push_text(txt.c_str(), (float)view_w * 0.5f, first.y - pad * 3.5f, px, view_w, view_h);
        flush(1.0f, 1.0f, 1.0f, 1.0f);
    }

    // keys, then the held one on top in a lighter shade
    for (size_t i = 0; i < keys.size(); i++)
        if ((int)i != pressed_key)
            push_rect(keys[i].x, keys[i].y, keys[i].w, keys[i].h, view_w, view_h);
    flush(0.20f, 0.20f, 0.24f, 0.95f);

    if (pressed_key >= 0)
    {
        const Key &k = keys[pressed_key];
        push_rect(k.x, k.y, k.w, k.h, view_w, view_h);
        flush(0.55f, 0.55f, 0.62f, 0.95f);
    }

    // labels: single characters big, three-letter names smaller so they fit
    for (const Key &k : keys)
    {
        const char buf[2] = {k.ch, 0};
        const char *label = k.label ? k.label : buf;
        // fill most of the key, leaving a margin so glyphs never touch the edge
        const float px = fit_px((int)strlen(label), k.w * 0.72f, k.h * 0.52f);
        push_text(label, k.x + k.w * 0.5f, k.y + k.h * 0.5f, px, view_w, view_h);
    }
    flush(0.95f, 0.95f, 0.95f, 1.0f);

    if (had_depth) glEnable(GL_DEPTH_TEST);
    if (had_cull) glEnable(GL_CULL_FACE);
    if (!had_blend) glDisable(GL_BLEND);
    if (had_scissor) glEnable(GL_SCISSOR_TEST);
}

void osk_deinit()
{
    if (gl.prog)
        glDeleteProgram(gl.prog);
    gl.prog = 0;
    gl.tried = false;
    keys.clear();
    laid_out_w = laid_out_h = 0;
}
