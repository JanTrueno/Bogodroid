// cursor.cpp -- the stick-driven on-screen pointer.
//
// A white dot with a black rim, drawn as a single quad with the shape done in
// the fragment shader: distance from the quad's centre picks white, black or
// transparent. That keeps it resolution independent and needs no texture.

#include "cursor.h"

#include <stdio.h>
#include <string.h>

#include "glad.h"

namespace
{

struct {
    GLuint prog = 0;
    GLint loc_pos = -1;   // vec2 attribute, quad corners in NDC
    GLint loc_uv = -1;    // -1..1 across the quad, for the distance test
    GLint loc_inner = -1; // white radius
    GLint loc_outer = -1; // black rim radius
    bool tried = false;
} cur;

GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        printf("cursor: shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

void cursor_init()
{
    if (cur.tried)
        return;
    cur.tried = true;

    const GLuint vs = compile(GL_VERTEX_SHADER,
        "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;"
        "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }");
    const GLuint fs = compile(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 vUV;"
        "uniform float inner; uniform float outer;"
        "void main(){"
        "  float d = length(vUV);"
        "  if (d > outer) discard;"
        "  float a = d < inner ? 1.0 : 0.0;"
        "  gl_FragColor = vec4(a, a, a, 1.0);"
        "}");
    if (!vs || !fs)
        return;

    cur.prog = glCreateProgram();
    glAttachShader(cur.prog, vs);
    glAttachShader(cur.prog, fs);
    glLinkProgram(cur.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(cur.prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        printf("cursor: shader link failed\n");
        glDeleteProgram(cur.prog);
        cur.prog = 0;
        return;
    }

    cur.loc_pos = glGetAttribLocation(cur.prog, "aPos");
    cur.loc_uv = glGetAttribLocation(cur.prog, "aUV");
    cur.loc_inner = glGetUniformLocation(cur.prog, "inner");
    cur.loc_outer = glGetUniformLocation(cur.prog, "outer");
}

void cursor_deinit()
{
    if (cur.prog)
        glDeleteProgram(cur.prog);
    cur.prog = 0;
    cur.tried = false;
}

void cursor_draw(float x, float y, int view_w, int view_h)
{
    if (!cur.prog || view_w <= 0 || view_h <= 0)
        return;

    // Radius as a fraction of the smaller view dimension, so the pointer is
    // the same apparent size whatever the resolution.
    const int small = view_w < view_h ? view_w : view_h;
    const float radius = (float)small * 0.018f;

    // quad in NDC around the cursor; y flips because view space is top-down
    const float cx = (x / (float)view_w) * 2.0f - 1.0f;
    const float cy = 1.0f - (y / (float)view_h) * 2.0f;
    const float rx = radius / (float)view_w * 2.0f;
    const float ry = radius / (float)view_h * 2.0f;

    const GLfloat pos[8] = {
        cx - rx, cy - ry,
        cx + rx, cy - ry,
        cx - rx, cy + ry,
        cx + rx, cy + ry,
    };
    static const GLfloat uv[8] = {-1, -1, 1, -1, -1, 1, 1, 1};

    GLboolean had_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean had_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean had_blend = glIsEnabled(GL_BLEND);
    GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(cur.prog);
    // 0.62 leaves a rim about a third of the radius thick -- enough to stay
    // visible against both light and dark backgrounds
    glUniform1f(cur.loc_inner, 0.62f);
    glUniform1f(cur.loc_outer, 1.0f);
    glEnableVertexAttribArray(cur.loc_pos);
    glEnableVertexAttribArray(cur.loc_uv);
    glVertexAttribPointer(cur.loc_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(cur.loc_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(cur.loc_pos);
    glDisableVertexAttribArray(cur.loc_uv);

    // the engine keeps its own GL state across frames; leave it as we found it
    if (had_depth) glEnable(GL_DEPTH_TEST);
    if (had_cull) glEnable(GL_CULL_FACE);
    if (had_blend) glEnable(GL_BLEND);
    if (had_scissor) glEnable(GL_SCISSOR_TEST);
}
